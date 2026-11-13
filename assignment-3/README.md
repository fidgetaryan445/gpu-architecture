# Assignment 3: Linux kernel modules and an accelerator device driver

Out-of-tree kernel modules, each built with `make` against the running kernel's headers
(`/lib/modules/$(uname -r)/build`). They need root and a Linux VM (the accelerator needs the
course's emulated PCI device under QEMU), so don't load them on a machine you care about.

| Folder | What it is | Origin | State |
|---|---|---|---|
| `chdev/` | Minimal character device (`/dev/democdev`) with `open/read/write/release` | Course example, studied | Unmodified |
| `pio/` | Port-mapped I/O: writes strings to the 16550 serial port (0x3F8) with `inb/outb` | Course example, studied | Unmodified |
| `mmio/` | Memory-mapped I/O: `ioremap`s the local APIC at 0xFEE00000 and exposes `apic_id` and `apic_version` in sysfs | Course example, studied | Unmodified |
| `accel_release/` | PCI driver for the `cs623accel` accelerator: memory allocation, DMA copies, vector compute | Assignment work | **Incomplete, see below** |

## The accelerator task
The emulated PCI device `cs623accel` has two BARs and its own on-board RAM:
- **BAR0**: control registers (command, error status, memory mode, DMA source/destination/page-count/command, IRQ status/ack).
- **BAR1**: a window for the compute operands (`SRC_ONE`, `SRC_TWO`, `DST`, `COUNT`) and the device-mapped (DiM) memory.

The driver exposes a character device, `/dev/cs623accel`. User space talks to it with a small
message protocol (`struct msg` with a command, flags and addresses), wrapped by `user/accellib.c`:

| Operation | Library call | What the driver does |
|---|---|---|
| Query | `accel_query` | Report device status, mode, and RAM / DiM usage |
| Set mode | | Switch between DMA copy mode and device-mapped (MMIO) mode |
| Allocate / free | `accel_malloc`, `accel_free` | Manage page-aligned ranges of device memory with a sorted range list (best-fit allocation, range splitting on free) |
| Copy | `accel_copy` | Host-to-device and device-to-host transfers: `copy_from_user` into a DMA-coherent buffer, program `DMA_SRC/DST/PAGES/CMD`, sleep until the completion interrupt, check the error register; transfers are chunked at 1 MB |
| Compute | `accel_add` (`mul`, `xor`) | Program the BAR1 operand registers, start the command, wait for the compute interrupt |
| Reset | | Free all ranges and put the device back in DMA mode |

`user/usertest.c` allocates three 4096-int vectors, copies two in, runs the operation, copies the result back and checks it.

## Status of `accel_release/`
Implemented in `accel-ops.c`: query, set-mode, best-fit alloc, free, DMA and MMIO copy, compute, reset.

- **alloc**: best-fit over the free gaps (before the first range, between ranges, after the last).
- **free**: the block must lie inside one allocated range; it removes the whole range, trims its head or tail, or splits it in two (the new node is allocated before taking the spinlock).
- **reset**: frees every range of both regions, returns the device to DMA mode, clears the operand registers and sets the status to READY. It runs with `aif->lock` held (from `accel_release()`); the `OP_RESET` path in `accel-api.c` takes the lock first.
- **copy**: the DMA path moves data in chunks of up to 1 MB through a coherent buffer. The MMIO path bounces through one kernel page with `memcpy_toio` / `memcpy_fromio`; device addresses there are offsets into BAR1.
- **compute**: validates the three vectors, marks the device busy, drops the lock, writes the operand registers and `BAR0_CMD`, sleeps on the completion interrupt, then checks the error register.
- Also fixed: `do_copy` could leave the device stuck in BUSY after an invalid address, and `allocate_mem_range` could overlap ranges.

## Test environment
`accel_release/` compiles without warnings against Linux 5.15 headers (IITK server `gpu0`, Ubuntu 22.04); that server is not publicly accessible. The module has **not** been loaded against the emulated `cs623accel` device, so the runtime behaviour is untested. `chdev/`, `pio/` and `mmio/` are course examples and were not modified.
