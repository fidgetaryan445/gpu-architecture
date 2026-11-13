# CS623 Assignment 1 — remote GPU runbook

Everything below was prepared on a machine with no NVIDIA GPU (WSL2, no
driver). All code compiles conceptually against the NVBit 1.7 API but has
**not been built or run** — do that on gpu0/gpu3. This file is the checklist
for that session.

## 0. What's already done here

```
assignment-1-nvbit/
├── app1, app2                      # given binaries
├── file1.1/.2/.3, file2.1/.2/.3     # given inputs
├── ass1.txt                        # assignment text
├── nvbit_release/                  # NVBit 1.7 SDK, unpacked
│   ├── core/                       # nvbit.h, libnvbit.a, headers
│   ├── test-apps/vectoradd/        # sanity-check app (Makefile uses -arch=sm_70)
│   └── tools/
│       ├── instr_count/            # stock NVBit example (untouched)
│       ├── mem_printf2/, mov_replace/, opcode_hist/, ... # stock examples
│       ├── control_div/            # PROBLEM I  — written here
│       ├── pc_trace/               # PROBLEM II — written here
│       ├── mem_trace/              # PROBLEM III — stock tool MODIFIED in place
│       ├── p2.c                    # PROBLEM II post-processing (pure C, already compiles)
│       ├── p3a.c                   # PROBLEM III-A post-processing (pure C, already compiles)
│       └── p3b.c                   # PROBLEM III-B cache simulator (pure C, already compiles)
```

All `ARCH?=all` lines in every `tools/*/Makefile` were already changed to
`ARCH=all` (assignment step 9). `p2.c`, `p3a.c`, `p3b.c` were compiled and
smoke-tested locally with synthetic data and are correct C — nothing to fix
there, just copy them over.

Copy the whole `assignment-1-nvbit/` directory to gpu0 or gpu3 (`scp -r` or
`rsync`), or just re-clone the NVBit tarball there and copy these specific
files over the top: `tools/control_div/`, `tools/pc_trace/`, the 3 modified
files in `tools/mem_trace/`, and `tools/p2.c`, `tools/p3a.c`, `tools/p3b.c`.

## 1. First-time environment check on the remote machine

```bash
nvidia-smi                 # confirm GPU model + driver
nvcc --version              # need >= 10.1 (ideally matches driver's CUDA)
nvcc --version | grep release
```

**Important — architecture flag.** The pasted guide assumed a Pascal-class
GPU (`sm_60`). Find the real compute capability of gpu0/gpu3:

```bash
nvidia-smi --query-gpu=name,compute_cap --format=csv
```

- `nvbit_release/test-apps/vectoradd/Makefile` currently hardcodes
  `-arch=sm_70` (this NVBit 1.7 release's default, not `sm_60` as in the
  pasted instructions — that's fine, just make sure it's `>=` the card's
  actual compute capability, or matches it exactly). Edit it if needed:
  ```
  nvcc -ccbin=$(CXX) -O3 -arch=sm_<XX> vectoradd.cu -o vectoradd
  ```
- All `tools/*/Makefile` already use `ARCH=all`, which asks `nvcc` to build
  for a broad set of architectures — should work as-is on Pascal through
  Hopper-class cards without edits.

## 2. Sanity check NVBit itself

```bash
cd nvbit_release/test-apps
make
./vectoradd/vectoradd          # must run cleanly, no errors

cd ../tools
make                            # builds every tool, including control_div/pc_trace/mem_trace

cd ..
LD_PRELOAD=./tools/instr_count/instr_count.so ./test-apps/vectoradd/vectoradd
```

If this last line errors, do not proceed to app1/app2 — debug NVBit/CUDA
setup first (driver/toolkit mismatch is the most common cause).

Then confirm the actual assignment binaries run un-instrumented at all:

```bash
cd /path/to/CS623_assignment1
./app1 file1.1 && ./app1 file1.2 && ./app1 file1.3
./app2 file2.1 && ./app2 file2.2 && ./app2 file2.3
```

## 3. Problem I — `tools/control_div`

```bash
cd nvbit_release
LD_PRELOAD=./tools/control_div/control_div.so ../assignment-1-nvbit/app1 ../CS623_assignment1/file1.1
```

(Adjust relative paths to wherever `app1`/`fileX.Y` actually live once
copied over — the binaries don't need to be inside `nvbit_release/`.)

Output per kernel launch includes grid dim, block dim, executed warp
instruction count, and the divergence estimate
`sum(1 - active/32) / num_warp_instrs`. Repeat for file1.2, file1.3 and
tabulate the three rows in the report.

Design notes already baked into the code (see comments in
`instr_count.cu` / `inject_funcs.cu`):
- Predicates are intentionally ignored, per the assignment — divergence
  uses `__activemask()`, not the guarded/predicated subset.
- Thread hierarchy is read straight from the `cuLaunchKernel*` params at
  kernel-launch time (no instrumentation needed for that part).

## 4. Problem II — `tools/pc_trace`

```bash
cd nvbit_release
PC_TRACE_FILE=/tmp/pc_1.bin  LD_PRELOAD=./tools/pc_trace/mem_trace.so <path>/app1 <path>/file1.1
PC_TRACE_FILE=/tmp/pc_2.bin  LD_PRELOAD=./tools/pc_trace/mem_trace.so <path>/app1 <path>/file1.2
PC_TRACE_FILE=/tmp/pc_3.bin  LD_PRELOAD=./tools/pc_trace/mem_trace.so <path>/app1 <path>/file1.3

cd tools
gcc -O2 -Wall -o p2 p2.c
./p2 /tmp/pc_1.bin /tmp/pc_2.bin /tmp/pc_3.bin
```

`p2` prints, per file: executed warp instructions, unique PCs (unique
instructions), unique 32-byte instruction cache blocks. Tabulate these
three rows in the report.

Note the `pc_trace` tool's `.so` is still built from a file literally named
`mem_trace.cu` (matches the assignment's "copy Makefile, mem_trace.cu,
inject_funcs.cu, common.h from tools/mem_trace/" instruction, but content is
adapted to trace every instruction's PC, not memory addresses).

## 5. Problem III — `tools/mem_trace` (modified in place) + Part A/B

```bash
cd nvbit_release
MEM_TRACE_FILE=/tmp/mem_1.bin LD_PRELOAD=./tools/mem_trace/mem_trace.so <path>/app2 <path>/file2.1
MEM_TRACE_FILE=/tmp/mem_2.bin LD_PRELOAD=./tools/mem_trace/mem_trace.so <path>/app2 <path>/file2.2
MEM_TRACE_FILE=/tmp/mem_3.bin LD_PRELOAD=./tools/mem_trace/mem_trace.so <path>/app2 <path>/file2.3

cd tools
gcc -O2 -Wall -o p3a p3a.c
gcc -O2 -Wall -o p3b p3b.c

./p3a /tmp/mem_1.bin /tmp/mem_2.bin /tmp/mem_3.bin     # Part A: 1 divergence value per file
./p3b /tmp/mem_1.bin /tmp/mem_2.bin /tmp/mem_3.bin     # Part B: hits/misses for SMs=1..64
```

Design notes:
- The trace record (`common.h`) keeps only `cta_id` (linearized), an
  `active_mask`, and `addrs[32]` — opcode id, warp id, grid-launch id were
  dropped as instructed. `active_mask` was **kept** even though it's not
  explicitly named in the assignment text, because without it you cannot
  tell which of the 32 `addrs[]` slots are real vs. undefined-from-inactive
  lanes; note this reasoning in your report if a TA asks why the trace
  isn't *just* two fields.
- `p3b.c` implements exactly the addressing scheme in the assignment text:
  `set_index = block_addr mod 32` and the remaining bits become the tag
  (`block_addr / 32`) — the assignment's two formulas for set index (`mod`
  and `&`) coincide because 32 sets is a power of two; note this
  equivalence in the report.
- `p3b.c` re-simulates the whole trace from scratch for every SM count
  (1,2,4,8,16,32,64) — this is O(7 × trace length), fine for these input
  sizes, but if a trace is huge and it's slow, that's the first thing to
  profile.

## 6. Before zipping

```bash
cd nvbit_release/tools
make clean          # per submission instructions, item 1
cd ..
zip -r GroupX.zip tools/mem_trace tools/pc_trace tools/control_div \
    tools/p2.c tools/p3a.c tools/p3b.c
```
Replace `X` with your actual group id. Write the PDF report (tabulated
results + brief discussion per problem + which machine, gpu0 or gpu3, was
used) and email both to `mainakc@cse.iitk.ac.in` per `ass1.txt`.

## 7. Known unknowns — verify on the GPU, don't take on faith

- I have not run any of this code; there could be a build error or an API
  mismatch I didn't catch by reading headers alone. Build early, don't
  leave it to the last minute.
- `app1`'s kernel is `ghost(float*)` (from `strings app1`); haven't
  inspected `app2`'s kernel name/behavior at all — the actual thread
  hierarchy, divergence, and memory-access patterns are unknown until you
  run it.
- Exact compute capability of gpu0/gpu3 is unknown — check it first (§1).
