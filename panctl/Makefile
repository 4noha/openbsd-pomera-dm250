# SPDX-License-Identifier: MIT
# Copyright (c) 2026 4noha
# panctl/Makefile — pomera 側 mux client の build entry point.
#
# 段階 1 (現状): frame.c の test_frame だけ。macOS と OpenBSD 両方で素直に通る
#                ことを目標にする。BTstack / libusb / pf 依存はまだ無し。
#
# 段階 2 以降で mux.c / main.c / divert.c / BTstack glue を足す予定。
# OpenBSD では `gmake`、macOS では `make` のどちらでも動くよう Makefile を
# 書く (BSD make と GNU make の両方で意味が変わる構文は避ける)。

UNAME_S := $(shell uname -s)

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -O2 -g
CFLAGS  += -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=
LD      ?= $(CC)

ifeq ($(UNAME_S),OpenBSD)
  # /usr/local/{include,lib} は BTstack / libusb1 用に後から効いてくる
  CFLAGS  += -I/usr/local/include
  LDFLAGS += -L/usr/local/lib
endif

# ---- Cross-compile target: macOS host → OpenBSD/armv7 ----
# Driven by tools/build-armv7.sh which sets these on the command line:
#   make armv7 CROSS_SYSROOT=/path CROSS_TARGET=armv7-unknown-openbsd LD=ld.lld
# Without those, the `armv7` target prints guidance and exits.
CROSS_TARGET   ?=
CROSS_SYSROOT  ?=

FRAME_OBJS = frame.o
MUX_OBJS   = mux.o $(FRAME_OBJS)
IPV4_OBJS  = ipv4.o

TEST_FRAME_OBJS = test_frame.o $(FRAME_OBJS)
TEST_MUX_OBJS   = test_mux.o $(MUX_OBJS)
TEST_IPV4_OBJS  = test_ipv4.o $(IPV4_OBJS)

# Optional dependencies: detect with pkg-config / file existence.
HAVE_LIBUSB := $(shell pkg-config --exists libusb-1.0 2>/dev/null && echo 1)
HAVE_BTSTACK := $(shell test -d ../third_party/btstack && echo 1)

ifeq ($(HAVE_LIBUSB),1)
  LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
  LIBUSB_LIBS   := $(shell pkg-config --libs   libusb-1.0)
endif

.PHONY: all test clean help info armv7

# Always-built targets (no external deps).
ALWAYS_TARGETS = test_frame test_mux test_ipv4 divert.o main.o

# Conditionally include tools/* and panctl based on dep availability.
OPTIONAL_TARGETS =
ifeq ($(HAVE_LIBUSB),1)
  OPTIONAL_TARGETS += ../tools/hci_smoke
endif
ifeq ($(HAVE_BTSTACK),1)
  OPTIONAL_TARGETS += panctl-libusb
endif

all: $(ALWAYS_TARGETS) $(OPTIONAL_TARGETS)

info:
	@echo "HAVE_LIBUSB=$(HAVE_LIBUSB)"
	@echo "HAVE_BTSTACK=$(HAVE_BTSTACK)"
	@echo "ALWAYS_TARGETS=$(ALWAYS_TARGETS)"
	@echo "OPTIONAL_TARGETS=$(OPTIONAL_TARGETS)"

help:
	@echo "Targets:"
	@echo "  all          — build everything available (default)"
	@echo "  test         — run frame/mux/ipv4 unit tests"
	@echo "  panctl-libusb — build panctl (requires HAVE_BTSTACK=1)"
	@echo "  ../tools/hci_smoke — build HCI疎通テスト (requires HAVE_LIBUSB=1)"
	@echo "  armv7        — cross-compile to OpenBSD/armv7 (requires ../tools/build-armv7.sh prereqs)"
	@echo "  clean        — remove build artifacts"
	@echo "  info         — show detected feature flags"

armv7:
	@cd .. && ./tools/build-armv7.sh

test_frame: $(TEST_FRAME_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TEST_FRAME_OBJS)

test_mux: $(TEST_MUX_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TEST_MUX_OBJS)

test_ipv4: $(TEST_IPV4_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TEST_IPV4_OBJS)

# divert.o is OpenBSD-gated; on macOS it compiles to a stub. We still
# build it in `all` to catch macro / signature drift early.
divert.o: divert.c divert.h frame.h mux.h ipv4.h
	$(CC) $(CFLAGS) -c -o $@ divert.c

# main.o is BTstack-gated. Without HAVE_BTSTACK it compiles to a stub
# that prints an error at runtime. We build it in `all` to catch
# signature drift between main.c <-> mux.h <-> divert.h.
main.o: main.c frame.h mux.h divert.h
	$(CC) $(CFLAGS) -c -o $@ main.c

# hci_smoke: pure libusb疎通テスト. Lives under btstack/tools/.
../tools/hci_smoke: ../tools/hci_smoke.c
	$(CC) $(CFLAGS) $(LIBUSB_CFLAGS) $(LDFLAGS) -o $@ ../tools/hci_smoke.c $(LIBUSB_LIBS)

# panctl-libusb: requires BTstack vendored under ../third_party/btstack/.
# This rule is intentionally a stub; the real build pulls in BTstack's
# Makefile.inc from port/libusb to get all source files. The user runs
# tools/fetch-btstack.sh first, then `make panctl-libusb`. Wiring up
# BTstack's source list cleanly is deferred to the actual integration
# session on OpenBSD.
panctl-libusb: main.c mux.o frame.o ipv4.o divert.o
	@echo "panctl-libusb: BTstack integration is platform-specific (OpenBSD)"
	@echo "Run on the actual target. This rule documents the expected sources:"
	@echo "  main.c mux.o frame.o ipv4.o divert.o + BTstack port/libusb sources"
	@echo "Aborting to avoid producing a broken binary."
	@false

test: test_frame test_mux test_ipv4
	./test_frame
	./test_mux
	./test_ipv4

clean:
	rm -f test_frame test_mux test_ipv4 *.o ../tools/hci_smoke

%.o: %.c frame.h mux.h ipv4.h
	$(CC) $(CFLAGS) -c -o $@ $<
