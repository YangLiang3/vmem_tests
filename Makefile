PREFIX ?= /usr
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include/vmem_test

all: driver lib test

driver:
	$(MAKE) -C driver

lib:
	$(MAKE) -C lib

test:
	$(MAKE) -C test

clean:
	$(MAKE) -C driver clean
	$(MAKE) -C lib clean
	$(MAKE) -C test clean

install: lib
	$(MAKE) -C lib install PREFIX=$(PREFIX) LIBDIR=$(LIBDIR) INCLUDEDIR=$(INCLUDEDIR)

print-install-dirs:
	@echo PREFIX=$(PREFIX)
	@echo LIBDIR=$(LIBDIR)
	@echo INCLUDEDIR=$(INCLUDEDIR)

.PHONY: all driver lib test clean install print-install-dirs