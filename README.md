# z80 — the 10 BIPS CP/M Monster

> *"WordStar at the speed of light on a virtual Kaypro from the year 2142."*

A ridiculously overpowered Z80 + CP/M execution environment, built for the sheer joy of making 1980s software run at absurd speeds on modern silicon. A 4 MHz Z80 managed roughly 0.5 MIPS. This one does **4.3 billion** Z80 instructions per second on an Apple Silicon laptop — over 8,000× the hardware WordStar was written for.

No good reason. Maximum vibes.

## Measured Performance

All numbers from an M-series MacBook, single core:

| Workload | Rate |
|----------|------|
| MS COBOL 4.65 benchmark (SQUARO, 1.6B insns of real CP/M code) | **4.3 BIPS** |
| zexdoc flag exerciser (5.76B insns, self-modifying-code torture) | **~2.0 BIPS** |
| Same workloads, reference interpreter | ~230 MIPS |

The JIT's interpreter-fallback rate on real workloads is ~0.02% — essentially everything runs as translated native code. Real software runs today: Zork 1, MS COBOL (the compiler *and* its output), and the zexdoc/zexall instruction exercisers pass 67/67 with correct CRCs.

## How It Goes Fast

This is a **dynamic binary translator** (DBT) first, interpreter second. The interpreter is the golden reference; the translator is the monster. The big levers, in the order they landed:

- **Pinned guest registers.** BC, DE, HL, SP, A, and F live permanently in AArch64 callee-saved registers across translated blocks *and* across block-to-block chains. `LD A,B` is one host instruction. `(HL)` accesses need no address load at all. Guest state only touches memory at JIT entry/exit and around the two remaining helper calls (DAA, LDIR/LDDR).
- **Direct block linking.** Every statically-known control-flow edge — fall-through, `JP`, `JR`, `CALL`, and both arms of every conditional — is a patchable branch aimed directly at the target block's native code. A hot loop's back-edge is literally `TST; B.cond; B` into the next translation. Blocks never return to the dispatcher until they must.
- **Superblocks.** Conditional branches don't end translation: the taken arm becomes an out-of-line side exit and the translator keeps going through the fall-through, so straight-line runs cross `JR cc` / `DJNZ` / `RET cc` without paying a block boundary. Length is capped in guest bytes because block span is also the self-modifying-code invalidation window — everything is a trade.
- **Dead-flag elimination.** The Z80 sets flags on nearly every instruction; almost nobody looks at them. A backward liveness pass over each block computes, per instruction and per flag bit, which bits can actually be observed — and the emitters skip the rest. `ADD` before another `ADD` emits no flag code at all; `ADD` before `JR C` emits just the carry. What survives is assembled inline from result-indexed lookup tables plus a few identities (carry-recovery for H, sign-xor for V) — no helper calls, no runtime lazy-flag descriptors, all decided at translation time.
- **Self-modifying-code tracking that doesn't give up.** A per-byte code bitmap makes every guest store check whether it just clobbered translated code (one ADD+LDRB+CBZ on the fast path). zexdoc patches its test instruction millions of times and re-runs it; the invalidation, unlink, and retranslation machinery survives the full run in lockstep with the interpreter.
- **Block-op intrinsics.** LDIR/LDDR run as host-speed copies with the documented overlap semantics and a batched SMC sweep.

### Verification

The correctness story is as unreasonable as the performance story:

- `-V` runs a **shadow interpreter in lockstep**: after every translated block, all registers (including the undocumented XY flags, MEMPTR, and the Q quirk) and all 64 KB of memory are compared. The *entire* 5.76-billion-instruction zexdoc run passes under this.
- zexdoc **and** zexall (undocumented flags included) pass 67/67 under the JIT.
- JIT and interpreter execute bit-identical instruction streams on real workloads — same instruction counts, same output.

## Layout

- `core/` — Z80 decoder, reference interpreter, CPU state
- `dbt/` — the translator: AArch64 backend, block cache, direct-link registry, flag tables, SMC tracking, shadow verify
- `cpm/` — BDOS/BIOS shims, `.COM` loader, host-directory-as-drive-A: mapping
- `kaypro/` — machine personality (early stub; terminal video/keyboard to come)
- `tests/`, `tools/`, `bench/` — generated test programs and the COBOL benchmark harness
- `disks/` — CP/M software used for testing. The freely redistributable zexdoc/zexall exercisers are included; commercial software we test with (Zork 1, MS COBOL 4.65) is git-ignored — drop your own copies into `disks/zork1/` and `disks/mscobol/` to reproduce those results

## Building & Running

Requires an AArch64 host for the JIT (developed on Apple Silicon macOS; the x86-64 backend is a stub that falls back to the interpreter).

```bash
make
./z80-monster -j -s disks/zex/zexdoc.com      # JIT + stats (67 tests, ~3s)
./z80-monster -i prog.com                     # reference interpreter
./z80-monster -V prog.com                     # JIT with lockstep shadow verify
make bench                                    # SQUARO benchmark, jit vs interp
                                              # (needs MS COBOL in disks/mscobol/)
```

The directory containing the `.COM` file becomes drive A:. Console I/O is raw termios with buffered output.

## Status & Road to 10 BIPS

Working today: the full documented + useful-undocumented instruction set split between translator and interpreter fallback, CP/M 2.2 BDOS/BIOS shims sufficient for real applications, SMC, and the verification machinery. Not yet: Kaypro terminal emulation, interrupts, banked memory (CP/M 3), cycle counting (we lie cheerfully).

The general-purpose levers are mostly pulled. What remains is smaller and more surgical: memptr dead-store elision, inline LDIR fast paths, per-entry SMC windows (which would let superblocks grow past their current cap), and eventually per-application specialization — recognizing WordStar's screen loop or dBASE's B-tree walk and cheating accordingly, which was always the endgame the design docs promised. (A hardware-paired return-address stack for `RET` was built, measured a consistent loss — Apple Silicon's indirect-branch predictor already nails the inline cache probe — and reverted; the design notes live in `dbt/dbt_a64.c` so nobody builds it twice.) The target is still a status line that says **10 BIPS** while WordStar search-and-replaces a 50-page document before the keyboard interrupt returns; the road there is now paved with special cases, and we are at peace with that.

See [CLAUDE.md](CLAUDE.md) for the full architecture notes and development history.

## Lineage

The techniques here were honed on two earlier DBTs by the same authors (an RV32IMFD translator and a full-lifting SSA-based one for a custom ISA), then aimed at one of the most irregular 8-bit ISAs ever shipped in volume: four prefix bytes, 700+ opcode variants, and a flag register designed in 1976.

MIT licensed. CP/M software in `disks/` belongs to its respective rights holders.

---

*For no good reason. Maximum disrespect to the clock cycles of the 1980s.*
