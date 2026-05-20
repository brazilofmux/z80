/* cpm_loader.c — .COM loader and CP/M memory image setup */
#include "cpm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void cpm_install(z80_cpu_t *cpu) {
    if (!cpu->mem || cpu->mem_size < 0x0100) return;

    /* Classic CP/M 2.2 layout for a 64K machine:
     *   0000-00FF : reserved (RST vectors, BDOS entry, etc.)
     *   0100-.... : TPA (transient program area) where .COM lives
     *   high memory : CCP + BDOS + BIOS (we fake them)
     *
     * We only need two jumps:
     *   0000: JP <warm boot handler>
     *   0005: JP <BDOS handler>
     *
     * For the interpreter we will detect CALL 0005 and CALL 0000
     * specially. For the DBT we will plant a recognizable illegal
     * instruction or a special "host call" opcode that the translator
     * turns into a fast exit to cpm_bdos_dispatch().
     */

    /* For now: put a recognizable pattern at 0x0005 that the interpreter
     * can trap on (we'll just check PC after CALL). */
    cpu->mem[0x0000] = 0xC3; /* JP */
    cpu->mem[0x0001] = 0x00;
    cpu->mem[0x0002] = 0xF0; /* warm boot at 0xF000 (we'll catch it) */

    cpu->mem[0x0005] = 0xC3; /* JP */
    cpu->mem[0x0006] = 0x00;
    cpu->mem[0x0007] = 0xF0; /* BDOS entry at 0xF000 (we'll catch CALL 5) */

    /* Put a small "BDOS dispatcher" signature in high memory so a
     * future full BIOS can live there. For now it's just markers. */
    memcpy(cpu->mem + 0xF000, "CPM2MONSTER", 12);
}

int cpm_load_com(z80_cpu_t *cpu, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 0xFE00) {
        fprintf(stderr, "%s: unreasonable .COM size (%ld bytes)\n", path, size);
        fclose(f);
        return -1;
    }

    /* .COM files load at 0x0100 */
    size_t n = fread(cpu->mem + 0x0100, 1, (size_t)size, f);
    fclose(f);

    if (n != (size_t)size) {
        fprintf(stderr, "%s: short read\n", path);
        return -1;
    }

    /* Set up CPU for CP/M .COM execution */
    cpu->pc = 0x0100;
    cpu->sp = 0xFF00;

    /* Classic .COM convention: the word at the very top of the stack
     * is usually 0x0000 so a final RET warm-boots. We simulate that by
     * pushing 0x0000 onto the stack before the program starts.
     */
    cpu->sp -= 2;
    cpu->mem[cpu->sp]     = 0x00;
    cpu->mem[cpu->sp + 1] = 0x00;

    /* Many .COM programs expect C=0 or the CCP command line at 0x0080 */
    cpu->c = 0;

    /* Make sure the BDOS trampoline is there */
    cpm_install(cpu);

    /* Install a real BIOS jump table — critical for programs that
     * call the BIOS vectors directly (very common in WordStar, etc.) */
    cpm_install_bios(cpu);

    return 0;
}
