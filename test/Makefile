CC=icpx
CFLAGS=-Wall -O2 -I/usr/include/level_zero/ -I../driver/ -g
LDFLAGS=-lmpi -lze_loader
TARGET=test_vmem
all: $(TARGET)
$(TARGET): test_vmem.o ../lib/libvmem.a
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
clean:
	rm -f *.o $(TARGET)
