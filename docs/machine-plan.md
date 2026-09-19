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
- **The terminal is a cell buffer** — 80×24 attributed cells fed by a
  Kaypro escape interpreter and rendered to the host terminal by a
  minimal-redraw ANSI renderer, with line 25 as the HUD. Not a stream
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
- Software on hand: zexdoc/zexall, MS-COBOL 4.65, Zork 1. No WordStar,
  dBASE, CP/M system files, or Kaypro ROMs — see *Sourcing*.

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

## Phase 1 — the Kaypro terminal

The gate for WordStar. Lives in `kaypro/`; the `cpm/` console shims and
the Phase 2 BIOS both feed it.

### kaypro_video.c — the cell buffer and escape interpreter

- `struct kaypro_cell { uint8_t ch, attr; }`, 24 rows × 80 cols, cursor,
  attribute state, scroll region (whole screen). A dirty-row bitmap for
  the renderer.
- Escape interpreter for the Kaypro '84 set (2X/4'84/10), which is
  ADM-3A plus extensions. First cut, verify each against the Kaypro 10
  user guide's terminal table before relying on it:
  - Controls: `^G` bell, `^H` left, `^J` down/scroll, `^K` up, `^L`
    right, `^M` CR, `^Z` clear screen + home, `^^` (0x1E) home,
    `^W` erase to end of screen, `^X` erase to end of line.
  - `ESC = row col` cursor address (each +0x20), the one every
    application uses.
  - `ESC B n` / `ESC C n` attribute on/off: 0 reverse, 1 reduced
    intensity, 2 blink, 3 underline, 4 cursor on/off, 5 cursor blink,
    6 status-line mode, 7 keyboard click (ignore).
  - `ESC R` delete line, `ESC E` insert line.
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
  changes. Reverse/dim/blink/underline map to SGR 7/2/5/4.
- Flush policy: on every console trap the BDOS/BIOS layer calls
  `kaypro_video_flush_if_due()`, which paints if ≥ ~16 ms have passed
  or if the cursor moved and the output burst ended; plus on the
  host-event flag. Bursts from a 4 BIPS program must not become 4 BIPS
  of terminal writes.
- Line 25 is the HUD: current BIPS (from `insn_count` deltas over the
  flush interval), blocks translated, fallback rate, drive activity.
  `-H` toggles it; it is off when stdout is not a tty.
- Host terminal setup: raw mode is already there; add alternate screen
  on entry/exit, SIGWINCH handling (just repaint; the Kaypro is 80×24
  regardless), and a clean restore on any exit path including crashes.

### kaypro_kbd.c — keyboard

- Host bytes/escape sequences → the bytes a Kaypro keyboard sent.
  Arrows on the '84 keyboard sent `^K ^J ^H ^L`... verify; the numeric
  keypad sent digits; the four function keys are user-programmable, so
  give them a config table defaulting to WordStar-friendly sequences.
  DEL and backspace both become `^H` unless configured otherwise.
- Input queue with a `constat()` that does not touch the host until
  the queue is empty, and **idle detection**: if CONST is polled N
  times with no input, block in `poll()` for a millisecond. WordStar
  spins on CONST between keystrokes; at 4 BIPS that is a full host
  core doing nothing, and the polls would otherwise each cost a
  syscall.

### Headless mode and tests — the reason for the cell buffer

- `--script <file>`: keystrokes with optional `~` delays and
  `@wait-idle` (block until the guest has polled CONST with the queue
  empty), like VCC's harness. `--screen-dump <file>` writes the 24×80
  text (and optionally attributes) on exit or on a script command.
- `tests/screen/`: small `.COM`s that draw with each escape, plus
  expected dumps. Later, WordStar smoke tests: open a file, type,
  search/replace, save, compare the dump and the saved file.
- Make `-V` usable interactively: console input goes through a
  record/replay layer so the shadow CPU replays what the real CPU
  consumed instead of reading the host again. Output from the shadow
  is discarded. This is the same lockstep story extended to the
  machine, and it is what lets us claim WordStar runs verified.

Acceptance: a `.COM` exercising every escape produces the expected
dump; Zork plays in the cell-buffer terminal with the HUD live;
`-V` runs Zork from a script without divergence.

## Phase 2 — real CP/M 2.2

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

| Port | Dir | Meaning |
|------|-----|---------|
| 0x00 | IN  | CONST → 0/FF |
| 0x01 | IN/OUT | CONIN / CONOUT |
| 0x02 | OUT | LIST (goes to a host file or `/dev/null`) |
| 0x03 | IN/OUT | READER / PUNCH (host files, optional) |
| 0x10 | OUT | select drive (A) |
| 0x11 | OUT | track low, 0x12 track high |
| 0x13 | OUT | sector low, 0x14 sector high |
| 0x15 | OUT | DMA low, 0x16 DMA high |
| 0x17 | OUT | 0 = read, 1 = write; result read back on 0x17 |
| 0x20 | IN  | host time (successive reads yield a BCD timestamp, for CP/M 3 later) |
| 0xF0 | OUT | debug/trace byte, `-d` only |

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
  use; The Unofficial CP/M Web Site hosts the archive. We keep only the
  CCP and BDOS sources (and our assembled output); the BIOS is ours.
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
