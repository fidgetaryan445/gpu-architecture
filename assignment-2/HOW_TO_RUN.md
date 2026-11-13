# How to run everything by hand and check the report (roll no. 230217)

Everything below was run on the IITK GPU3 server (not publicly accessible): NVIDIA A40, compute capability 8.6 (the Makefile's `-arch=sm_86` is right), CUDA 12.6.
The report's numbers come from the submitted code, run on GPU3 on 2026-10-27; this file tells you how to reproduce each of them and what you should see.

## 0. Read this first: the GPU is shared, so timings move

Other students run on the same GPU. My own Problem 2 timings changed by up to 2.5x depending on what else was running (e.g. version (iii): 34.9 s on an idle GPU, 91 s under load).
So for timing numbers:

1. Look at the GPU first: `nvidia-smi` (utilisation should be near 0 and no other compute process listed).
2. Repeat every measurement several times, interleaved, at different times, and compare the **best** (fastest) run. Contention only adds time.
3. Do not compare numbers taken at different load levels with each other.

Correctness results (point counts, counters, false-positive rates) are deterministic and do not depend on load.

## 1. Build

```bash
export PATH=/usr/local/cuda/bin:$PATH        # nvcc is not on the default PATH
tar xzf 230217-assign2.tar.gz
cd 230217-assign2
make                                           # builds everything
```

`make` builds `problem1/problem1.out`, `problem2/problem2-v{1,2,3}.out`, `problem3/problem3-{vanilla,blocked}.out`
(`make problem1|problem2|problem3` builds one problem, `make clean` removes the .out files and result files).
Expected: no errors. Harmless warnings: unused `fscanf` results (Problem 2) and an unused `Input_Height` (Problem 1).
In Problems 2 and 3 every CUDA allocation, copy, memset and kernel launch is wrapped in `cudaCheckError` (from the Problem 1 template); a failure prints `GPUassert: ...` and exits.

## 2. Problem 1 (report: Problem 1 tables)

The size is the constant `Input_Width = (1 << 8)` in `problem1/230217-prob1.cu`. To get N = 64, 128, 256 without touching the submission file, build copies:

```bash
mkdir -p ~/p1run
for e in 6 7 8; do
  sed "s/Input_Width = (1 << 8)/Input_Width = (1 << $e)/" problem1/230217-prob1.cu > ~/p1run/p1_$e.cu
  nvcc -O2 -std=c++17 -arch=sm_86 ~/p1run/p1_$e.cu -o ~/p1run/p1_$e.out
done
for e in 6 7 8; do for r in 1 2 3; do echo "N=$((1<<e)) run $r"; ~/p1run/p1_$e.out; done; done
```

| program line | report column |
|---|---|
| `Serial version: X ms` | CPU |
| `Kernel3D_basic time (ms): X` | Basic kernel |
| `Kernel3D_basic end-to-end time (ms): X` | Basic end-to-end |
| `Kernel3D_opt time (ms): X` | Opt kernel |
| `Kernel3D_opt end-to-end time (ms): X` | Opt end-to-end |

Both `check_result` lines per run must say `No differences found between base and test versions` (every run, every size).
The report uses the **best** value over 6 runs. Stricter correctness check (random integer input instead of all ones):
copy the source, replace `std::fill_n(h_input, Input_Num_Elems, 1.0F);` with `for (size_t i = 0; i < Input_Num_Elems; i++) h_input[i] = rand() % 4;`, build and run; it must again say "No differences found".

## 3. Problem 2 (report: Problem 2 correctness table and performance table)

Run from inside `problem2/` (the programs read `./disp.txt` and `./grid.txt`).
The original `grid.txt` has Windows line endings and 10 whitespace-only lines at the end; always build test grids from its first 10 lines.

### 3a. Correctness on small and medium grids (fast)

The CPU reference is the provided `problem2-v0.c` (from the assignment's `source-files`):

```bash
gcc -O2 -std=c17 -D_POSIX_C_SOURCE=199309L problem2-v0.c -o v0.out -lm
mkdir -p ~/p2small && cd ~/p2small && cp ~/230217-assign2/problem2/disp.txt .
head -10 ~/230217-assign2/problem2/grid.txt | awk '{print "0.5 3.5 0.5"}' > grid.txt     # 6 values per variable
cp ~/230217-assign2/problem2/problem2-v?.out . ; cp /path/to/v0.out .
./v0.out; ./problem2-v1.out; ./problem2-v2.out; ./problem2-v3.out
for v in 1 2 3; do /usr/bin/diff -q results-v0.txt results-v$v.txt && echo "v$v identical"; done
```

Expected: `result pnts: 2071` from all four programs and "identical" three times.
With `0.5 4.5 0.5` (8 values per variable) expect `result pnts: 9316`.

### 3b. Full grid (13 values per variable)

```bash
cd ~/230217-assign2/problem2
./problem2-v2.out      # `Total time` about 6.3 s on an idle GPU
./problem2-v3.out      # about 35 s
./problem2-v1.out      # about 36 s
/usr/bin/diff -q results-v1.txt results-v2.txt; /usr/bin/diff -q results-v2.txt results-v3.txt
```

Each prints `result pnts: 11608`; the three result files must be identical. The `Total time = X` line is the number for the report table.
For timing, repeat in interleaved rounds and note the GPU load (this is how the report's best/worst were obtained):

```bash
for round in 1 2 3 4; do for v in 2 3 1; do
  echo "round $round v$v util=$(nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits) other_procs=$(nvidia-smi --query-compute-apps=pid --format=csv,noheader | wc -l)"
  ./problem2-v$v.out | grep "Total time"
done; done
```

Report table = best (smallest) `Total time` per version; "worst" = largest seen.
The CPU reference on the full grid takes about 11 minutes on one core: `nohup ./v0.out > v0.log 2>&1 &` in a directory with `disp.txt` and `grid.txt`;
it prints `result pnts: 11608`, `Total time` about 676 s, and its `results-v0.txt` must be identical to `results-v1.txt`.
`results-v*.txt` are written into the current directory and are not part of the submission.

## 4. Problem 3 (report: false-positive table, deletion experiment, profiling table, bottlenecks)

### 4a. Run with the submitted traces (fast, deterministic)

```bash
cd ~/230217-assign2/problem3
./problem3-vanilla.out 1000000 $PWD/insert_100000.bin $PWD/search_100000.bin $PWD/delete_100000.bin
./problem3-blocked.out 1000000 $PWD/insert_100000.bin $PWD/search_100000.bin $PWD/delete_100000.bin
```

(The assignment asks for absolute paths, hence `$PWD/`.) Output per run:

```
after insert: 0 counters differ from the host model
search after insert: 99999 searches, ... 0 false negatives, ... false positive rate ...
after delete: 0 counters differ from the host model
deleted keys that were never inserted but looked present: 0
search after delete: 99999 searches, ... 0 false negatives, ... false positive rate ...
```

The false-positive rate is false positives divided by *all* searches (the assignment's definition). The numbers are exactly reproducible:

| trace | N | variant | FP after insert | FP after delete |
|---|---|---|---|---|
| 100k | 1,000,000 | vanilla | 0.005470 | 0.003890 |
| 100k | 1,000,000 | blocked | 0.006620 | 0.005540 |
| 100k | 300,000 | vanilla | 0.084161 | 0.077311 |
| 100k | 300,000 | blocked | 0.085201 | 0.079061 |
| 2M | 20,000,000 | vanilla | 0.005828 | 0.003896 |
| 2M | 20,000,000 | blocked | 0.006648 | 0.005002 |

(`blocked` rounds N up to a multiple of 128.)

### 4b. Larger traces and the deletion experiment

`make traces` builds the generator (`problem3/trace_generator.out n [dup_pct] [ghost_pct] [seed]`), which writes `insert_n.bin`, `search_n.bin`, `delete_n.bin` into the current directory.

```bash
mkdir -p ~/big && cd ~/big && ~/230217-assign2/problem3/trace_generator.out 2000000          # for the 2M rows above
mkdir -p ~/ghost && cd ~/ghost && ~/230217-assign2/problem3/trace_generator.out 100000 10 30 1   # delete trace with 10,020 never-inserted keys
B=~/230217-assign2/problem3
for r in 1 2 3; do $B/problem3-vanilla.out 300000 $PWD/insert_100000.bin $PWD/search_100000.bin $PWD/delete_100000.bin; done
```

Large input (the assignment allows up to 10^7 keys per operation): `trace_generator.out 9000000` gives 9,900,000 insert keys; with `N = 90000000` both variants print `0 counters differ`, 0 false negatives, and FP rates 0.005709 / 0.003891 (vanilla) and 0.006731 / 0.005053 (blocked); a run takes about 12 s.

Expected for the deletion experiment (identical in all 3 runs): after delete `0 counters differ from the host model`;
vanilla: 2469 never-inserted keys looked present, 2025 false negatives, FP rate 0.067671;
blocked: 2549, 1989, 0.069041. (The false negatives are real members damaged by those never-inserted keys; this is inherent to counting Bloom filters.)

### 4c. Profiling with Nsight Compute

```bash
cd ~/big
B=~/230217-assign2/problem3
ARGS="20000000 $PWD/insert_2000000.bin $PWD/search_2000000.bin $PWD/delete_2000000.bin"
M=dram__throughput.avg.pct_of_peak_sustained_elapsed,sm__warps_active.avg.pct_of_peak_sustained_active,smsp__thread_inst_executed_per_inst_executed.ratio,lts__t_sector_hit_rate.pct,smsp__warp_issue_stalled_long_scoreboard_per_warp_active.pct,dram__bytes.sum,gpu__time_duration.sum,smsp__sass_thread_inst_executed_op_integer_pred_on.sum,smsp__sass_thread_inst_executed_op_fadd_pred_on.sum,smsp__sass_thread_inst_executed_op_ffma_pred_on.sum,smsp__sass_thread_inst_executed_op_fmul_pred_on.sum
for v in vanilla blocked; do for k in insert_kernel search_kernel; do
  ncu --metrics $M -k $k -c 1 $B/problem3-$v.out $ARGS > metrics_${v}_$k.txt
  ncu --set full  -k $k -c 1 $B/problem3-$v.out $ARGS > full_${v}_$k.txt
done; done
```

| report row | ncu metric / source |
|---|---|
| Memory bandwidth utilisation | `dram__throughput.avg.pct_of_peak_sustained_elapsed` |
| DRAM traffic | `dram__bytes.sum` |
| Arithmetic intensity (FLOP/byte) | all FP instruction counts are 0, so 0; "integer instructions per DRAM byte" = `..._op_integer_pred_on.sum` / `dram__bytes.sum` |
| Achieved occupancy | `sm__warps_active.avg.pct_of_peak_sustained_active` |
| Warp efficiency | `smsp__thread_inst_executed_per_inst_executed.ratio` (32 = all lanes active) |
| Warp efficiency, stricter | "Avg. Not Predicated Off Threads Per Warp" in the Warp State Statistics section of `full_*.txt` |
| L2 hit rate | `lts__t_sector_hit_rate.pct` |
| Long-scoreboard stalls | `smsp__warp_issue_stalled_long_scoreboard_per_warp_active.pct` |
| Kernel duration | `gpu__time_duration.sum` |
| Top-3 bottlenecks | the `OPT` lines in `full_*.txt`: `grep -A4 OPT full_vanilla_insert_kernel.txt`, ordered by "Est. Speedup" |

ncu prints numbers with Indian digit grouping (`56,87,524` = 5,687,524). Profile values move by a few percent between runs; the bottleneck list and the vanilla-vs-blocked ordering were stable in every run I made.

## 5. Cheat sheet: the numbers in the report

**Problem 1** (ms; best of 6 runs): 

| N | CPU | basic kernel | basic end-to-end | opt kernel | opt end-to-end | opt vs basic kernel |
|---|---|---|---|---|---|---|
| 64 | 38 | 0.047 | 0.80 | 0.026 | 0.76 | 1.80x |
| 128 | 337 | 0.293 | 4.19 | 0.143 | 4.00 | 2.04x |
| 256 | 2754 | 2.321 | 26.94 | 1.106 | 24.48 | 2.10x |

**Problem 2** (full grid, 11608 result points, `Total time`, 10 runs each):

| version | best (idle GPU) | worst seen | vs (i) | vs CPU |
|---|---|---|---|---|
| CPU `problem2-v0.c` (one run) | 676.2 s | | | 1x |
| (i) | 36.13 s | 80.9 s | 1.00x | 18.7x |
| (ii) | 6.28 s | 14.7 s | 5.76x | 107.7x |
| (iii) | 34.89 s | 91.2 s | 1.04x | 19.4x |

Correctness anchors: 6-value grid 2071 points, 8-value grid 9316, full grid 11608; all files byte-identical to `results-v0.txt`.

**Problem 3 profiling** (2M-key traces, N = 20,000,000):

| | vanilla insert | vanilla search | blocked insert | blocked search |
|---|---|---|---|---|
| duration | 1.46 ms | 625 us | 777 us | 371 us |
| DRAM traffic | 689 MB | 347 MB | 431 MB | 220 MB |
| DRAM utilisation | 67.9 % | 79.8 % | 79.8 % | 85.5 % |
| achieved occupancy | 69.8 % | 81.2 % | 72.2 % | 81.8 % |
| threads not predicated off (of 32) | 29.9 | 30.1 | 30.7 | 30.8 |
| L2 hit rate | 9.7 % | 10.4 % | 36.5 % | 19.9 % |
| long-scoreboard stalls | 22.6 % | 50.9 % | 22.2 % | 57.5 % |

Raw outputs behind these tables are in `final-data/` (final run), and `timing-data/` (all timing runs pooled), next to the submission folder and not inside the tar.

## 6. Rebuilding the report PDF

The LaTeX source is not in the tar; it is `230217-assign2/230217-assign2.tex`. Build with `tectonic 230217-assign2.tex` (any LaTeX works; it needs `booktabs`, `enumitem`, `fancyvrb` and `needspace`).

## 7. Assumptions and limits (also stated in the report)

* Problem 2 shared-memory tiling and prefetching were not used (the input is 150 numbers in constant memory, read with the same address by all threads of a warp).
* Problem 3: a never-inserted key whose three counters are all set by other keys cannot be recognised and is deleted like a member (see 4b). The main traces delete only inserted keys.
* Timings were taken on a GPU shared with other users (section 0).
