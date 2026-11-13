# pio: port-mapped I/O

`pio.c` talks to the 16550 UART at I/O port 0x3F8 with `inb`/`outb`. `serial_putc` polls the Line
Status Register until the transmitter is ready, then writes the byte (it adds `\r` after `\n`).
The module prints a greeting on load and a farewell on unload.

Run it in a VM and redirect the emulated serial port to a file on the host to see the output
(for QEMU, `-serial file:serial.log`).
```bash
make && sudo insmod pio.ko && sudo rmmod pio
```
