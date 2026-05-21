/* z80_interp.c — Reference interpreter (golden model)
 *
 * This must be 100% correct (even if slow). The DBT will be validated
 * against it using the -V shadow mode, exactly like the riscv/dbt does.
 *
 * For the larval stage we implement only the instructions needed to:
 *   - Load a tiny .COM
 *   - Execute the classic "LD DE,msg; LD C,9; CALL 5; RET" hello
 *   - Hit the BDOS shim for function 9 (print $-terminated string)
 *
 * Everything else will print a loud "UNIMPLEMENTED" and stop.
 */

#include "z80.h"
#include "../cpm/cpm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* 8-bit register access helpers (the classic 0-7 encoding) */
/* Register code map:
 *   0..5,7 = B,C,D,E,H,L,A (standard 8-bit registers)
 *   6      = (HL) / (IX+d) / (IY+d), handled by callers using effective_addr
 *   8      = IXH or IYH (high byte of the prefix-selected index register)
 *   9      = IXL or IYL
 *
 * Codes 8/9 are only ever emitted by the decoder when a DD/FD prefix is
 * active and the instruction is a pure register form (no memory operand).
 * The decoder picks IXH vs IYH based on the prefix — the helpers below
 * resolve which byte to access via dec->prefix. */
static inline uint8_t *reg8_ptr_p(z80_cpu_t *cpu, int r, uint8_t prefix) {
    switch (r) {
    case 0: return &cpu->b;
    case 1: return &cpu->c;
    case 2: return &cpu->d;
    case 3: return &cpu->e;
    case 4: return &cpu->h;
    case 5: return &cpu->l;
    case 7: return &cpu->a;
    case 8: {
        uint16_t *p = (prefix == 0xFD) ? &cpu->iy : &cpu->ix;
        return ((uint8_t *)p) + 1;   /* high byte */
    }
    case 9: {
        uint16_t *p = (prefix == 0xFD) ? &cpu->iy : &cpu->ix;
        return (uint8_t *)p;         /* low byte */
    }
    default: return NULL;
    }
}

static inline uint8_t *reg8_ptr(z80_cpu_t *cpu, int r) {
    return reg8_ptr_p(cpu, r, 0);
}

/* Return the effective address for a memory operand, taking DD/FD prefix into account */
static inline uint16_t effective_addr(z80_cpu_t *cpu, const z80_decoded *dec, uint16_t hl_fallback) {
    if (dec->prefix == 0xDD) return cpu->ix + (int16_t)dec->disp;
    if (dec->prefix == 0xFD) return cpu->iy + (int16_t)dec->disp;
    return hl_fallback;
}

static inline uint8_t read_reg8(z80_cpu_t *cpu, int r, const z80_decoded *dec) {
    if (r == 6) { /* (HL) or (IX+d) or (IY+d) */
        uint16_t addr = effective_addr(cpu, dec, cpu->hl);
        return cpu->mem[addr & 0xFFFF];
    }
    uint8_t *p = reg8_ptr_p(cpu, r, dec->prefix);
    return p ? *p : 0;
}

static inline void write_reg8(z80_cpu_t *cpu, int r, uint8_t val, const z80_decoded *dec) {
    if (r == 6) {
        uint16_t addr = effective_addr(cpu, dec, cpu->hl);
        z80_mem_w(cpu, addr, val);
        return;
    }
    uint8_t *p = reg8_ptr_p(cpu, r, dec->prefix);
    if (p) *p = val;
}

/* 16-bit register pair access (0=BC,1=DE,2=HL,3=SP or AF for PUSH/POP) */
static inline uint16_t read_rr(z80_cpu_t *cpu, int rr) {
    switch (rr) {
    case 0: return cpu->bc;
    case 1: return cpu->de;
    case 2: return cpu->hl;
    case 3: return cpu->sp;   /* careful: PUSH/POP AF uses rr==3 as AF */
    default: return 0;
    }
}

static inline void write_rr(z80_cpu_t *cpu, int rr, uint16_t val) {
    switch (rr) {
    case 0: cpu->bc = val; break;
    case 1: cpu->de = val; break;
    case 2: cpu->hl = val; break;
    case 3: cpu->sp = val; break;
    }
}

/* XY = bits 5 and 3 of `b`, packed into F's positions. */
static inline uint8_t xy_from(uint8_t b) { return b & (Z80_FLAG_5 | Z80_FLAG_3); }

/* Parity-even = 1 (P/V flag set), parity-odd = 0. */
static inline uint8_t parity8(uint8_t v) {
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (v & 1) ? 0 : Z80_FLAG_PV;
}

/* Flag helpers — simple eager version for the interpreter */
static inline void set_flags_add(z80_cpu_t *cpu, uint8_t a, uint8_t b, uint8_t res) {
    uint8_t f = xy_from(res);
    if (res == 0) f |= Z80_FLAG_Z;
    if (res & 0x80) f |= Z80_FLAG_S;
    if (((a & 0xF) + (b & 0xF)) > 0xF) f |= Z80_FLAG_H;
    if ((int16_t)a + (int16_t)b > 0xFF) f |= Z80_FLAG_C;
    /* P/V for addition is overflow */
    if (((a ^ b) & 0x80) == 0 && ((res ^ a) & 0x80)) f |= Z80_FLAG_PV;
    cpu->f = f;
    cpu->q = 1;
}

static inline void set_flags_sub(z80_cpu_t *cpu, uint8_t a, uint8_t b, uint8_t res) {
    uint8_t f = Z80_FLAG_N | xy_from(res);
    if (res == 0) f |= Z80_FLAG_Z;
    if (res & 0x80) f |= Z80_FLAG_S;
    if ((a & 0xF) < (b & 0xF)) f |= Z80_FLAG_H;
    if (a < b) f |= Z80_FLAG_C;
    /* Signed overflow for A-B: A,B have different signs AND result has
     * the sign of B (equivalently, sign differs from A). */
    if (((a ^ b) & 0x80) && ((res ^ a) & 0x80)) f |= Z80_FLAG_PV;
    cpu->f = f;
    cpu->q = 1;
}

/* CP A,b — same as SUB but XY come from the OPERAND, not the result.
 * (One of the documented Z80 quirks; zexall checks this.) */
static inline void set_flags_cp(z80_cpu_t *cpu, uint8_t a, uint8_t b, uint8_t res) {
    uint8_t f = Z80_FLAG_N | xy_from(b);
    if (res == 0) f |= Z80_FLAG_Z;
    if (res & 0x80) f |= Z80_FLAG_S;
    if ((a & 0xF) < (b & 0xF)) f |= Z80_FLAG_H;
    if (a < b) f |= Z80_FLAG_C;
    if (((a ^ b) & 0x80) && ((res ^ a) & 0x80)) f |= Z80_FLAG_PV;
    cpu->f = f;
    cpu->q = 1;
}

static inline void set_flags_logic(z80_cpu_t *cpu, uint8_t res) {
    uint8_t f = xy_from(res);
    if (res == 0) f |= Z80_FLAG_Z;
    if (res & 0x80) f |= Z80_FLAG_S;
    f |= parity8(res);
    cpu->f = f;
    cpu->q = 1;
}

/* Condition code test */
static inline int cond_true(z80_cpu_t *cpu, int cc) {
    switch (cc) {
    case 0: return !(cpu->f & Z80_FLAG_Z);   /* NZ */
    case 1: return  (cpu->f & Z80_FLAG_Z);   /* Z  */
    case 2: return !(cpu->f & Z80_FLAG_C);   /* NC */
    case 3: return  (cpu->f & Z80_FLAG_C);   /* C  */
    case 4: return !(cpu->f & Z80_FLAG_PV);  /* PO */
    case 5: return  (cpu->f & Z80_FLAG_PV);  /* PE */
    case 6: return !(cpu->f & Z80_FLAG_S);   /* P  */
    case 7: return  (cpu->f & Z80_FLAG_S);   /* M  */
    default: return 0;
    }
}

/* ========================================================================
 * The actual step function
 * ===================================================================== */
int z80_step(z80_cpu_t *cpu) {
    z80_decoded dec;
    int n = z80_decode_one(cpu->mem, cpu->pc, &dec);
    if (n == 0) {
        fprintf(stderr, "z80_step: decode failed at %04X\n", cpu->pc);
        return -1;
    }

    uint16_t old_pc = cpu->pc;
    cpu->pc = (cpu->pc + dec.bytes) & 0xFFFF;   /* default: advance */

    /* Save Q from the previous instruction (was-F-modified?) for the
     * SCF/CCF XY quirk. Default this instruction to Q=0; any case that
     * writes F must set cpu->q = 1. */
    uint8_t prev_q = cpu->q;
    cpu->q = 0;
    (void)prev_q;   /* used only by SCF/CCF */

    switch (dec.type) {
    case Z80_OP_NOP:
        break;

    case Z80_OP_LD_RR_NN:
        if (dec.reg1 == 3) { /* SP */
            cpu->sp = dec.imm16;
        } else if (dec.reg1 == 2 && dec.prefix == 0xDD) {
            cpu->ix = dec.imm16;
        } else if (dec.reg1 == 2 && dec.prefix == 0xFD) {
            cpu->iy = dec.imm16;
        } else {
            write_rr(cpu, dec.reg1, dec.imm16);
        }
        break;

    case Z80_OP_LD_R_N:
        write_reg8(cpu, dec.reg1, dec.imm8, &dec);
        break;

    case Z80_OP_LD_R_R:
        write_reg8(cpu, dec.reg1, read_reg8(cpu, dec.reg2, &dec), &dec);
        break;

    case Z80_OP_INC_HL_ind:
        {
            uint16_t a = effective_addr(cpu, &dec, cpu->hl);
            uint8_t v = cpu->mem[a] + 1;
            z80_mem_w(cpu, a, v);
            uint8_t f = (cpu->f & Z80_FLAG_C) | xy_from(v);
            if (v == 0) f |= Z80_FLAG_Z;
            if (v & 0x80) f |= Z80_FLAG_S;
            if ((v & 0xF) == 0) f |= Z80_FLAG_H;
            if (v == 0x80) f |= Z80_FLAG_PV;
            cpu->f = f;
            cpu->q = 1;
        }
        break;

    case Z80_OP_DEC_HL_ind:
        {
            uint16_t a = effective_addr(cpu, &dec, cpu->hl);
            uint8_t v = cpu->mem[a] - 1;
            z80_mem_w(cpu, a, v);
            uint8_t f = (cpu->f & Z80_FLAG_C) | Z80_FLAG_N | xy_from(v);
            if (v == 0) f |= Z80_FLAG_Z;
            if (v & 0x80) f |= Z80_FLAG_S;
            if ((v & 0xF) == 0xF) f |= Z80_FLAG_H;
            if (v == 0x7F) f |= Z80_FLAG_PV;
            cpu->f = f;
            cpu->q = 1;
        }
        break;

    case Z80_OP_LD_NN_A:
        z80_mem_w(cpu, dec.imm16, cpu->a);
        /* MEMPTR.low = (nn+1) & 0xFF, MEMPTR.high = A */
        cpu->memptr = ((dec.imm16 + 1) & 0xFF) | ((uint16_t)cpu->a << 8);
        break;

    case Z80_OP_LD_A_NN:
        cpu->a = cpu->mem[dec.imm16 & 0xFFFF];
        cpu->memptr = (uint16_t)(dec.imm16 + 1);
        break;

    case Z80_OP_LD_A_BC:
        cpu->a = cpu->mem[cpu->bc & 0xFFFF];
        cpu->memptr = (uint16_t)(cpu->bc + 1);
        break;

    case Z80_OP_LD_A_DE:
        cpu->a = cpu->mem[cpu->de & 0xFFFF];
        cpu->memptr = (uint16_t)(cpu->de + 1);
        break;

    /* IX/IY indexed memory forms (use the prefix/disp-aware helpers) */
    case Z80_OP_LD_A_HL_ind:
        cpu->a = read_reg8(cpu, 6, &dec);
        break;
    case Z80_OP_LD_HL_A_ind:
        write_reg8(cpu, 6, cpu->a, &dec);
        break;

    case Z80_OP_LD_R_HL_ind:
        write_reg8(cpu, dec.reg2, read_reg8(cpu, 6, &dec), &dec);
        break;

    case Z80_OP_LD_HL_R_ind:
        write_reg8(cpu, 6, read_reg8(cpu, dec.reg2, &dec), &dec);
        break;

    case Z80_OP_LD_HL_N_ind:
        write_reg8(cpu, 6, dec.imm8, &dec);
        break;

    case Z80_OP_LD_HL_N:   /* LD (HL), n */
        write_reg8(cpu, 6, dec.imm8, &dec);
        break;

    case Z80_OP_LD_DE_A:
        z80_mem_w(cpu, cpu->de, cpu->a);
        cpu->memptr = ((cpu->de + 1) & 0xFF) | ((uint16_t)cpu->a << 8);
        break;

    case Z80_OP_LD_BC_A:
        z80_mem_w(cpu, cpu->bc, cpu->a);
        cpu->memptr = ((cpu->bc + 1) & 0xFF) | ((uint16_t)cpu->a << 8);
        break;

    case Z80_OP_INC_R:
        if (dec.reg1 == 6) { /* (HL) or (IX+d)/(IY+d) */
            uint16_t a = effective_addr(cpu, &dec, cpu->hl);
            uint8_t v = cpu->mem[a] + 1;
            z80_mem_w(cpu, a, v);
            uint8_t f = (cpu->f & Z80_FLAG_C) | xy_from(v);
            if (v == 0) f |= Z80_FLAG_Z;
            if (v & 0x80) f |= Z80_FLAG_S;
            if ((v & 0xF) == 0) f |= Z80_FLAG_H;
            if (v == 0x80) f |= Z80_FLAG_PV;
            cpu->f = f;
            cpu->q = 1;
        } else {
            uint8_t *p = reg8_ptr_p(cpu, dec.reg1, dec.prefix);
            if (p) {
                uint8_t v = *p + 1;
                *p = v;
                uint8_t f = (cpu->f & Z80_FLAG_C) | xy_from(v);
                if (v == 0) f |= Z80_FLAG_Z;
                if (v & 0x80) f |= Z80_FLAG_S;
                if ((v & 0xF) == 0) f |= Z80_FLAG_H;
                if (v == 0x80) f |= Z80_FLAG_PV;
                cpu->f = f;
                cpu->q = 1;
            }
        }
        break;

    case Z80_OP_DEC_R:
        if (dec.reg1 == 6) {
            uint16_t a = effective_addr(cpu, &dec, cpu->hl);
            uint8_t v = cpu->mem[a] - 1;
            z80_mem_w(cpu, a, v);
            uint8_t f = (cpu->f & Z80_FLAG_C) | Z80_FLAG_N | xy_from(v);
            if (v == 0) f |= Z80_FLAG_Z;
            if (v & 0x80) f |= Z80_FLAG_S;
            if ((v & 0xF) == 0xF) f |= Z80_FLAG_H;
            if (v == 0x7F) f |= Z80_FLAG_PV;
            cpu->f = f;
            cpu->q = 1;
        } else {
            uint8_t *p = reg8_ptr_p(cpu, dec.reg1, dec.prefix);
            if (p) {
                uint8_t v = *p - 1;
                *p = v;
                uint8_t f = (cpu->f & Z80_FLAG_C) | Z80_FLAG_N | xy_from(v);
                if (v == 0) f |= Z80_FLAG_Z;
                if (v & 0x80) f |= Z80_FLAG_S;
                if ((v & 0xF) == 0xF) f |= Z80_FLAG_H;
                if (v == 0x7F) f |= Z80_FLAG_PV;
                cpu->f = f;
                cpu->q = 1;
            }
        }
        break;

    case Z80_OP_INC_RR:
        if (dec.reg1 == 3) cpu->sp++;
        else if (dec.reg1 == 2 && dec.prefix == 0xDD) cpu->ix++;
        else if (dec.reg1 == 2 && dec.prefix == 0xFD) cpu->iy++;
        else write_rr(cpu, dec.reg1, read_rr(cpu, dec.reg1) + 1);
        break;

    case Z80_OP_DEC_RR:
        if (dec.reg1 == 3) cpu->sp--;
        else if (dec.reg1 == 2 && dec.prefix == 0xDD) cpu->ix--;
        else if (dec.reg1 == 2 && dec.prefix == 0xFD) cpu->iy--;
        else write_rr(cpu, dec.reg1, read_rr(cpu, dec.reg1) - 1);
        break;

    case Z80_OP_ADD_A_R:
        {
            uint8_t b = read_reg8(cpu, dec.reg1, &dec);
            uint8_t res = cpu->a + b;
            set_flags_add(cpu, cpu->a, b, res);
            cpu->a = res;
        }
        break;

    case Z80_OP_ADD_A_N:
        {
            uint8_t res = cpu->a + dec.imm8;
            set_flags_add(cpu, cpu->a, dec.imm8, res);
            cpu->a = res;
        }
        break;

    case Z80_OP_ADD_A_HL_ind:
        {
            uint8_t b = read_reg8(cpu, 6, &dec);
            uint8_t res = cpu->a + b;
            set_flags_add(cpu, cpu->a, b, res);
            cpu->a = res;
        }
        break;

    case Z80_OP_SUB_A_HL_ind:
        {
            uint8_t b = read_reg8(cpu, 6, &dec);
            uint8_t res = cpu->a - b;
            set_flags_sub(cpu, cpu->a, b, res);
            cpu->a = res;
        }
        break;

    case Z80_OP_AND_A_HL_ind:
        cpu->a &= read_reg8(cpu, 6, &dec);
        set_flags_logic(cpu, cpu->a);
        cpu->f &= ~Z80_FLAG_N; cpu->f &= ~Z80_FLAG_C; cpu->f |= Z80_FLAG_H;
        break;

    case Z80_OP_XOR_A_HL_ind:
        cpu->a ^= read_reg8(cpu, 6, &dec);
        set_flags_logic(cpu, cpu->a);
        cpu->f &= ~Z80_FLAG_N; cpu->f &= ~Z80_FLAG_C; cpu->f &= ~Z80_FLAG_H;
        break;

    case Z80_OP_OR_A_HL_ind:
        cpu->a |= read_reg8(cpu, 6, &dec);
        set_flags_logic(cpu, cpu->a);
        cpu->f &= ~Z80_FLAG_N; cpu->f &= ~Z80_FLAG_C; cpu->f &= ~Z80_FLAG_H;
        break;

    case Z80_OP_CP_A_HL_ind:
        {
            uint8_t b = read_reg8(cpu, 6, &dec);
            uint8_t res = cpu->a - b;
            set_flags_cp(cpu, cpu->a, b, res);
        }
        break;

    case Z80_OP_SUB_A_R:
        {
            uint8_t b = read_reg8(cpu, dec.reg1, &dec);
            uint8_t res = cpu->a - b;
            set_flags_sub(cpu, cpu->a, b, res);
            cpu->a = res;
        }
        break;

    case Z80_OP_ADC_A_R:
    case Z80_OP_ADC_A_HL_ind:
        {
            uint8_t b = (dec.type == Z80_OP_ADC_A_HL_ind)
                            ? read_reg8(cpu, 6, &dec)
                            : read_reg8(cpu, dec.reg1, &dec);
            uint8_t cin = (cpu->f & Z80_FLAG_C) ? 1 : 0;
            uint16_t wide = (uint16_t)cpu->a + b + cin;
            uint8_t res = (uint8_t)wide;
            uint8_t f = xy_from(res);
            if (res == 0) f |= Z80_FLAG_Z;
            if (res & 0x80) f |= Z80_FLAG_S;
            if (((cpu->a & 0xF) + (b & 0xF) + cin) > 0xF) f |= Z80_FLAG_H;
            if (wide > 0xFF) f |= Z80_FLAG_C;
            if (((cpu->a ^ b) & 0x80) == 0 && ((res ^ cpu->a) & 0x80)) f |= Z80_FLAG_PV;
            cpu->f = f;
            cpu->q = 1;
            cpu->a = res;
        }
        break;

    case Z80_OP_ADC_A_N:
        {
            uint8_t b = dec.imm8;
            uint8_t cin = (cpu->f & Z80_FLAG_C) ? 1 : 0;
            uint16_t wide = (uint16_t)cpu->a + b + cin;
            uint8_t res = (uint8_t)wide;
            uint8_t f = xy_from(res);
            if (res == 0) f |= Z80_FLAG_Z;
            if (res & 0x80) f |= Z80_FLAG_S;
            if (((cpu->a & 0xF) + (b & 0xF) + cin) > 0xF) f |= Z80_FLAG_H;
            if (wide > 0xFF) f |= Z80_FLAG_C;
            if (((cpu->a ^ b) & 0x80) == 0 && ((res ^ cpu->a) & 0x80)) f |= Z80_FLAG_PV;
            cpu->f = f;
            cpu->q = 1;
            cpu->a = res;
        }
        break;

    case Z80_OP_SBC_A_R:
    case Z80_OP_SBC_A_HL_ind:
        {
            uint8_t b = (dec.type == Z80_OP_SBC_A_HL_ind)
                            ? read_reg8(cpu, 6, &dec)
                            : read_reg8(cpu, dec.reg1, &dec);
            uint8_t cin = (cpu->f & Z80_FLAG_C) ? 1 : 0;
            int wide = (int)cpu->a - b - cin;
            uint8_t res = (uint8_t)wide;
            uint8_t f = Z80_FLAG_N | xy_from(res);
            if (res == 0) f |= Z80_FLAG_Z;
            if (res & 0x80) f |= Z80_FLAG_S;
            if (((cpu->a & 0xF) - (b & 0xF) - cin) & 0x10) f |= Z80_FLAG_H;
            if (wide < 0) f |= Z80_FLAG_C;
            if (((cpu->a ^ b) & 0x80) && ((res ^ cpu->a) & 0x80)) f |= Z80_FLAG_PV;
            cpu->f = f;
            cpu->q = 1;
            cpu->a = res;
        }
        break;

    case Z80_OP_SBC_A_N:
        {
            uint8_t b = dec.imm8;
            uint8_t cin = (cpu->f & Z80_FLAG_C) ? 1 : 0;
            int wide = (int)cpu->a - b - cin;
            uint8_t res = (uint8_t)wide;
            uint8_t f = Z80_FLAG_N | xy_from(res);
            if (res == 0) f |= Z80_FLAG_Z;
            if (res & 0x80) f |= Z80_FLAG_S;
            if (((cpu->a & 0xF) - (b & 0xF) - cin) & 0x10) f |= Z80_FLAG_H;
            if (wide < 0) f |= Z80_FLAG_C;
            if (((cpu->a ^ b) & 0x80) && ((res ^ cpu->a) & 0x80)) f |= Z80_FLAG_PV;
            cpu->f = f;
            cpu->q = 1;
            cpu->a = res;
        }
        break;

    case Z80_OP_EX_SP_HL:
        {
            /* DD/FD prefix selects IX/IY for this opcode. */
            uint8_t lo = cpu->mem[cpu->sp];
            uint8_t hi = cpu->mem[(cpu->sp + 1) & 0xFFFF];
            uint16_t *dst = (dec.prefix == 0xDD) ? &cpu->ix
                          : (dec.prefix == 0xFD) ? &cpu->iy
                          : &cpu->hl;
            z80_mem_w(cpu, cpu->sp, (uint8_t)(*dst & 0xFF));
            z80_mem_w(cpu, (uint16_t)(cpu->sp + 1), (uint8_t)(*dst >> 8));
            *dst = lo | ((uint16_t)hi << 8);
            cpu->memptr = *dst;
        }
        break;

    case Z80_OP_DI:
        cpu->iff1 = 0; cpu->iff2 = 0;
        break;

    case Z80_OP_EI:
        cpu->iff1 = 1; cpu->iff2 = 1;
        break;

    case Z80_OP_HALT:
        /* No interrupt sources implemented — treat as terminate so we
         * don't spin forever, but loud so we notice. */
        fprintf(stderr, "z80_step: HALT at %04X (no IRQ system yet) — stopping\n", old_pc);
        return -1;

    case Z80_OP_SUB_A_N:
        {
            uint8_t res = cpu->a - dec.imm8;
            set_flags_sub(cpu, cpu->a, dec.imm8, res);
            cpu->a = res;
        }
        break;

    case Z80_OP_AND_A_R:
        cpu->a &= read_reg8(cpu, dec.reg1, &dec);
        set_flags_logic(cpu, cpu->a);
        cpu->f &= ~Z80_FLAG_N; cpu->f &= ~Z80_FLAG_C; cpu->f |= Z80_FLAG_H;
        break;

    case Z80_OP_AND_A_N:
        cpu->a &= dec.imm8;
        set_flags_logic(cpu, cpu->a);
        cpu->f &= ~Z80_FLAG_N; cpu->f &= ~Z80_FLAG_C; cpu->f |= Z80_FLAG_H;
        break;

    case Z80_OP_OR_A_R:
        cpu->a |= read_reg8(cpu, dec.reg1, &dec);
        set_flags_logic(cpu, cpu->a);
        cpu->f &= ~Z80_FLAG_N; cpu->f &= ~Z80_FLAG_C; cpu->f &= ~Z80_FLAG_H;
        break;

    case Z80_OP_OR_A_N:
        cpu->a |= dec.imm8;
        set_flags_logic(cpu, cpu->a);
        cpu->f &= ~Z80_FLAG_N; cpu->f &= ~Z80_FLAG_C; cpu->f &= ~Z80_FLAG_H;
        break;

    case Z80_OP_XOR_A_R:
        cpu->a ^= read_reg8(cpu, dec.reg1, &dec);
        set_flags_logic(cpu, cpu->a);
        cpu->f &= ~Z80_FLAG_N; cpu->f &= ~Z80_FLAG_C; cpu->f &= ~Z80_FLAG_H;
        break;

    case Z80_OP_XOR_A_N:
        cpu->a ^= dec.imm8;
        set_flags_logic(cpu, cpu->a);
        cpu->f &= ~Z80_FLAG_N; cpu->f &= ~Z80_FLAG_C; cpu->f &= ~Z80_FLAG_H;
        break;

    case Z80_OP_CP_A_R:
        {
            uint8_t b = read_reg8(cpu, dec.reg1, &dec);
            uint8_t res = cpu->a - b;
            set_flags_cp(cpu, cpu->a, b, res);
        }
        break;

    case Z80_OP_CP_A_N:
        {
            uint8_t res = cpu->a - dec.imm8;
            set_flags_cp(cpu, cpu->a, dec.imm8, res);
        }
        break;

    case Z80_OP_JP_NN:
        cpu->pc = dec.imm16;
        cpu->memptr = dec.imm16;
        if (cpu->pc == CPM_BDOS_ENTRY) {
            int cont = cpm_bdos_dispatch(cpu);
            if (!cont) return 1;
            cpu->pc = cpu->mem[cpu->sp] | (cpu->mem[(cpu->sp + 1) & 0xFFFF] << 8);
            cpu->sp = (cpu->sp + 2) & 0xFFFF;
        } else if (cpu->pc >= CPM_BIOS_BASE && cpu->pc < CPM_BIOS_BASE + 0x80) {
            int cont = cpm_bios_dispatch(cpu);
            if (!cont) return 1;
            cpu->pc = cpu->mem[cpu->sp] | (cpu->mem[(cpu->sp + 1) & 0xFFFF] << 8);
            cpu->sp = (cpu->sp + 2) & 0xFFFF;
        }
        break;

    case Z80_OP_JP_CC_NN:
        /* MEMPTR = nn regardless of branch taken (per Sean Young). */
        cpu->memptr = dec.imm16;
        if (cond_true(cpu, dec.cc)) {
            cpu->pc = dec.imm16;
            if (cpu->pc == CPM_BDOS_ENTRY) {
                int cont = cpm_bdos_dispatch(cpu);
                if (!cont) return 1;
                cpu->pc = cpu->mem[cpu->sp] | (cpu->mem[(cpu->sp + 1) & 0xFFFF] << 8);
                cpu->sp = (cpu->sp + 2) & 0xFFFF;
            } else if (cpu->pc >= CPM_BIOS_BASE && cpu->pc < CPM_BIOS_BASE + 0x80) {
                int cont = cpm_bios_dispatch(cpu);
                if (!cont) return 1;
                cpu->pc = cpu->mem[cpu->sp] | (cpu->mem[(cpu->sp + 1) & 0xFFFF] << 8);
                cpu->sp = (cpu->sp + 2) & 0xFFFF;
            }
        }
        break;

    case Z80_OP_LD_SP_HL:
        /* F9: LD SP,HL (or LD SP,IX/IY with DD/FD prefix). */
        if (dec.prefix == 0xDD) cpu->sp = cpu->ix;
        else if (dec.prefix == 0xFD) cpu->sp = cpu->iy;
        else cpu->sp = cpu->hl;
        break;

    case Z80_OP_JP_HL:
        cpu->pc = cpu->hl;
        /* CP/M vector indirection (BDOS at 0005 or BIOS via 0001) */
        if (cpu->pc == CPM_BDOS_ENTRY) {
            int cont = cpm_bdos_dispatch(cpu);
            if (!cont) return 1;
            cpu->pc = cpu->mem[cpu->sp] | (cpu->mem[(cpu->sp + 1) & 0xFFFF] << 8);
            cpu->sp = (cpu->sp + 2) & 0xFFFF;
        } else if (cpu->pc >= CPM_BIOS_BASE && cpu->pc < CPM_BIOS_BASE + 0x80) {
            int cont = cpm_bios_dispatch(cpu);
            if (!cont) return 1;
            cpu->pc = cpu->mem[cpu->sp] | (cpu->mem[(cpu->sp + 1) & 0xFFFF] << 8);
            cpu->sp = (cpu->sp + 2) & 0xFFFF;
        }
        break;

    case Z80_OP_JR_E:
        cpu->pc = (old_pc + dec.bytes + dec.disp) & 0xFFFF;
        cpu->memptr = cpu->pc;
        break;

    case Z80_OP_JR_CC_E:
        if (cond_true(cpu, dec.cc)) {
            cpu->pc = (old_pc + dec.bytes + dec.disp) & 0xFFFF;
            cpu->memptr = cpu->pc;
        }
        break;

    case Z80_OP_CALL_NN:
        /* push return address */
        cpu->sp = (cpu->sp - 2) & 0xFFFF;
        z80_mem_w(cpu, cpu->sp, (uint8_t)(cpu->pc & 0xFF));
        z80_mem_w(cpu, (uint16_t)(cpu->sp + 1), (uint8_t)(cpu->pc >> 8));
        cpu->pc = dec.imm16;
        cpu->memptr = dec.imm16;

        /* CP/M BDOS / warm boot trap */
        if (cpu->pc == CPM_BDOS_ENTRY || cpu->pc == CPM_WBOOT_ENTRY) {
            int cont = cpm_bdos_dispatch(cpu);
            if (!cont) {
                /* Program terminated via BDOS 0 or unknown call */
                return 1;   /* positive = clean CP/M exit */
            }
            /* Simulate RET from the BDOS call */
            cpu->pc = cpu->mem[cpu->sp] | (cpu->mem[(cpu->sp + 1) & 0xFFFF] << 8);
            cpu->sp = (cpu->sp + 2) & 0xFFFF;
        }

        /* BIOS vector table trap — catches both direct jumps to the
         * vectors at BIOS_BASE and jumps to the call-gate area.
         * This is required for the classic "LD HL,(0001); ADD HL,DE; JP (HL)" pattern.
         */
        if (cpu->pc >= CPM_BIOS_BASE &&
            cpu->pc <  CPM_BIOS_BASE + 0x80) {          /* generous range covering vectors + gates */
            int cont = cpm_bios_dispatch(cpu);
            if (!cont) {
                return 1;
            }
            /* Simulate return from BIOS call (most BIOS functions are expected to return) */
            cpu->pc = cpu->mem[cpu->sp] | (cpu->mem[(cpu->sp + 1) & 0xFFFF] << 8);
            cpu->sp = (cpu->sp + 2) & 0xFFFF;
        }
        break;

    case Z80_OP_CALL_CC_NN:
        cpu->memptr = dec.imm16;
        if (cond_true(cpu, dec.cc)) {
            cpu->sp = (cpu->sp - 2) & 0xFFFF;
            z80_mem_w(cpu, cpu->sp, (uint8_t)(cpu->pc & 0xFF));
            z80_mem_w(cpu, (uint16_t)(cpu->sp + 1), (uint8_t)(cpu->pc >> 8));
            cpu->pc = dec.imm16;
        }
        break;

    case Z80_OP_RET:
        cpu->pc = cpu->mem[cpu->sp] | (cpu->mem[(cpu->sp + 1) & 0xFFFF] << 8);
        cpu->sp = (cpu->sp + 2) & 0xFFFF;
        cpu->memptr = cpu->pc;
        if (cpu->pc == 0x0000) {
            /* Classic CP/M .COM final RET to warm boot */
            return 1;   /* clean exit */
        }
        break;

    case Z80_OP_RET_CC:
        if (cond_true(cpu, dec.cc)) {
            cpu->pc = cpu->mem[cpu->sp] | (cpu->mem[(cpu->sp + 1) & 0xFFFF] << 8);
            cpu->sp = (cpu->sp + 2) & 0xFFFF;
            cpu->memptr = cpu->pc;
        }
        break;

    case Z80_OP_PUSH_RR:
        {
            uint16_t v;
            if (dec.reg1 == 4) {            /* DD/FD E5: PUSH IX/IY */
                v = (dec.prefix == 0xFD) ? cpu->iy : cpu->ix;
            } else if (dec.reg1 == 3) {     /* AF */
                v = cpu->af;
            } else {
                v = read_rr(cpu, dec.reg1);
            }
            cpu->sp = (cpu->sp - 2) & 0xFFFF;
            z80_mem_w(cpu, cpu->sp, (uint8_t)(v & 0xFF));
            z80_mem_w(cpu, (uint16_t)(cpu->sp + 1), (uint8_t)(v >> 8));
        }
        break;

    case Z80_OP_POP_RR:
        {
            uint16_t v = cpu->mem[cpu->sp] | (cpu->mem[(cpu->sp + 1) & 0xFFFF] << 8);
            cpu->sp = (cpu->sp + 2) & 0xFFFF;
            if (dec.reg1 == 4) {            /* DD/FD E1: POP IX/IY */
                if (dec.prefix == 0xFD) cpu->iy = v; else cpu->ix = v;
            } else if (dec.reg1 == 3) {
                cpu->af = v;
            } else {
                write_rr(cpu, dec.reg1, v);
            }
        }
        break;

    case Z80_OP_EX_DE_HL:
        {
            uint16_t t = cpu->de; cpu->de = cpu->hl; cpu->hl = t;
        }
        break;

    case Z80_OP_EX_AF_AF:
        {
            uint16_t t = cpu->af; cpu->af = cpu->af_; cpu->af_ = t;
        }
        break;

    /* --- Accumulator rotates (0x07/0x0F/0x17/0x1F) --- */
    case Z80_OP_RLCA: {
        uint8_t a = cpu->a;
        uint8_t c = (a & 0x80) ? 1 : 0;
        a = (a << 1) | c;
        cpu->a = a;
        cpu->f = (cpu->f & (Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV)) |
                 (c ? Z80_FLAG_C : 0) |
                 (a & (Z80_FLAG_3 | Z80_FLAG_5));
        break;
    }
    case Z80_OP_RRCA: {
        uint8_t a = cpu->a;
        uint8_t c = a & 1;
        a = (a >> 1) | (c << 7);
        cpu->a = a;
        cpu->f = (cpu->f & (Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV)) |
                 (c ? Z80_FLAG_C : 0) |
                 (a & (Z80_FLAG_3 | Z80_FLAG_5));
        break;
    }
    case Z80_OP_RLA: {
        uint8_t a = cpu->a;
        uint8_t c = (a & 0x80) ? 1 : 0;
        a = (a << 1) | ((cpu->f & Z80_FLAG_C) ? 1 : 0);
        cpu->a = a;
        cpu->f = (cpu->f & (Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV)) |
                 (c ? Z80_FLAG_C : 0) |
                 (a & (Z80_FLAG_3 | Z80_FLAG_5));
        break;
    }
    case Z80_OP_RRA: {
        uint8_t a = cpu->a;
        uint8_t c = a & 1;
        a = (a >> 1) | ((cpu->f & Z80_FLAG_C) ? 0x80 : 0);
        cpu->a = a;
        cpu->f = (cpu->f & (Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV)) |
                 (c ? Z80_FLAG_C : 0) |
                 (a & (Z80_FLAG_3 | Z80_FLAG_5));
        break;
    }

    case Z80_OP_EXX:
        {
            uint16_t t;
            t = cpu->bc; cpu->bc = cpu->bc_; cpu->bc_ = t;
            t = cpu->de; cpu->de = cpu->de_; cpu->de_ = t;
            t = cpu->hl; cpu->hl = cpu->hl_; cpu->hl_ = t;
        }
        break;

    case Z80_OP_DJNZ:
        cpu->b--;
        if (cpu->b != 0) {
            cpu->pc = (old_pc + dec.bytes + dec.disp) & 0xFFFF;
            cpu->memptr = cpu->pc;
        }
        break;

    case Z80_OP_RST:
        cpu->sp = (cpu->sp - 2) & 0xFFFF;
        z80_mem_w(cpu, cpu->sp, (uint8_t)(cpu->pc & 0xFF));
        z80_mem_w(cpu, (uint16_t)(cpu->sp + 1), (uint8_t)(cpu->pc >> 8));
        cpu->pc = dec.imm8;
        cpu->memptr = cpu->pc;
        break;

    case Z80_OP_OUT_N_A:
        /* For now we just ignore port writes (later the cpm/kaypro layer will catch some) */
        (void)dec.imm8;
        break;

    case Z80_OP_IN_A_N:
        /* Return 0xFF for now (typical for floating bus / unconnected ports) */
        cpu->a = 0xFF;
        (void)dec.imm8;
        break;

    case Z80_OP_LDIR:
    case Z80_OP_LDDR:
        /* Extremely common in CP/M for screen and disk buffers. We run the
         * whole block copy atomically; BC reaches 0 on the last iteration.
         * Flags at exit (final iter, BC==0):
         *   PV=0, H=0, N=0; S/Z/C preserved.
         *   Y = bit 1 of (A + transferred_byte_of_last_iter)
         *   X = bit 3 of (A + transferred_byte_of_last_iter) */
        {
            int incr = (dec.type == Z80_OP_LDIR) ? 1 : -1;
            uint8_t last_byte = 0;
            while (cpu->bc != 0) {
                last_byte = cpu->mem[cpu->hl & 0xFFFF];
                z80_mem_w(cpu, cpu->de, last_byte);
                cpu->hl = (cpu->hl + incr) & 0xFFFF;
                cpu->de = (cpu->de + incr) & 0xFFFF;
                cpu->bc = (cpu->bc - 1) & 0xFFFF;
            }
            uint8_t n = (uint8_t)(cpu->a + last_byte);
            uint8_t f = cpu->f & (Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_C);
            f |= (n & Z80_FLAG_3);            /* X = bit 3 */
            if (n & 0x02) f |= Z80_FLAG_5;    /* Y = bit 1 of n */
            cpu->f = f;
            cpu->q = 1;
        }
        break;

    /* --------------------------------------------------------------------
     * 16-bit arithmetic — very common in CP/M address manipulation
     * ------------------------------------------------------------------ */
    case Z80_OP_ADD_HL_RR:
        {
            /* With DD/FD prefix this is ADD IX,rr or ADD IY,rr — the "HL"
             * position is replaced, and reg1==RR_HL means the same index
             * register (ADD IX,IX / ADD IY,IY), not HL. */
            uint16_t *dst = (dec.prefix == 0xDD) ? &cpu->ix
                          : (dec.prefix == 0xFD) ? &cpu->iy
                          : &cpu->hl;
            uint16_t src;
            if (dec.reg1 == 3) src = cpu->sp;
            else if (dec.reg1 == 2) src = *dst;       /* "HL" position == dst register */
            else src = read_rr(cpu, dec.reg1);

            uint32_t res = (uint32_t)*dst + src;
            uint16_t r16 = (uint16_t)res;
            uint8_t f = cpu->f & (Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV);
            if (res & 0x10000) f |= Z80_FLAG_C;
            if (((*dst & 0x0FFF) + (src & 0x0FFF)) & 0x1000) f |= Z80_FLAG_H;
            f &= ~Z80_FLAG_N;
            f &= ~(Z80_FLAG_5 | Z80_FLAG_3);
            f |= xy_from((uint8_t)(r16 >> 8));
            /* MEMPTR = source HL + 1 */
            cpu->memptr = (uint16_t)(*dst + 1);
            cpu->f = f;
            cpu->q = 1;
            *dst = r16;
        }
        break;

    case Z80_OP_ADC_HL_RR:
        {
            /* ED 4A/5A/6A/7A — ADC HL,BC/DE/HL/SP. Writes ALL flags (unlike ADD HL). */
            uint16_t a = cpu->hl;
            uint16_t b = (dec.reg1 == 3) ? cpu->sp : read_rr(cpu, dec.reg1);
            uint16_t cin = (cpu->f & Z80_FLAG_C) ? 1 : 0;
            uint32_t res = (uint32_t)a + b + cin;
            uint16_t r = (uint16_t)res;
            uint8_t f = xy_from((uint8_t)(r >> 8));
            if (r == 0) f |= Z80_FLAG_Z;
            if (r & 0x8000) f |= Z80_FLAG_S;
            if (((a & 0x0FFF) + (b & 0x0FFF) + cin) & 0x1000) f |= Z80_FLAG_H;
            if (res & 0x10000) f |= Z80_FLAG_C;
            if (((a ^ b) & 0x8000) == 0 && ((r ^ a) & 0x8000)) f |= Z80_FLAG_PV;
            cpu->memptr = (uint16_t)(a + 1);
            cpu->f = f;
            cpu->q = 1;
            cpu->hl = r;
        }
        break;

    case Z80_OP_LD_NN_RR:
        {
            /* ED 43/53/63/73 — LD (nn),rr  for BC/DE/HL/SP */
            uint16_t v = (dec.reg1 == 3) ? cpu->sp : read_rr(cpu, dec.reg1);
            z80_mem_w(cpu, dec.imm16, (uint8_t)(v & 0xFF));
            z80_mem_w(cpu, (uint16_t)(dec.imm16 + 1), (uint8_t)(v >> 8));
            cpu->memptr = (uint16_t)(dec.imm16 + 1);
        }
        break;

    case Z80_OP_LD_RR_NN_IND:
        {
            /* ED 4B/5B/6B/7B — LD rr,(nn) for BC/DE/HL/SP */
            uint16_t v = cpu->mem[dec.imm16 & 0xFFFF]
                      | (cpu->mem[(dec.imm16 + 1) & 0xFFFF] << 8);
            if (dec.reg1 == 3) cpu->sp = v;
            else write_rr(cpu, dec.reg1, v);
            cpu->memptr = (uint16_t)(dec.imm16 + 1);
        }
        break;

    case Z80_OP_SBC_HL_RR:
        {
            /* ED 42/52/62/72 — SBC HL,BC/DE/HL/SP. Writes ALL flags, N=1. */
            uint16_t a = cpu->hl;
            uint16_t b = (dec.reg1 == 3) ? cpu->sp : read_rr(cpu, dec.reg1);
            uint16_t cin = (cpu->f & Z80_FLAG_C) ? 1 : 0;
            uint32_t res = (uint32_t)a - b - cin;
            uint16_t r = (uint16_t)res;
            uint8_t f = Z80_FLAG_N | xy_from((uint8_t)(r >> 8));
            if (r == 0) f |= Z80_FLAG_Z;
            if (r & 0x8000) f |= Z80_FLAG_S;
            if (((a & 0x0FFF) - (b & 0x0FFF) - cin) & 0x1000) f |= Z80_FLAG_H;
            if (res & 0x10000) f |= Z80_FLAG_C;
            if (((a ^ b) & 0x8000) && ((r ^ a) & 0x8000)) f |= Z80_FLAG_PV;
            cpu->memptr = (uint16_t)(a + 1);
            cpu->f = f;
            cpu->q = 1;
            cpu->hl = r;
        }
        break;

    case Z80_OP_LD_NN_HL:
        {
            uint16_t v = (dec.prefix == 0xDD) ? cpu->ix
                       : (dec.prefix == 0xFD) ? cpu->iy
                       : cpu->hl;
            z80_mem_w(cpu, dec.imm16, (uint8_t)(v & 0xFF));
            z80_mem_w(cpu, (uint16_t)(dec.imm16 + 1), (uint8_t)(v >> 8));
            cpu->memptr = (uint16_t)(dec.imm16 + 1);
        }
        break;

    case Z80_OP_LD_HL_indNN:
        {
            uint16_t v = cpu->mem[dec.imm16 & 0xFFFF]
                      | (cpu->mem[(dec.imm16 + 1) & 0xFFFF] << 8);
            if (dec.prefix == 0xDD) cpu->ix = v;
            else if (dec.prefix == 0xFD) cpu->iy = v;
            else cpu->hl = v;
            cpu->memptr = (uint16_t)(dec.imm16 + 1);
        }
        break;

    case Z80_OP_CPL:
        cpu->a = ~cpu->a;
        cpu->f = (cpu->f & ~(Z80_FLAG_5 | Z80_FLAG_3))
               | Z80_FLAG_H | Z80_FLAG_N | xy_from(cpu->a);
        cpu->q = 1;
        break;

    case Z80_OP_SCF:
        {
            /* XY: if the previous instruction modified F, source XY from A;
             * otherwise source from (A | F_before). The Q quirk. */
            uint8_t xy_src = prev_q ? cpu->a : (cpu->a | cpu->f);
            cpu->f = (cpu->f & (Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV))
                   | Z80_FLAG_C | xy_from(xy_src);
            cpu->q = 1;
        }
        break;

    case Z80_OP_CCF:
        {
            uint8_t old_c = cpu->f & Z80_FLAG_C;
            uint8_t xy_src = prev_q ? cpu->a : (cpu->a | cpu->f);
            cpu->f = (cpu->f & (Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV))
                   | (old_c ? Z80_FLAG_H : 0)
                   | (old_c ? 0 : Z80_FLAG_C)
                   | xy_from(xy_src);
            cpu->q = 1;
        }
        break;

    case Z80_OP_DAA:
        {
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
                /* Subtract correction. H reflects whether bit-4 borrow
                 * occurred during the conceptual subtraction. */
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
        break;

    case Z80_OP_NEG:
        {
            uint8_t a = cpu->a;
            uint8_t res = 0 - a;
            set_flags_sub(cpu, 0, a, res);
            cpu->a = res;
        }
        break;

    case Z80_OP_LDI:
    case Z80_OP_LDD:
        {
            int incr = (dec.type == Z80_OP_LDI) ? 1 : -1;
            uint8_t v = cpu->mem[cpu->hl & 0xFFFF];
            z80_mem_w(cpu, cpu->de, v);
            cpu->hl = (cpu->hl + incr) & 0xFFFF;
            cpu->de = (cpu->de + incr) & 0xFFFF;
            cpu->bc = (cpu->bc - 1) & 0xFFFF;
            uint8_t n = (uint8_t)(cpu->a + v);
            uint8_t f = cpu->f & (Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_C);
            if (cpu->bc != 0) f |= Z80_FLAG_PV;
            f |= (n & Z80_FLAG_3);            /* X = bit 3 of n */
            if (n & 0x02) f |= Z80_FLAG_5;    /* Y = bit 1 of n */
            cpu->f = f;
            cpu->q = 1;
        }
        break;

    case Z80_OP_RRD:
    case Z80_OP_RLD:
        {
            /* RRD: A_low <- (HL)_low ; (HL)_low <- (HL)_high ; (HL)_high <- A_low
             * RLD: A_low <- (HL)_high ; (HL)_high <- (HL)_low ; (HL)_low <- A_low
             * Flags: S, Z, P/V (parity) from new A; H=0, N=0, C unchanged. */
            uint8_t m = cpu->mem[cpu->hl & 0xFFFF];
            uint8_t a_lo = cpu->a & 0x0F;
            uint8_t a_hi = cpu->a & 0xF0;
            uint8_t m_lo = m & 0x0F;
            uint8_t m_hi = (m & 0xF0) >> 4;
            uint8_t new_a, new_m;
            if (dec.type == Z80_OP_RRD) {
                new_a = a_hi | m_lo;
                new_m = (a_lo << 4) | m_hi;
            } else {
                new_a = a_hi | m_hi;
                new_m = (m_lo << 4) | a_lo;
            }
            cpu->a = new_a;
            z80_mem_w(cpu, cpu->hl, new_m);
            uint8_t f = (cpu->f & Z80_FLAG_C) | xy_from(new_a);
            if (new_a == 0) f |= Z80_FLAG_Z;
            if (new_a & 0x80) f |= Z80_FLAG_S;
            f |= parity8(new_a);
            cpu->f = f;
            cpu->q = 1;
        }
        break;

    case Z80_OP_CPI:
    case Z80_OP_CPD:
    case Z80_OP_CPIR:
    case Z80_OP_CPDR:
        {
            /* CPI/CPD/CPIR/CPDR all do: cmp A,(HL); HL +/-= 1; BC--.
             *   Flags: S, Z from cmp result. H per low-nibble borrow.
             *   PV = (BC after decrement != 0)
             *   N = 1
             *   C unchanged.
             *   X = bit 3 of n, Y = bit 1 of n, where n = A - (HL) - H_after. */
            uint8_t v = cpu->mem[cpu->hl & 0xFFFF];
            uint8_t res = cpu->a - v;
            uint8_t f = (cpu->f & Z80_FLAG_C) | Z80_FLAG_N;
            if (res == 0) f |= Z80_FLAG_Z;
            if (res & 0x80) f |= Z80_FLAG_S;
            uint8_t h = ((cpu->a & 0xF) < (v & 0xF)) ? Z80_FLAG_H : 0;
            f |= h;
            uint8_t n = (uint8_t)(res - (h ? 1 : 0));
            f |= (n & Z80_FLAG_3);
            if (n & 0x02) f |= Z80_FLAG_5;
            int incr = (dec.type == Z80_OP_CPI || dec.type == Z80_OP_CPIR) ? 1 : -1;
            cpu->hl = (cpu->hl + incr) & 0xFFFF;
            cpu->bc = (cpu->bc - 1) & 0xFFFF;
            if (cpu->bc != 0) f |= Z80_FLAG_PV;
            cpu->f = f;
            cpu->q = 1;
            /* Repeating forms: stay on the instruction until BC==0 or match. */
            if ((dec.type == Z80_OP_CPIR || dec.type == Z80_OP_CPDR)
                && cpu->bc != 0 && res != 0) {
                cpu->pc = old_pc;
            }
        }
        break;

    /* --------------------------------------------------------------------
     * CB prefix group — rotates, shifts, BIT, RES, SET
     * This is one of the most frequently used groups in real CP/M code.
     * ------------------------------------------------------------------ */
    case Z80_OP_CB: {
        uint8_t sub = dec.imm8;          /* the 0x00-0xFF CB sub-opcode */
        int r   = sub & 7;               /* register encoding (6 = (HL)) */
        int grp = (sub >> 3) & 7;

        bool is_bit = (sub >= 0x40 && sub < 0x80);
        bool is_res = (sub >= 0x80 && sub < 0xC0);
        bool is_set = (sub >= 0xC0);

        /* DD/FD CB d xx — operand is always (IX+d) or (IY+d), regardless
         * of the register encoding in the sub-opcode. The reg field still
         * selects an undocumented write-back target for non-BIT ops, which
         * we don't model yet. */
        bool indexed = (dec.prefix == 0xDD || dec.prefix == 0xFD);
        bool mem = indexed || (r == 6);
        uint16_t addr = 0;
        uint8_t val;

        if (mem) {
            if (indexed) {
                addr = (dec.prefix == 0xDD ? cpu->ix : cpu->iy) + (int16_t)dec.disp;
            } else {
                addr = cpu->hl;
            }
            val = cpu->mem[addr & 0xFFFF];
        } else {
            uint8_t *p = reg8_ptr(cpu, r);
            val = p ? *p : 0;
        }

        uint8_t result = val;
        uint8_t new_f = cpu->f;
        bool c = (new_f & Z80_FLAG_C) != 0;
        bool new_c = false;

        if (is_bit) {
            int bit = (sub >> 3) & 7;
            bool b = (val & (1u << bit)) != 0;
            /* Z80 BIT n,r flag rules:
             *   C: unchanged
             *   N: 0
             *   H: 1
             *   Z = !b ;  PV = Z
             *   S = b only when n == 7 (else 0)
             *   X (bit 3) and Y (bit 5):
             *     - register form: from the operand byte
             *     - BIT n,(HL):    from MEMPTR.high
             *     - BIT n,(IX+d):  from high byte of the computed address */
            new_f &= Z80_FLAG_C;
            if (!b) new_f |= Z80_FLAG_Z | Z80_FLAG_PV;
            new_f |= Z80_FLAG_H;
            if (b && bit == 7) new_f |= Z80_FLAG_S;
            uint8_t xy_byte;
            if (indexed) {
                xy_byte = (uint8_t)(addr >> 8);
            } else if (r == 6) {
                xy_byte = (uint8_t)(cpu->memptr >> 8);
            } else {
                xy_byte = val;
            }
            new_f |= xy_from(xy_byte);
            result = val;
        } else if (is_res) {
            int bit = (sub >> 3) & 7;
            result &= ~(1u << bit);
        } else if (is_set) {
            int bit = (sub >> 3) & 7;
            result |= (1u << bit);
        } else {
            /* Rotate / shift group */
            switch (grp) {
            case 0: /* RLC r / (HL) */
                new_c = (val & 0x80) != 0;
                result = (val << 1) | (new_c ? 1 : 0);
                break;
            case 1: /* RRC */
                new_c = (val & 0x01) != 0;
                result = (val >> 1) | (new_c ? 0x80 : 0);
                break;
            case 2: /* RL */
                new_c = (val & 0x80) != 0;
                result = (val << 1) | (c ? 1 : 0);
                break;
            case 3: /* RR */
                new_c = (val & 0x01) != 0;
                result = (val >> 1) | (c ? 0x80 : 0);
                break;
            case 4: /* SLA */
                new_c = (val & 0x80) != 0;
                result = (val << 1);
                break;
            case 5: /* SRA */
                new_c = (val & 0x01) != 0;
                result = (val >> 1) | (val & 0x80);   /* sign extend */
                break;
            case 6: /* SLL (undocumented, often acts like SLA but sets bit 0) */
                new_c = (val & 0x80) != 0;
                result = (val << 1) | 1;
                break;
            case 7: /* SRL */
                new_c = (val & 0x01) != 0;
                result = (val >> 1);
                break;
            }

            if (new_c) new_f |= Z80_FLAG_C; else new_f &= ~Z80_FLAG_C;
            new_f &= ~Z80_FLAG_H;
            new_f &= ~Z80_FLAG_N;
        }

        /* Write back (BIT writes nothing).
         * For the undocumented DD/FD CB d <sub> form, RES/SET/rotate
         * write the result to BOTH the memory operand AND register r
         * (when r != 6). zexdoc's <set,res> n,(<ix,iy>+1) test relies
         * on this side effect being visible in the registers. */
        if (!is_bit) {
            if (mem) {
                z80_mem_w(cpu, addr, result);
                if (indexed && r != 6) {
                    uint8_t *p = reg8_ptr(cpu, r);
                    if (p) *p = result;
                }
            } else {
                uint8_t *p = reg8_ptr(cpu, r);
                if (p) *p = result;
            }
        }

        /* Flag updates for rotate/shift only. RES and SET do not modify
         * any flag on real Z80. */
        if (!is_bit && !is_res && !is_set) {
            new_f &= Z80_FLAG_C;   /* keep only C from new_f */
            if (result == 0) new_f |= Z80_FLAG_Z;
            if (result & 0x80) new_f |= Z80_FLAG_S;
            new_f |= parity8(result);
            new_f |= xy_from(result);
        }

        if (!is_res && !is_set) {
            cpu->f = new_f;
            cpu->q = 1;
        }
        break;
    }

    /* Half index register operations (IXH/IXL, IYH/IYL) */
    case Z80_OP_LD_A_IXH:
        cpu->a = cpu->ix >> 8;
        break;
    case Z80_OP_LD_A_IXL:
        cpu->a = cpu->ix & 0xFF;
        break;

    /* More half-reg cases (LD IXH, A, INC IXH, ADD A,IXH, etc.) added as needed */

    default:
        fprintf(stderr, "z80_step: UNIMPLEMENTED opcode %02X (prefix %02X) at %04X\n",
                dec.opcode, dec.prefix, old_pc);
        return -1;
    }

    cpu->insn_count++;
    return 0;
}

/* ========================================================================
 * Run loop (used by both interp and future DBT)
 * ===================================================================== */
void z80_run(z80_cpu_t *cpu) {
    /* In pure interpreter mode we just keep stepping until something stops us.
       Real termination will be via BDOS function 0 (warm boot) or a special
       host call we install at 0x0000/0x0005. */
    for (;;) {
        if (z80_step(cpu) != 0)
            break;
        /* Crude safety: if we execute an absurd number of instructions, stop */
        if (cpu->insn_count > 100000000ULL) {
            fprintf(stderr, "z80_run: safety limit hit (100M instructions)\n");
            break;
        }
    }
}
