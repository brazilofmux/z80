/* Z80 CPU state implementation */
#include "z80.h"
#include <stdlib.h>
#include <string.h>

void z80_cpu_init(z80_cpu_t *cpu) {
    memset(cpu, 0, sizeof(*cpu));

    /* 64 KB flat memory for classic CP/M */
    cpu->mem = malloc(65536);
    if (cpu->mem) {
        cpu->mem_size = 65536;
        memset(cpu->mem, 0, 65536);
    }

    /* Reasonable power-on defaults */
    cpu->sp = 0xFFFF;
    cpu->im = 1;
    cpu->iff1 = 0;
    cpu->iff2 = 0;
}

void z80_cpu_reset(z80_cpu_t *cpu) {
    /* Preserve memory allocation */
    uint8_t *mem = cpu->mem;
    uint32_t mem_size = cpu->mem_size;

    memset(cpu, 0, sizeof(*cpu));

    cpu->mem = mem;
    cpu->mem_size = mem_size;

    cpu->sp = 0xFFFF;
    cpu->im = 1;
}

int z80_load_com(z80_cpu_t *cpu, const char *path) {
    /* Stub for now — real implementation will open the file,
       read it into cpu->mem + 0x0100, set PC = 0x0100, etc. */
    (void)path;
    return -1;  /* not implemented yet */
}

uint8_t z80_materialize_flags(z80_cpu_t *cpu) {
    /* For the lazy-flag future. Right now just return what we have. */
    return cpu->f;
}
