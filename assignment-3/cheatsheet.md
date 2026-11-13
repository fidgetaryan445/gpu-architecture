# CS623 accel driver cheat sheet

Tags: **[CODE]** = confirmed from the source. **[INFER]** = inferred from names; confirm with the lab handout.

## 1. Program order

### Load (`insmod accel.ko`)
1. `acceldev_pci_driver_init`: `kzalloc` global `cif`, `pci_register_driver`, `init_waitqueue_head`, `init_chdev(cif)` [CODE]
2. If the device exists, PCI core calls `acceldev_pci_probe`:
   `kzalloc(acceldev)` -> `pci_enable_device` -> `pci_request_regions` -> `pci_ioremap_bar` BAR0 -> read ID, RAM start, RAM size -> `pci_ioremap_bar` BAR1 -> `config_irq` (`pci_alloc_irq_vectors`, `pci_irq_vector`, `request_irq`) -> `dma_set_mask_and_coherent(32-bit)` -> `pci_set_drvdata` -> `init_completion` x2 -> `accel_prepare` [CODE]
3. `accel_prepare`: build `devmap` (BAR1, start 0x1000) and `devram` (from BAR0 start/size), zero the four BAR1 regs, write `ACCEL_STATUS_IRQ` to `BAR0_STATUS`, `set_accelops`, `status = READY` [CODE]
4. `init_chdev`: `alloc_chrdev_region` -> `class_create` -> `cdev_init` + `cdev_add` -> `device_create` -> `/dev/cs623accel` (mode 0666) [CODE]

### Use
`open` (find `cif` via `container_of`, `usage++`) -> many `read(fd, &msg, sizeof msg)` -> `close` (last close calls `accel_reset(cif, NULL)`) [CODE]

### Unload (`rmmod`)
`exit_chdev` (`device_destroy`, `cdev_del`, `class_destroy`, `unregister_chrdev_region`) -> `pci_unregister_driver` -> `remove` (`free_irq`, `pci_free_irq_vectors`, `pci_release_regions`, `iounmap` x2, `pci_disable_device`, `kfree`) [CODE]

## 2. Register map (`acceldev.h`)

### BAR0 (control). `_cread/_cwrite` = 32-bit, `_creadq/_cwriteq` = 64-bit

| Offset | Name | Width | Used today? |
|---|---|---|---|
| 0x00 | `BAR0_ID` | 32 | read in probe |
| 0x04 | `BAR0_CMD` | 32 | not yet (likely compute "go") [INFER] |
| 0x08 | `BAR0_ERR_STATUS` | 32 | cleared before DMA, read after |
| 0x0C | `BAR0_MEM_MODE` | 32 | `set_dev_mode` (0 = DMA, 1 = MMIO) |
| 0x10 | `BAR0_ACCEL_RAM_START` | 64 | read (`devram->start`) |
| 0x18 | `BAR0_ACCEL_RAM_SIZE` | 64 | read |
| 0x20 | `BAR0_IRQSTATUS` | 32 | read in handler |
| 0x24 | `BAR0_STATUS` | 32 | written once (`ACCEL_STATUS_IRQ` 0x80) |
| 0x60 | `BAR0_IRQRAISE` | 32 | unused |
| 0x64 | `BAR0_IRQACK` | 32 | written in handler |
| 0x80 | `BAR0_DMA_SRC` | 64 | DMA |
| 0x88 | `BAR0_DMA_DST` | 64 | DMA |
| 0x90 | `BAR0_DMA_PAGES` | 64 | DMA |
| 0x98 | `BAR0_DMA_CMD` | 64 | DMA |

### BAR1 (compute operands + mapped memory). Only zeroed in `accel_prepare` today

| Offset | Name |
|---|---|
| 0x0 / 0x4 / 0x8 / 0xC | `BAR1_SRC_ONE` / `SRC_TWO` / `DST` / `COUNT` |
| 0x10 | `ACCEL_MAPMEM` |
| 0x1000 onward | DiM memory (`devmap->start`) |

### Constants
- Compute commands: `ACCEL_CMD_VADD=1`, `VMUL=2`, `VXOR=3`
- DMA command bits: `0x1` run, `0x2` direction (from device), `0x4` raise IRQ. Built as `0x5 | (dir << 1)`.
- Directions: `ACCEL_COPY_TO_DEV=0`, `ACCEL_COPY_FROM_DEV=1`
- IRQ status bits: `COMP_DONE_IRQ 0x1`, `COMP_ERROR_IRQ 0x10`, `DMA_IRQ 0x100`
- Errors: `ACCEL_SUCCESS=0`, `ACCEL_ERROR_*` family
- Software status: `ACI_STATUS_READY=3`, `BUSY_DCOPY=4`, `BUSY_COMP=5`
- Limits: `MAX_DMA_SIZE = 1 MB`, allocations must be page multiples

## 3. Operation recipes

### Alloc [CODE] (`do_alloc`)
status >= READY -> mode <= MMIO -> size page multiple -> pick `devram` or `devmap` -> lock -> `used + size <= total` -> `allocate_mem_range` -> `m->d_handle = addr`, `used += size` -> unlock. On failure `sprintf(m->status, ...)` and return negative errno.

### Copy, DMA mode [CODE] (`do_copy` + `do_accel_dma`)
decode mode (`flags & 0xff`) and dir (`(flags >> 8) & 1`) -> status READY check -> lock, set `BUSY_DCOPY`, `find_range`, unlock -> loop in chunks <= 1 MB: `accel_dma_get`; clear `ERR_STATUS`; H2D `copy_from_user` into `kva`; write `SRC`, `DST`, `PAGES`, then `CMD`; `wait_for_completion(&dma_comp)`; check `ERR_STATUS`; D2H `copy_to_user`; `accel_dma_put`; advance addrs -> lock, status READY.
In D2H, SRC and DST swap.

### Compute [INFER] (`do_compute`, to write)
1. Validate: status READY; mode picks `devram`/`devmap`; `find_range` on all three operand ranges (`count * sizeof(int)` bytes each).
2. Lock, `status = BUSY_COMP`, unlock.
3. Clear `BAR0_ERR_STATUS`.
4. Write `BAR1_SRC_ONE`, `SRC_TWO`, `DST`, `COUNT` (absolute vs offset: check spec).
5. Write command (`ACCEL_CMD_VADD/VMUL/VXOR`, from `m->cmd`) to its register, last.
6. `wait_for_completion(&adev->compute_comp)`.
7. Check `ERR_STATUS` and `irq_status & COMP_ERROR_IRQ`; set `m->status` on error.
8. Lock, status READY, unlock. Restore status on every exit path.

### Free [INFER] (`do_free`, stub)
validate size and alignment -> lock -> `find_range(cc, handle, handle + size)` -> exact match: `list_del` + `kfree`; at start or end: shrink; in the middle: split into two nodes (GFP_ATOMIC) -> `cc->used -= size` -> unlock.

### Reset [INFER] (`do_reset`, stub; lock held; `m == NULL` from release)
free every range in both lists, `used = 0`, mode back to DMA, status READY. Never touch `m`.

### MMIO-mode copy [INFER] (stub)
No DMA. For `devmap`: H2D `copy_from_user` into a bounce buffer then `memcpy_toio(cc->kva + daddr, buf, size)`; D2H `memcpy_fromio` then `copy_to_user`.

### Set mode [CODE]
`flags <= AC_MODE_MMIO_UMAP`; status READY; `AC_MODE_DMA` needs `dma_cap`; store `cur_devmem_mode`; write `BAR0_MEM_MODE`.

## 4. Data structures

| Struct | Holds | Key fields |
|---|---|---|
| `acceldev` | per-PCI-device state | `bar0`, `bar1`, `irq`, `irq_status`, `dma_comp`, `compute_comp` |
| `accel_interface` (`cif`) | global driver object | `lock`, `devops`, `acdev`, `devmap`, `devram`, `status`, `usage`, `dma_handles[]`, `cur_devmem_mode`, `dma_cap` |
| `compute_context` | one memory region | `start`, `end`, `used`, `mem_range_list`, `kva` |
| `mem_range` | one allocated interval | `start`, `end`, `list` |
| `accel_dma_entry` | one DMA buffer | `dev_addr`, `kva`, `usage`, `size` |
| `msg` | user -> kernel request | `cmd`, `flags`, `d_handle`, `h_handle`, `size`, `s1_addr`, `s2_addr`, `d_addr`, `count`, `status[64]` |
| `acceldev_ops` | op table | one function pointer per op |

User `msg` field names: `d_handle`=`daddr`, `h_handle`=`uaddr`, `d_addr`=`d_address`. Layout must match.

## 5. Kernel APIs
- Module/char: `init_module`, `cleanup_module`, `register_chrdev`, `alloc_chrdev_region`, `cdev_init`, `cdev_add`, `class_create`, `device_create`, `container_of`
- User copy: `copy_to_user(user, kern, n)`, `copy_from_user(kern, user, n)` (destination first)
- Memory: `kzalloc(size, GFP_KERNEL/GFP_ATOMIC)`, `kfree`
- MMIO: `ioremap`, `iounmap`, `readl/writel`, `readq/writeq`, `memcpy_toio/memcpy_fromio`
- Port I/O: `inb`, `outb`
- PCI: `pci_register_driver`, `pci_enable_device`, `pci_request_regions`, `pci_ioremap_bar`, `pci_resource_start/len`, `pci_set_drvdata/get_drvdata`
- IRQ: `pci_alloc_irq_vectors`, `pci_irq_vector`, `request_irq/free_irq`, `irqreturn_t`, `IRQ_HANDLED`
- Sync: `spin_lock/spin_unlock`, `init_completion`, `wait_for_completion`, `complete`
- DMA: `dma_set_mask_and_coherent`, `dma_alloc_coherent`, `dma_free_coherent`
- Lists: `list_for_each_entry`, `list_add_tail`, `list_del`, `INIT_LIST_HEAD`

## 6. Rules that cost marks
1. Setup registers first, "go" last.
2. Clear `ERR_STATUS` before an op; read it after the completion.
3. Never sleep (`wait_for_completion`, `copy_*_user`, `GFP_KERNEL`) while holding a spinlock; never sleep in the IRQ handler.
4. Restore `aif->status` to READY on every return path.
5. Validate the user's `len`; validate device ranges with `find_range` before touching hardware.
6. Free what you allocated, in reverse order (`goto` unwind).
7. A handle is a device address, not a pointer you can dereference.

## 7. Check in the lab handout
- Which register takes the compute command; are SRC/DST absolute or offsets?
- Address translation for `devmap` vs `devram`?
- What does reset write to the device?
- MMIO copy via `memcpy_toio`, or `mmap` (`AC_MODE_MMIO_UMAP`)?

## 8. Known bugs in the given code
- `accel_read`: user `len` goes straight to `copy_from_user(&msg, ...)` (stack overflow if too big).
- `do_copy`: status stays `BUSY_DCOPY` on the invalid-address and MMIO-copy return paths.
- `probe`: `size_bar1` uses `pci_resource_len(dev, 0)`; should be BAR 1.
- `accel_release` ignores the return value of `do_reset`.
- `allocate_mem_range`: `cstart` is never updated, so the "gap at the beginning" test applies to every node, not just the first. A large allocation can overlap an existing node (read by eye, not run).

## 9. T1 to T5 run-through

### 9.1 Data structures used

| Struct | What it is | Defined in | Fields used (task) |
|---|---|---|---|
| `struct msg` | request envelope user <-> kernel | `interface.h`, `user/interface.h` | `cmd` (T1,T5), `flags` (all), `d_handle` (T2,T4), `h_handle` (T4), `size` (T2,T4), `s1_addr`/`s2_addr`/`d_addr`/`count` (T1), `status[64]` (error text). User names: `daddr`, `uaddr`, `d_address`. |
| `struct accel_interface` (`aif`) | driver's global object, software state | `acceldev.h` | `lock` (T1,T2,T4), `status` (T1-T4), `devram`/`devmap` (all), `cur_devmem_mode` (T3), `pci_dev` (T1,T3) |
| `struct acceldev` (`adev = aif->pci_dev`) | hardware side: BARs, IRQ, completions | `acceldev.h` | `bar0`/`bar1` (T1,T3), `compute_comp` (T1), `irq_status` (T1) |
| `struct compute_context` (`cc`) | one memory region of the device | `acceldev.h` | `mem_range_list` (T1,T2,T3,T4), `used` (T2,T3), `kva` (T4), `start`/`end` (read only) |
| `struct mem_range` | one in-use stretch (list node) | `acceldev.h` | `start`, `end`, `list` (T2,T3) |
| `struct list_head` | kernel link type, embedded in `mem_range` | kernel | via list API |
| `struct completion` | one-shot "wake me" signal | kernel | `compute_comp` (T1) |
| `struct acceldev_ops` | table of function pointers | `interface.h` | not edited; how T1-T4 get called |

```
aif -> devram -> cc -> mem_range_list -> [mem_range] <-> [mem_range] ...
    -> devmap -> cc -> ...               (cc->kva points at BAR1, devmap only)
    -> pci_dev -> adev -> bar0, bar1, compute_comp, irq_status
```

### 9.2 API calls

| Call | Meaning |
|---|---|
| `spin_lock(&aif->lock)` / `spin_unlock` | one CPU at a time; never sleep inside |
| `aif->status = ACI_STATUS_*` | software state: READY / BUSY_COMP / BUSY_DCOPY |
| `_cwrite(val, base, off)` / `_cread(base, off)` | 32-bit device register write / read |
| `_cwriteq` / `_creadq` | 64-bit versions (DMA registers) |
| `reinit_completion(&c)` | reset counter to 0 (drop stale signals) |
| `wait_for_completion(&c)` | sleep until the IRQ handler calls `complete(&c)` |
| `find_range(cc, start, end)` | node that fully contains `[start,end)`, else NULL |
| `list_del(&node->list)` | unlink a node (pass the link field) |
| `list_add(&new->list, &node->list)` | insert `new` right after `node` |
| `list_for_each_entry_safe(n, tmp, head, member)` | walk a list while deleting the current node |
| `kzalloc(size, GFP_ATOMIC)` | zeroed alloc, never sleeps (lock held, T2) |
| `kmalloc(size, GFP_KERNEL)` | alloc, may sleep (no lock, T4) |
| `kfree(p)` | release |
| `copy_from_user(kbuf, ubuf, n)` / `copy_to_user(ubuf, kbuf, n)` | user <-> kernel (destination first) |
| `memcpy_toio(dev, kbuf, n)` / `memcpy_fromio(kbuf, dev, n)` | kernel buffer <-> device memory |
| `set_dev_mode(adev, mode)` | writes `BAR0_MEM_MODE` (existing static helper) |
| `snprintf(m->status, sizeof, ...)` | error text back to the user |
| user side: `memset`, `read(fd,&m,sizeof m)`, `printf` | zero msg, send command, print error |

### 9.3 Order per task

**T1 `do_compute`**
1. Check mode, count; map `m->cmd` to `ACCEL_CMD_*` (no lock)
2. Pick `cc`; `len = count * sizeof(int)`
3. lock -> status READY? -> `find_range` x3 -> status = BUSY_COMP -> unlock
4. `_cwrite` ERR_STATUS = 0; `reinit_completion`
5. `_cwrite` BAR1 `SRC_ONE`, `SRC_TWO`, `DST`, `COUNT`
6. `_cwrite` command to `BAR0_CMD` (last) [INFER]
7. `wait_for_completion(&compute_comp)`
8. `_cread` ERR_STATUS; check `irq_status & COMP_ERROR_IRQ`
9. lock -> status = READY -> unlock -> return

**T2 `do_free`**
1. Check mode, size (positive, page aligned); pick `cc`
2. lock -> status READY? -> `find_range`
3. Cases: exact (`list_del`+`kfree`), front, back, middle (`kzalloc` ATOMIC + `list_add`)
4. `cc->used -= size`
5. unlock (single exit `unlock_ret`); set error text if any

**T3 `do_reset`** (never touch `m`; no lock inside)
1. `list_for_each_entry_safe`: `list_del` + `kfree` for `devram`, then `devmap`
2. `used = 0` for both
3. `cur_devmem_mode = DMA`; `set_dev_mode`; clear ERR_STATUS (if `adev`)
4. `status = READY`; return 0

**T4 `do_copy` MMIO branch**
1. Check mode, size; pick `cc`
2. lock -> status READY? -> `find_range` -> status = BUSY_DCOPY -> unlock
3. `kmalloc` page buffer
4. Per chunk: H2D `copy_from_user` then `memcpy_toio`; D2H `memcpy_fromio` then `copy_to_user`
5. `kfree`; `ret:` -> status = READY

**T5 user side**
1. `memset` msg; set `cmd`, operands, `flags`
2. `read(fd,...)`; print `m.status` if negative
3. `usertest`: clear output, call op, copy back D2H, compare (use inputs 5 and 3: add 8, mul 15, xor 6)

### 9.4 One compute call, end to end
```
usertest -> accel_add -> fill msg, read()
  -> accel_read: copy_from_user(msg) -> switch(cmd) -> devops->accel_compute
    -> do_compute: validate, mark busy, clear error, operands -> BAR1, command -> BAR0_CMD
      -> device computes on its own
      -> device raises IRQ -> accel_irq_handler: read IRQSTATUS, ack, complete(&compute_comp)
    -> do_compute wakes: check error, mark ready, return
  -> accel_read: copy_to_user(msg) -> return to user
-> accel_copy D2H (do_copy / DMA) -> compare
```

## 10. Variable naming (same objects, different names)
- `cif` (global in `accel-main.c`/`accel-api.c`) = `aif` (parameter in `accel-ops.c`): the `struct accel_interface *`.
- `accel` (in `accel-main.c`) = `adev` (in `accel-ops.c`): the `struct acceldev *`. `adev = aif->pci_dev`.
- `dev` (in `accel-main.c`) = the kernel's `struct pci_dev *`; kept in `accel->dev`.
