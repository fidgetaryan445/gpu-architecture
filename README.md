# gpu-architecture

Coursework for CS623 (GPU Architecture): GPU program instrumentation, CUDA optimization, and Linux device drivers for a GPU accelerator.

## Layout

```
assignment-1-nvbit/   Dynamic binary instrumentation with NVBit
assignment-2/         CUDA kernel optimization and a GPU data structure
assignment-3/         Linux kernel modules for char, PIO/MMIO and accelerator devices
```

### Assignment 1: NVBit (`assignment-1-nvbit/`)
Problem: write NVBit tools that instrument two prebuilt CUDA binaries at run time to measure
(I) thread hierarchy and warp divergence, (II) unique instructions and 32-byte instruction-cache
blocks from a PC trace, (III-A) memory divergence per memory reference, and (III-B) L1 hit/miss
counts from a tag-only LRU cache simulator for 1 to 64 SMs.
Approach: custom tools derived from NVBit's `instr_count` and `mem_trace` (injected device
functions record active masks, PCs and addresses), plus plain-C trace post-processors.
Binaries, inputs, traces and `libnvbit.a` are not in the repo.

### Assignment 2: CUDA (`assignment-2/`)
Problem: (1) 3D convolution, basic vs. optimized kernel; (2) parallelize a 13^10-point grid search
in three ways; (3) a GPU counting Bloom filter, vanilla vs. cache-blocked, with host-side
verification and Nsight Compute profiling.
Approach and results: shared-memory tiling, constant-memory filter and unrolling (1.8x to 2.1x
faster kernel); reused row sums with early exit (108x faster than the CPU reference, 5.8x faster
than the simple port), plus a Thrust version; blocked Bloom filter (1.7x to 1.9x faster, about
37% less DRAM traffic, more false positives).
Everything is reproducible from `HOW_TO_RUN.md`.

### Assignment 3: Drivers (`assignment-3/`)
- `chdev/`, `pio/`, `mmio/`: course example modules, unmodified (character device, port I/O to the serial port, MMIO to the local APIC with sysfs).
- `accel_release/`: PCI driver for a `cs623accel` accelerator device (allocation, DMA copies, vector add/mul/xor).
  Implements best-fit alloc, free, DMA copy, vector compute and reset. DMA and MMIO copies included. It compiles
  cleanly against Linux 5.15 but hasn't been loaded against the emulated device. Details in `assignment-3/README.md`.

## Notes
- Needs an NVIDIA GPU and CUDA for assignments 1 and 2.
- Test environment: assignments 1 and 2 were built and run on IITK department GPU servers (`gpu0`: CUDA 11.3; `gpu3`: A40, CUDA 12.6), which aren't publicly accessible. Assignment 3 was only compile-checked (Linux 5.15). Each folder README has the details.
- Generated data (traces, tarballs, binaries, `.o`/`.a`) is excluded by `.gitignore`.
- Each assignment folder has its own README with the problem statement, approach and run instructions.
