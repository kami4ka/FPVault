# Host-side unit tests: plain cc, no cross-toolchain. `make` builds and runs
# everything; each test binary exits non-zero on failure.

CC      ?= cc
CFLAGS  += -O1 -g -Wall -Wextra -std=gnu99 -I../../src

TESTS := $(patsubst %.c,%,$(wildcard test_*.c))

all: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$$t; done; echo "ALL HOST TESTS PASS"

test_avi: test_avi.c ../../src/avi.c ../../src/avi.h
	$(CC) $(CFLAGS) -o $@ test_avi.c ../../src/avi.c

test_dcf: test_dcf.c ../../src/dcf.c ../../src/dcf.h
	$(CC) $(CFLAGS) -o $@ test_dcf.c ../../src/dcf.c

test_runcam: test_runcam.c ../../src/runcam.c ../../src/runcam.h
	$(CC) $(CFLAGS) -o $@ test_runcam.c ../../src/runcam.c

test_jpegtab: test_jpegtab.c ../../src/jpegtab.c ../../src/jpegtab.h
	$(CC) $(CFLAGS) -o $@ test_jpegtab.c ../../src/jpegtab.c

clean:
	rm -f $(TESTS) *.avi

.PHONY: all clean
