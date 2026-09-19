# Wrapping the engine in a computer

Status: plan, agreed 2026-09-18. The compute engine is done for now (4.3
BIPS AArch64, 1.8 BIPS x86-64, both lockstep-verified). What is missing
is everything around it: a terminal, an operating system with a prompt,
disks, and the applications the whole thing was built for. This is the
order we do that in, and why.

Decisions already made (don't re-litigate, do push back if implementation
proves them wrong):

- **The OS is real CP/M 2.2** — Digital Research's CCP and BDOS running
  as guest code, on a BIOS we write in Z80 assembly that reaches the host
  through a few `OUT`/`IN` ports. Not a bigger C shim, not full Kaypro
  hardware emulation.
- **The terminal is a cell buffer** — 80×25 attributed cells (24 text
  rows plus the Kaypro 10's status line) fed by a Kaypro escape
  interpreter and rendered to the host terminal by a minimal-redraw ANSI
  renderer, with host line 26 as the HUD. Not a stream
  translator. An SDL window is a second renderer on the same buffer,
  later, if wanted.
- **The existing shim stays** as `-shim` (the default until Phase 2 lands):
  it is what the benchmarks run on and what "just run this .COM" means.

## Where things stand

- `kaypro/` is two placeholder globals. Console output goes straight to
  stdout; CONST is a `select()`.
- The BDOS shim implements ~20 functions on one host directory as A:.
  No user areas, no disk images, one-extent bookkeeping
  (`cpm_disk.c` caps RC at 0x80), none of 24/27–32/35–37. Enough for
  MS-COBOL and Zork; WordStar's overlay loader and dBASE's random-access
  files will find the edges.
- BIOS: 17-entry jump table at `0xF200`; console vectors work, disk
  vectors are stubs. BDOS entry is `0xE400`. That is the standard 62K
  CP/M 2.2 layout (CCP would be at `0xDC00`), which is convenient: the
  real system drops in at the same addresses.
- Core: `HALT` terminates the process. `IN` returns 0xFF, `OUT` is
  dropped, and the ED-prefixed I/O family (`IN r,(C)`, `OUT (C),r`,
  `INI`/`IND`/`OUTI`/`OUTD` and the repeating forms) is not decoded. No
  interrupt injection. The JIT runs until a trap PC; there is no
  periodic exit.
- `-V` (lockstep verify) cannot run interactive programs: real and
  shadow CPUs both execute the console BDOS calls and consume keystrokes
  alternately (see the note in `dbt_run`).
- Software on hand (all in `disks/`, git-ignored): zexdoc/zexall,
  MS-COBOL 4.65, Zork 1, WordStar 3.00 (INSTALLed for ADM-3A: creates,
  edits and saves documents under the shim), dBASE II 2.41 (INSTALLed
  for Kaypro II: creates a database, appends, replaces and displays
  records, `tests/dbase.sh`), Turbo Pascal 3.00A (already
  installed "Kaypro with hilite": reaches its menu), MBASIC 5.2x, BBC
  BASIC, M80/L80. No CP/M system files yet — see *Sourcing*.

## Phase 0 — core prerequisites

Small, independent, all measurable with `-V`.

1. **Decode the ED I/O family** and give the interpreter a port hook:
   `uint8_t (*port_in)(uint8_t port, uint8_t high)` /
   `void (*port_out)(uint8_t port, uint8_t high, uint8_t val)` on the
   cpu (or a static table in `cpm/`). `high` is B (for `(C)` forms) or A
   (for `(n)` forms) — some BIOSes use the full 16-bit port address.
   The translator keeps refusing all port ops so they trap to the
   interpreter; they are rare (BIOS-only) and that keeps the JIT ABI
   untouched.
2. **`HALT` waits instead of exiting.** With no interrupts, `HALT` blocks
   on the console until a key is available, then continues at the next
   instruction (the BIOS will `HALT` in its idle loop later). Keep the
   loud message behind `-d`.
3. **Host-event flag at the dispatcher path.** One byte in the cpu that
   the JIT's exit stub and the `dbt_run` loop test; a host signal
   (SIGALRM, SIGWINCH, SIGINT) sets it. Nothing in translated code
   changes yet. This is enough for the HUD to repaint on a timer for
   programs that do console I/O (they trap constantly). Compute-bound
   programs with no I/O won't repaint the HUD until they exit; when
   that matters, add the VCC-style back-edge check and measure it.

Acceptance: zexdoc/zexall still 67/67 under `-j` and `-V` on both
backends; a test `.COM` that uses `OUT (C),r`, `OTIR`, and `IN A,(n)`
against a scripted port table passes under `-i`, `-j`, `-V`.

Status (2026-09-18, Linux/x86-64): landed. The hooks are
`cpu->port_in` / `cpu->port_out` (with `high` = B or A); the whole ED
family decodes and runs in the interpreter, repeating forms atomically
like LDIR, with the documented final-iteration flags. `cpm/cpm_ports.c`
is the placeholder device (a latch per port, 0xF0 as a `-d` debug
byte) that `tests/ports.com` (`tools/mkports.c`) exercises. `HALT`
blocks in `select()` on stdin via `cpm_console_wait()` — EOF counts as
input so a headless run can't hang. `cpu->host_event` is set by
SIGINT/SIGWINCH/SIGALRM and drained in `dbt_run` and the interp loop
through `cpu->on_host_event`; today's handler makes ^C a clean exit
that restores the terminal. Not yet verified on AArch64.

## Phase 1 — the Kaypro terminal

The gate for WordStar. Lives in `kaypro/`; the `cpm/` console shims and
the Phase 2 BIOS both feed it.

### kaypro_video.c — the cell buffer and escape interpreter

- `struct kaypro_cell { uint8_t ch, attr; }`, 25 rows × 80 cols (row 24
  is the status line, kept out of scrolling while ESC B 7 "status line
  preservation" is on — the default), cursor, attribute state. A
  dirty-row bitmap for the renderer. **Done**, with a host unit test
  (`make test-video`).
- Escape interpreter for the Kaypro '84 set (2X/4'84/10), which is
  ADM-3A plus extensions. Verified against the Kaypro 10 User's Guide
  (pp. 64-69 and 77-78/86-87 of the guide); the table below is what it
  says, with the one discrepancy noted:
  - Controls: `^G` bell, `^H` left, `^J` down/scroll, `^K` up, `^L`
    right, `^M` CR, `^Z` clear screen + home, `^^` (0x1E) home,
    `^W` erase to end of screen, `^X` erase to end of line.
  - `ESC = row col` cursor address (each +0x20), the one every
    application uses.
  - `ESC B n` / `ESC C n` attribute on/off: 0 inverse, 1 reduced
    intensity, 2 blink, 3 underline, 4 cursor on/off, 5 "video mode",
    6 remember cursor (B) / return to it (C), 7 status-line
    preservation on/off.
  - `ESC R` delete line, `ESC E` insert line (guide p. 87, the Sept 1984
    Addendum p. 28, and termcap; guide p. 77 prints them the other way
    round).
  - `ESC A` displays a lower-case alphabet (factory test; no-op here).
  - NUL prints as an accent grave (p. 87). Bytes ≥ 0x80 are 2×4 pixel
    graphics characters (p. 67); stored as-is, rendered later.
  - `ESC * r c` / `ESC space r c` pixel on/off, `ESC L` / `ESC D` line
    draw/erase — the 160×100 "bit graphics". Accept and ignore in the
    first cut; a graphics plane can be added under the text later.
- Anything unrecognised is logged once under `-d` and swallowed. The
  early Kaypro II set is a strict subset, so one interpreter covers
  both; a `-kaypro2` switch can turn the extensions off if an
  application probes for them.

### kaypro_render_tty.c — host terminal renderer

- Diff-based: keep a "last painted" copy; on each flush emit cursor
  moves and only the changed cells, coalescing runs and attribute
  changes. Reverse/dim/blink/underline map to SGR 7/2/5/4. **Done**
  (`kaypro/kaypro_render_tty.c`, `make test-render`); graphics blocks
  render as braille.
- Flush policy: on every console trap the BDOS/BIOS layer calls
  `kaypro_video_flush_if_due()`, which paints if ≥ ~16 ms have passed
  or if the cursor moved and the output burst ended; plus on the
  host-event flag. Bursts from a 4 BIPS program must not become 4 BIPS
  of terminal writes.
- Host line 26 is the HUD (the Kaypro's own line 25 is its status
  line): BIPS over the last quarter second, blocks translated, fallback
  rate, SMC invalidations. **Done**: a 4 Hz SIGALRM through Phase 0's
  host-event flag; `--no-hud` turns it off; it needs a 26-row terminal.
  A pure compute loop that never traps shows its last figure until it
  does (the back-edge check is the known fix, unmeasured).
- Host terminal setup: raw mode is already there; add alternate screen
  on entry/exit, SIGWINCH handling (just repaint; the Kaypro is 80×24
  regardless), and a clean restore on any exit path including crashes.

### kaypro_kbd.c — keyboard

- Host escape sequences → the bytes a Kaypro keyboard sent. **Done.**
  The guide gives no factory table (CONFIG made every arrow and keypad
  key user-definable), so the defaults are the ADM-3A cursor controls
  `^K ^J ^H ^L`, Backspace (0x7F or 0x08 from the host) is the Kaypro
  BACKSPACE `^H`, the Delete key is DEL; `Z80_KEYS=wordstar` gives
  `^E ^X ^S ^D`, PgUp/PgDn `^R ^C`, Backspace = DEL (delete left),
  Delete = `^G`; `Z80_KEYS="f1=\^KD,up=..."` sets any key. Unmapped
  keys are swallowed whole. `make test-kbd`; verified through a pty
  against WordStar's cursor.
- Idle detection: after 20000 consecutive quiet polls (no console
  output between them) each poll sleeps a millisecond. The threshold
  sits above programs' own silent delay loops (WordStar paces "NEW
  FILE" with ~16500 polls); at 64 those 1-second delays became 16
  seconds. **Done.**

### Headless mode and tests — the reason for the cell buffer

- `--script <file>`: keystrokes with `~` / `@wait-idle` (block until
  the guest has polled CONST N times with the queue empty, or blocked
  in a read), `@dump`, `@sleep`, like VCC's harness; when the script
  runs out and the guest asks again, the session ends.
  `--screen-dump <file>` writes the 25×80 text on exit. **Done**
  (`kaypro/kaypro_kbd.c`, which is now the single input path for CONST/
  CONIN/HALT, host terminal or script, with the idle-poll sleep).
- `tests/scripts/` + `tests/wordstar.sh` / `tests/zork.sh` (`make
  test-apps`, skipped when the software is absent): WordStar creates a
  document, types, saves; the editor status line, the No-File menu
  afterwards and the host file's bytes are checked. **Done.** Still to
  come: `.COM`s that draw with each escape, and search/replace.
- Make `-V` usable interactively. **Done**, and more simply than
  record/replay: a host service (BDOS/BIOS trap, port I/O) is an
  interpreter-fallback step, and after one the shadow is re-synced from
  the real CPU instead of stepped, so every console byte is consumed
  once and every disk operation happens once. The shadow runs with
  `defer_traps` so it stops *at* a trap address the way a translated
  block does. `Z80_VERIFY_STRICT=1` makes every block return to the
  dispatcher (no links, no inline probe, and on x86-64 no RAS pairing)
  so a divergence is localised to one block — that is how the overlay
  bug below was found in minutes. Both backends have it. The WordStar
  and dBASE sessions verify clean under it on both.
- **Scripts need pacing for programs that purge type-ahead.** dBASE II
  reads a key, then drains and discards everything else queued before
  echoing — at human speed invisible, from a script it ate all but the
  first byte of every line. `@pace N` withholds the next key until the
  guest has polled empty N times since the last read.
- **I/O through a closed FCB is normal CP/M.** CLOSE only flushes the
  directory; dBASE creates a file, closes it, and USEs it through the
  same FCB with no re-open. The shim re-opens implicitly by the FCB's
  name when I/O arrives on an FCB with no live slot. The Phase 2 BDOS
  is DRI's, so this class of shim bug ends there.
- **Host writes must invalidate translated code.** BDOS reads land in
  the DMA buffer behind the JIT's store check; WordStar reloads overlay
  segments into the same addresses, and a translated block for the old
  overlay survived — `z80_mem_host_wrote()` now runs the SMC
  invalidation for every host write into guest memory (disk reads,
  directory entries, the line editor). The real BIOS in Phase 2 will
  write sectors the same way and needs the same call.

Acceptance: a `.COM` exercising every escape produces the expected
dump; Zork plays in the cell-buffer terminal with the HUD live;
`-V` runs Zork from a script without divergence.

## Phase 2 — real CP/M 2.2

**Status (2026-09-18, late): landed.** `make system` assembles DRI's CCP
and BDOS (vendored from github.com/brouhaha/cpm22 into `cpm/cpm22/`,
byte-exact) and our `bios.asm` with Macro Assembler AS
(`tools/get-asl.sh` builds it; `cpm/cpm22/system.bin` is checked in).
`./z80-monster -A disk.dsk` boots to `A>`; `tools/mkdsk` makes, fills,
lists and extracts the two image formats in `tools/diskdefs` (cpmtools
reads them with `-T raw`). Host ports are `E0-EF` as tabled below,
implemented in `cpm/cpm_host.c`; while `cpm_traps_enabled` is 0 the
interpreter and translators treat 0000/0005/the BIOS vectors as code
and `PC=0` is a warm boot. Verified: `tests/cpm22.sh` (boot, DIR, run
two programs) under `-j`, `-i` and strict `-V`; WordStar creates,
saves and TYPEs a document from the native CCP; MS-COBOL compiles and
links SQUARO under the native BDOS. `^]` leaves an interactive session.
Still to do here: real system tracks on the image (the loader takes
`system.bin` from the host), `--list` to a printer file is wired but
untested, CP/M 3 is out of scope as planned.

### Layout and pieces

- Memory: 62K system, exactly what the shim already assumes —
  CCP `DC00`, BDOS `E400` (entry `E406`), BIOS `F200`. Page zero as
  CP/M sets it: `JP F203` at 0, IOBYTE at 3, drive/user at 4,
  `JP E406` at 5. DRI's sources assemble at any base; we keep 62K so
  the TPA matches the Kaypro's.
- `cpm/cpm22/`: CCP and BDOS sources (DRI, 8080 mnemonics) plus a
  build via an assembler we can run — either a small 8080/Z80
  assembler in `tools/` or, once the machine works, ASM.COM/MAC.COM on
  the machine itself. Check in the assembled binaries with their
  hashes so a fresh clone boots without the toolchain.
- `cpm/bios/bios.z80`: our BIOS. Standard 17 vectors, all disk state
  in the BIOS, DPH/DPB per drive, a 128-byte sector deblocking-free
  design (host does 128-byte sectors directly, no need for blocking).
  Every host interaction is an `OUT` to the port map below; the
  interpreter's port hook (Phase 0) dispatches into `cpm/cpm_host.c`.

### Port map (proposal; the BIOS and `cpm_host.c` are the two users)

The real Kaypro's ports are documented in the 1984 Addendum pp. 24-27
(keyboard SIO 0x05/0x07, serial 0x00-0x0F, FDC 1793 at 0x10-0x13,
system latch 0x14-0x17, printer 0x18-0x1B, 6545 CRTC 0x1C/0x1D, RTC
0x20-0x24, hard disk WD1002 at 0x80-0x87). Kaypro utilities (CONFIG,
MFDISK, the "video mode" graphics) poke some of those directly, so our
host ports stay clear of them, up in 0xE0-0xEF where no Kaypro model
had anything:

| Port | Dir | Meaning |
|------|-----|---------|
| 0xE0 | IN  | CONST → 0/FF |
| 0xE1 | IN/OUT | CONIN / CONOUT |
| 0xE2 | OUT | LIST (goes to a host file or `/dev/null`) |
| 0xE3 | IN/OUT | READER / PUNCH (host files, optional) |
| 0xE8 | OUT | select drive (A) |
| 0xE9 | OUT | track low, 0xEA track high |
| 0xEB | OUT | sector low, 0xEC sector high |
| 0xED | OUT | DMA low, 0xEE DMA high |
| 0xEF | OUT | 0 = read, 1 = write; result read back on 0xEF |
| 0xE4 | IN  | host time (successive reads yield a BCD timestamp, for CP/M 3 later) |
| 0xE7 | OUT | debug/trace byte, `-d` only |

Real Kaypro ports that software touches anyway get a stub in
`cpm_host.c` that logs under `-d` and returns 0xFF / drops the write,
which is what the interpreter does today for everything.

Blocked `CONIN`, when the queue is empty, blocks the host thread — no
guest spinning at all.

### Disks

- `.DSK` images in cpmtools formats so `cpmls`/`cpmcp` work on them
  out of the box. Two diskdefs shipped in `tools/diskdefs`: a floppy
  (Kaypro '84 DSDD, 40 tracks × 2 × 10 sectors × 512 B ≈ 390K, the
  common Kaypro format) and a hard disk (`z80-monster-hd`, 8 MB,
  1024-B blocks, 1024 directory entries) for the applications.
- `tools/mkdsk`: build an image from a host directory (and extract
  back). Not a substitute for cpmtools, just enough that
  `make disks` works without them.
- Drive mapping on the command line: `-A path.dsk -B path.dsk` (up to
  P:). `-shim` keeps the host-directory behaviour.
- Later, if wanted: a "host directory as a drive" done at the BIOS
  level by synthesising a directory in the image on the fly. RunCPM
  does this in the BDOS; doing it under a real BDOS is fiddlier and
  not needed for the goal.

### Boot

- `z80-monster` with no `.COM` argument and at least one drive boots:
  load CCP+BDOS+BIOS from `cpm/cpm22/system.bin` into `DC00..`, set
  page zero, jump to BIOS cold boot. The CCP prints `A>`. `-shim`
  remains for direct `.COM` execution and the benchmark scripts.
- CP/M 3 and banked memory are explicitly out of scope for this phase;
  the port map leaves room.

Acceptance: boots to `A>`; `DIR`, `TYPE`, `PIP`, `STAT`, `SUBMIT`
work; MS-COBOL compiles and links SQUARO from an image and the
resulting `.COM` runs at the same BIPS as under the shim; `-V` runs the
whole boot+compile+link in lockstep.

## Phase 3 — applications and the 10 BIPS road

**Baseline (2026-09-18, late).** `bench/wsreplace.sh` is the README's
target workload made repeatable: WordStar 3.00 opens a 50-page document
on a hard-disk image under the native CCP/BDOS, replaces every "kaypro"
with "monster" (818 of them), saves, exits. 293 M instructions.
First measurement 2.84 BIPS with a quarter of the wall clock in ~905 K
console port traps; with `IN A,(n)` / `OUT (n),A` translated as direct
helper calls (AArch64; off under `-V` so the shadow re-sync stays
exact; `Z80_NO_INLINE_PORTS=1` for A/B) there are no fallbacks at all
and it runs at 3.17 BIPS. Native SQUARO is at parity with the shim.

The interpreter profiler (`Z80_PROFILE=1 -i`) then showed 22% of all
instructions in one two-instruction loop: WordStar's timing tick, `DEC
A; JP NZ` spun 256 times, 124 600 times per replace. Countdown loops
(`DEC r; JP NZ/JR NZ,self`, `DJNZ $`) are now translated in closed form
with the skipped instructions added to the count (tests/loop.com under
-i/-j/-V agrees to the instruction): **4.6 BIPS**. The 8080-style
byte-copy and fill loops (`LD A,(HL); LD (DE),A; INC HL; INC DE; DEC r;
JP NZ` and `LD (HL),A; INC HL; DEC r; JP NZ`) are folded the same way
through helpers with the LDIR-style SMC sweep — exact, but only ~4% of
this workload. What remains is the filtered-copy search loop at
5771-57A1 (~7.5%, early exits on control characters — app-specific)
and a long diffuse tail: WordStar's display refresh, the DRI BDOS, line
management. `Z80_PROFILE=1 -i` and `-d`'s `[fold]` lines are the tools.
The whole replace session verifies under strict -V; zexdoc -V still
passes. x86-64 needs the three translator additions (ports, countdown,
copy/fill) mirrored.

- WordStar 3.3 with the Kaypro terminal definition (or WS 4 installed
  for Kaypro), dBASE II 2.41, Turbo Pascal 3.0 (has a Kaypro install),
  MBASIC 5.21, and whatever else people bring. Each gets a scripted
  headless smoke test.
- Then the profiling: `Z80_JIT_DUMP` plus `perf`/Instruments on
  WordStar's screen refresh and search loop and dBASE's index walk,
  and the per-application specialisation the design docs promised.
  The target is unchanged: a status line reading **10 BIPS** while
  WordStar search-and-replaces a 50-page document.

## Sourcing (nothing here is in the repo; `disks/` stays git-ignored)

- **CP/M 2.2 CCP/BDOS source and binaries**: Digital Research's, via
  the successor rights holder's 2022 statement permitting unrestricted
  use. Start from https://github.com/brouhaha/cpm22 (the DRI sources
  cleaned up to assemble with modern tools); The Unofficial CP/M Web
  Site has the original archive. We keep only the CCP and BDOS sources
  (and our assembled output); the BIOS is ours.
- **Kaypro 10 User's Guide + September 1984 Addendum**: in hand
  (~/Downloads). The Addendum's p. 28 video protocol and pp. 24-27 port
  map are the references the terminal and the port plan cite.
- **WordStar 3.3 / 4 for CP/M, dBASE II, Turbo Pascal 3**: still
  commercial in the strict sense; bring your own copies, as with
  MS-COBOL and Zork today. The same archive and the Internet Archive
  have them.
- **Kaypro ROMs**: not needed. Our BIOS replaces the ROM; we only
  borrow the terminal's escape set and the disk format.
- **cpmtools**: optional on the host; `brew install cpmtools` /
  `apt install cpmtools` for people who want to poke at images.

## Not doing (yet), and why

- Interrupts: Kaypro-era CP/M software polls. The Phase 0 event flag
  plus a back-edge check (measured before it lands, as everything is)
  covers the timer case when an application needs it.
- Cycle accuracy: still lying cheerfully.
- 6545 pixel-accurate video, SDL window: second renderer on the Phase 1
  buffer, when someone wants the 1983 look more than they want ssh.
- CP/M 3, MP/M, banked memory: after WordStar and dBASE run.
