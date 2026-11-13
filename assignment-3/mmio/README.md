# mmio: memory-mapped I/O

`mmio.c` maps the local APIC registers (physical base 0xFEE00000) with `ioremap`, reads them with
`ioread32`, and publishes them as read-only sysfs attributes `apic_id` and `apic_version`. It also
walks the page tables (`pgd` to `pte`) to print the PTE of the new mapping. The base address is
hard-coded; check it against `/proc/iomem`. `helper.txt` has a sample `e820` map and `iomem` listing
for reference.

```bash
make && sudo insmod mmio.ko
cat /sys/kernel/apic_id /sys/kernel/apic_version
sudo rmmod mmio
```
