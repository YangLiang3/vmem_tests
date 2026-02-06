CC=gcc
CFLAGS=-Wall -O2
LDFLAGS=
TARGET=test_vmem
all: $(TARGET)
$(TARGET): test_vmem.o ../lib/libvmem.a
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
clean:
	rm -f *.o $(TARGET)
