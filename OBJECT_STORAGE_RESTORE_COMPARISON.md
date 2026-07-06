# Two-Step Object-Storage Restore Comparison

Prepared for the June 3, 2026 design discussion.

## Executive Framing

This is a staged decision.

**Step 1 ships object-storage restore quickly** by materializing checkpoint files locally:

```text
object storage -> local files on tmpfs or FUSE mount -> CRIU restore
```

This path optimizes for short implementation time, debuggability, and a fallback we can keep after the streamer lands.

**Step 2 chooses the streamer client** for the PoC-aligned design:

```text
object storage -> preallocated memfd/mmap -> CRIU async handoff -> PIE mmap zero-copy
```

This path optimizes for restore latency and memory-copy avoidance. It must be judged by compatibility with the streaming-memfd contract, not raw downloader throughput alone.

## Step 1: Quick IO Path

Question: how fast can we ship a useful restore from object storage without the streamer?

| Candidate | Role | API / Packaging | Storage Backends | Credentials / Endpoint Fit | Restore Shape | Many-Object / Concurrency | Upload Path | Observability | Ops Risk | Effort |
|---|---|---|---|---|---|---|---|---|---|---|
| `s5cmd` | Primary Step 1 baseline | CLI, single Go binary | S3, S3-compatible, GCS through S3 API | AWS SDK env/profile/credentials file, EC2 IAM, EKS IAM, custom endpoints, no-sign | Downloads checkpoint files to tmpfs; CRIU reads regular files | Strong: wildcard/listing, parallel copies, multipart/range behavior internally | `cp`, `sync`, `pipe` | CLI logs plus wrapper timings; per-request detail limited | CPU/cgroup throttling under `nsenter`; S3-focused; CLI-only | **S** |
| MSC | Multi-cloud Step 1 candidate | Python library, CLI, FUSE/MSFS, experimental Rust client for some providers | S3, S3-compatible, GCS, Azure, OCI, AIStore, POSIX, others | Provider config, cloud-default credentials, static creds, WIF/service-account style configs; endpoint support depends on provider | Download files or file-like objects; CRIU still reads regular files | `download_files(..., max_workers=N)` and sync APIs; must measure against `s5cmd` | `write`, `upload_file(s)`, `sync` | Telemetry exists; need exact per-file timings in our wrapper | Python/runtime dependency; throughput unknown; Rust-client scope must be verified | **M** |
| JuiceFS/FUSE | POSIX alternative | CSI/FUSE mount | Object-store-backed filesystem | CSI/FUSE and backing-store credentials | CRIU reads mounted POSIX files directly | FUSE/cache dependent; prior cold path limited by CRIU read pattern | Filesystem writes | FUSE/JuiceFS stats plus wrapper timings | Adds CSI/cache/metadata infra; cold vs warm semantics are operationally tricky | **M** |
| NIXL OBJ | Not preferred for Step 1 | Embeddable transfer library / OBJ plugin | Object-store backend through NIXL OBJ; exact provider support depends on build | Must validate OBJ credential/endpoint config | Could materialize files, but this wastes the library's value | Strong only if we build scheduling around it | Must validate | Transfer logs/status can be wired | More code than needed for quick path | **M-L** |
| runai-model-streamer | Not preferred for Step 1 | Python SDK with C++ backend | S3 package exists; public API is model/tensor oriented | Must validate S3 credential/endpoint behavior | Could be forced into file materialization, but it is not the natural API | Must validate outside model-loader flow | Unclear / not primary upstream story | Must build wrapper metrics | Adapter work before any Step 1 value | **M-L** |

Step 1 success criteria:

- Restore from object storage with no streaming-memfd handoff.
- End-to-end inference probe passes.
- Report `s3_download_s`, `criu_s`, `cuda_s`, `wake_up_s`, and `restore_total`.
- Report download GiB/s, CPU%, RSS, tmpfs or cache footprint, and retry/error counts where available.
- Measure checkpoint upload/capture path, not only restore download.
- Keep this path as the fallback after Step 2 lands.

Step 1 benchmark plan:

| Benchmark | Exact Workload | Required Result |
|---|---|---|
| `s5cmd -> tmpfs -> CRIU` | Full checkpoint prefix, Qwen3 0.6B / 8B / 14B; sweep `--numworkers` and per-object concurrency under fixed agent CPU | Establish simple cold baseline and upload timing |
| MSC-to-files -> CRIU | Same checkpoint and tmpfs target; sweep `download_files(..., max_workers=N)` and Rust-client config where applicable | Decide whether MSC is viable for Step 1 multi-cloud |
| JuiceFS/FUSE -> CRIU | Same checkpoint through FUSE; cold, warm, and explicit warmup | Keep only if POSIX visibility or repeated same-node restore is valuable |

## Step 2: Streamer / PoC Path

Question: which client can power the streaming-memfd PoC?

PoC contract:

| Requirement | Why It Matters |
|---|---|
| Range GET for `(object, offset, length)` | The streamer fetches ranges from large `pages-*.img` files, not only whole files. |
| Caller-owned destination | The client must write into preallocated `memfd`, `mmap`, fd, or a low-copy equivalent. Returning `bytes` may add an unacceptable copy. |
| Async completion | CRIU/PIE can release readiness only after the relevant range or memfd is complete. |
| Abort/cancel | Restore failure must stop in-flight object reads and avoid blocked PIE/daemon waits. |
| Retry/error/checksum semantics | Failed ranges must not signal readiness as success. |
| Upload story | Checkpoint creation still needs object writes, even if restore is the hot path. |
| Observability | Need per-object/range timing, throughput, retries, errors, CPU, and RSS. |

| Candidate | API / Language | Range GET | Destination Fit | Async / Completion Model | Error / Retry / Cancel | Upload Support | Build / Size / License / Maturity | Observability | PoC Fit | Effort |
|---|---|---|---|---|---|---|---|---|---|---|
| NIXL OBJ | C++/Python transfer library with OBJ backend | Yes in current local PoC shape | Best current fit: transfer into registered memory / memfd-oriented streamer | Transfer requests plus status polling; local PoC already uses this shape | Must harden terminal error, abort, and readiness propagation | Must validate for checkpoint creation | Larger native dependency stack; upstream NIXL is active; exact wheel/plugin contents must be pinned | Transfer status/logging can be wrapped; need per-range metrics | **High** | **S-M** |
| runai-model-streamer | Python SDK with C++ backend | Likely possible, but must prove arbitrary CRIU ranges | Local bench suggests mmap'd memfd destination is possible; public API is model/tensor oriented | Must validate lower-level request completion outside model loader | Must validate cancellation, retries, checksum/error propagation | Unclear / not primary upstream story | Apache-2.0; native libs including S3 package; recent releases; packaging risk | Local bench emits elapsed, GiB/s, CPU, peak RSS; production hooks unknown | **Medium-High** | **M-L** |
| MSC | Python client, CLI, FUSE/MSFS, experimental Rust client | Yes: API has `read(path, byte_range=...)` | `read` returns bytes, so likely extra copy; `download_file` accepts file-like destinations but fd/memfd behavior must be proven | Must build scheduler and per-range completion | Retry config exists; cancel/checksum semantics must be inspected | Yes: `write`, `upload_file(s)`, `sync` | Strong backend coverage; Python dependency plus optional Rust client; active NVIDIA project | Telemetry exists; need wrapper-level per-range timings | **Medium** | **M-L** |
| AWS CRT direct | C++ SDK / CRT | Yes | We can build exact memfd/mmap writes | We own scheduler/completion | We own retries, cancellation, checksum policy, and errors | Yes | Most custom code and packaging; mature AWS substrate | We own all instrumentation | **High potential** | **L** |
| `s5cmd` | CLI | Yes internally | Poor: files/stdout, no library-level memfd destination | CLI process completion only | CLI-level failures only | Yes | Simple binary, but not embeddable for this contract | Wrapper timings only | **Low** | **Poor fit** |
| JuiceFS/FUSE | FUSE/POSIX | Not a range-to-memfd API | Poor: CRIU reads via filesystem | Kernel/FUSE path, not streamer readiness | FUSE/filesystem semantics | Filesystem writes | Extra cluster infra | FUSE/JuiceFS stats | **Low** | **Not worth adapting** |

Step 2 success criteria:

- Fetch top `pages-*.img` byte ranges into preallocated `memfd` or `mmap` memory.
- Signal per-range or per-memfd completion to CRIU safely.
- Abort restore cleanly without blocked PIE/daemon waits.
- Beat or match Step 1 end-to-end restore, not just downloader-only throughput.
- Preserve Step 1 fallback.

Step 2 benchmark plan:

| Benchmark | Exact Workload | Required Result |
|---|---|---|
| NIXL OBJ streamer | Sweep `crtThroughputGbps`, chunk size, object count, and number of agents/clients; full Qwen3-14B restore | Decide whether the current PoC client is good enough after tuning |
| runai range-to-memfd | Run `runai-streamer-bench` variants: `single`, `many-files`, `byte-ranges`; then one full restore if it passes | Prove or reject runai as a CRIU range downloader |
| MSC range adapter | Fetch byte ranges via `read(byte_range=...)` and file-like/fd-backed destinations; measure copies and RSS | Decide whether MSC can be hot path or only abstraction/fallback |
| AWS CRT direct spike | Minimal C++ range scheduler into memfd | Keep only if NIXL OBJ, runai-model-streamer, and MSC cannot satisfy the contract |

## Shared Metrics

Collect these for both steps:

| Metric | Purpose |
|---|---|
| Download wall time | Basic downloader comparison |
| Effective GiB/s and wire Gbps | Normalize by data size and NIC ceiling |
| CPU% and peak RSS | Catch TLS, Python, copy, tmpfs, and memfd pressure |
| Request count, range count, concurrency | Explain scaling and object-store pressure |
| Retries, errors, checksum/version failures | Operational safety |
| `restore_total`, `s3_download_s`, `criu_s`, `cuda_s`, `wake_up_s` | End-to-end restore decision |
| Inference probe result | Correctness gate |
| LOC, dependencies, image size | Integration cost |

Use identical hardware for comparisons: same instance type, bucket/region, checkpoint ID, pod placement, NIC path, agent CPU/memory limits, cache-drop procedure, checkpoint layout, and benchmark harness version.

## Evidence Anchors

Known:

- `s5cmd -> tmpfs -> CRIU` is the current simple cold baseline. Prior April 12 notes show Qwen3 0.6B / 8B / 14B restore totals of **10.5s / 24.5s / 37.9s** on the larger AWS box, with 14B S3 download around **8.3s** and raw `s5cmd` peak around **6240 MB/s**.
- `nsrestore` runs through `nsenter` without changing cgroup namespace, so helper tools inherit the agent daemonset cgroup. Prior notes show CPU limits can throttle `s5cmd` throughput materially.
- NIXL OBJ throughput depends on `crtThroughputGbps`, object count, and client/agent shape. Single-object sweeps can be the wrong workload.
- The local `runai-streamer-bench` already matches Step 2 access patterns: `single`, `many-files`, and `byte-ranges` into mmap'd memfds, with `s5cmd` as comparison.
- JuiceFS/FUSE is useful for POSIX visibility and warm-cache comparison, but prior cold-path notes show it can be limited unless warmed.
- Streaming-memfd restore changes the CRIU-side cost model: private pages move from `io_submit` / `copy_to_user` into `MAP_PRIVATE|MAP_FIXED` over the streamer's memfd.

Inferred:

- Step 1 and Step 2 may have different winners: `s5cmd` for fast fallback; NIXL OBJ, runai-model-streamer, and MSC evaluated separately for the streamer.
- MSC may be strongest as a product-facing storage abstraction even if it loses the hot path.
- runai may be a strong S3 downloader substrate only if the lower-level API can be cleanly adapted outside the model-loader surface.

Must benchmark:

- MSC `read(byte_range)` and file-like/fd-backed downloads into a memfd-like target.
- runai arbitrary CRIU byte ranges into memfd, including failure and cancel behavior.
- NIXL tuned/multi-agent full restore versus Step 1 on identical hardware.
- Upload/capture path for every serious candidate.

## Presentation Options

| Option | Proposal | Tradeoff | Next Experiment |
|---|---|---|---|
| Ship fast | `s5cmd -> tmpfs -> CRIU` | Simple and proven, not streamer-compatible, needs full checkpoint RAM | Production-like full restore plus upload timing with fixed agent CPU and flags |
| PoC performance path | NIXL OBJ streamer | Best fit for current async/zero-copy PoC, needs tuning and failure-path hardening | Tuned NIXL full Qwen3-14B restore against Step 1 |
| Product abstraction path | MSC for Step 1 or storage abstraction; keep hot-path escape hatch | Best backend/credential coverage, uncertain restore hot-path performance | MSC byte-range and full-restore benchmark before committing |

## Recommendation

Commit to Step 1 as the fallbackable object-storage restore path, with `s5cmd` as baseline and MSC measured as the multi-cloud alternative.

Treat Step 2 as a PoC compatibility bakeoff: NIXL OBJ first, runai-model-streamer second, MSC third, AWS CRT direct only if the libraries cannot satisfy the memfd/range/completion contract.

Do not claim Step 2 is decided until the full Qwen3-14B end-to-end restore beats or matches Step 1 and inference passes.

## Source Pointers

Local:

- `/home/dfeigin/Work/documentation/obsidian-vault/Checkpoint-Restore/S3 CRIU Benchmarking.md`
- `/home/dfeigin/Work/documentation/obsidian-vault/Checkpoint-Restore/S3 Optimization Journey.md`
- `/home/dfeigin/Work/checkpoints/juicefs-vs-s5cmd-benchmark.md`
- `/home/dfeigin/Work/checkpoints/criu-restore-overlap-summary.md`
- `/home/dfeigin/Work/checkpoints/snapshot-testing/benchmarking/runai-streamer-bench/README.md`
- `/home/dfeigin/Work/checkpoints/snapshot-testing/benchmarking/smart-streaming-emul/nixl-throughput-results.md`
- `/home/dfeigin/Work/checkpoints/snapshot-testing/benchmarking/smart-streaming-emul/nixl-throughput-results-ec2-host-p4d.md`
- `/home/dfeigin/.claude/plans/wondrous-prancing-flamingo.md`
- `/home/dfeigin/.claude/projects/-home-dfeigin-Work-checkpoints/a7e80e23-1f1d-459f-bc22-179b9f965554.jsonl`

Upstream:

- `s5cmd`: https://github.com/peak/s5cmd
- runai-model-streamer: https://github.com/run-ai/runai-model-streamer
- NIXL: https://github.com/ai-dynamo/nixl
- MSC: https://nvidia.github.io/multi-storage-client/
- MSC API: https://nvidia.github.io/multi-storage-client/references/api.html
- MSC config: https://nvidia.github.io/multi-storage-client/references/configuration.html
- AWS S3 CRT C++ examples: https://docs.aws.amazon.com/sdk-for-cpp/v1/developer-guide/examples-s3-crt.html
