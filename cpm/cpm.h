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

/* BIOS base — chosen so we have a 62K TPA (common for CP/M 2.2 on 64K machines) */
#define CPM_BIOS_BASE   0xF200

/* Number of BIOS functions in the jump table (classic is 17 for CP/M 2.2) */
#define CPM_BIOS_COUNT  17

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

/* BIOS function numbers (index into the jump table) */
enum {
    BIOS_BOOT   = 0,
    BIOS_WBOOT  = 1,
    BIOS_CONST  = 2,   /* Console status */
    BIOS_CONIN  = 3,   /* Console input */
    BIOS_CONOUT = 4,   /* Console output (char in C) */
    BIOS_LIST   = 5,
    BIOS_PUNCH  = 6,
    BIOS_READER = 7,
    BIOS_HOME   = 8,
    BIOS_SELDSK = 9,
    BIOS_SETTRK = 10,
    BIOS_SETSEC = 11,
    BIOS_SETDMA = 12,
    BIOS_READ   = 13,
    BIOS_WRITE  = 14,
    BIOS_LISTST = 15,
    BIOS_SECTRAN= 16,
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

/* BIOS support */
void cpm_install_bios(z80_cpu_t *cpu);
int  cpm_bios_dispatch(z80_cpu_t *cpu);

/* Unified console functions (used by both BDOS and BIOS) */
void    cpm_conout(uint8_t ch);
uint8_t cpm_conin(void);
uint8_t cpm_constat(void);   /* returns 0 or 0xFF */

/* BDOS 10 line input (implemented in bios layer for sharing) */
void cpm_read_console_buffer(z80_cpu_t *cpu, uint16_t de);

/* Disk / FCB support */
#define CPM_DEFAULT_DMA   0x0080
#define CPM_FCB_SIZE      36

/* Public disk functions */
void cpm_disk_init(void);
void cpm_disk_install_defaults(z80_cpu_t *cpu);
void cpm_set_a_root(const char *path);
void cpm_set_dma(uint16_t addr);
int  cpm_bdos_open_file(z80_cpu_t *cpu, uint16_t fcb_addr);
int  cpm_bdos_close_file(z80_cpu_t *cpu, uint16_t fcb_addr);
int  cpm_bdos_read_sequential(z80_cpu_t *cpu, uint16_t fcb_addr);
int  cpm_bdos_set_dma(z80_cpu_t *cpu);  /* uses DE */
int  cpm_bdos_search_first(z80_cpu_t *cpu, uint16_t fcb_addr);
int  cpm_bdos_search_next(z80_cpu_t *cpu);
int  cpm_bdos_make_file(z80_cpu_t *cpu, uint16_t fcb_addr);
int  cpm_bdos_write_sequential(z80_cpu_t *cpu, uint16_t fcb_addr);

/* Random record access (BDOS 33/34) */
int  cpm_bdos_random_read(z80_cpu_t *cpu, uint16_t fcb_addr);
int  cpm_bdos_random_write(z80_cpu_t *cpu, uint16_t fcb_addr);

/* Load a .COM file at 0x0100, set up the CP/M memory image, and
 * prepare the CPU to run it (PC=0x0100, stack high, C=0 for CCP compat).
 */
int cpm_load_com(z80_cpu_t *cpu, const char *path);

#endif
