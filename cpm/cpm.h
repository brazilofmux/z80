/* cpm.h — Minimal CP/M 2.2 environment for the Z80 monster
 *
 * We install a tiny BDOS trampoline at 0x0005 and a warm-boot trampoline
 * at 0x0000. When the guest does CALL 5, we catch it in the interpreter
 * (or later in the DBT via a special exit) and dispatch to host code.
 *
 * This is the "host services" layer, exactly analogous to the ECALL
 * mechanism in the RV32 DBT.
 */

#ifndef CPM_H
#define CPM_H

#include "../core/z80.h"
#include <stdint.h>

#define CPM_BDOS_ENTRY  0x0005
#define CPM_WBOOT_ENTRY 0x0000

/* BDOS function numbers we care about for the first bring-up */
enum {
    CPM_F_WBOOT   = 0,   /* System reset */
    CPM_F_CONIN   = 1,   /* Console input (wait) */
    CPM_F_CONOUT  = 2,   /* Console output (char in E) */
    CPM_F_RDCON   = 3,
    CPM_F_WRCON   = 4,
    CPM_F_LIST    = 5,
    CPM_F_PUNCH   = 6,
    CPM_F_RDR     = 7,
    CPM_F_RDSTR   = 10,  /* Read console buffer */
    CPM_F_CONSTAT = 11,  /* Console status */
    CPM_F_VERSION = 12,
    CPM_F_RESET   = 13,
    CPM_F_SELDSK  = 14,
    CPM_F_OPEN    = 15,
    CPM_F_CLOSE   = 16,
    CPM_F_SFIRST  = 17,
    CPM_F_SNEXT   = 18,
    CPM_F_DELETE  = 19,
    CPM_F_READ    = 20,
    CPM_F_WRITE   = 21,
    CPM_F_MAKE    = 22,
    CPM_F_RENAME  = 23,
    CPM_F_RETLOGIN= 24,
    CPM_F_RETDSK  = 25,
    CPM_F_SETDMA  = 26,
    CPM_F_GETALLOC= 27,
    CPM_F_WRPROT  = 28,
    CPM_F_GETROV  = 29,
    CPM_F_SETATTR = 30,
    CPM_F_GETDPB  = 31,
    CPM_F_RANDRD  = 33,
    CPM_F_RANDWR  = 34,
    CPM_F_GETFSIZ = 35,
    CPM_F_RANDWRZF= 36,
    /* The big one for "hello world" */
    CPM_F_PRINTSTR = 9,   /* Print $-terminated string at DE */
};

/* Install the CP/M environment into the Z80's memory:
 *   - JP WBOOT at 0x0000
 *   - JP BDOS  at 0x0005 (with a recognizable signature so the DBT can
 *     special-case the CALL 5 without decoding every time)
 */
void cpm_install(z80_cpu_t *cpu);

/* Dispatch a BDOS call. Called from the interpreter when PC hits the
 * trampoline or when we detect a CALL 5 in the DBT.
 * Returns 1 if the program should continue, 0 if it wants to terminate.
 */
int cpm_bdos_dispatch(z80_cpu_t *cpu);

/* Load a .COM file at 0x0100, set up the CP/M memory image, and
 * prepare the CPU to run it (PC=0x0100, stack high, C=0 for CCP compat).
 */
int cpm_load_com(z80_cpu_t *cpu, const char *path);

#endif
