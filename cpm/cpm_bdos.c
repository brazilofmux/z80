/* cpm_bdos.c — BDOS function dispatcher (the host<->guest bridge) */
#include "cpm.h"
#include <stdio.h>
#include <unistd.h>
#include <termios.h>

/* We will wire this into the interpreter's CALL handling.
 * For the DBT we will later make CALL 5 a fast path that exits the
 * translated block with a special "BDOS" exit reason.
 */
int cpm_bdos_dispatch(z80_cpu_t *cpu) {
    uint8_t func = cpu->c;

    switch (func) {
    case CPM_F_WBOOT:   /* 0 - warm boot / exit */
        /* Program wants to return to CCP or terminate */
        return 0;   /* tell the run loop to stop */

    case CPM_F_CONOUT:  /* 2 - console output char in E */
        cpm_conout(cpu->e);
        cpu->a = 0;
        return 1;

    case CPM_F_PRINTSTR: /* 9 - print $-terminated string at DE */
        {
            uint16_t addr = cpu->de;
            for (;;) {
                uint8_t ch = cpu->mem[addr & 0xFFFF];
                if (ch == '$') break;
                cpm_conout(ch);
                addr++;
            }
            cpu->a = 0;
        }
        return 1;

    case CPM_F_CONSTAT: /* 11 - console status */
        cpu->a = cpm_constat();
        return 1;

    case CPM_F_CONIN:   /* 1 - console input (blocking) */
        cpu->a = cpm_conin();
        return 1;

    case CPM_F_VERSION: /* 12 */
        cpu->hl = 0x0022; /* CP/M 2.2 */
        return 1;

    case CPM_F_SETDMA:  /* 26 - set DMA address (DE) */
        /* We don't have full FCB handling yet, but we record it */
        /* For now just ignore; real disk I/O will need it. */
        return 1;

    default:
        fprintf(stderr, "\n[BDOS] unimplemented function %d (C=%d)  DE=%04X\n",
                func, func, cpu->de);
        /* For development, treat unknown BDOS calls as fatal so we notice */
        return 0;
    }
}
