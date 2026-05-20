# CLAUDE.md — Z80 + CP/M Dynamic Binary Translator

## What This Is

A high-performance execution environment for **Z80** guest code running **CP/M 2.2/3** (and classic applications like WordStar, dBASE II, Turbo Pascal) on modern x86-64 and AArch64 hosts, with the explicit goal of reaching **multiple billions of Z80 instructions per second**.

It is the spiritual successor to the RV32IMFD DBT in `~/riscv/dbt` and the more ambitious SLOW-32 DBT in `~/slow-32/tools/dbt`. We are stealing every good idea and applying it to one of the most irregular 8-bit ISAs ever shipped in volume.

## Why This Exists (The Vibe)

- Because a 4 MHz Z80 running WordStar in 1983 was magical.
- Because making that same experience run at **2,000–10,000×** the speed on a $3000 laptop in 2026 is hilarious and beautiful.
- Because the DBT techniques we developed for clean RISC ISAs are even more valuable (and harder) when applied to a CISC 8-bit monster with 700+ opcode variants, four prefix bytes, and a flag register that was designed in 1976.
- "10 BIPS CP/M monster for no good reason" is the entire design document.

## High-Level Architecture

### Guest Environment

- **CPU**: Z80 (all documented instructions + the useful undocumented ones: `SLL`, `OUT (C),0`, IX/IY bit ops, etc.)
- **Memory**: 64 KB flat (0x0000–0xFFFF). Bank switching (for CP/M 3 / MP/M / Kaypro 4/10 with RAM disk) is a later concern.
- **CP/M 2.2**:
  - `0000h` — warm boot vector (usually JP BIOS+3)
  - `0005h` — BDOS entry (C = function, DE = param, return in A/HL)
  - BIOS at top of memory (usually 0xF200 or similar for 62K TPA)
- **.COM files** load at `0100h`, stack at high memory, `RET` at `0000h` for warm boot.

### Host Services Model (like the RV32 one)

Instead of raw port I/O everywhere, we provide a clean "BDOS/BIOS shim" layer that translates to host:

- Console: raw termios + kbhit + non-blocking read
- Disk: host directory mapped as drive A:, B:, etc. FCB ↔ host `open/read/write`
- Later: real .DSK / .RAW image support for authenticity
- Video: Kaypro 80×24 character cell + attributes (reverse, underline, dim, etc.)

### DBT Pipeline (modeled on riscv/dbt)

1. **Loader** — `.COM` at 0x100, or full system image, or "boot sector" style.
2. **Decoder** — single-instruction + prefix state machine (DD/FD can nest with CB/ED).
3. **Translator** (per-arch) — emit native code for hot blocks.
4. **Block cache** — same 64K-entry direct-mapped 16-byte entries as riscv.
5. **Interpreter** — slow but correct reference, used for `-i` and for `-V` shadow verification.
6. **Shadow interpreter** (optional, like `shadow.c` in riscv) for lockstep checking during bring-up.

## Z80 CPU Context (the thing the JIT will point registers at)

We will lay this out carefully for fast JIT access:

```c
typedef struct {
    uint16_t af, bc, de, hl;   /* main set */
    uint16_t af_, bc_, de_, hl_; /* alternate */
    uint16_t ix, iy, sp, pc;
    uint8_t  i, r;             /* interrupt vector, refresh */
    uint8_t  iff1, iff2, im;   /* interrupt flip-flops */
    /* ... more for DBT: next_pc, ras[], block_cache pointer, mem_base, etc. */
} z80_ctx_t;
```

**Register caching strategy** (the key to BIPS):
- On x86-64: map the 4 main 16-bit pairs + IX/IY/SP into host GPRs (we have 16, it's doable).
- On AArch64: even more GPRs available (X19–X28 callee-saved).
- The accumulator + flags (A+F) is the hottest; keep it in a single host register when possible and materialize F only on demand.

## The Eternal War: Flags

Z80 flags (F register): `S Z 5 H 3 P/V N C`

- Almost every ALU op writes them.
- Many conditional jumps/branches only read a subset.
- Some programs (copy protection, self-modifying code, "undocumented" tricks) rely on bits 3 and 5 (the "XY" flags).

**Lazy flags plan** (critical for 10 BIPS):
- In translated code, most ALU ops only set a "flag descriptor" (what operation was performed + the two operands or result).
- Only when a `PUSH AF`, `LD A,F`, conditional branch that needs a specific flag, or `DAA` is reached do we materialize the real F byte.
- This is the single biggest lever for performance on Z80 DBT.

We will steal ideas from:
- The "lazy flags" in various x86 emulators (Bochs, QEMU TCG, etc.)
- The SLOW-32 experience with complex side effects

## Block Chaining & Control Flow

Same pattern as riscv/dbt:
- Every translated block ends by looking up `cache[ (next_pc >> 2) & MASK ]` (adapted for Z80 byte addressing).
- Direct jump if the target is already hot (chaining).
- Inline cache probe for speed.

Return Address Stack (RAS) for `CALL`/`RET` prediction will be extremely effective for CP/M and typical apps (they are not deeply recursive).

## Special Z80 Intrinsics (cheat codes)

These are worth native host implementations:

- `LDIR` / `LDDR` / `CPIR` / `CPDR` — `memcpy`, `memset`, `memchr` equivalents
- `OTIR` / `OTDR` — block port output (often used for video)
- `DAA` — decimal adjust (can be a small lookup or clever arithmetic)
- `RLD` / `RRD` — nibble rotates used in BCD math

When the DBT sees a tight loop containing only these, we can even replace the entire loop with a single host call.

## Kaypro Personality

Classic Kaypro video:
- 80 columns × 24 rows
- Character generator ROM (we can embed a dump or use CP/M 3.0 "character attributes")
- Escape sequences for cursor, clear, reverse video, etc.
- 4–8 function keys that send specific byte sequences
- Some models had graphics via "bit graphics" or the 6845 CRTC

For the monster we will start with a **high-speed 80×24 cell buffer + host terminal renderer** (or a proper curses / raw + double-buffered thing). Later we can add pixel-accurate 6545 emulation if someone brings real Kaypro ROMs.

## Performance Measurement

The binary will report on exit (with `-s`):

```
Z80 instructions executed:  4,812,394,201
Host time:                   0.87 s
Effective rate:              5.53 BIPS
Blocks translated:           18,442
Block cache hit rate:        99.7%
Lazy flag materializations:  0.4% of ALU ops
```

We will add a live HUD (optional) showing current BIPS in the corner while WordStar is running.

## Apple Silicon Specifics

Exactly like `~/riscv/dbt/dbt.h`:

- Use `MAP_JIT | MAP_ANONYMOUS`
- Bracket every write to the code buffer with `pthread_jit_write_protect_np(0/1)`
- `__builtin___clear_cache` for I-cache

The riscv/dbt already has the portable helpers — we will copy/adapt them.

## Development Workflow

1. Grow the **interpreter** until it can boot CP/M and run a simple .COM (hello, dir, stat, etc.).
2. Add the **DBT skeleton** (block cache + trampoline + one backend) using the riscv/dbt as a direct template.
3. Implement the decoder in a way that both interp and DBT can share (or the DBT has its own fast decoder).
4. Bring up **verification mode** (`-V`) early — this will save us from months of debugging subtle flag bugs.
5. Once basic .COMs work under DBT, start attacking real apps: `WS.COM`, `MBASIC`, `ZORK1.COM`, etc.
6. Profile the hot paths in those apps and add specializations.

## File Naming & Layout (strict)

- `z80_*.c` for core Z80 things
- `dbt_*.c` for the translator
- `cpm_*.c` for the operating system layer
- `kaypro_*.c` for the machine personality
- No "z80emu", "z80core", "fakez80" — we own the name space.

## Known Hard Problems (future us problems)

- Self-modifying code (common in CP/M, especially copy protection and overlays)
- Interrupt-driven keyboard / timer (some Kaypro software uses them)
- Banked memory (CP/M 3.0, GSX graphics, etc.)
- Exact cycle timing (some protection schemes used it; we will lie cheerfully)
- The "undocumented" Z80 instructions that real silicon implements but Zilog never documented

## Success Metric

When you can type `ws` at the `A>` prompt, open a 50-page document, search/replace across it, and the whole experience feels "instant" while the status line says **4.8 BIPS**, we have won.

---

*Now go make the 1980s cry.*
