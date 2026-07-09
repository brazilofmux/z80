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

/* ---- Result-indexed flag tables, bound to X24 inside JIT blocks.
 *
 * Everything an 8-bit op's F byte needs that depends ONLY on the result
 * byte lives here, so inline-emitted ALU code gets S/Z/XY (+parity, or
 * +H/PV for INC/DEC) with a single LDRB instead of a helper call.
 * Layout (dbt_flags.h): LOGIC +0, SZXY +256, INC +512, DEC +768. */
uint8_t z80_f_tables[1024];

void z80_flag_tables_init(void) {
    for (int r = 0; r < 256; r++) {
        uint8_t szxy = xy_from((uint8_t)r);
        if (r == 0)     szxy |= Z80_FLAG_Z;
        if (r & 0x80)   szxy |= Z80_FLAG_S;
        z80_f_tables[FT_LOGIC + r] = szxy | parity8((uint8_t)r);
        z80_f_tables[FT_SZXY  + r] = szxy;
        z80_f_tables[FT_INC   + r] = szxy
            | (((r & 0xF) == 0x0) ? Z80_FLAG_H  : 0)
            | ((r == 0x80)        ? Z80_FLAG_PV : 0);
        z80_f_tables[FT_DEC   + r] = szxy | Z80_FLAG_N
            | (((r & 0xF) == 0xF) ? Z80_FLAG_H  : 0)
            | ((r == 0x7F)        ? Z80_FLAG_PV : 0);
    }
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

/* DAA — byte-for-byte mirror of the interp's Z80_OP_DAA case. */
void z80_jit_daa(z80_cpu_t *cpu) {
    uint8_t a = cpu->a;
    uint8_t fcin = cpu->f & Z80_FLAG_C;
    uint8_t fhin = cpu->f & Z80_FLAG_H;
    uint8_t fnin = cpu->f & Z80_FLAG_N;
    uint8_t corr = 0;
    uint8_t new_c = 0;

    if ((a & 0x0F) > 9 || fhin) corr |= 0x06;
    if (a > 0x99 || fcin)       { corr |= 0x60; new_c = Z80_FLAG_C; }

    uint8_t new_a;
    uint8_t new_h;
    if (fnin) {
        new_a = a - corr;
        new_h = (fhin && (a & 0x0F) < 6) ? Z80_FLAG_H : 0;
    } else {
        new_a = a + corr;
        new_h = ((a & 0x0F) > 9) ? Z80_FLAG_H : 0;
    }

    cpu->a = new_a;
    uint8_t f = fnin | new_c | new_h | xy_from(new_a);
    if (new_a == 0) f |= Z80_FLAG_Z;
    if (new_a & 0x80) f |= Z80_FLAG_S;
    f |= parity8(new_a);
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

/* ---- CB-prefix rotate / shift family.
 *
 * Each helper takes the current byte value, computes the new value and
 * the C bit that was shifted out, and writes F with the unified flag
 * rule:  F = (new_c ? C : 0) | (result==0 ? Z : 0) | (result&0x80 ? S : 0)
 *            | parity8(result) | xy_from(result);    // H=0, N=0
 * The JIT only needs one BLR per CB rotate/shift, just like 8-bit ALU. */
static inline uint8_t rotshift_flags(uint8_t result, uint8_t new_c) {
    uint8_t f = xy_from(result) | parity8(result);
    if (new_c)         f |= Z80_FLAG_C;
    if (result == 0)   f |= Z80_FLAG_Z;
    if (result & 0x80) f |= Z80_FLAG_S;
    return f;
}

uint8_t z80_jit_rlc(z80_cpu_t *cpu, uint8_t val) {
    uint8_t new_c = (val >> 7) & 1;
    uint8_t res = (uint8_t)((val << 1) | new_c);
    cpu->f = rotshift_flags(res, new_c);
    cpu->q = 1;
    return res;
}

uint8_t z80_jit_rrc(z80_cpu_t *cpu, uint8_t val) {
    uint8_t new_c = val & 1;
    uint8_t res = (uint8_t)((val >> 1) | (new_c ? 0x80 : 0));
    cpu->f = rotshift_flags(res, new_c);
    cpu->q = 1;
    return res;
}

uint8_t z80_jit_rl(z80_cpu_t *cpu, uint8_t val) {
    uint8_t c_in = (cpu->f & Z80_FLAG_C) ? 1 : 0;
    uint8_t new_c = (val >> 7) & 1;
    uint8_t res = (uint8_t)((val << 1) | c_in);
    cpu->f = rotshift_flags(res, new_c);
    cpu->q = 1;
    return res;
}

uint8_t z80_jit_rr(z80_cpu_t *cpu, uint8_t val) {
    uint8_t c_in = (cpu->f & Z80_FLAG_C) ? 1 : 0;
    uint8_t new_c = val & 1;
    uint8_t res = (uint8_t)((val >> 1) | (c_in ? 0x80 : 0));
    cpu->f = rotshift_flags(res, new_c);
    cpu->q = 1;
    return res;
}

uint8_t z80_jit_sla(z80_cpu_t *cpu, uint8_t val) {
    uint8_t new_c = (val >> 7) & 1;
    uint8_t res = (uint8_t)(val << 1);
    cpu->f = rotshift_flags(res, new_c);
    cpu->q = 1;
    return res;
}

uint8_t z80_jit_sra(z80_cpu_t *cpu, uint8_t val) {
    uint8_t new_c = val & 1;
    uint8_t res = (uint8_t)((val >> 1) | (val & 0x80));
    cpu->f = rotshift_flags(res, new_c);
    cpu->q = 1;
    return res;
}

/* SLL — undocumented "shift left, set bit 0". Real silicon does this; some
 * software depends on it (and zexall checks it). */
uint8_t z80_jit_sll(z80_cpu_t *cpu, uint8_t val) {
    uint8_t new_c = (val >> 7) & 1;
    uint8_t res = (uint8_t)((val << 1) | 1);
    cpu->f = rotshift_flags(res, new_c);
    cpu->q = 1;
    return res;
}

uint8_t z80_jit_srl(z80_cpu_t *cpu, uint8_t val) {
    uint8_t new_c = val & 1;
    uint8_t res = (uint8_t)(val >> 1);
    cpu->f = rotshift_flags(res, new_c);
    cpu->q = 1;
    return res;
}

/* BIT n,<src>. C preserved; H=1, N=0; Z=PV=!bit; S=(bit && n==7).
 * XY from caller-supplied xy_byte (val for register form, memptr.high
 * for (HL) form). */
void z80_jit_bit(z80_cpu_t *cpu, uint8_t val, uint8_t bit_n, uint8_t xy_byte) {
    uint8_t mask = (uint8_t)(1u << (bit_n & 7));
    uint8_t b = (val & mask) ? 1 : 0;
    uint8_t f = (cpu->f & Z80_FLAG_C) | Z80_FLAG_H | xy_from(xy_byte);
    if (!b) f |= Z80_FLAG_Z | Z80_FLAG_PV;
    if (b && (bit_n & 7) == 7) f |= Z80_FLAG_S;
    cpu->f = f;
    cpu->q = 1;
}

/* ---- LDIR / LDDR — host-memmove intrinsics.
 *
 * The interp does the same loop atomically (one Z80_step call covers
 * the whole transfer regardless of BC) and bumps insn_count by 1, so
 * we match that exactly. The block copy itself is byte-at-a-time
 * because LDIR's documented overlap semantics (DE = HL + 1) deliberately
 * propagate the source byte through the destination range — a plain
 * memmove would preserve the original bytes and break that idiom.
 *
 * SMC: a single bitmap scan over the destination range catches the
 * case where the copy lands on code we've translated. If any byte was
 * code, we do one window-sized cache sweep instead of paying the
 * per-byte SMC helper.
 *
 * BC=0: the interp's `while (bc != 0)` skips the body entirely.
 * Documented Z80 behaviour is "copy 65536 bytes" but we match interp
 * so -V stays clean. */
static void ldir_lddr_common(z80_cpu_t *cpu, int incr) {
    uint16_t bc = cpu->bc;
    uint16_t hl = cpu->hl;
    uint16_t de = cpu->de;
    uint8_t  last_byte = 0;

    if (bc != 0) {
        uint32_t count = bc;
        for (uint32_t i = 0; i < count; i++) {
            uint16_t s = (uint16_t)(hl + (uint16_t)(incr * (int32_t)i));
            uint16_t d = (uint16_t)(de + (uint16_t)(incr * (int32_t)i));
            last_byte = cpu->mem[s];
            cpu->mem[d] = last_byte;
        }
    }

    /* Register updates: HL, DE move by (incr * bc); BC ends at 0. */
    cpu->hl = (uint16_t)(hl + (uint16_t)(incr * (int32_t)bc));
    cpu->de = (uint16_t)(de + (uint16_t)(incr * (int32_t)bc));
    cpu->bc = 0;

    /* Flags */
    uint8_t n = (uint8_t)(cpu->a + last_byte);
    uint8_t f = cpu->f & (Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_C);
    f |= (n & Z80_FLAG_3);              /* X = bit 3 of n */
    if (n & 0x02) f |= Z80_FLAG_5;      /* Y = bit 1 of n */
    cpu->f = f;
    cpu->q = 1;

    /* SMC: did we overwrite any byte that lives in a cached block? */
    if (bc == 0 || !cpu->dbt) return;
    z80_dbt_t *dbt = (z80_dbt_t *)cpu->dbt;

    /* Walk the destination range looking for any bitmap hit. */
    int any_code = 0;
    {
        int32_t step = incr;
        uint32_t count = bc;
        for (uint32_t i = 0; i < count; i++) {
            uint16_t d = (uint16_t)(de + (uint16_t)(step * (int32_t)i));
            if (dbt->code_bitmap[d]) { any_code = 1; break; }
        }
    }
    if (!any_code) return;

    /* One sweep over the affected range plus the max-block window: any
     * cache slot whose start address is in [dst_lo - window + 1, dst_hi]
     * could cover a byte we just rewrote. */
    uint32_t window = dbt->max_block_bytes;
    uint16_t dst_lo = (incr > 0) ? de : (uint16_t)(de - (bc - 1));
    uint32_t total = (uint32_t)bc + window;
    for (uint32_t k = 0; k < total; k++) {
        uint16_t p = (uint16_t)(dst_lo + k - window + 1);
        z80_block_entry_t *e = &dbt->cache[p & BLOCK_CACHE_MASK];
        if (e->guest_pc == BLOCK_EMPTY_PC) continue;
        /* Distance from block start p up to the first written byte;
         * zero once p is inside the written range. The block covers a
         * written byte iff its span reaches past that gap (1:1 direct
         * mapping — see dbt_invalidate_for_store). */
        uint32_t gap = (window - 1 > k) ? (window - 1 - k) : 0;
        if (gap >= e->span) continue;
        e->guest_pc    = BLOCK_EMPTY_PC;
        e->native_code = NULL;
        dbt_links_repatch(dbt, p, NULL);   /* see dbt_invalidate_for_store */
    }
    dbt->smc_invalidations++;
}

void z80_jit_ldir(z80_cpu_t *cpu) { ldir_lddr_common(cpu, +1); }
void z80_jit_lddr(z80_cpu_t *cpu) { ldir_lddr_common(cpu, -1); }

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
