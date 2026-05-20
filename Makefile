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
CPM_SRCS  = cpm/cpm_bdos.c cpm/cpm_bios.c cpm/cpm_loader.c
KAYPRO_SRCS = kaypro/kaypro_video.c kaypro/kaypro_kbd.c

# DBT sources
DBT_COMMON_SRCS = dbt/dbt_common.c dbt/block_cache.c
DBT_SRCS = $(DBT_COMMON_SRCS) $(DBT_ARCH_SRC)

# For now the main is a stub that will grow into the monster
SRCS = main.c $(CORE_SRCS) $(CPM_SRCS) $(KAYPRO_SRCS) $(DBT_SRCS)
OBJS = $(SRCS:.c=.o)

TARGET = z80-monster

.PHONY: all clean test dirs

all: dirs $(TARGET)
	@echo "Built $(TARGET) for $(UNAME_M)"
	@echo "Run with: ./$(TARGET) -h   (once we have something to run)"

dirs:
	@mkdir -p core dbt cpm kaypro tests tools

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

# Generic rule (dependencies will be added as we create headers)
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
	rm -rf core/*.o dbt/*.o cpm/*.o kaypro/*.o
	rm -f tools/mkhello tools/mkblock
	rm -f tests/*.com tests/*.bin

# Placeholder test target — will expand when we have .COM tests
test:
	@echo "No tests yet — the monster is still in the larval stage."
	@echo "Soon: ./$(TARGET) -i tests/hello.com && ./$(TARGET) -V tests/hello.com"

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

# Quick sanity: does it even compile and say hello?
smoke: $(TARGET) tests/hello.com tests/block.com tests/cb.com tests/ix.com tests/console.com
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
