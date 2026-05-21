/* dbt_flags.c — eager flag-computation helpers for JIT blocks.
 *
 * Each helper is a plain C function the JIT calls via BLR after setting
 * X0=cpu and W1=operand. Semantics are byte-for-byte mirror of the
 * static helpers in core/z80_interp.c — set_flags_add / set_flags_sub /
 * set_flags_cp / set_flags_logic + parity8 + xy_from. If those diverge,
 * zex* will catch it. */
#include "dbt_flags.h"
#include "../core/z80.h"
#include "dbt.h"

static inline uint8_t xy_from(uint8_t b) {
    return b & (Z80_FLAG_5 | Z80_FLAG_3);
}

static inline uint8_t parity8(uint8_t v) {
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (v & 1) ? 0 : Z80_FLAG_PV;
}

void z80_jit_add(z80_cpu_t *cpu, uint8_t b) {
    uint8_t a = cpu->a;
    uint8_t res = (uint8_t)(a + b);
    uint8_t f = xy_from(res);
    if (res == 0) f |= Z80_FLAG_Z;
    if (res & 0x80) f |= Z80_FLAG_S;
    if (((a & 0xF) + (b & 0xF)) > 0xF) f |= Z80_FLAG_H;
    if ((uint16_t)a + (uint16_t)b > 0xFF) f |= Z80_FLAG_C;
    if (((a ^ b) & 0x80) == 0 && ((res ^ a) & 0x80)) f |= Z80_FLAG_PV;
    cpu->f = f;
    cpu->a = res;
    cpu->q = 1;
}

void z80_jit_adc(z80_cpu_t *cpu, uint8_t b) {
    uint8_t a = cpu->a;
    uint8_t c_in = cpu->f & Z80_FLAG_C;     /* 0 or 1 */
    uint16_t sum = (uint16_t)a + (uint16_t)b + c_in;
    uint8_t res = (uint8_t)sum;
    uint8_t f = xy_from(res);
    if (res == 0) f |= Z80_FLAG_Z;
    if (res & 0x80) f |= Z80_FLAG_S;
    if (((a & 0xF) + (b & 0xF) + c_in) > 0xF) f |= Z80_FLAG_H;
    if (sum > 0xFF) f |= Z80_FLAG_C;
    if (((a ^ b) & 0x80) == 0 && ((res ^ a) & 0x80)) f |= Z80_FLAG_PV;
    cpu->f = f;
    cpu->a = res;
    cpu->q = 1;
}

void z80_jit_sub(z80_cpu_t *cpu, uint8_t b) {
    uint8_t a = cpu->a;
    uint8_t res = (uint8_t)(a - b);
    uint8_t f = Z80_FLAG_N | xy_from(res);
    if (res == 0) f |= Z80_FLAG_Z;
    if (res & 0x80) f |= Z80_FLAG_S;
    if ((a & 0xF) < (b & 0xF)) f |= Z80_FLAG_H;
    if (a < b) f |= Z80_FLAG_C;
    if (((a ^ b) & 0x80) && ((res ^ a) & 0x80)) f |= Z80_FLAG_PV;
    cpu->f = f;
    cpu->a = res;
    cpu->q = 1;
}

void z80_jit_sbc(z80_cpu_t *cpu, uint8_t b) {
    uint8_t a = cpu->a;
    uint8_t c_in = cpu->f & Z80_FLAG_C;
    int16_t diff = (int16_t)a - (int16_t)b - (int16_t)c_in;
    uint8_t res = (uint8_t)diff;
    uint8_t f = Z80_FLAG_N | xy_from(res);
    if (res == 0) f |= Z80_FLAG_Z;
    if (res & 0x80) f |= Z80_FLAG_S;
    if (((int)(a & 0xF) - (int)(b & 0xF) - (int)c_in) < 0) f |= Z80_FLAG_H;
    if (diff < 0) f |= Z80_FLAG_C;
    if (((a ^ b) & 0x80) && ((res ^ a) & 0x80)) f |= Z80_FLAG_PV;
    cpu->f = f;
    cpu->a = res;
    cpu->q = 1;
}

void z80_jit_and(z80_cpu_t *cpu, uint8_t b) {
    uint8_t res = cpu->a & b;
    uint8_t f = Z80_FLAG_H | xy_from(res) | parity8(res);
    if (res == 0)   f |= Z80_FLAG_Z;
    if (res & 0x80) f |= Z80_FLAG_S;
    cpu->f = f;
    cpu->a = res;
    cpu->q = 1;
}

void z80_jit_or(z80_cpu_t *cpu, uint8_t b) {
    uint8_t res = cpu->a | b;
    uint8_t f = xy_from(res) | parity8(res);
    if (res == 0)   f |= Z80_FLAG_Z;
    if (res & 0x80) f |= Z80_FLAG_S;
    cpu->f = f;
    cpu->a = res;
    cpu->q = 1;
}

void z80_jit_xor(z80_cpu_t *cpu, uint8_t b) {
    uint8_t res = cpu->a ^ b;
    uint8_t f = xy_from(res) | parity8(res);
    if (res == 0)   f |= Z80_FLAG_Z;
    if (res & 0x80) f |= Z80_FLAG_S;
    cpu->f = f;
    cpu->a = res;
    cpu->q = 1;
}

/* CP A,b — like SUB but A unchanged. XY come from the OPERAND b, not the
 * result. (Documented Z80 quirk; zex* checks this — see set_flags_cp.) */
void z80_jit_cp(z80_cpu_t *cpu, uint8_t b) {
    uint8_t a = cpu->a;
    uint8_t res = (uint8_t)(a - b);
    uint8_t f = Z80_FLAG_N | xy_from(b);
    if (res == 0) f |= Z80_FLAG_Z;
    if (res & 0x80) f |= Z80_FLAG_S;
    if ((a & 0xF) < (b & 0xF)) f |= Z80_FLAG_H;
    if (a < b) f |= Z80_FLAG_C;
    if (((a ^ b) & 0x80) && ((res ^ a) & 0x80)) f |= Z80_FLAG_PV;
    cpu->f = f;
    cpu->q = 1;
}

uint8_t z80_jit_inc8(z80_cpu_t *cpu, uint8_t v) {
    uint8_t res = (uint8_t)(v + 1);
    uint8_t f = (cpu->f & Z80_FLAG_C) | xy_from(res);
    if (res == 0) f |= Z80_FLAG_Z;
    if (res & 0x80) f |= Z80_FLAG_S;
    if ((v & 0xF) == 0xF) f |= Z80_FLAG_H;
    if (v == 0x7F) f |= Z80_FLAG_PV;        /* +127 -> -128 */
    cpu->f = f;
    cpu->q = 1;
    return res;
}

uint8_t z80_jit_dec8(z80_cpu_t *cpu, uint8_t v) {
    uint8_t res = (uint8_t)(v - 1);
    uint8_t f = Z80_FLAG_N | (cpu->f & Z80_FLAG_C) | xy_from(res);
    if (res == 0) f |= Z80_FLAG_Z;
    if (res & 0x80) f |= Z80_FLAG_S;
    if ((v & 0xF) == 0x00) f |= Z80_FLAG_H;
    if (v == 0x80) f |= Z80_FLAG_PV;        /* -128 -> +127 */
    cpu->f = f;
    cpu->q = 1;
    return res;
}

/* Called from JIT-emitted store sequences after a guest byte store. The
 * inline LDRB+CBZ around the call already short-circuits the no-SMC
 * case, so by the time we get here we know we have a real hit and the
 * invalidation must run. Delegate to z80_mem_w's shared invalidation
 * implementation in block_cache.c — but we've already done the store,
 * so just re-call it as if we were a one-byte interp write. */
void z80_jit_post_store(z80_cpu_t *cpu, uint16_t addr) {
    /* z80_mem_w would re-do the store, but cpu->mem[addr] is already
     * the new value; doing it once more is a no-op write. The whole
     * point of this entry point is to share the invalidation. */
    z80_mem_w(cpu, addr, cpu->mem[addr & 0xFFFF]);
}
