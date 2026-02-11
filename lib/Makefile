CC=gcc
AR=ar
CFLAGS=-Wall -O2 -I/usr/include/level_zero/
TARGET=libvmem.a
OBJS=vmem_lib.o
all: $(TARGET)
$(TARGET): $(OBJS)
	$(AR) rcs $@ $^
clean:
	rm -f *.o $(TARGET)
