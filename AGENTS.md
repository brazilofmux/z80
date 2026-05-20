# Repository Guidelines — z80 CP/M Monster

## Project Spirit

This is a **play project** with an absurd performance goal: make a Z80 + CP/M system (Kaypro, WordStar, etc.) run at 2–10+ billion instructions per second on modern hosts.

We are allowed (encouraged) to:
- Go completely overboard on micro-optimizations in the DBT
- Special-case the hell out of real applications (WordStar screen loops, dBASE B-tree walks, etc.)
- Mix "authentic emulation" with "cheat for speed" (e.g., host-accelerated block moves, native file I/O, modern clipboard in a CP/M app)
- Have fun and ship ridiculous numbers

## Project Structure

```
z80/
├── core/           # Z80 ISA: decoder, interpreter, state
├── dbt/            # The monster JIT (x64 + a64 backends)
├── cpm/            # BDOS 2.2/3, BIOS, .COM loader, drive mapping
├── kaypro/         # Video (80x24), keyboard, "Kaypro terminal" personality
├── tools/          # Utilities, disk tools, stats reporter
├── tests/          # .COM test programs + expected output
└── Makefile
```

Generated artifacts (`*.o`, `z80-monster`, `*.log`) are **not** committed.

## Build & Test Commands

```bash
make                # build everything (picks x64 or a64 backend automatically)
make clean
make test           # run the regression suite (interpreter + DBT + diff)
./z80-monster -i tests/hello.com     # force interpreter
./z80-monster -s tests/hello.com     # show stats (BIPS, block count, etc.)
./z80-monster -V tests/hello.com     # verify mode (shadow interp vs JIT)
```

## Coding Style (same as riscv + slow-32)

- C11, `-Wall -Wextra -O2 -g`, warning-clean
- 4 spaces, no tabs
- `snake_case` for everything except macros (`UPPER_CASE`)
- Keep headers small and focused; pair `foo.h` + `foo.c`
- No external dependencies. Pure libc + mmap + termios for now.
- The DBT must be **Apple Silicon W^X safe** (see riscv/dbt/dbt.h for the pattern with `dbt_jit_writable_begin/end` and `MAP_JIT`).

## Z80-Specific Notes

- Decoder must handle all four prefixes: `CB`, `DD` (IX), `ED`, `FD` (IY). DD/FD can stack.
- Flags are the eternal pain: S Z 5 H 3 P/V N C. Some apps (especially copy protection and games) care about bits 3 and 5.
- For DBT performance we will aggressively pursue **lazy / deferred flag materialization**.
- Block instructions (`LDIR`, `CPIR`, etc.) and `DAA`/`RLD`/`RRD` are prime candidates for host intrinsics.
- CP/M entry points: `RST 0` (warm boot), `CALL 0005h` (BDOS), `CALL 0000h` (warm boot in some setups).

## Validation Strategy

1. **Interpreter is golden** — every DBT change must pass `-V` (shadow verification) against it.
2. Real CP/M binaries are the ultimate test: WordStar, Turbo Pascal, Zork, MBASIC, etc.
3. Performance numbers are measured with a high-resolution cycle counter (or `clock_gettime`) and reported as "Z80 MIPS/BIPS".

## Commit Tone

Short, imperative, technically specific:

- `z80: add ED-prefixed block move decoder`
- `dbt: implement lazy flag materialization for ADD/SUB/INC`
- `cpm: map host dir as drive A: with FCB translation`
- `kaypro: 80x24 memory-mapped video + reverse video attribute`

## When Things Go Sideways

- Z80 flag weirdness? Write a tiny test .COM that exercises the exact sequence and put it in `tests/`.
- DBT correctness? The shadow interpreter (`-V`) is your best friend.
- Apple Silicon W^X crash? You forgot to bracket a code buffer write with `dbt_jit_writable_*`.

## References (local)

- `~/riscv/dbt/` — the clean, lightweight DBT template we are forking
- `~/slow-32/tools/dbt/` — the heavy research version with full stage5 compiler pipeline (lift → BURG → SSA → RA)
- CP/M 2.2 Interface Guide (in your head or docs/)
- "The Z80 CPU User Manual" (Zilog) for the gory opcode details

---

*Ship fast. Measure BIPS. Make 1985 jealous.*
