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

.PHONY: all driver lib test clean
