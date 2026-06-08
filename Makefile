obj-m += sorabbyngofs.o

sorabbyngofs-objs := \
    src/sorabbyngofs_main.o \
    src/sorabbyngofs_inode.o \
    src/sorabbyngofs_file.o \
    src/sorabbyngofs_consent.o \
    src/sorabbyngofs_compress.o \
    src/sorabbyngofs_journal.o

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean

install:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules_install
