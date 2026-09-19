# z80 — 10 BIPS CP/M Monster
# Multi-architecture DBT: x86-64 or AArch64

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -std=c11
CFLAGS += -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L

# macOS needs extra defines for mmap / termios
ifeq ($(shell uname -s),Darwin)
    CFLAGS += -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE
endif

# Architecture detection (same pattern as riscv/dbt and slow-32)
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),arm64)
    UNAME_M := aarch64
endif

ifeq ($(UNAME_M),x86_64)
    DBT_ARCH_SRC = dbt/dbt_x64.c
    DBT_ARCH_HDR = dbt/emit_x64.h
else ifeq ($(UNAME_M),aarch64)
    DBT_ARCH_SRC = dbt/dbt_a64.c
    DBT_ARCH_HDR = dbt/emit_a64.h
else
    $(error Unsupported host: $(UNAME_M) — only x86_64 and aarch64 supported)
endif

# Core sources (will grow)
CORE_SRCS = core/z80_decode.c core/z80_interp.c core/z80_state.c
CPM_SRCS  = cpm/cpm_bdos.c cpm/cpm_bios.c cpm/cpm_loader.c cpm/cpm_disk.c cpm/cpm_ports.c cpm/cpm_host.c
KAYPRO_SRCS = kaypro/kaypro_video.c kaypro/kaypro_render_tty.c kaypro/kaypro_kbd.c

# DBT sources
DBT_COMMON_SRCS = dbt/dbt_common.c dbt/block_cache.c dbt/dbt_flags.c
DBT_SRCS = $(DBT_COMMON_SRCS) $(DBT_ARCH_SRC)

# For now the main is a stub that will grow into the monster
SRCS = main.c $(CORE_SRCS) $(CPM_SRCS) $(KAYPRO_SRCS) $(DBT_SRCS)
OBJS = $(SRCS:.c=.o)

TARGET = z80-monster

.PHONY: all clean test dirs

all: dirs $(TARGET) tools/mkdsk
	@echo "Built $(TARGET) for $(UNAME_M)"
	@echo "Run with: ./$(TARGET) -h   (once we have something to run)"

dirs:
	@mkdir -p core dbt cpm kaypro tests tools

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

# Generic rule with automatic header dependencies (-MMD), so a struct
# change in core/z80.h rebuilds every object that includes it.
%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(OBJS:.o=.d)

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET)
	rm -rf core/*.o dbt/*.o cpm/*.o kaypro/*.o
	rm -f tools/mkhello tools/mkblock
	rm -f tests/*.com tests/*.bin

test: test-video test-render test-kbd test-guest test-apps

# Guest-side tests: the generated .COMs under the interpreter, the JIT,
# and lockstep verify. ports.com is the Phase 0 acceptance test (every
# port op traps, so all three modes exercise the same interpreter path).
GUEST_TESTS = tests/hello.com tests/block.com tests/cb.com tests/ix.com tests/ports.com tests/loop.com
.PHONY: test-guest
test-guest: $(TARGET) $(GUEST_TESTS)
	@fail=0; \
	for mode in -i -j -V; do \
	    out=$$(./$(TARGET) $$mode tests/ports.com 2>&1 | tr -d '\000'); \
	    case "$$out" in *"PORT I/O OK"*) echo "ports.com $$mode: ok" ;; \
	        *) echo "ports.com $$mode: FAIL"; fail=1 ;; esac; \
	done; \
	for t in hello block cb ix; do \
	    out=$$(./$(TARGET) -V tests/$$t.com 2>&1 | tr -d '\000'); \
	    case "$$out" in *divergence*|*"lockstep broken"*|*"stopped with error"*) \
	        echo "$$t.com -V: FAIL"; fail=1 ;; \
	        *) echo "$$t.com -V: ok" ;; esac; \
	done; \
	[ $$fail -eq 0 ] && echo "test-guest: all pass" || { echo "test-guest: FAILURES"; exit 1; }

# Headless application smoke tests (skip when the software is not in disks/).
.PHONY: test-apps
test-apps: $(TARGET) tools/mkdsk tests/hello.com tests/block.com
	@tests/cpm22.sh jit
	@tests/zork.sh jit
	@tests/wordstar.sh jit
	@tests/dbase.sh jit

# Host-side unit test of the Kaypro screen model (no Z80 involved).
.PHONY: test-video
tests/video_test: tests/video_test.c kaypro/kaypro_video.c kaypro/kaypro_video.h
	$(CC) $(CFLAGS) -o $@ tests/video_test.c kaypro/kaypro_video.c
test-video: tests/video_test
	./tests/video_test

# Host-side test of the key decoder / key map.
.PHONY: test-kbd
tests/kbd_test: tests/kbd_test.c kaypro/kaypro_kbd.c kaypro/kaypro_video.c kaypro/kaypro_kbd.h
	$(CC) $(CFLAGS) -o $@ tests/kbd_test.c kaypro/kaypro_kbd.c kaypro/kaypro_video.c
test-kbd: tests/kbd_test
	./tests/kbd_test

# Host-side test of the terminal renderer (escape stream captured in memory).
.PHONY: test-render
tests/render_test: tests/render_test.c kaypro/kaypro_render_tty.c kaypro/kaypro_video.c kaypro/kaypro_render_tty.h kaypro/kaypro_video.h
	$(CC) $(CFLAGS) -o $@ tests/render_test.c kaypro/kaypro_render_tty.c kaypro/kaypro_video.c
test-render: tests/render_test
	./tests/render_test

# The standard work disk: an 8 MB image holding every file under disks/
# except the exercisers (WordStar, dBASE, Turbo Pascal, MS-COBOL, MBASIC,
# M80/L80, Zork... whatever you have there). Git-ignored like its inputs.
#   ./z80-monster -j -K -A disks/work.dsk
.PHONY: workdisk
workdisk: tools/mkdsk
	@rm -f disks/work.dsk
	@files=$$(find disks -mindepth 2 -maxdepth 2 -type f ! -path 'disks/zex/*' ! -name '.*' ! -name '*.txt' ! -name '*,280' | sort); \
	  n=0; for f in $$files; do ./tools/mkdsk disks/work.dsk "$$f" > /dev/null; n=$$((n+1)); done; \
	  echo "disks/work.dsk: $$n files"; ./tools/mkdsk -l disks/work.dsk | tail -1

# Disk image tool (tools/mkdsk.c): make, fill, list, extract z80m images.
tools/mkdsk: tools/mkdsk.c
	$(CC) $(CFLAGS) -o $@ tools/mkdsk.c

# The CP/M 2.2 system image: DRI's CCP (DC00) and BDOS (E400) plus our
# BIOS (F200), assembled with Macro Assembler AS (tools/get-asl.sh
# builds it into tools/asl/). cpm/cpm22/system.bin is checked in, so a
# clone boots without the assembler; `make system` rebuilds it.
ASL   = tools/asl/asl
P2BIN = tools/asl/p2bin
.PHONY: system
system: cpm/cpm22/system.bin
cpm/cpm22/system.bin: cpm/cpm22/ccp.asm cpm/cpm22/bdos.asm cpm/cpm22/bios.asm
	@test -x $(ASL) || { echo "need $(ASL): run tools/get-asl.sh"; exit 1; }
	$(ASL) -q -D origin=0dc00h -o cpm/cpm22/ccp.p  -L -OLIST cpm/cpm22/ccp.lst  cpm/cpm22/ccp.asm
	$(P2BIN) -q -l '$$00' -r '$$dc00-$$e3ff' cpm/cpm22/ccp.p  cpm/cpm22/ccp.bin
	$(ASL) -q -D origin=0e400h -o cpm/cpm22/bdos.p -L -OLIST cpm/cpm22/bdos.lst cpm/cpm22/bdos.asm
	$(P2BIN) -q -l '$$00' -r '$$e400-$$f1ff' cpm/cpm22/bdos.p cpm/cpm22/bdos.bin
	$(ASL) -q -o cpm/cpm22/bios.p -L -OLIST cpm/cpm22/bios.lst cpm/cpm22/bios.asm
	$(P2BIN) -q -l '$$00' -r '$$f200-$$f9ff' cpm/cpm22/bios.p cpm/cpm22/bios.bin
	cat cpm/cpm22/ccp.bin cpm/cpm22/bdos.bin cpm/cpm22/bios.bin > $@
	@ls -l $@

# Run the MS COBOL square-root benchmark (jit vs interp).
# Override N=... for a different workload size.
N ?= 2000
.PHONY: bench
bench: $(TARGET)
	@bench/squaro.sh $(N) jit
	@bench/squaro.sh $(N) interp

# Build the tiny hello.com test program
tests/hello.com: tools/mkhello
	@mkdir -p tests
	./tools/mkhello

tools/mkhello: tools/mkhello.c
	$(CC) -o $@ $<

tests/block.com: tools/mkblock
	@mkdir -p tests
	./tools/mkblock

tools/mkblock: tools/mkblock.c
	$(CC) -o $@ $<

tests/cb.com: tools/mkcb
	@mkdir -p tests
	./tools/mkcb

tools/mkcb: tools/mkcb.c
	$(CC) -o $@ $<

tests/loop.com: tools/mkloop
	@mkdir -p tests
	./tools/mkloop

tools/mkloop: tools/mkloop.c
	$(CC) -o $@ $<

tests/ix.com: tools/mkix
	@mkdir -p tests
	./tools/mkix

tools/mkix: tools/mkix.c
	$(CC) -o $@ $<

tests/console.com: tools/mkconsole
	@mkdir -p tests
	./tools/mkconsole

tools/mkconsole: tools/mkconsole.c
	$(CC) -o $@ $<

tests/fileio.com: tools/mkfileio
	@mkdir -p tests
	./tools/mkfileio

tools/mkfileio: tools/mkfileio.c
	$(CC) -o $@ $<

tests/random.com: tools/mkrandom
	@mkdir -p tests
	./tools/mkrandom

tools/mkrandom: tools/mkrandom.c
	$(CC) -o $@ $<

tests/ports.com: tools/mkports
	@mkdir -p tests
	./tools/mkports

tools/mkports: tools/mkports.c
	$(CC) -o $@ $<

# Quick sanity: does it even compile and say hello?
smoke: $(TARGET) tests/hello.com tests/block.com tests/cb.com tests/ix.com tests/console.com tests/ports.com
	@echo "=== Smoke test ==="
	./$(TARGET) --version || true
	@echo
	@echo "=== Running hello.com ==="
	./$(TARGET) -s tests/hello.com
	@echo
	@echo "=== Running block.com (LDIR test) ==="
	./$(TARGET) -s tests/block.com
	@echo
	@echo "=== Running cb.com (CB rotate + BIT test) ==="
	./$(TARGET) -s tests/cb.com
	@echo
	@echo "=== Running ix.com (IX+d + IXH test) ==="
	./$(TARGET) -s tests/ix.com
	@echo
	@echo "=== Running console.com (interactive CONST/CONIN/CONOUT test) ==="
	@echo "    (Press keys — it should echo. ESC or 'q' to exit cleanly)"
	./$(TARGET) -s tests/console.com
	@echo
	@echo "Environment console layer is live if the echo test feels responsive."
