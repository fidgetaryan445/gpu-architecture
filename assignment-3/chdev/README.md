# chdev: minimal character device

`simplecdev.c` registers a character device `democdev` (major number allocated dynamically) and
creates `/dev/democdev` through a device class. It implements `open`, `release` (which take and
drop a module reference, tracked in an `atomic_t` open count), `read` and `write`. `testcdev.c`
is a user-space program that opens the node and exercises read/write.

```bash
make && sudo insmod simplecdev.ko
gcc testcdev.c -o testcdev && sudo ./testcdev
dmesg | tail           # shows the "In read" / "In write" messages
sudo rmmod simplecdev && make clean
```
