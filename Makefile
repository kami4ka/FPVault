#
# f1c200s-dvr - open-source airborne FPV DVR.
#
# CVBS camera -> TVD DMA -> Cedar VE hardware JPEG -> SD (MJPEG-in-AVI).
# Record-only tap: no video output path.
#
# Build:   make            (needs arm-none-eabi-gcc)
# Deploy:  make deploy     (U-Boot on the board: loady 0x80000000 + go)
#

PROJECT_NAME = dvr

PROJROOT := vendor/F1C100s_projects

# Vendored link script: carries the ASSERT that keeps code/heap/stacks below
# the capture planes (see src/board.h for the whole DRAM map).
LINK_SCRIPT = src/f1c200s_dvr.ld

# SRCS must be complete BEFORE the include below - f1c100s_common.mk expands
# OBJS immediately, so anything appended afterwards is silently dropped.
SRCS += src/main.c src/system.c src/exception.c src/console.c \
        src/ve.c src/vejpeg.c src/jpegtab.c src/testpat.c src/enctest.c

INCLUDES += -Isrc

OPT = -O2

# Puts `b _start` at the load address, which is what makes U-Boot's
# `loady 0x80000000` + `go 0x80000000` work.
DEFS += -DLOAD_HEADER

# Base of the capture planes, given to BOTH the C and the link script from one
# variable so they cannot drift. The -D alone does NOT satisfy the script's
# ASSERT - that needs the --defsym, evaluated by the linker.
CAPTURE_BASE = 0x81000000
DEFS    += -DCAPTURE_BASE=$(CAPTURE_BASE)
LDFLAGS += -Wl,--defsym=CAPTURE_BASE=$(CAPTURE_BASE)

# F1C200s: 64 MB, verified byte-exact by the predecessor project. Must stay
# <= the cacheable window mapped in src/system.c.
DRAM_SIZE = 64M

GIT_REV := $(shell git rev-parse --short HEAD 2>/dev/null || echo none)
DEFS += -DGIT_REV=\"$(GIT_REV)\"

# Header dependency tracking (the vendor mk has none). Objects also depend on
# the makefiles, so changing DEFS forces a rebuild instead of a stale link.
CFLAGS += -MMD -MP

include $(PROJROOT)/f1c100s_common.mk

-include $(OBJS:.o=.d)
$(OBJS): Makefile $(PROJROOT)/f1c100s_common.mk

# ---- deploy over U-Boot YMODEM ------------------------------------------
PORT ?= /dev/cu.usbserial-0001

deploy: $(BIN)
	python3 tools/loader.py $(PORT) $(BIN)

.PHONY: deploy
