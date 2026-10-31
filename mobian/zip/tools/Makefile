# s10e-part: static aarch64 binary for LineageOS recovery (the zip ships
# this one) and a host build for tests on the dev box.
#   make            -> s10e-part (aarch64, static) + s10e-part-host
CROSS_CC ?= aarch64-linux-gnu-gcc
HOST_CC  ?= gcc
CFLAGS   ?= -Os -std=gnu11 -Wall -Wextra

all: s10e-part s10e-part-host

# Recovery has no glibc we could count on, hence -static.
s10e-part: s10e-part.c
	$(CROSS_CC) -static $(CFLAGS) -o $@ $<
	aarch64-linux-gnu-strip $@

s10e-part-host: s10e-part.c
	$(HOST_CC) $(CFLAGS) -o $@ $<

clean:
	rm -f s10e-part s10e-part-host

.PHONY: all clean
