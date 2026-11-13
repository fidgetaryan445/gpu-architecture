# Assignment 1: Dynamic GPU instrumentation with NVBit

## Problem statement
Given two prebuilt CUDA binaries (`app1`, `app2`, each run on three input files), write
[NVBit](https://github.com/NVlabs/NVBit) tools that instrument the binaries at run time and
measure:

| Part | Task | Binary |
|---|---|---|
| I | Thread hierarchy (grid/block dimensions) and **thread divergence**: the average of `1 - active_threads/32` over all executed warp instructions (predicates ignored) | `app1` |
| II | Number of **unique instructions** and unique **32-byte instruction-cache blocks** touched, from a trace of per-warp-instruction PCs | `app1` |
| III-A | **Memory divergence** per memory reference: unique 128-byte data-cache blocks accessed / active threads, averaged over all warp memory references | `app2` |
| III-B | **L1 cache simulation**: one 128 KB, 32-way, 128-byte-block, LRU cache per SM; thread block *k* runs on SM *k mod n*; hits/misses for n = 1, 2, 4, ..., 64 SMs | `app2` |

The full text is in `ass1.txt`.

## Approach
- **I, `tools/control_div`**: derived from `instr_count`. Grid/block dimensions are read from the
  `cuLaunchKernel` parameters at launch time. A device function injected before every instruction
  counts the active lanes (via the warp's active mask), and accumulates `1 - active/32` per warp
  instruction.
- **II, `tools/pc_trace`**: derived from `mem_trace`. NVBit exposes the function start PC and each
  instruction's size, so the tool computes every instruction's PC and streams one PC per executed
  warp instruction to the host. `tools/p2.c` reads the trace and counts unique PCs and unique
  `pc / 32` blocks.
- **III, `tools/mem_trace`**: stock tool trimmed so each record keeps only the CTA id, the active
  mask, and the 32 addresses. The mask is kept because without it the addresses of inactive lanes
  can't be told apart from garbage. `tools/p3a.c` computes memory divergence. `tools/p3b.c`
  replays the trace through a tag-only LRU cache model for each SM count.

## Layout
```
ass1.txt                  problem statement
REMOTE_GPU_GUIDE.md       build / run / submit runbook for the GPU server
nvbit_release/
  core/                   NVBit headers (libnvbit.a not included, see below)
  tools/control_div/      Part I
  tools/pc_trace/         Part II tracer
  tools/mem_trace/        Part III tracer
  tools/p2.c p3a.c p3b.c  trace post-processors (plain C)
  tools/<others>/         stock NVBit example tools, unchanged
mem_div/                  standalone copy of the memory-divergence / cache-simulator sources
```

## Build and run
See `REMOTE_GPU_GUIDE.md` for the full steps. In short:
```bash
cd nvbit_release/tools && make                       # builds every tool (needs nvcc and libnvbit.a)
LD_PRELOAD=./control_div/control_div.so ./app1 file1.1
MEM_TRACE_FILE=/tmp/mem_1.bin LD_PRELOAD=./mem_trace/mem_trace.so ./app2 file2.1
gcc -O2 -o p3b p3b.c && ./p3b /tmp/mem_1.bin /tmp/mem_2.bin /tmp/mem_3.bin
```

## Not included
- `app1`, `app2` and their inputs (course-provided), multi-GB traces, and `core/libnvbit.a`
  (download the NVBit 1.7 release and copy it into `nvbit_release/core/`).

## Status
The tools were built and run on the IITK GPU server. `REMOTE_GPU_GUIDE.md` was written before that first run and still says they were untested; trust this README over it.
