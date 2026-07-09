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

/* Result-indexed flag tables for inline-emitted ALU flag code. The
 * trampoline binds X24 to z80_f_tables so a translated op can fetch
 * the result-dependent flag bits with one LDRB [X24, res(+seg)].
 *   LOGIC : S|Z|XY|parity          (AND/OR/XOR; AND additionally ORs H)
 *   SZXY  : S|Z|XY                 (ADD/ADC/SUB/SBC/CP skeleton)
 *   INC   : S|Z|XY|H(nib==0)|PV(res==0x80)          — OR in old C
 *   DEC   : S|Z|XY|N|H(nib==0xF)|PV(res==0x7F)      — OR in old C */
#define FT_LOGIC   0
#define FT_SZXY  256
#define FT_INC   512
#define FT_DEC   768
extern uint8_t z80_f_tables[1024];
void z80_flag_tables_init(void);

/* 8-bit ALU writing A. ADC/SBC also read the carry input from cpu->f.
 * NOTE: the AArch64 backend now emits these inline (see emit_alu_inline
 * in dbt_a64.c); the helpers remain as the reference implementation and
 * for future backends. */
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

/* DAA — decimal adjust A after BCD add/subtract. Reads C/H/N from
 * cpu->f, rewrites A and all flags. Mirrors the Z80_OP_DAA case in
 * core/z80_interp.c. */
void z80_jit_daa(z80_cpu_t *cpu);

/* CB-prefix rotate/shift family. Take val, return new val, set
 * C/S/Z/PV/X/Y from result (or shifted-out bit for C); H=0, N=0.
 * Mirrors the CB body of core/z80_interp.c. */
uint8_t z80_jit_rlc(z80_cpu_t *cpu, uint8_t val);
uint8_t z80_jit_rrc(z80_cpu_t *cpu, uint8_t val);
uint8_t z80_jit_rl (z80_cpu_t *cpu, uint8_t val);
uint8_t z80_jit_rr (z80_cpu_t *cpu, uint8_t val);
uint8_t z80_jit_sla(z80_cpu_t *cpu, uint8_t val);
uint8_t z80_jit_sra(z80_cpu_t *cpu, uint8_t val);
uint8_t z80_jit_sll(z80_cpu_t *cpu, uint8_t val);   /* undocumented */
uint8_t z80_jit_srl(z80_cpu_t *cpu, uint8_t val);

/* CB-prefix BIT n,<src>. Flag-only (no writeback). C preserved; H=1, N=0;
 * Z = PV = !bit; S = (bit && n==7). XY come from xy_byte, which the JIT
 * passes as val (register form) or memptr.high (HL form). */
void z80_jit_bit(z80_cpu_t *cpu, uint8_t val, uint8_t bit_n, uint8_t xy_byte);

/* LDIR / LDDR — host-memmove intrinsics. Runs the entire block copy in
 * one go (matches the interp, which also bumps insn_count by 1 per
 * LDIR/LDDR regardless of BC). SMC is handled by a single batched
 * invalidation pass after the copy: if any destination byte was in the
 * code bitmap, the cache slots whose start could cover that byte are
 * cleared. BC==0 is a no-op (matches interp); the real Z80 would copy
 * 64K, but our interp doesn't and we keep parity with -V.
 *
 * Flags on completion: S/Z/C preserved, H=N=PV=0, XY = bits 3/1 of
 * (A + last_byte_transferred). */
void z80_jit_ldir(z80_cpu_t *cpu);
void z80_jit_lddr(z80_cpu_t *cpu);

/* Self-modifying-code detector. Called after every JIT-emitted guest
 * store. Checks dbt->code_bitmap[addr]: if the byte lay inside a
 * currently-cached translated block, blow away the entire cache so the
 * next block lookup forces a fresh translation. Cheap when SMC is
 * absent (one memory load + branch); pays the full cache wipe when
 * SMC actually fires. */
void z80_jit_post_store(z80_cpu_t *cpu, uint16_t addr);

#endif /* DBT_FLAGS_H */
