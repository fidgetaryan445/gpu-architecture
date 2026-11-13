#include<linux/module.h>
#include<linux/kernel.h>
#include<linux/sched.h>
#include<linux/io.h>

/*XXX Note for the VM, redirect the serial port o/p to a file in the host*/

/*Definitions taken from specs*/
#define XMTRDY          0x20
#define RCVRDY          0x1

#define DLAB            0x80

#define TXR             0       /*  Transmit register (WRITE) */
#define RXR             0       /*  Receive register  (READ)  */
#define IER             1       /*  Interrupt Enable          */
#define IIR             2       /*  Interrupt ID              */
#define FCR             2       /*  FIFO control              */
#define LCR             3       /*  Line control              */
#define MCR             4       /*  Modem control             */
#define LSR             5       /*  Line Status               */
#define MSR             6       /*  Modem Status              */
#define DLL             0       /*  Divisor Latch Low         */
#define DLH             1       /*  Divisor latch High        */

u16 base = 0x3f8;  /* serial device base port address*/;

static void serial_putc(u8 ch)
{
        while ((inb(base + LSR) & XMTRDY) == 0);
        outb(ch, base + TXR);
}

void serial_write(char *buf)
{
    u8 ch;
    while ((ch = *buf++)){
        serial_putc(ch);
        if(ch == '\n')
             serial_putc('\r');
    }
}

int init_module(void)
{
        serial_write("Hello from serial device\n");
	return 0;
}

void cleanup_module(void)
{
        serial_write("Bye from serial device\n");

}

MODULE_AUTHOR("deba@cse.iitk.ac.in");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CS614 example modules");
