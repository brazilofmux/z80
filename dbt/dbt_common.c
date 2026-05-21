/* dbt_common.c — Architecture-neutral DBT machinery.
 *
 * Owns init/run/cleanup and the main run loop. Code emission lives in
 * dbt_a64.c / dbt_x64.c behind the dbt_translate_block / dbt_emit_trampoline
 * hooks declared in dbt.h.
 *
 * Run-loop policy
 * ---------------
 * At each guest PC:
 *   1. Cache lookup. Hit -> trampoline into native block.
 *   2. Miss -> ask the backend to translate. If translation succeeds
 *      (>=1 instruction emitted), insert into cache and enter it.
 *   3. If the backend refuses (the very first instruction is not
 *      translatable), step the interpreter for that one instruction
 *      and retry the JIT at the next PC.
 *
 * Every translated block returns by writing cpu->pc (16-bit) and RETing.
 * Anything that can trap into a host service — CALL nn (BDOS/BIOS hook
 * lives there), OUT/IN, LDIR-family, EI/DI, HALT — is currently treated
 * as untranslatable, so it falls through to the interpreter, which
 * already knows how to dispatch the host shim. */
#include "dbt.h"
#include "../core/z80.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* z80_step lives in core/z80_interp.c — reference interpreter step.
 *   returns  0  normal continue
 *           +1  clean CP/M termination
 *           -1  error / unimplemented opcode */
extern int z80_step(z80_cpu_t *cpu);

int dbt_init(z80_dbt_t *dbt, z80_cpu_t *cpu) {
    memset(dbt, 0, sizeof(*dbt));
    dbt->cpu = cpu;
    dbt_cache_invalidate_all(dbt);

    dbt->code_buf = mmap(NULL, CODE_BUF_SIZE,
                         PROT_READ | PROT_WRITE | PROT_EXEC,
                         DBT_JIT_MMAP_FLAGS, -1, 0);
    if (dbt->code_buf == MAP_FAILED) {
        fprintf(stderr, "dbt_init: cannot mmap %u-byte JIT buffer\n", CODE_BUF_SIZE);
        return -1;
    }

    dbt_jit_writable_begin();
    dbt_emit_trampoline(dbt);
    dbt_jit_writable_end();

    return 0;
}

void dbt_cleanup(z80_dbt_t *dbt) {
    if (dbt->code_buf && dbt->code_buf != MAP_FAILED) {
        munmap(dbt->code_buf, CODE_BUF_SIZE);
        dbt->code_buf = NULL;
    }
}

/* Trampoline signature: called from C with the four register-binding
 * arguments the backend trampoline expects. */
typedef void (*trampoline_fn_t)(z80_cpu_t *cpu, uint8_t *mem,
                                void *block, void *cache_base);

int dbt_run(z80_dbt_t *dbt) {
    z80_cpu_t *cpu = dbt->cpu;
    trampoline_fn_t trampoline = (trampoline_fn_t)(void *)dbt->code_buf;

    for (;;) {
        /* Mirror main.c's "RET to warm boot" termination check. */
        if (cpu->pc == 0 && cpu->insn_count > 4) {
            return 0;
        }
        if (cpu->insn_count > 50000000000ULL) {
            fprintf(stderr, "dbt_run: 50B insn safety limit reached\n");
            return -1;
        }

        z80_block_entry_t *be = dbt_cache_lookup(dbt, cpu->pc);
        uint8_t *code = be ? be->native_code : NULL;

        if (!code) {
            dbt_jit_writable_begin();
            code = dbt_translate_block(dbt, cpu->pc);
            dbt_jit_writable_end();
            if (code) {
                dbt_cache_insert(dbt, cpu->pc, code);
                dbt->blocks_translated++;
            }
        }

        if (code) {
            dbt->jit_block_entries++;
            trampoline(cpu, cpu->mem, code, dbt->cache);
            continue;
        }

        /* Backend gave up on the first insn — step the interpreter once. */
        dbt->interp_fallback_insns++;
        int rc = z80_step(cpu);
        if (rc < 0) {
            fprintf(stderr, "dbt_run: interpreter stopped at PC=%04X\n", cpu->pc);
            return -1;
        }
        if (rc > 0) {
            return 0;   /* clean CP/M exit */
        }
    }
}

void dbt_print_stats(z80_dbt_t *dbt, FILE *out) {
    fprintf(out, "  blocks translated:      %llu\n",
            (unsigned long long)dbt->blocks_translated);
    fprintf(out, "  cache hits/misses:      %llu / %llu\n",
            (unsigned long long)dbt->cache_hits,
            (unsigned long long)dbt->cache_misses);
    fprintf(out, "  JIT block entries:      %llu\n",
            (unsigned long long)dbt->jit_block_entries);
    fprintf(out, "  interp fallback insns:  %llu\n",
            (unsigned long long)dbt->interp_fallback_insns);
}
