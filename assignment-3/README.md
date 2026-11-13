# Assignment 3: Linux kernel modules and an accelerator device driver

Out-of-tree kernel modules, each built with `make` against the running kernel's headers
(`/lib/modules/$(uname -r)/build`). They need root and a Linux VM (the accelerator needs the
course's emulated PCI device under QEMU), so don't load them on a machine you care about.

| Folder | What it is | Origin | State |
|---|---|---|---|
| `chdev/` | Minimal character device (`/dev/democdev`) with `open/read/write/release` | Course example, studied | Working example |
| `pio/` | Port-mapped I/O: writes strings to the 16550 serial port (0x3F8) with `inb/outb` | Course example, studied | Working example |
| `mmio/` | Memory-mapped I/O: `ioremap`s the local APIC at 0xFEE00000 and exposes `apic_id` and `apic_version` in sysfs | Course example, studied | Working example |
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
Be upfront about this when presenting the project.

Finished (in `accel-ops.c`, `accel-main.c`, `accel-api.c`):
- PCI probe/remove, BAR mapping, legacy IRQ setup and the character-device plumbing.
- Query, set-mode, allocation (`do_alloc`, `allocate_mem_range`), and the DMA copy path (`do_copy`, `do_accel_dma`).

**Not finished** (`accel-ops.c`):
- `do_free`, `do_reset` and `do_compute` are half-written drafts. They contain compile errors (for
  example `long = cc->start`, `strcut`, `m->flag`, and undeclared variables). `do_compute` never
  programs the destination, count or command registers, and `do_free` / `do_reset` still end in
  `-ENOTSUPP`.
- So the module does not build as it stands, and `usertest` can't succeed until `do_compute` is done.
- The device-mapped (MMIO) copy path returns `-ENOTSUPP`.

I haven't tested any of this against the emulated device.
