# KubeCon CFP Draft

## Session Title

Accelerating GPU Inference Scale-Up on Kubernetes with Snapshot Restore

## Session Description

Scaling GPU inference on Kubernetes is often limited by replica startup time. Every scale-out event repeats model load, process initialization, GPU state reconstruction, and memory warmup before a new replica can serve traffic. This session presents Snapshot restore as a different scale-up path: capture a fully initialized inference service once, then restore warm replicas instead of paying the full cold-start cost on every scale event. Built on CRIU and CUDA checkpoint/restore, the session will cover the restore path for GPU inference workloads, including parallel reopening of memfd-backed regions, asynchronous shmem restore, and AIO-based restoration of private pages. It will share cold-cache results from Qwen3-0.6B to gpt-oss-120B, including 5.5-second total restore at 0.6B and 34.1-second restore for 120B versus a 250-second cold start, and place Snapshot alongside [ModelExpress placeholder] and [GMS placeholder] in the broader deployment workflow.

## Track

AI Inference + Agentic

## Session Format

Session Presentation (30 minutes - 1-2 speakers)

## Level

Intermediate

## Benefits to the Ecosystem

This session gives the Kubernetes and AI inference community a practical framework for reducing time-to-serving for large GPU workloads. Rather than treating checkpoint/restore as a niche systems feature, it shows how warm replica restoration can be used as a scale-up mechanism for real inference services, and how the bottlenecks shift as models and checkpoints get larger. The talk is grounded in implementation and benchmark data across multiple model sizes, storage backends, and cluster environments, so attendees will leave with concrete guidance on what actually matters: checkpoint size, memfd-backed memory behavior, restore-path parallelism, GPU rehydration cost, and the production limits that still remain. Because the work is built on open components including CRIU, Kubernetes, and cuda-checkpoint, the lessons are useful beyond a single internal stack.

## Case Study

No

## Presented This Talk Before

No

## CNCF-Hosted Software

Kubernetes

## Open Source Projects

CRIU, Kubernetes, cuda-checkpoint

## Additional Resources

- https://github.com/checkpoint-restore/criu
- https://criu.org/
- https://github.com/NVIDIA/cuda-checkpoint

## Notes

- Session description length: 966 characters
- ModelExpress and GMS are placeholders until exact wording is supplied
