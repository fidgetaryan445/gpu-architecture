obj-m += accel.o
accel-objs = accel-main.o accel-api.o accel-ops.o

KVERSION = $(shell uname -r)
EXTRA_CFLAGS += -I$(PWD) -g

all:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) modules
	$(MAKE) -C user

clean:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) clean
	$(MAKE) -C user clean
