/* dbt_x64.c — x86-64 backend stub.
 *
 * The first cut of the Z80 DBT is AArch64-only. On x86-64 hosts we report
 * "JIT unavailable" so main.c falls back to the interpreter automatically.
 * Filling this out is a clean follow-up: mirror dbt_a64.c with x86-64
 * encodings (the same trampoline / translator / epilogue shape works). */
#include "dbt.h"
#include <stdint.h>

int dbt_jit_available(void) { return 0; }

void dbt_emit_trampoline(z80_dbt_t *dbt) {
    (void)dbt;
}

uint8_t *dbt_translate_block(z80_dbt_t *dbt, uint16_t guest_pc) {
    (void)dbt;
    (void)guest_pc;
    return NULL;
}
