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
#include <string.h>

/* Simple input queue to make CONST + CONIN / BDOS 6 (0xFF) polling reliable.
 * Prevents races where a character arrives between the poll and the read. */
#define INPUT_QUEUE_SIZE 32
static unsigned char input_queue[INPUT_QUEUE_SIZE];
static int q_head = 0;
static int q_tail = 0;

static int queue_has_data(void) {
    return q_head != q_tail;
}

static void queue_push(unsigned char ch) {
    int next = (q_head + 1) % INPUT_QUEUE_SIZE;
    if (next != q_tail) {           /* drop oldest if full (rare) */
        input_queue[q_head] = ch;
        q_head = next;
    }
}

static unsigned char queue_pop(void) {
    if (q_head == q_tail) return 0;
    unsigned char ch = input_queue[q_tail];
    q_tail = (q_tail + 1) % INPUT_QUEUE_SIZE;
    return ch;
}

/* Forward decls for console helpers (we can share logic with BDOS later) */
/* (BIOS wrappers removed — we now call the unified cpm_* functions directly) */

/* Install the standard CP/M 2.2 BIOS jump table.
 *
 * Layout at BIOS_BASE:
 *    +0  JP BOOT
 *    +3  JP WBOOT
 *    +6  JP CONST
 *    ...
 *
 * The targets are inside the BIOS area itself. The interpreter
 * special-cases any PC that lands inside the vector table range
 * and dispatches based on the offset.
 */
void cpm_install_bios(z80_cpu_t *cpu) {
    if (!cpu->mem) return;

    uint16_t base = CPM_BIOS_BASE;

    /* Write the 17 standard 3-byte JP vectors.
     * Each JP points to a unique address inside the BIOS area
     * (base + 0x30 + i*3). The interpreter will catch execution
     * in this range and map it back to the function index.
     */
    for (int i = 0; i < CPM_BIOS_COUNT; i++) {
        uint16_t vec   = base + i * 3;
        uint16_t dest  = base + 0x30 + i * 3;   /* unique destination per function */

        cpu->mem[vec]     = 0xC3;               /* JP */
        cpu->mem[vec + 1] = dest & 0xFF;
        cpu->mem[vec + 2] = dest >> 8;
    }

    /* Also write the classic warm-boot vector at 0001 (points to WBOOT) */
    cpu->mem[0x0001] = (base + 3) & 0xFF;
    cpu->mem[0x0002] = (base + 3) >> 8;

    /* Mark the BIOS area */
    cpu->mem[base + 0x00] = 'B';
    cpu->mem[base + 0x01] = 'I';
    cpu->mem[base + 0x02] = 'O';
    cpu->mem[base + 0x03] = 'S';
}

/* Called from the interpreter when execution reaches a BIOS vector
 * (either by CALL/JP through the table at BIOS_BASE or by calculating
 * the address from (0001) and jumping).
 *
 * We detect the call by PC being inside the vector table or the
 * "call gate" area (base + 0x30 .. base + 0x30 + 0x33).
 */
int cpm_bios_dispatch(z80_cpu_t *cpu) {
    uint16_t base = CPM_BIOS_BASE;
    uint16_t pc   = cpu->pc;

    int func = -1;

    /* Case 1: PC is directly at one of the vector slots (base + n*3) */
    if (pc >= base && pc < base + CPM_BIOS_COUNT * 3) {
        func = (pc - base) / 3;
    }
    /* Case 2: PC is in the call-gate area we pointed the vectors at */
    else if (pc >= base + 0x30 && pc < base + 0x30 + CPM_BIOS_COUNT * 3) {
        func = (pc - (base + 0x30)) / 3;
    }

    if (func < 0 || func >= CPM_BIOS_COUNT) {
        /* Unexpected entry — treat as no-op for now */
        return 1;
    }

    switch (func) {
    case BIOS_WBOOT:
    case BIOS_BOOT:
        return 0;                       /* clean termination */

    case BIOS_CONST:
        cpu->a = cpm_constat();         /* already returns 0 or 0xFF */
        return 1;

    case BIOS_CONIN:
        cpu->a = cpm_conin();
        return 1;

    case BIOS_CONOUT:
        /* Many real BIOSes expect the char in C when the vector is called */
        cpm_conout(cpu->c);
        return 1;

    default:
        /* Stub for disk, list, etc. — return success so programs continue */
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
    /* Drain any pre-fetched characters first (makes polling + CONIN reliable) */
    if (queue_has_data()) {
        return queue_pop();
    }

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
    /* First, anything already queued? */
    if (queue_has_data()) {
        return 0xFF;
    }

    fd_set rfds;
    struct timeval tv = {0, 0};

    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);

    int ret = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
    if (ret > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
        /* Pre-read the character into the queue so the next CONIN gets it
         * without another syscall/race. */
        unsigned char ch;
        ssize_t n = read(STDIN_FILENO, &ch, 1);
        if (n > 0) {
            queue_push(ch);
            return 0xFF;
        }
    }

    /* Small host delay when no input is available.
     * This prevents tight guest polling loops (very common in CP/M loaders
     * and games) from spamming output like "Load Game Disk..." thousands
     * of times per second. 1ms is usually enough to make it feel normal
     * while still being very responsive. */
    usleep(1000);   /* 1ms yield */

    return 0;
}

/* --- BIOS wrappers (these are what the BIOS dispatch calls) --- */

/* BIOS entry points now call the unified cpm_* functions directly in the dispatch */

/* ========================================================================
 * BDOS 10 - Read Console Buffer (with line editing)
 * Buffer at DE:
 *   [0] = max chars (input)
 *   [1] = chars actually read (output)
 *   [2..] = the line (CR not stored)
 * ======================================================================== */
void cpm_read_console_buffer(z80_cpu_t *cpu, uint16_t de) {
    uint8_t *buf = &cpu->mem[de];
    uint8_t max = buf[0];
    if (max == 0) {
        buf[1] = 0;
        cpu->a = 0;
        return;
    }

    /* Aggressively clean the default low-memory buffer area.
     * The Zork loader (and many other Infocom titles) do their initial
     * header verification random reads into the default DMA at 0x80.
     * This leaves story file data sitting in what the game later uses
     * for its command input buffer. The result is exactly the "flood
     * of �" right after the first `>` prompt.
     *
     * Zeroing here gives the game a clean slate for the first command
     * without affecting later, well-behaved DMA usage.
     */
    memset(&cpu->mem[0x80], 0, 128);

    uint8_t len = 0;
    buf[1] = 0;

    for (;;) {
        uint8_t ch = cpm_conin();

        if (ch == '\r' || ch == '\n') {
            cpm_conout('\r');
            cpm_conout('\n');
            break;
        } else if (ch == 0x08 || ch == 0x7F) {   /* BS / DEL */
            if (len > 0) {
                len--;
                cpm_conout('\b');
                cpm_conout(' ');
                cpm_conout('\b');
            }
        } else if (ch == 0x15) {                 /* ^U - kill line */
            while (len > 0) {
                cpm_conout('\b');
                cpm_conout(' ');
                cpm_conout('\b');
                len--;
            }
        } else if (ch == 0x12) {                 /* ^R - retype line (nice to have) */
            cpm_conout('#');
            cpm_conout('\r');
            cpm_conout('\n');
            for (uint8_t i = 0; i < len; i++) {
                cpm_conout(buf[2 + i]);
            }
        } else if (ch >= 0x20 && ch <= 0x7E && len < max) {
            buf[2 + len] = ch;
            len++;
            cpm_conout(ch);
        }
        /* ignore other control characters */
    }

    buf[1] = len;
    cpu->a = 0;
}
