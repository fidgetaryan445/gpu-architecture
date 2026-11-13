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
HOW_TO_RUN.md           reproduction steps and expected output for every number in the report
230217-assign2/         the submission (Makefile, problem1/, problem2/, problem3/, report .tex/.pdf)
source-files/           course-provided starter files (template Makefile, problem1.cu, problem2-v0.c, inputs)
final-data/             raw outputs of the final measurement run (timing, profiler, correctness)
timing-data/            all timing runs pooled
```

## Build and run
```bash
cd 230217-assign2 && make                 # builds all three problems
./problem1/problem1.out
cd problem2 && ./problem2-v2.out          # reads disp.txt and grid.txt
cd ../problem3 && ./problem3-blocked.out 1000000 $PWD/insert_100000.bin $PWD/search_100000.bin $PWD/delete_100000.bin
```
Timings come from a shared GPU. See section 0 of `HOW_TO_RUN.md` before comparing numbers.

## Test environment
Built and measured on the IITK department GPU server `gpu3`: NVIDIA A40 (`sm_86`), CUDA 12.6. The server is shared and not publicly accessible, so timings depend on load (see `HOW_TO_RUN.md`) and you'll need your own CUDA GPU (adjust `-arch` in the Makefile) to rerun them.
