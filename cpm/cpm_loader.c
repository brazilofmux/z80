/* cpm_loader.c — .COM loader and CP/M memory image setup */
#include "cpm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void cpm_install(z80_cpu_t *cpu) {
    if (!cpu->mem || cpu->mem_size < 0x0100) return;

    /* Classic CP/M 2.2 layout for a 64K machine */

    cpu->mem[0x0000] = 0xC3; /* JP */
    cpu->mem[0x0001] = 0x00;
    cpu->mem[0x0002] = 0xF0;

    cpu->mem[0x0005] = 0xC3; /* JP 0x0005 — self-referential so the interpreter trap on PC==0x0005 fires */
    cpu->mem[0x0006] = 0x05;
    cpu->mem[0x0007] = 0x00;

    memcpy(cpu->mem + 0xF000, "CPM2MONSTER", 12);

    /* Initialize disk layer (DMA, open file table, etc.) */
    cpm_disk_init();
    cpm_disk_install_defaults(cpu);
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

    /* Disk layer */
    cpm_disk_init();
    cpm_disk_install_defaults(cpu);

    /* Install a real BIOS jump table — critical for programs that
     * call the BIOS vectors directly (very common in WordStar, etc.) */
    cpm_install_bios(cpu);

    return 0;
}
