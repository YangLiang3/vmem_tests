CC ?= gcc
AR ?= ar

PREFIX ?= /usr
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include/vmem_test

CFLAGS ?= -Wall -O2
CFLAGS += -fPIC -I/usr/include/level_zero/ -I../driver/

TARGET_STATIC = libvmem.a
TARGET_SHARED = libvmem.so
OBJS = vmem_lib.o

all: $(TARGET_STATIC) $(TARGET_SHARED)

$(TARGET_STATIC): $(OBJS)
	$(AR) rcs $@ $^

$(TARGET_SHARED): $(OBJS)
	$(CC) -shared -Wl,-soname,$(TARGET_SHARED) -o $@ $^

install: all
	install -d $(LIBDIR) $(INCLUDEDIR)
	install -m 644 $(TARGET_STATIC) $(LIBDIR)/$(TARGET_STATIC)
	install -m 755 $(TARGET_SHARED) $(LIBDIR)/$(TARGET_SHARED)
	install -m 644 vmem_lib.h $(INCLUDEDIR)/vmem_lib.h
	install -m 644 ../driver/vmem_ioctl.h $(INCLUDEDIR)/vmem_ioctl.h

print-install-dirs:
	@echo LIBDIR=$(LIBDIR)
	@echo INCLUDEDIR=$(INCLUDEDIR)

clean:
	rm -f *.o $(TARGET_STATIC) $(TARGET_SHARED)
