# Mock lab: OP_USAGE

Rules (same as the real lab)
- You may use the source in this directory, the kernel headers on the machine, and paper.
- No internet, no phone, no cheatsheet.md, no asking me until you are done with a task.
- Edit the files in THIS directory (`mock/`), not `accel_release/`.
- Suggested time: 60 minutes.

## Background
The driver supports alloc, copy, compute, free. There is no way for a program to ask how much
device memory it is currently using.

## Task 1 (paper, 10 marks)
Before coding, write on paper:
(a) which files and which places in them must change to add a new operation end to end
    (user call -> kernel function). Name each struct / switch / table.
(b) the steps your kernel function will take, as numbered pseudocode.
Show this to me before you start Task 2.

## Task 2 (kernel, 40 marks)
Add a new operation `OP_USAGE`.

Request:  `m.flags` = region (0 = DMA RAM, 1 = mapped memory).
Reply:    `m.size`  = number of bytes currently allocated in that region,
          `m.count` = number of allocated ranges (list nodes) in that region.
          Return 0 on success, a negative errno otherwise, with a short text in `m.status`.

Requirements
- Reject an invalid region.
- The numbers must be consistent: another process may be allocating at the same time.
- Do not touch the hardware (it is pure bookkeeping), and do not sleep while holding a spinlock.
- Do not change the behaviour of any existing operation.

## Task 3 (user space, 20 marks)
In `user/`:
- add `int accel_usage(int fd, int where, long *bytes, long *nodes)` to the library,
- declare it in `user/interface.h`,
- add a check to `usertest.c`: usage in region DMA is 0 bytes and 0 nodes at start, and after the
  three `accel_malloc` calls it is 3 * 4096 * sizeof(int) bytes. (Because allocations merge,
  think about what the node count will be and justify it in a comment.)

## Task 4 (reasoning, 15 marks)
Run `usertest` twice without unloading the module. Describe, with reference to the code,
what the second run's `accel_usage` would report, and why. Name the function responsible.
(You do not have to fix it.)

## Task 5 (15 marks)
The `OP_USAGE` message and its reply travel through `accel_read`. List two existing weaknesses
in `accel_read` or `do_query` that your new operation inherits, and how each could be exploited
or mis-triggered.

## Build and test
- `make` in this directory builds the module and the user test.
- `sudo insmod accel.ko`, then `./user/usertest`, then `dmesg | tail`.
- Building needs kernel headers and the device. If you cannot build here, say so and explain
  how you would verify the change.
