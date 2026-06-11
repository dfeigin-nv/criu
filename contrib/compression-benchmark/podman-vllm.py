#!/usr/bin/env python3
"""
Podman vLLM Checkpoint/Restore Benchmark

Starts a vLLM container with Podman, validates inference, checkpoints it with
Podman, removes it, restores it with Podman, and validates inference again.

This benchmarks Podman's container checkpoint/restore path while varying CRIU
memory-page compression through /etc/criu/runc.conf. Podman's own checkpoint
archive compression is kept at "none" by default so the reported archive size
reflects CRIU image size rather than tar-level gzip/zstd compression.

Example:
  sudo HF_TOKEN=... python3 contrib/compression-benchmark/podman-vllm.py \\
       --model Qwen/Qwen3-0.6B -n 3 \\
       --modes none per-page region
"""

import argparse
import json
import os
import platform
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

_tempdirs = set()
_started_containers = set()
_cleanup_containers = True
# Sentinel distinguishing "runc.conf never touched" from "stashed, but the
# file did not exist" (which stashes None).
_RUNC_CONF_UNSET = object()
_original_runc_conf = _RUNC_CONF_UNSET
PODMAN = "podman"
RUNC_CONF_BEGIN = "# BEGIN criu-compression-benchmark"
RUNC_CONF_END = "# END criu-compression-benchmark"


def _cleanup():
    restore_runc_conf()
    if _cleanup_containers:
        for name in list(_started_containers):
            subprocess.run([PODMAN, "rm", "-f", name],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        _started_containers.clear()
    for path in list(_tempdirs):
        shutil.rmtree(path, ignore_errors=True)
    _tempdirs.clear()


def _sighandler(signum, frame):
    _cleanup()
    sys.exit(1)


def median(v):
    s = sorted(v)
    n = len(s)
    if n == 0:
        return 0
    return s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2


def fsz(b):
    return f"{b/1073741824:.2f} GB" if b >= 1073741824 else f"{b/1048576:.1f} MB"


def fus(us):
    return f"{us/1e6:.3f} s" if us >= 1e6 else f"{us/1000:.1f} ms"


def cfg_label(cfg):
    if cfg["mode"] == "none":
        return "none"
    if cfg["mode"] == "per-page":
        return "per-page"
    return f"region-{cfg['region_size']//1024}K"


def json_object(value):
    try:
        obj = json.loads(value)
    except json.JSONDecodeError as e:
        raise argparse.ArgumentTypeError(str(e))
    if not isinstance(obj, dict):
        raise argparse.ArgumentTypeError("expected a JSON object")
    return obj


def collect_system_info():
    info = {"kernel": platform.release(), "arch": platform.machine(),
            "cpus": os.cpu_count()}
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    info["cpu"] = line.split(":", 1)[1].strip()
                    break
    except OSError:
        info["cpu"] = "unknown"
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal"):
                    info["memory_mb"] = int(line.split()[1]) // 1024
                    break
    except OSError:
        info["memory_mb"] = 0
    for cmd, key in (([PODMAN, "--version"], "podman"),
                     (["criu", "--version"], "criu")):
        try:
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode == 0:
                info[key] = r.stdout.strip()
        except OSError:
            info[key] = "unknown"
    try:
        r = subprocess.run(["nvidia-smi", "--query-gpu=name",
                            "--format=csv,noheader"],
                           capture_output=True, text=True)
        if r.returncode == 0:
            info["gpus"] = [line.strip() for line in r.stdout.splitlines()
                            if line.strip()]
    except OSError:
        info["gpus"] = []
    return info


def http_json(method, url, payload=None, timeout=120):
    data = None
    headers = {}
    if payload is not None:
        data = json.dumps(payload).encode()
        headers["Content-Type"] = "application/json"
        headers["Authorization"] = "Bearer EMPTY"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        body = resp.read()
    return json.loads(body.decode()) if body else {}


def podman_text(args):
    r = subprocess.run([PODMAN, *args], capture_output=True, text=True)
    return (r.stdout + r.stderr).strip()


def container_diagnostics(name):
    parts = [f"container={name}"]
    ps = podman_text(["ps", "-a", "--filter", f"name=^{name}$",
                      "--format", "{{.Names}} {{.Status}} {{.Ports}}"])
    if ps:
        parts.append(f"podman ps:\n{ps}")
    logs = podman_text(["logs", "--tail", "80", name])
    if logs:
        parts.append(f"last logs:\n{logs[-6000:]}")
    return "\n\n".join(parts)


def wait_health(base_url, health_path, timeout, container_name=None):
    deadline = time.monotonic() + timeout
    last = None
    url = f"{base_url.rstrip('/')}/{health_path.lstrip('/')}"
    while time.monotonic() < deadline:
        try:
            urllib.request.urlopen(url, timeout=5).read()
            return
        except (OSError, urllib.error.URLError) as e:
            last = e
            time.sleep(1)
    detail = ""
    if container_name:
        detail = "\n\n" + container_diagnostics(container_name)
    raise RuntimeError(f"vLLM health check timed out after {timeout}s: {last}{detail}")


def chat_once(base_url, model, prompt, max_tokens, temperature, timeout,
              extra_body=None):
    payload = {
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
        "temperature": temperature,
        "max_tokens": max_tokens,
    }
    if extra_body:
        payload.update(extra_body)
    t0 = time.monotonic()
    data = http_json("POST", f"{base_url.rstrip('/')}/v1/chat/completions",
                     payload, timeout)
    latency_us = int((time.monotonic() - t0) * 1e6)
    content = data.get("choices", [{}])[0].get("message", {}).get("content", "")
    if not content.strip():
        raise RuntimeError("vLLM validation returned an empty response")
    return latency_us


def run_cmd(cmd, env=None):
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode:
        msg = (r.stderr or r.stdout).strip()
        raise RuntimeError(f"{' '.join(cmd)} failed: {msg[-1000:]}")
    return r


def podman_env(args):
    env = os.environ.copy()
    if args.criu_libdir:
        env["CRIU_LIBS_DIR"] = args.criu_libdir
    return env


def read_file(path):
    try:
        with open(path) as f:
            return f.read()
    except FileNotFoundError:
        return None


def write_file(path, data):
    directory = os.path.dirname(path)
    if directory:
        os.makedirs(directory, exist_ok=True)
    tmp = f"{path}.tmp.{os.getpid()}"
    with open(tmp, "w") as f:
        f.write(data)
    os.replace(tmp, path)


def strip_benchmark_runc_block(text):
    if not text:
        return ""
    lines = text.splitlines()
    out = []
    skipping = False
    for line in lines:
        if line.strip() == RUNC_CONF_BEGIN:
            skipping = True
            continue
        if line.strip() == RUNC_CONF_END:
            skipping = False
            continue
        if not skipping:
            out.append(line)
    return "\n".join(out).rstrip()


def compression_config_lines(cfg, acceleration, decompress_threads=0):
    if cfg["mode"] == "none":
        return []
    if cfg["mode"] == "per-page":
        lines = ["compress"]
    else:
        lines = [f"compress-region {cfg['region_size']}"]
    if acceleration != 1:
        lines.append(f"compress-acceleration {acceleration}")
    if decompress_threads:
        lines.append(f"decompress-threads {decompress_threads}")
    return lines


def set_runc_conf_for_cfg(path, cfg, acceleration, decompress_threads=0):
    global _original_runc_conf

    if _original_runc_conf is _RUNC_CONF_UNSET:
        _original_runc_conf = read_file(path)
        restore_runc_conf.path = path

    base = strip_benchmark_runc_block(_original_runc_conf)
    lines = compression_config_lines(cfg, acceleration, decompress_threads)
    if lines:
        block = "\n".join([RUNC_CONF_BEGIN, *lines, RUNC_CONF_END])
        text = f"{base}\n\n{block}\n" if base else f"{block}\n"
    else:
        text = f"{base}\n" if base else ""
    write_file(path, text)


def restore_runc_conf():
    global _original_runc_conf

    if _original_runc_conf is _RUNC_CONF_UNSET:
        return
    path = getattr(restore_runc_conf, "path", None)
    if not path:
        return
    if _original_runc_conf is None:
        try:
            os.unlink(path)
        except FileNotFoundError:
            _original_runc_conf = _RUNC_CONF_UNSET
            return
    else:
        write_file(path, _original_runc_conf)
    _original_runc_conf = _RUNC_CONF_UNSET


def start_container(name, args):
    cmd = [
        PODMAN, "run", "-d",
        "--name", name,
        "--device", args.gpu_device,
        "--security-opt", args.security_opt,
        "--network", "host",
        "--shm-size", args.shm_size,
        "-v", f"{args.hf_cache}:/root/.cache/huggingface",
        "--env", f"HF_TOKEN={os.environ.get('HF_TOKEN', '')}",
        "--env", f"CUDA_VISIBLE_DEVICES={args.cuda_visible_devices}",
        "--env", "NCCL_P2P_DISABLE=1",
        "--env", "NCCL_SHM_DISABLE=1",
        "--env", "NCCL_IB_DISABLE=1",
        "--env", "NCCL_CUMEM_ENABLE=0",
    ]
    for item in args.env:
        cmd += ["--env", item]
    for item in args.volume:
        cmd += ["-v", item]
    for item in args.run_arg:
        cmd.append(item)
    for item in args.ulimit:
        cmd += ["--ulimit", item]

    if args.vllm_entrypoint == "serve":
        cmd += [args.image, "vllm", "serve"]
    else:
        cmd += [args.image, "python3", "-m",
                "vllm.entrypoints.openai.api_server"]

    cmd += ["--model", args.model,
            "--host", "0.0.0.0",
            "--port", str(args.port),
            "--gpu-memory-utilization", str(args.gpu_memory_utilization),
            "--max-model-len", str(args.max_model_len),
            "--tensor-parallel-size", str(args.tensor_parallel_size)]
    if args.dtype:
        cmd += ["--dtype", args.dtype]
    if args.served_model_name:
        cmd += ["--served-model-name", args.served_model_name]
    for item in args.vllm_arg:
        cmd.append(item)

    run_cmd([PODMAN, "rm", "-f", name])
    run_cmd(cmd, env=podman_env(args))
    _started_containers.add(name)
    print(f"  waiting for {name} health on {args.base_url}", flush=True)
    wait_health(args.base_url, args.health_path, args.wait_seconds, name)


def checkpoint_container(name, archive, cfg, args):
    set_runc_conf_for_cfg(args.runc_conf, cfg, args.compress_acceleration,
                          args.decompress_threads)
    cmd = [
        PODMAN, "container", "checkpoint",
        "--export", archive,
        "--compress", args.archive_compression,
        "--ignore-volumes",
        "--tcp-established",
    ]
    if args.print_stats:
        cmd.append("--print-stats")
    if args.keep_checkpoint_files:
        cmd.append("--keep")
    cmd.append(name)

    t0 = time.monotonic()
    r = run_cmd(cmd, env=podman_env(args))
    checkpoint_us = int((time.monotonic() - t0) * 1e6)
    _started_containers.discard(name)
    return checkpoint_us, r.stdout.strip()


def restore_container(name, archive, args):
    cmd = [
        PODMAN, "container", "restore",
        "--import", archive,
        "--ignore-volumes",
        "--tcp-established",
    ]
    if args.print_stats:
        cmd.append("--print-stats")

    t0 = time.monotonic()
    r = run_cmd(cmd, env=podman_env(args))
    restore_us = int((time.monotonic() - t0) * 1e6)
    _started_containers.add(name)
    print(f"  waiting for restored {name} health on {args.base_url}", flush=True)
    wait_health(args.base_url, args.health_path, args.wait_seconds, name)
    return restore_us, r.stdout.strip()


def run_trial(cfg, workdir, args, trial_id):
    name = f"{args.container_name}-{trial_id}"
    archive = os.path.join(workdir, f"{name}.tar")
    if args.archive_compression == "gzip":
        archive += ".gz"
    elif args.archive_compression == "zstd":
        archive += ".zst"

    start_container(name, args)
    request_model = args.served_model_name or args.model
    for _ in range(args.warmup_requests):
        chat_once(args.base_url, request_model, args.prompt,
                  args.max_tokens, args.temperature, args.request_timeout,
                  args.chat_extra_json)
    pre_us = chat_once(args.base_url, request_model, args.prompt,
                       args.max_tokens, args.temperature, args.request_timeout,
                       args.chat_extra_json)
    checkpoint_us, checkpoint_stats = checkpoint_container(name, archive, cfg, args)
    run_cmd([PODMAN, "rm", "-f", name])
    restore_us, restore_stats = restore_container(name, archive, args)
    post_us = chat_once(args.base_url, request_model, args.prompt,
                        args.max_tokens, args.temperature, args.request_timeout,
                        args.chat_extra_json)
    if not args.keep_running:
        run_cmd([PODMAN, "rm", "-f", name])
        _started_containers.discard(name)

    return {
        "archive_size": os.path.getsize(archive),
        "checkpoint_wall_us": checkpoint_us,
        "restore_wall_us": restore_us,
        "pre_request_us": pre_us,
        "post_request_us": post_us,
        "checkpoint_stats": checkpoint_stats,
        "restore_stats": restore_stats,
        "valid": True,
        "framework": "vllm",
    }


def report(results_by_cfg, order):
    width = 88
    print()
    print("=" * width)
    print(f"  PODMAN VLLM (n={len(next(iter(results_by_cfg.values())))})")
    print("=" * width)
    ok = all(r["valid"] for trials in results_by_cfg.values() for r in trials)
    print(f"  Inference validation: {'PASS' if ok else 'FAIL'}")
    print()

    baseline = "none" if "none" in results_by_cfg else order[0]
    base = median([r["archive_size"] for r in results_by_cfg[baseline]])
    print(f"  STORAGE          {'archive':>12s}  {'ratio':>10s}  {'saved':>8s}")
    print("  " + "-" * (width - 2))
    for label in order:
        size = median([r["archive_size"] for r in results_by_cfg[label]])
        ratio = size / base if base else 0
        print(f"  {label:<14s}  {fsz(size):>12s}  {ratio:>8.3f}x  {(1-ratio):>7.0%}")
    print()

    rows = [
        ("Checkpoint", "checkpoint_wall_us"),
        ("Restore", "restore_wall_us"),
        ("Request after", "post_request_us"),
    ]
    header = f"  {'metric':<14s} | " + " | ".join(f"{lab:>12s}" for lab in order)
    print("  LATENCY (median)")
    print(header)
    print("  " + "-" * (width - 2))
    for label, key in rows:
        cells = [fus(median([r[key] for r in results_by_cfg[lab]]))
                 for lab in order]
        print(f"  {label:<14s} | " + " | ".join(f"{cell:>12s}" for cell in cells))


def main():
    global _cleanup_containers

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", default="vllm/vllm-openai:latest")
    ap.add_argument("--vllm-entrypoint", choices=["serve", "module"],
                    default="serve",
                    help="Use the modern 'vllm serve' CLI or the legacy "
                         "vllm.entrypoints.openai.api_server module")
    ap.add_argument("--model", default="Qwen/Qwen3-0.6B")
    ap.add_argument("--served-model-name",
                    help="Model name to use in OpenAI API requests; defaults "
                         "to --model")
    ap.add_argument("--port", type=int, default=30000)
    ap.add_argument("--base-url", default="http://127.0.0.1:30000")
    ap.add_argument("--health-path", default="/health")
    ap.add_argument("--container-name", default="vllm-criu-bench")
    ap.add_argument("-n", "--iterations", type=int, default=3)
    ap.add_argument("--modes", nargs="+", default=["none", "per-page", "region"],
                    choices=["none", "per-page", "region"],
                    help="CRIU memory page compression modes to compare")
    ap.add_argument("--region-sizes", nargs="+", type=int,
                    default=[65536, 262144, 1048576],
                    help="Region sizes in bytes when --modes contains region")
    ap.add_argument("--compress-acceleration", type=int, default=1,
                    help="CRIU LZ4 acceleration level")
    ap.add_argument("--decompress-threads", type=int, default=0,
                    help="CRIU restore-time decompression worker threads "
                         "(default: 0; 0 or 1 = serial); written to runc.conf")
    ap.add_argument("--runc-conf", default="/etc/criu/runc.conf",
                    help="CRIU config file read by runc during Podman checkpoint")
    ap.add_argument("--archive-compression", default="none",
                    choices=["none", "gzip", "zstd"],
                    help="Podman checkpoint archive compression")
    ap.add_argument("--criu-libdir", default=os.environ.get("CRIU_LIBS_DIR"),
                    help="Directory containing CRIU plugin .so files")
    ap.add_argument("--hf-cache", default=os.path.expanduser("~/.cache/huggingface"))
    ap.add_argument("--gpu-device", default="nvidia.com/gpu=all",
                    help="GPU device selector passed to podman --device")
    ap.add_argument("--security-opt", default="label=disable",
                    help="Security option passed to podman --security-opt")
    ap.add_argument("--cuda-visible-devices", default="0")
    ap.add_argument("--shm-size", default="32g")
    ap.add_argument("--gpu-memory-utilization", type=float, default=0.35)
    ap.add_argument("--max-model-len", type=int, default=8192)
    ap.add_argument("--tensor-parallel-size", type=int, default=1)
    ap.add_argument("--dtype", default="auto",
                    help="vLLM dtype argument; use an empty string to omit")
    ap.add_argument("--prompt", default="Say hello in one short sentence.")
    ap.add_argument("--prompt-file",
                    help="Read the validation/warmup prompt from a text file")
    ap.add_argument("--max-tokens", type=int, default=32)
    ap.add_argument("--temperature", type=float, default=0,
                    help="Chat completion temperature for validation requests")
    ap.add_argument("--chat-extra-json", type=json_object,
                    help="JSON object merged into every chat completion request")
    ap.add_argument("--warmup-requests", type=int, default=0,
                    help="Extra successful chat requests to issue before the "
                         "measured pre-checkpoint request")
    ap.add_argument("--wait-seconds", type=int, default=900)
    ap.add_argument("--request-timeout", type=int, default=180)
    ap.add_argument("--env", action="append", default=[],
                    help="Extra container environment entry, e.g. KEY=VALUE")
    ap.add_argument("--volume", action="append", default=[],
                    help="Extra container volume, e.g. /host:/container")
    ap.add_argument("--run-arg", action="append", default=[],
                    help="Extra raw argument for podman run before the image")
    ap.add_argument("--ulimit", action="append", default=[],
                    help="Extra Podman ulimit, e.g. nofile=65535:524288")
    ap.add_argument("--vllm-arg", action="append", default=[],
                    help="Extra raw argument appended to the vLLM server command")
    ap.add_argument("--print-stats", action="store_true",
                    help="Ask Podman to print checkpoint/restore stats")
    ap.add_argument("--keep-checkpoint-files", action="store_true",
                    help="Pass --keep to podman checkpoint")
    ap.add_argument("--keep-running", action="store_true",
                    help="Leave the restored container running after each trial")
    ap.add_argument("--json", metavar="FILE", help="Write raw results to JSON")
    args = ap.parse_args()

    if os.getuid() != 0:
        sys.exit("Error: run as root so Podman/CRIU can checkpoint the container")
    _cleanup_containers = not args.keep_running

    import atexit
    signal.signal(signal.SIGINT, _sighandler)
    signal.signal(signal.SIGTERM, _sighandler)
    atexit.register(_cleanup)

    info = collect_system_info()
    print()
    if args.dtype == "":
        args.dtype = None
    if args.prompt_file:
        prompt = read_file(args.prompt_file)
        if prompt is None:
            sys.exit(f"Error: prompt file not found: {args.prompt_file}")
        args.prompt = prompt.rstrip("\n")

    print("Podman vLLM Checkpoint/Restore Benchmark")
    print(f"  Kernel : {info.get('kernel', '?')}")
    print(f"  CPU    : {info.get('cpu', '?')}")
    print(f"  Memory : {info.get('memory_mb', '?')} MB")
    print(f"  GPU    : {', '.join(info.get('gpus', ['?']))}")
    print(f"  Podman : {info.get('podman', '?')}")
    print(f"  CRIU   : {info.get('criu', '?')}")
    print(f"  Plugin : {args.criu_libdir or 'default CRIU plugin path'}")
    print(f"  Runc conf: {args.runc_conf}")
    print(f"  Archive: podman --compress={args.archive_compression}")

    cfgs = []
    for mode in args.modes:
        if mode == "region":
            for rs in args.region_sizes:
                cfgs.append({"mode": "region", "region_size": rs})
        else:
            cfgs.append({"mode": mode, "region_size": 0})
    labels = [cfg_label(cfg) for cfg in cfgs]

    print(f"  Config : {args.iterations}+1 iterations, "
          f"modes={','.join(labels)}")
    print(f"  Restore: decompress-threads={args.decompress_threads or 'serial'}")
    print(f"  Server : {args.base_url}, model={args.model}, "
          f"entrypoint={args.vllm_entrypoint}")
    print(f"  Request: max_tokens={args.max_tokens}, "
          f"temperature={args.temperature:g}, "
          f"warmup_requests={args.warmup_requests}")
    if args.prompt_file:
        print(f"  Prompt : {args.prompt_file}")
    if args.ulimit:
        print(f"  Ulimit : {','.join(args.ulimit)}")

    results = {label: [] for label in labels}
    total = args.iterations + 1
    trial = 0
    for i in range(total):
        warmup = (i == 0)
        for cfg, label in zip(cfgs, labels):
            trial += 1
            workdir = tempfile.mkdtemp(prefix="podman-vllm-bench-")
            _tempdirs.add(workdir)
            try:
                result = run_trial(cfg, workdir, args, trial)
                if not warmup:
                    results[label].append(result)
            finally:
                shutil.rmtree(workdir, ignore_errors=True)
                _tempdirs.discard(workdir)
        print(f"  completed {'warmup' if warmup else f'{i}/{args.iterations}'}")

    report(results, labels)

    if args.json:
        with open(args.json, "w") as f:
            json.dump({"system": info,
                       "framework": "vllm",
                       "config": vars(args),
                       "results": results}, f, indent=2)
        print(f"\nResults written to {args.json}")

    print()


if __name__ == "__main__":
    main()
