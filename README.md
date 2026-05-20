# z80 — 10 BIPS CP/M Monster

> *"WordStar at the speed of light on a virtual Kaypro from the year 2142."*

A ridiculously overpowered Z80 + CP/M execution environment, built for the sheer joy of making 1980s software run at absurd speeds on modern silicon.

## The Dream

- Run real CP/M 2.2 / 3.0 software (WordStar, dBASE II, Turbo Pascal, MBASIC, Zork, etc.) at **billions** of Z80 instructions per second.
- Emulate a "Kaypro 5000" (or any classic machine) with perfect terminal feel, but with 10GB/s RAM disks and zero-wait-state everything.
- Use the same dynamic binary translation techniques honed in `~/slow-32/tools/dbt` and `~/riscv/dbt`, but applied to the glorious mess that is the Z80 ISA.
- No good reason. Maximum vibes.

## Architecture

This is a **dynamic binary translator** (DBT) first, interpreter second.

```
guest:  Z80 + CP/M 2.2/3 + Kaypro video/keyboard
host:   x86-64 or AArch64 (Apple Silicon preferred for the numbers)
```

Key components (will grow):

- `core/` — Z80 decoder, interpreter (golden reference), state
- `dbt/`  — the monster: block cache, register caching for AF/BC/DE/HL/IX/IY/SP, flag minimization, block chaining, superblocks, intrinsics for LDIR/LDDR etc.
- `cpm/`  — BDOS + BIOS shims, .COM loader, drive mapping (host dir ↔ drive A:)
- `kaypro/` — 80×24 video, function keys, character set, "Kaypro terminal" escape sequences
- `tools/` — disk image tools, stats, disassembler aids

## Performance Target

| Workload                  | Target (M-series / Zen 5) |
|---------------------------|---------------------------|
| Straight-line Z80 (MOV/ADD/INC loops) | **5–12 BIPS** |
| Typical WordStar editing  | **2–4 BIPS** (with screen updates) |
| dBASE II indexed queries  | **1+ BIPS** |
| vs. qemu-z80 or z80pack  | 30–100× faster |

(These numbers are ridiculous on purpose. We will cheat with every trick: lazy flags, intrinsic block ops, hot-path specialization, RAS for CALL/RET, etc.)

## Status

**Fresh directory.** Clean slate. This is where the monster is born.

See [CLAUDE.md](CLAUDE.md) for architecture notes and development workflow.

## Building & Running (eventual)

```bash
make
./z80-monster -k wordstar.com     # or ./z80-monster disk.img
```

## References & Lineage

- Dynamic binary translation techniques from `~/slow-32/tools/dbt` (full lifting + BURG + SSA) and the lighter `~/riscv/dbt`
- CP/M 2.2/3.0 interface (BDOS function 0–40+, BIOS)
- Classic machines: Kaypro II/4/10, Osborne 1, Morrow, etc.
- The eternal question: "What would WordStar feel like if it had been written for a 3 GHz Z80 with 64 MB of RAM?"

---

*For no good reason. Maximum disrespect to the clock cycles of the 1980s.*
