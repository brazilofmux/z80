# z80 — the 10 BIPS CP/M Monster

> *"WordStar at the speed of light on a virtual Kaypro from the year 2142."*

A ridiculously overpowered Z80 + CP/M execution environment, built for the sheer joy of making 1980s software run at absurd speeds on modern silicon. A 4 MHz Z80 managed roughly 0.5 MIPS. This one does **4.3 billion** Z80 instructions per second on an Apple Silicon laptop — over 8,000× the hardware WordStar was written for.

No good reason. Maximum vibes.

## Measured Performance

Single core, JIT unless noted:

| Workload | M5 Max MacBook | Raspberry Pi 4 |
|----------|----------------|----------------|
| MS COBOL 4.65 benchmark (SQUARO, 1.6B insns of real CP/M code) | **4.3 BIPS** | **532 MIPS** |
| zexdoc flag exerciser (5.76B insns, self-modifying-code torture) | **~3.3 BIPS** | **0.47 BIPS** |
| Same workloads, reference interpreter | ~230 MIPS | ~24 MIPS |

The Pi 4 (Cortex-A72 @ 1.5 GHz, Debian 11, GCC 10, Linux/aarch64) built from a clean clone with no source changes; zexdoc and zexall pass 67/67 under the JIT, and the full zexdoc run passes under `-V` lockstep verification in six minutes. No Mac required.

The JIT's interpreter-fallback rate on real workloads is ~0.02% — essentially everything runs as translated native code. Real software runs today: Zork 1, MS COBOL (the compiler *and* its output), and the zexdoc/zexall instruction exercisers pass 67/67 with correct CRCs.

## How It Goes Fast

This is a **dynamic binary translator** (DBT) first, interpreter second. The interpreter is the golden reference; the translator is the monster. The big levers, in the order they landed:

- **Pinned guest registers.** BC, DE, HL, SP, A, and F live permanently in AArch64 callee-saved registers across translated blocks *and* across block-to-block chains. `LD A,B` is one host instruction. `(HL)` accesses need no address load at all. Guest state only touches memory at JIT entry/exit and around the two remaining helper calls (DAA, LDIR/LDDR).
- **Direct block linking.** Every statically-known control-flow edge — fall-through, `JP`, `JR`, `CALL`, and both arms of every conditional — is a patchable branch aimed directly at the target block's native code. A hot loop's back-edge is literally `TST; B.cond; B` into the next translation. Blocks never return to the dispatcher until they must.
- **Superblocks.** Conditional branches don't end translation: the taken arm becomes an out-of-line side exit and the translator keeps going through the fall-through, so straight-line runs cross `JR cc` / `DJNZ` / `RET cc` without paying a block boundary. Length is capped in guest bytes because block span is also the self-modifying-code invalidation window — everything is a trade.
- **Dead-flag elimination.** The Z80 sets flags on nearly every instruction; almost nobody looks at them. A backward liveness pass over each block computes, per instruction and per flag bit, which bits can actually be observed — and the emitters skip the rest. `ADD` before another `ADD` emits no flag code at all; `ADD` before `JR C` emits just the carry. What survives is assembled inline from result-indexed lookup tables plus a few identities (carry-recovery for H, sign-xor for V) — no helper calls, no runtime lazy-flag descriptors, all decided at translation time.
- **Self-modifying-code tracking that doesn't give up.** A per-byte code bitmap makes every guest store check whether it just clobbered translated code (one ADD+LDRB+CBZ on the fast path). And because the block cache maps guest PCs 1:1, each entry records its block's exact byte span, so an invalidating store kills only the blocks that truly cover the written byte — zexdoc patches its test instruction 7.4 million times and re-runs it, and the invalidation, unlink, and retranslation machinery survives the full run in lockstep with the interpreter.
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
./z80-monster -j -s disks/zex/zexdoc.com      # JIT + stats (67 tests, ~2s)
./z80-monster -i prog.com                     # reference interpreter
./z80-monster -V prog.com                     # JIT with lockstep shadow verify
make bench                                    # SQUARO benchmark, jit vs interp
                                              # (needs MS COBOL in disks/mscobol/)
```

The directory containing the `.COM` file becomes drive A:. Console I/O is raw termios with buffered output.

## Status & Road to 10 BIPS

Working today: the full documented + useful-undocumented instruction set split between translator and interpreter fallback, CP/M 2.2 BDOS/BIOS shims sufficient for real applications, SMC, and the verification machinery. Not yet: Kaypro terminal emulation, interrupts, banked memory (CP/M 3), cycle counting (we lie cheerfully).

The general-purpose levers are now *all* pulled. Memptr dead-store elision and per-entry SMC windows landed (the latter took zexdoc from 2.0 to 3.3 BIPS and cut the full lockstep-verify run from four minutes to 46 seconds); the ones that didn't survive measurement are documented at the scene so nobody builds them twice — a hardware-paired return-address stack for `RET` (consistent loss; Apple Silicon's indirect-branch predictor already nails the inline cache probe), sinking the per-exit q/count bookkeeping across chained edges (2–3% ceiling, and it costs the lockstep-verify invariant), longer superblocks (the cap barely binds; blocks end at natural control flow first), and inline LDIR fast paths (our current workloads execute almost no LDIRs — that one waits for WordStar). What remains is the endgame the design docs promised from day one: per-application specialization — recognizing WordStar's screen loop or dBASE's B-tree walk and cheating accordingly. The target is still a status line that says **10 BIPS** while WordStar search-and-replaces a 50-page document before the keyboard interrupt returns; the road there is now paved with special cases, and we are at peace with that.

## Gotchas

Someone asked whether buying an Apple Silicon laptop and loading this gets you "the soothing green text of a CP/M prompt, running eight thousand times faster, no gotchas?" No. There are gotchas, and they are the fun kind:

1. **It doesn't boot CP/M.** There is no `A>` prompt and no CCP. You run one `.COM` from the host shell, and the BDOS/BIOS underneath it is a shim written in C that maps a host directory to drive A: and talks to your terminal. It's a program runner, not a machine. Booting real system images is on the list, after terminal emulation.
2. **You can't experience 8,000× at a prompt anyway.** A prompt waits for a human at any speed. The speed shows in compute: MS-COBOL 4.65 compiles and links a program before the terminal finishes repainting. Anything that needs real terminal emulation — WordStar, dBASE — doesn't run yet, because the Kaypro screen/keyboard personality is a stub and there are no interrupts. Programs that only need BDOS console I/O (Zork) run fine.
3. **Apple isn't required, but ARM64 is — for now.** The translator emits AArch64. The only macOS-specific bit is the W^X page-flipping dance Apple requires for JITs, and it's behind an `#ifdef` in `dbt/dbt.h`. Linux/aarch64 is confirmed (Raspberry Pi 4, numbers above); FreeBSD/aarch64 should build and JIT but nobody has tried, and reports are welcome. On x86-64 there's no backend yet — and that's a *yet*: the authors' other DBTs (RISC-V, a custom ISA, and a 6809 whole-machine emulator) all have first-class AMD64 backends. This one was designed on AArch64's 31 GPRs first, and porting the register-pinning scheme to x86-64's smaller callee-saved set is a triage problem, not a research problem. Until then, x86-64 falls back to the reference interpreter at ~230 MIPS — still ~460× a 4 MHz Z80.
4. **4.3 BIPS was measured on an M5 Max**, not a base-model chip. It's single-threaded, so the gap on a smaller M-series part won't be dramatic, but no number is quoted here that wasn't measured.
5. **There is no good reason for any of this.** It's a dynamic binary translator research toy: the same techniques used on RISC-V and a custom ISA, pointed at the most irregular 8-bit ISA ever shipped in volume, to see how far static dead-flag elimination and self-modifying-code tracking can be pushed. The design document is the phrase "10 BIPS CP/M monster for no good reason."

For a sense of what a whole computer costs: the same authors' fork of VCC (a CoCo 3 emulator — 6809/6309, GIME, PIAs, disk controller, banked memory, the works) uses the same JIT techniques and does ~0.9 BIPS on the same laptop (measured, not estimated — see its README), with a working prompt, interrupts, and an x86-64 backend. Every few instructions the 6809 wants to talk to a chip that has opinions. The Z80 number is what you get when nothing does; bolting a Kaypro onto it will cost some of that, and the number will be reported honestly when it does.

If you want an actual green prompt at silly speed on FreeBSD *today*, that's RunCPM or z80pack on a fast x86 — tens of MIPS, a hundred times faster than the real thing, and it comes with a CCP.

See [CLAUDE.md](CLAUDE.md) for the full architecture notes and development history.

## Lineage

The techniques here were honed on two earlier DBTs by the same authors (an RV32IMFD translator and a full-lifting SSA-based one for a custom ISA), then aimed at one of the most irregular 8-bit ISAs ever shipped in volume: four prefix bytes, 700+ opcode variants, and a flag register designed in 1976.

MIT licensed. CP/M software in `disks/` belongs to its respective rights holders.

---

*For no good reason. Maximum disrespect to the clock cycles of the 1980s.*
