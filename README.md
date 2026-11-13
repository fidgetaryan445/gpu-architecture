# gpu-architecture

Coursework for CS623 (GPU Architecture): GPU program instrumentation, CUDA optimization, and Linux device drivers for a GPU accelerator.

## Layout

```
assignment-1-nvbit/   Dynamic binary instrumentation with NVBit
assignment-2/         CUDA kernel optimization and a GPU data structure
assignment-3/         Linux kernel modules for char, PIO/MMIO and accelerator devices
```

### Assignment 1: NVBit (`assignment-1-nvbit/`)
- `nvbit_release/`: NVBit 1.7 SDK and the instrumentation tools (`pc_trace`, `mem_trace`, `control_div`, ...).
- `mem_div/`: memory-divergence tool and cache simulator (`mem_trace.cu`, `inject_funcs.cu`, `divergence.cpp`, `cache_sim.cpp`).
- `ass1.txt` is the problem statement; `REMOTE_GPU_GUIDE.md` is the build and run guide for the GPU server.
- Not included: the `app1`/`app2` binaries, input files and multi-GB traces, and `libnvbit.a`. Get the library from the NVBit release.

### Assignment 2: CUDA (`assignment-2/`)
- `230217-assign2/`: final submission. `problem1` (3D kernel, basic vs. optimized), `problem2` (three kernel versions), `problem3` (GPU hash-style structure, vanilla vs. blocked), plus `Makefile` and report.
- `HOW_TO_RUN.md`: build and reproduction steps. `assignment2.pdf` is the problem statement.
- `final-data/`, `timing-data/`: raw measurements behind the report. `source-files/`: working sources.

### Assignment 3: Drivers (`assignment-3/`)
- `chdev/`: simple character device and test program.
- `pio/`, `mmio/`: port-I/O and memory-mapped I/O examples.
- `accel_release/`: accelerator device module with user library and test (`user/`).
- Build each directory with `make` (needs kernel headers).

## Notes
- Needs CUDA 12.x and an NVIDIA GPU (developed for an A40, `sm_86`) for assignments 1 and 2.
- Generated data (traces, tarballs, binaries, `.o`/`.a`) is excluded by `.gitignore`.
