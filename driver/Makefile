obj-m += vmem_drv.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
all:
	make -C $(KDIR) M=$(PWD) modules
clean:
	rm -rf *.o *.ko *.mod.* *.symvers *.order *.cmd
