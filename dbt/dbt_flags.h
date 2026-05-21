/* dbt_flags.h — eager flag-computation helpers, callable from JIT blocks.
 *
 * Each helper has the AAPCS64 calling convention so the translator can
 * MOV X0, X19 ; MOVZ/K X9, <addr> ; BLR X9 around it. All write cpu->a
 * (where the op stores into A) and cpu->f, and set cpu->q = 1.
 *
 * Semantics mirror set_flags_add/sub/cp/logic in core/z80_interp.c —
 * keep them in lock-step or the JIT will diverge from the interpreter
 * on zex* / WordStar etc.
 */
#ifndef DBT_FLAGS_H
#define DBT_FLAGS_H

#include "../core/z80.h"
#include <stdint.h>

/* 8-bit ALU writing A. ADC/SBC also read the carry input from cpu->f. */
void z80_jit_add(z80_cpu_t *cpu, uint8_t b);
void z80_jit_adc(z80_cpu_t *cpu, uint8_t b);
void z80_jit_sub(z80_cpu_t *cpu, uint8_t b);
void z80_jit_sbc(z80_cpu_t *cpu, uint8_t b);
void z80_jit_and(z80_cpu_t *cpu, uint8_t b);
void z80_jit_or (z80_cpu_t *cpu, uint8_t b);
void z80_jit_xor(z80_cpu_t *cpu, uint8_t b);
void z80_jit_cp (z80_cpu_t *cpu, uint8_t b);   /* like SUB but A unchanged */

/* INC/DEC of an 8-bit register. C is preserved, all other flags computed
 * from the new value (and v itself for half-carry / PV overflow detection).
 * Returns the new value so the JIT can STRB it back to the register slot. */
uint8_t z80_jit_inc8(z80_cpu_t *cpu, uint8_t v);
uint8_t z80_jit_dec8(z80_cpu_t *cpu, uint8_t v);

/* Self-modifying-code detector. Called after every JIT-emitted guest
 * store. Checks dbt->code_bitmap[addr]: if the byte lay inside a
 * currently-cached translated block, blow away the entire cache so the
 * next block lookup forces a fresh translation. Cheap when SMC is
 * absent (one memory load + branch); pays the full cache wipe when
 * SMC actually fires. */
void z80_jit_post_store(z80_cpu_t *cpu, uint16_t addr);

#endif /* DBT_FLAGS_H */
