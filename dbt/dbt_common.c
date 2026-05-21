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
    cpu->dbt = dbt;
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
    if (dbt->cpu) dbt->cpu->dbt = NULL;
    if (dbt->code_buf && dbt->code_buf != MAP_FAILED) {
        munmap(dbt->code_buf, CODE_BUF_SIZE);
        dbt->code_buf = NULL;
    }
    free(dbt->shadow_mem);
    dbt->shadow_mem = NULL;
}

/* ----------------------------------------------------------------------
 * -V shadow-verify support
 * ---------------------------------------------------------------------- */

/* Compare the registers that hold guest-visible state. We deliberately
 * skip the DBT-only fields (mem, mem_size, block_cache, dbt, insn_count
 * stats, etc.). Returns 1 if equal. */
static int cpu_regs_equal(const z80_cpu_t *a, const z80_cpu_t *b) {
    if (a->af != b->af) return 0;
    if (a->bc != b->bc) return 0;
    if (a->de != b->de) return 0;
    if (a->hl != b->hl) return 0;
    if (a->af_ != b->af_) return 0;
    if (a->bc_ != b->bc_) return 0;
    if (a->de_ != b->de_) return 0;
    if (a->hl_ != b->hl_) return 0;
    if (a->ix != b->ix) return 0;
    if (a->iy != b->iy) return 0;
    if (a->sp != b->sp) return 0;
    if (a->pc != b->pc) return 0;
    if (a->i != b->i) return 0;
    if (a->r != b->r) return 0;
    if (a->iff1 != b->iff1) return 0;
    if (a->iff2 != b->iff2) return 0;
    if (a->im != b->im) return 0;
    if (a->q != b->q) return 0;
    if (a->memptr != b->memptr) return 0;
    return 1;
}

static void dump_regs(FILE *out, const char *tag, const z80_cpu_t *c) {
    fprintf(out, "  %s: PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X "
                 "IX=%04X IY=%04X AF'=%04X memptr=%04X q=%u\n",
            tag, c->pc, c->sp, c->af, c->bc, c->de, c->hl,
            c->ix, c->iy, c->af_, c->memptr, c->q);
}

static int verify_first_diff(const z80_cpu_t *jit, const z80_cpu_t *interp) {
    /* Print one-line "what differs" hint. */
    if (jit->af != interp->af)
        fprintf(stderr, "    AF differs: jit=%04X interp=%04X\n", jit->af, interp->af);
    if (jit->bc != interp->bc)
        fprintf(stderr, "    BC differs: jit=%04X interp=%04X\n", jit->bc, interp->bc);
    if (jit->de != interp->de)
        fprintf(stderr, "    DE differs: jit=%04X interp=%04X\n", jit->de, interp->de);
    if (jit->hl != interp->hl)
        fprintf(stderr, "    HL differs: jit=%04X interp=%04X\n", jit->hl, interp->hl);
    if (jit->ix != interp->ix)
        fprintf(stderr, "    IX differs: jit=%04X interp=%04X\n", jit->ix, interp->ix);
    if (jit->iy != interp->iy)
        fprintf(stderr, "    IY differs: jit=%04X interp=%04X\n", jit->iy, interp->iy);
    if (jit->sp != interp->sp)
        fprintf(stderr, "    SP differs: jit=%04X interp=%04X\n", jit->sp, interp->sp);
    if (jit->pc != interp->pc)
        fprintf(stderr, "    PC differs: jit=%04X interp=%04X\n", jit->pc, interp->pc);
    if (jit->memptr != interp->memptr)
        fprintf(stderr, "    memptr differs: jit=%04X interp=%04X\n", jit->memptr, interp->memptr);
    if (jit->q != interp->q)
        fprintf(stderr, "    q differs: jit=%u interp=%u\n", jit->q, interp->q);
    return 1;
}

/* Trampoline signature: called from C with the register-binding
 * arguments the backend trampoline expects. */
typedef void (*trampoline_fn_t)(z80_cpu_t *cpu, uint8_t *mem,
                                void *block, void *cache_base,
                                void *bitmap_base);

int dbt_run(z80_dbt_t *dbt) {
    z80_cpu_t *cpu = dbt->cpu;
    trampoline_fn_t trampoline = (trampoline_fn_t)(void *)dbt->code_buf;

    /* Lazily initialise the parallel shadow on first -V entry.
     *
     * NOTE on -V and interactive programs: the shadow cpu's interp
     * shares stdin/stdout with the real cpu — both execute BDOS dispatch
     * and both call cpm_conin(), so each keystroke is consumed twice.
     * That's fine for the OUTPUT (you just see characters duplicated)
     * but breaks lockstep for INPUT: real and shadow read different
     * bytes from the same stream and their cpu state diverges. zex* and
     * any other output-only workloads work; Zork and other interactive
     * programs will report spurious lockstep divergences whose AF bytes
     * are the ASCII codes of consecutive keystrokes (e.g. real=0x71 'q'
     * vs shadow=0x75 'u' from a piped "quit"). */
    if (dbt->verify && !dbt->shadow_mem) {
        dbt->shadow_mem = calloc(1, 65536);
        if (!dbt->shadow_mem) {
            fprintf(stderr, "dbt_run: cannot allocate shadow memory\n");
            return -1;
        }
        memcpy(dbt->shadow_mem, cpu->mem, 65536);
        dbt->shadow_cpu = *cpu;
        dbt->shadow_cpu.mem = dbt->shadow_mem;
        dbt->shadow_cpu.dbt = NULL;       /* shadow must not recurse into JIT */
    }

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
            uint16_t entry_pc = cpu->pc;

            if (dbt->verify) {
                uint64_t insns_before = cpu->insn_count;
                z80_cpu_t pre = *cpu;
                z80_cpu_t shadow_pre = dbt->shadow_cpu;

                /* Sanity: the shadow MUST mirror the real cpu pre-block
                 * (lockstep invariant). If it doesn't, the prior block
                 * silently diverged — bail with a focused report. */
                int pre_regs_ok = cpu_regs_equal(&pre, &shadow_pre);
                int pre_mem_ok  = memcmp(cpu->mem, dbt->shadow_mem, 65536) == 0;
                if (!pre_regs_ok || !pre_mem_ok) {
                    fprintf(stderr,
                            "\n[verify] lockstep broken BEFORE JIT block at PC=%04X "
                            "(pre_regs_ok=%d pre_mem_ok=%d)\n",
                            entry_pc, pre_regs_ok, pre_mem_ok);
                    dump_regs(stderr, "real  ", &pre);
                    dump_regs(stderr, "shadow", &shadow_pre);
                    if (!pre_regs_ok) verify_first_diff(&pre, &shadow_pre);
                    if (!pre_mem_ok) {
                        int shown = 0;
                        for (int i = 0; i < 65536 && shown < 8; i++) {
                            if (cpu->mem[i] != dbt->shadow_mem[i]) {
                                fprintf(stderr,
                                        "    mem[%04X] differs: real=%02X shadow=%02X\n",
                                        i, cpu->mem[i], dbt->shadow_mem[i]);
                                shown++;
                            }
                        }
                    }
                    return -1;
                }

                /* Run the JIT block on the real cpu/mem. */
                dbt->jit_block_entries++;
                trampoline(cpu, cpu->mem, code, dbt->cache, dbt->code_bitmap);

                uint64_t jit_insns = cpu->insn_count - insns_before;
                z80_cpu_t post_jit = *cpu;

                /* Step the parallel shadow forward by the same insn count. */
                for (uint64_t i = 0; i < jit_insns; i++) {
                    int rc = z80_step(&dbt->shadow_cpu);
                    if (rc != 0) break;
                }
                dbt->verify_blocks_checked++;

                int regs_ok = cpu_regs_equal(&post_jit, &dbt->shadow_cpu);
                int mem_ok  = memcmp(cpu->mem, dbt->shadow_mem, 65536) == 0;
                if (!regs_ok || !mem_ok) {
                    fprintf(stderr,
                            "\n[verify] divergence after JIT block at PC=%04X (%llu insns)\n",
                            entry_pc, (unsigned long long)jit_insns);
                    dump_regs(stderr, "pre   ", &pre);
                    dump_regs(stderr, "JIT   ", &post_jit);
                    dump_regs(stderr, "shadow", &dbt->shadow_cpu);
                    if (!regs_ok) verify_first_diff(&post_jit, &dbt->shadow_cpu);
                    if (!mem_ok) {
                        int shown = 0;
                        for (int i = 0; i < 65536 && shown < 8; i++) {
                            if (cpu->mem[i] != dbt->shadow_mem[i]) {
                                fprintf(stderr,
                                        "    mem[%04X] differs: jit=%02X shadow=%02X\n",
                                        i, cpu->mem[i], dbt->shadow_mem[i]);
                                shown++;
                            }
                        }
                    }
                    /* Dump the bytes of the executed block so we can
                     * see if JIT-translated code drifted from what's
                     * currently in memory (SMC suspicion). */
                    fprintf(stderr, "    block bytes (from real mem):");
                    for (int i = 0; i < 48; i++) {
                        if ((i & 0xF) == 0) fprintf(stderr, "\n      %04X:", entry_pc + i);
                        fprintf(stderr, " %02X", cpu->mem[(entry_pc + i) & 0xFFFF]);
                    }
                    fprintf(stderr, "\n    block bytes (from shadow mem):");
                    for (int i = 0; i < 48; i++) {
                        if ((i & 0xF) == 0) fprintf(stderr, "\n      %04X:", entry_pc + i);
                        fprintf(stderr, " %02X", dbt->shadow_mem[(entry_pc + i) & 0xFFFF]);
                    }
                    fprintf(stderr, "\n");
                    return -1;
                }
                continue;
            }

            dbt->jit_block_entries++;
            trampoline(cpu, cpu->mem, code, dbt->cache, dbt->code_bitmap);
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
        if (dbt->verify) {
            /* Keep the shadow in lockstep on the interp-fallback path too —
             * the real cpu just advanced via z80_step, so the shadow must
             * advance the same one step. */
            int srx = z80_step(&dbt->shadow_cpu);
            (void)srx;
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
    fprintf(out, "  SMC invalidations:      %llu\n",
            (unsigned long long)dbt->smc_invalidations);
}
