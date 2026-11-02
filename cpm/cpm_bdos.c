/* cpm_bdos.c — BDOS function dispatcher (the host<->guest bridge) */
#include "cpm.h"
#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>

int cpm_debug = 0;

/* CP/M returns 16-bit results in HL with A = L and B = H. */
static void ret_hl(z80_cpu_t *cpu, uint16_t v) {
    cpu->hl = v;
    cpu->a = (uint8_t)v;
    cpu->b = (uint8_t)(v >> 8);
}

/* The shim has one drive (A:, the host directory), one user area for
 * every user number, and no physical devices behind RDR:/PUN:. */
static uint16_t ro_vector;   /* BDOS 28/29/37: drives marked read-only */
static uint8_t  user_code;   /* BDOS 32 */

/* We will wire this into the interpreter's CALL handling.
 * For the DBT we will later make CALL 5 a fast path that exits the
 * translated block with a special "BDOS" exit reason.
 */
int cpm_bdos_dispatch(z80_cpu_t *cpu) {
    uint8_t func = cpu->c;

    /* Light startup logging to help bring-up of real binaries: the first
     * 30 calls plus every open/make/delete, or every call up to
     * Z80_BDOS_TRACE_MAX (0 = no limit) when that is set. */
    static int early_calls = 0;
    static long trace_max = -1;
    if (trace_max < 0) {
        const char *e = getenv("Z80_BDOS_TRACE_MAX");
        trace_max = e ? atol(e) : 30;
        if (trace_max == 0) trace_max = 1L << 30;
    }
    if (cpm_debug && (early_calls < trace_max || func == 15 || func == 22 || func == 19)) {
        early_calls++;
        if (func == 33 || func == 34) {
            /* Dump the random record the game is asking for */
            uint8_t *fcb = &cpu->mem[cpu->de];
            uint32_t rec = (uint32_t)fcb[33] | ((uint32_t)fcb[34]<<8) | ((uint32_t)fcb[35]<<16);
            fprintf(stderr, "[BDOS #%d] func=%d (random %s) DE=%04X rec=%u (0x%06X)\n",
                    early_calls, func, (func==33?"read":"write"), cpu->de, rec, rec);
        } else if (func == 15 || func == 22 || func == 19 || func == 17) {
            /* Filename from FCB */
            uint8_t *fcb = &cpu->mem[cpu->de];
            char name[13];
            int j = 0;
            for (int i = 1; i < 9 && fcb[i] != ' '; i++) name[j++] = fcb[i];
            name[j++] = '.';
            for (int i = 9; i < 12 && fcb[i] != ' '; i++) name[j++] = fcb[i];
            name[j] = 0;
            fprintf(stderr, "[BDOS #%d] func=%d (%s) FCB@%04X drive=%d name='%s' EX=%02X S2=%02X CR=%02X raw=",
                    early_calls, func,
                    func == 15 ? "OPEN" : func == 22 ? "MAKE" : func == 19 ? "DEL" : "SFIRST",
                    cpu->de, fcb[0], name, fcb[12], fcb[14], fcb[32]);
            for (int i = 1; i < 12; i++) fprintf(stderr, "%02X", fcb[i]);
            fputc('\n', stderr);
        } else if (func == 20 || func == 21) {
            /* Sequential I/O: the FCB's own position fields drive it. */
            uint8_t *fcb = &cpu->mem[cpu->de];
            fprintf(stderr, "[BDOS #%d] func=%d (%s) FCB@%04X EX=%02X S1=%02X S2=%02X RC=%02X CR=%02X\n",
                    early_calls, func, func == 20 ? "READ" : "WRITE", cpu->de,
                    fcb[12], fcb[13], fcb[14], fcb[15], fcb[32]);
        } else {
            fprintf(stderr, "[BDOS #%d] func=%d DE=%04X\n", early_calls, func, cpu->de);
        }
    }

    switch (func) {
    case CPM_F_WBOOT:   /* 0 - warm boot / exit */
        /* Program wants to return to CCP or terminate */
        fflush(stdout);   /* buffered console output must land before exit */
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

    case CPM_F_RDR:     /* 3 - reader input: no reader attached, read EOF */
        cpu->a = 0x1A;
        return 1;

    case CPM_F_PUNCH:   /* 4 - punch output: discarded */
        return 1;

    case CPM_F_LIST:    /* 5 - list output to the --list file */
        cpm_list_out(cpu->e);
        return 1;

    case CPM_F_GETIOB:  /* 7 - IOBYTE lives at 0003 */
        cpu->a = cpu->mem[0x0003];
        return 1;

    case CPM_F_SETIOB:  /* 8 */
        cpu->mem[0x0003] = cpu->e;
        z80_mem_host_wrote(cpu, 0x0003, 1);
        return 1;

    case CPM_F_LOGINV:  /* 24 - only A: exists */
        ret_hl(cpu, 0x0001);
        return 1;

    case CPM_F_GETALV:  /* 27 */
        return cpm_bdos_alloc_vector(cpu);

    case CPM_F_WRPROT:  /* 28 - current disk (always A:) */
        ro_vector |= 0x0001;
        cpu->a = 0;
        return 1;

    case CPM_F_GETROV:  /* 29 */
        ret_hl(cpu, ro_vector);
        return 1;

    case CPM_F_SETATTR: /* 30 */
        return cpm_bdos_set_attributes(cpu, cpu->de);

    case CPM_F_GETDPB:  /* 31 */
        ret_hl(cpu, CPM_SHIM_DPB);
        return 1;

    case CPM_F_USER:    /* 32 - E = FF: get; else set E & 0F */
        if (cpu->e == 0xFF) cpu->a = user_code;
        else { user_code = cpu->e & 0x0F; cpu->a = 0; }
        return 1;

    case CPM_F_FSIZE:   /* 35 */
        return cpm_bdos_file_size(cpu, cpu->de);

    case CPM_F_SETRAND: /* 36 */
        return cpm_bdos_set_random(cpu, cpu->de);

    case CPM_F_RESETDRV:/* 37 - DE = drives to reset: they lose R/O */
        ro_vector &= (uint16_t)~cpu->de;
        cpu->a = 0;
        return 1;

    case CPM_F_RANDWRZ: /* 40 - a host file past its end reads as zeros already */
        return cpm_bdos_random_write(cpu, cpu->de);

    case CPM_F_CONSTAT: /* 11 - console status */
        cpu->a = cpm_constat();
        return 1;

    case CPM_F_CONIN:   /* 1 - console input (blocking) */
        cpu->a = cpm_conin();
        return 1;

    case CPM_F_DIRIO:   /* 6 - Direct Console I/O (very common) */
        /* CP/M dispatches on E only — D is ignored. The old check
         * `cpu->de == 0x00FF` required D==0, which Zork (and many other
         * programs) don't bother to clear. The result was that every
         * E=0xFF read poll fell into the else branch and wrote 0xFF to
         * the console — exactly the "flood of �" right after the prompt. */
        if (cpu->e == 0xFF) {
            /* Read char if available (non-blocking, no echo) */
            cpu->a = cpm_constat() ? cpm_conin() : 0;
        } else if (cpu->e == 0xFE) {
            /* CP/M 3 status: 0xFF if key ready, 0 otherwise */
            cpu->a = cpm_constat();
        } else if (cpu->e == 0xFD) {
            /* CP/M 3 read-without-status: blocking read, no echo */
            cpu->a = cpm_conin();
        } else {
            /* Write char in E */
            cpm_conout(cpu->e);
            cpu->a = cpu->e;   /* some programs expect echo in A */
        }
        return 1;

    case CPM_F_RDSTR:   /* 10 - Read console buffer (the one real programs use) */
        cpm_read_console_buffer(cpu, cpu->de);
        return 1;

    case CPM_F_VERSION: /* 12 */
        cpu->hl = 0x0022; /* CP/M 2.2 */
        return 1;

    case CPM_F_RETDSK:  /* 25 - Return Current Disk */
        cpu->a = 0;     /* We only support drive A: (0) for now */
        return 1;

    case CPM_F_SELDSK:  /* 14 - Select Disk */
        /* For now we only have drive A: (0). Accept the call. */
        cpu->a = 0;
        return 1;

    case CPM_F_RESET:   /* 13 - Reset Disk System (very common at startup) */
        /* CP/M 2.2: log out all drives, select A:, DMA back to 0x80 —
         * and nothing else. Open FCBs stay valid; WordStar resets the
         * disk system between overlay loads and keeps reading through
         * the same FCB, and closing its files here produced its "E38
         * bad overlay file". */
        cpm_set_dma(CPM_DEFAULT_DMA);
        ro_vector = 0;
        cpu->a = 0;
        return 1;

    case CPM_F_SETDMA:   /* 26 */
        return cpm_bdos_set_dma(cpu);

    case CPM_F_OPEN:     /* 15 */
        return cpm_bdos_open_file(cpu, cpu->de);

    case CPM_F_CLOSE:    /* 16 */
        return cpm_bdos_close_file(cpu, cpu->de);

    case CPM_F_READ:     /* 20 - read sequential */
        return cpm_bdos_read_sequential(cpu, cpu->de);

    case CPM_F_SFIRST:   /* 17 - search first */
        return cpm_bdos_search_first(cpu, cpu->de);

    case CPM_F_SNEXT:    /* 18 - search next */
        return cpm_bdos_search_next(cpu);

    case CPM_F_MAKE:     /* 22 - create file */
        return cpm_bdos_make_file(cpu, cpu->de);

    case CPM_F_RENAME:   /* 23 - rename file (old FCB at DE, new name at DE+16) */
        return cpm_bdos_rename(cpu, cpu->de);

    case CPM_F_DELETE:   /* 19 - delete file (by FCB) */
        return cpm_bdos_delete_file(cpu, cpu->de);

    case CPM_F_WRITE:    /* 21 - write sequential */
        return cpm_bdos_write_sequential(cpu, cpu->de);

    case CPM_F_RANDRD:   /* 33 - random read */
        return cpm_bdos_random_read(cpu, cpu->de);

    case CPM_F_RANDWR:   /* 34 - random write */
        return cpm_bdos_random_write(cpu, cpu->de);

    default:
        /* Many real .COMs call functions we don't care about yet (Reset, Get DPB,
         * Login Vector, Read Buffer, etc.). Log it but don't kill the program.
         * Above 40 is CP/M 3 / MP/M territory: a 2.2 BDOS ignores those and
         * programs probe them on purpose (BBC BASIC 5 calls 45), so only -d
         * reports them. Return HL = 0 (A = L, B = H) like DRI's BDOS. */
        if (func <= 40 || cpm_debug)
            fprintf(stderr, "[BDOS] unimplemented function %d (C=%d) DE=%04X — continuing\n",
                    func, func, cpu->de);
        cpu->hl = 0;
        cpu->a = 0;
        cpu->b = 0;
        return 1;   /* continue execution */
    }
}
