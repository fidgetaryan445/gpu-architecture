# Assignment 2: CUDA kernels (convolution, grid search, counting Bloom filter)

Target: NVIDIA A40 (`sm_86`), CUDA 12.6. The problem statement is `assignment2.pdf`.

## Problems and approach

### Problem 1: 3D convolution (5x5x5 filter)
Single-precision N^3 volume (N = 64, 128, 256), zero ghost cells, checked against a single-threaded
CPU version.
- **Basic kernel**: one thread per output element, 8x8x8 blocks, everything from global memory.
- **Optimized kernel**: a 12x12x12 shared-memory tile per block (8^3 outputs plus a halo of 2),
  filter in `__constant__` memory, hand-unrolled inner loop.
- **Result**: 1.8x to 2.1x faster kernel than basic; end-to-end times are nearly equal because the
  copies dominate.

### Problem 2: Parallelizing a 10-dimensional grid search
A 13^10 (about 1.4e11) point search with 150 constants (ten rows) held in constant memory, ported from `problem2-v0.c`.
- **(i) Simple port**: host loops over two variables (169 launches); each thread loops over the last
  four variables and recomputes everything.
- **(ii) Optimized**: running row sums reused across inner variables, early exit on the first failing
  row, only one variable on the host (13 launches). **Best: 6.3 s vs 676 s on CPU (108x).**
- **(iii) Thrust**: slices of 2^28 points through `transform`, `count_if`, `copy_if`.
- All versions reproduce the reference output byte for byte (11,608 result points on the full grid).

### Problem 3: Counting Bloom filter
GPU insert / search / delete kernels over 8-bit counters (packed four to a 32-bit word, since CUDA
has no 8-bit atomics), k = 3 hash functions.
- **Vanilla**: three independent hashes.
- **Blocked**: a fourth hash picks a 128-counter block (one cache line) and the other three hash
  inside it. This gives 1.7x to 1.9x faster kernels and about 37% less DRAM traffic, at the cost of
  more false positives.
- `check_result()` replays everything on a host model (`std::set` plus a counter array) and checks
  every counter, the false negatives and the false-positive rate.
- Profiled with Nsight Compute: the kernels are memory-bound with uncoalesced access and
  long-scoreboard stalls. See the report for the numbers.

## Layout
```
assignment2.pdf         problem statement
230217-assign2/         the solution: Makefile, problem1/, problem2/, problem3/, report (.tex and .pdf)
source-files/           course-provided starter files (template Makefile, problem1.cu, problem2-v0.c CPU reference, inputs)
```
The report (`230217-assign2/230217-assign2.pdf`) has the full tables and discussion.

## Build and run
Needs `nvcc` (CUDA 12.x). Set `-arch` in the Makefile for your GPU (`sm_86` for an A40).
```bash
cd 230217-assign2 && make        # or: make problem1 | problem2 | problem3
```

**Problem 1**: `./problem1/problem1.out`. The size is `Input_Width = (1 << 8)` in `230217-prob1.cu`; change the exponent to 6, 7 or 8 for N = 64, 128, 256. Both kernels must print `No differences found`.

**Problem 2**: run from `problem2/` (reads `disp.txt` and `grid.txt` from the current directory).
```bash
cd problem2 && ./problem2-v1.out   # also -v2.out, -v3.out; writes results-vN.txt
```
Each prints `result pnts: 11608` and a `Total time`; the three `results-v*.txt` files must be identical. To compare with the CPU reference (about 11 minutes):
`gcc -O2 -std=c17 -D_POSIX_C_SOURCE=199309L ../../source-files/problem2-v0.c -o v0.out -lm && ./v0.out`.

**Problem 3**: pass the filter size and absolute paths to the three traces.
```bash
cd problem3
./problem3-vanilla.out 1000000 $PWD/insert_100000.bin $PWD/search_100000.bin $PWD/delete_100000.bin   # or problem3-blocked.out
```
Expect 0 counters differing from the host model and 0 false negatives. False-positive rates after insert / delete for N = 10^6: vanilla 0.547% / 0.389%, blocked 0.662% / 0.554%.
`make traces` builds `trace_generator.out n [dup_pct] [ghost_pct] [seed]` for larger traces. Profile a kernel with, for example,
`ncu --set full -k insert_kernel -c 1 ./problem3-vanilla.out <args>`.

## Test environment
Built and measured on the IITK department GPU server `gpu3`: NVIDIA A40 (`sm_86`), CUDA 12.6. The server is shared and not publicly accessible, so absolute timings depend on its load; correctness results (point counts, counters, false-positive rates) are deterministic.
