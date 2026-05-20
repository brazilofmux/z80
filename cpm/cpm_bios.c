/* cpm_bios.c — Minimal but functional CP/M 2.2 BIOS implementation
 *
 * We install a standard 17-entry jump table at CPM_BIOS_BASE.
 * The interpreter traps when execution reaches the trampoline area
 * and dispatches to the real host functions (console, etc.).
 *
 * This is critical for real programs (WordStar, Turbo Pascal, etc.)
 * that bypass BDOS and call the BIOS vectors directly for speed.
 */

#include "cpm.h"
#include "../core/z80.h"
#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/time.h>

/* Forward decls for console helpers (we can share logic with BDOS later) */
static int  bios_const(z80_cpu_t *cpu);
static int  bios_conin(z80_cpu_t *cpu);
static void bios_conout(z80_cpu_t *cpu, uint8_t ch);

/* Install the standard CP/M BIOS jump table + trampolines.
 * Each entry is a JP to CPM_BIOS_BASE + 0x100 + (func * 4)
 * We use a small trampoline area so the interpreter can easily detect
 * which function was called by looking at PC.
 */
void cpm_install_bios(z80_cpu_t *cpu) {
    if (!cpu->mem) return;

    uint16_t base = CPM_BIOS_BASE;
    uint16_t tramp = base + 0x100;   /* trampoline area */

    /* Write the 17 jump vectors (standard layout) */
    for (int i = 0; i < CPM_BIOS_COUNT; i++) {
        uint16_t addr = base + i * 3;
        cpu->mem[addr]     = 0xC3;                    /* JP */
        cpu->mem[addr + 1] = (tramp + i * 4) & 0xFF;
        cpu->mem[addr + 2] = (tramp + i * 4) >> 8;
    }

    /* Write tiny trampolines:
     *   LD A, func
     *   JP   trampoline_dispatch
     *
     * The interpreter will detect PC in the trampoline range and
     * treat the byte after the LD A as the function number.
     */
    for (int i = 0; i < CPM_BIOS_COUNT; i++) {
        uint16_t t = tramp + i * 4;
        cpu->mem[t]     = 0x3E;          /* LD A, imm */
        cpu->mem[t + 1] = i;             /* function number */
        cpu->mem[t + 2] = 0xC3;          /* JP */
        cpu->mem[t + 3] = (tramp + 0x100) & 0xFF; /* common dispatcher */
        cpu->mem[t + 4] = (tramp + 0x100) >> 8;   /* (we'll reuse the same page) */
    }

    /* Common dispatcher stub at tramp + 0x100
     * We don't actually execute it — the interpreter intercepts before.
     */
    /* Mark the BIOS area so we know it's installed */
    cpu->mem[base + 0x50] = 'B';
    cpu->mem[base + 0x51] = 'I';
    cpu->mem[base + 0x52] = 'O';
    cpu->mem[base + 0x53] = 'S';
}

/* Called from the interpreter when PC is in the BIOS trampoline area.
 * The byte at PC+1 after the LD A,imm tells us the function.
 */
int cpm_bios_dispatch(z80_cpu_t *cpu) {
    /* The trampoline puts the function number in A right before the JP */
    uint8_t func = cpu->a;

    switch (func) {
    case BIOS_WBOOT:
    case BIOS_BOOT:
        return 0;   /* terminate like BDOS 0 */

    case BIOS_CONST:
        cpu->a = bios_const(cpu) ? 0xFF : 0;
        return 1;

    case BIOS_CONIN:
        cpu->a = bios_conin(cpu);
        return 1;

    case BIOS_CONOUT:
        /* Character is in C for many real BIOS implementations */
        bios_conout(cpu, cpu->c);
        return 1;

    default:
        /* For now, silently ignore disk/others — real programs will
         * hit this and we can add them as needed. */
        return 1;
    }
}

/* ======================================================================
 * Real Console Implementation (used by both BDOS and BIOS)
 * ===================================================================== */

/* CONOUT — write one character (used by BDOS 2 and BIOS CONOUT) */
void cpm_conout(uint8_t ch) {
    putchar(ch);
    fflush(stdout);
}

/* CONIN — blocking read of one character.
 * With raw mode + VMIN=1 this works nicely. */
uint8_t cpm_conin(void) {
    unsigned char ch;
    ssize_t n = read(STDIN_FILENO, &ch, 1);
    if (n <= 0) {
        return 0x1A;   /* CP/M EOF / ^Z */
    }
    return ch;
}

/* CONST — console status (non-blocking "is a key available?").
 * Returns 0xFF if ready, 0 if not. */
uint8_t cpm_constat(void) {
    fd_set rfds;
    struct timeval tv = {0, 0};   /* zero timeout = poll */

    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);

    int ret = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
    if (ret > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
        return 0xFF;   /* character ready */
    }
    return 0;
}

/* --- BIOS wrappers (these are what the BIOS dispatch calls) --- */

static int bios_const(z80_cpu_t *cpu) {
    (void)cpu;
    return cpm_constat() == 0xFF ? 1 : 0;
}

static int bios_conin(z80_cpu_t *cpu) {
    (void)cpu;
    return cpm_conin();
}

static void bios_conout(z80_cpu_t *cpu, uint8_t ch) {
    (void)cpu;
    cpm_conout(ch);
}
