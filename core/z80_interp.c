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
static inline uint8_t *reg8_ptr(z80_cpu_t *cpu, int r) {
    switch (r) {
    case 0: return &cpu->b;
    case 1: return &cpu->c;
    case 2: return &cpu->d;
    case 3: return &cpu->e;
    case 4: return &cpu->h;
    case 5: return &cpu->l;
    case 7: return &cpu->a;
    default: return NULL; /* (HL) / (IX+d) / (IY+d) handled specially */
    }
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
    uint8_t *p = reg8_ptr(cpu, r);
    return p ? *p : 0;
}

static inline void write_reg8(z80_cpu_t *cpu, int r, uint8_t val, const z80_decoded *dec) {
    if (r == 6) {
        uint16_t addr = effective_addr(cpu, dec, cpu->hl);
        cpu->mem[addr & 0xFFFF] = val;
        return;
    }
    uint8_t *p = reg8_ptr(cpu, r);
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

/* Flag helpers — simple eager version for the interpreter */
static inline void set_flags_add(z80_cpu_t *cpu, uint8_t a, uint8_t b, uint8_t res) {
    uint8_t f = 0;
    if (res == 0) f |= Z80_FLAG_Z;
    if (res & 0x80) f |= Z80_FLAG_S;
    if (((a & 0xF) + (b & 0xF)) > 0xF) f |= Z80_FLAG_H;
    if ((int16_t)a + (int16_t)b > 0xFF) f |= Z80_FLAG_C;
    /* P/V for addition is overflow */
    if (((a ^ b) & 0x80) == 0 && ((res ^ a) & 0x80)) f |= Z80_FLAG_PV;
    cpu->f = f;
}

static inline void set_flags_sub(z80_cpu_t *cpu, uint8_t a, uint8_t b, uint8_t res) {
    uint8_t f = Z80_FLAG_N;
    if (res == 0) f |= Z80_FLAG_Z;
    if (res & 0x80) f |= Z80_FLAG_S;
    if ((a & 0xF) < (b & 0xF)) f |= Z80_FLAG_H;
    if (a < b) f |= Z80_FLAG_C;
    if (((a ^ b) & 0x80) && ((res ^ a) & 0x80) == 0) f |= Z80_FLAG_PV;
    cpu->f = f;
}

static inline void set_flags_logic(z80_cpu_t *cpu, uint8_t res) {
    uint8_t f = 0;
    if (res == 0) f |= Z80_FLAG_Z;
    if (res & 0x80) f |= Z80_FLAG_S;
    /* P/V = parity for logic ops */
    int p = 0; for (int i=0;i<8;i++) p ^= (res >> i) & 1; if (p==0) f |= Z80_FLAG_PV;
    cpu->f = f;
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

    switch (dec.type) {
    case Z80_OP_NOP:
        break;

    case Z80_OP_LD_RR_NN:
        if (dec.reg1 == 3) { /* SP */
            cpu->sp = dec.imm16;
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
            cpu->mem[a] = v;
            uint8_t f = cpu->f & Z80_FLAG_C;
            if (v == 0) f |= Z80_FLAG_Z;
            if (v & 0x80) f |= Z80_FLAG_S;
            if ((v & 0xF) == 0) f |= Z80_FLAG_H;
            cpu->f = f;
        }
        break;

    case Z80_OP_DEC_HL_ind:
        {
            uint16_t a = effective_addr(cpu, &dec, cpu->hl);
            uint8_t v = cpu->mem[a] - 1;
            cpu->mem[a] = v;
            uint8_t f = (cpu->f & Z80_FLAG_C) | Z80_FLAG_N;
            if (v == 0) f |= Z80_FLAG_Z;
            if (v & 0x80) f |= Z80_FLAG_S;
            if ((v & 0xF) == 0xF) f |= Z80_FLAG_H;
            cpu->f = f;
        }
        break;

    case Z80_OP_LD_NN_A:
        cpu->mem[dec.imm16 & 0xFFFF] = cpu->a;
        break;

    case Z80_OP_LD_A_NN:
        cpu->a = cpu->mem[dec.imm16 & 0xFFFF];
        break;

    case Z80_OP_LD_A_BC:
        cpu->a = cpu->mem[cpu->bc & 0xFFFF];
        break;

    case Z80_OP_LD_A_DE:
        cpu->a = cpu->mem[cpu->de & 0xFFFF];
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
        cpu->mem[cpu->de & 0xFFFF] = cpu->a;
        break;

    case Z80_OP_LD_BC_A:
        cpu->mem[cpu->bc & 0xFFFF] = cpu->a;
        break;

    case Z80_OP_INC_R:
        if (dec.reg1 == 6) { /* (HL) or (IX+d)/(IY+d) */
            uint16_t a = effective_addr(cpu, &dec, cpu->hl);
            uint8_t v = cpu->mem[a] + 1;
            cpu->mem[a] = v;
            /* set flags for INC */
            uint8_t f = cpu->f & (Z80_FLAG_C);
            if (v == 0) f |= Z80_FLAG_Z;
            if (v & 0x80) f |= Z80_FLAG_S;
            if ((v & 0xF) == 0) f |= Z80_FLAG_H;
            if (v == 0x80) f |= Z80_FLAG_PV;
            cpu->f = f;
        } else {
            uint8_t *p = reg8_ptr(cpu, dec.reg1);
            if (p) {
                uint8_t v = *p + 1;
                *p = v;
                uint8_t f = cpu->f & (Z80_FLAG_C);
                if (v == 0) f |= Z80_FLAG_Z;
                if (v & 0x80) f |= Z80_FLAG_S;
                if ((v & 0xF) == 0) f |= Z80_FLAG_H;
                if (v == 0x80) f |= Z80_FLAG_PV;
                cpu->f = f;
            }
        }
        break;

    case Z80_OP_DEC_R:
        /* similar but subtract 1, set N */
        if (dec.reg1 == 6) {
            uint16_t a = effective_addr(cpu, &dec, cpu->hl);
            uint8_t v = cpu->mem[a] - 1;
            cpu->mem[a] = v;
            uint8_t f = (cpu->f & Z80_FLAG_C) | Z80_FLAG_N;
            if (v == 0) f |= Z80_FLAG_Z;
            if (v & 0x80) f |= Z80_FLAG_S;
            if ((v & 0xF) == 0xF) f |= Z80_FLAG_H;
            if (v == 0x7F) f |= Z80_FLAG_PV;
            cpu->f = f;
        } else {
            uint8_t *p = reg8_ptr(cpu, dec.reg1);
            if (p) {
                uint8_t v = *p - 1;
                *p = v;
                uint8_t f = (cpu->f & Z80_FLAG_C) | Z80_FLAG_N;
                if (v == 0) f |= Z80_FLAG_Z;
                if (v & 0x80) f |= Z80_FLAG_S;
                if ((v & 0xF) == 0xF) f |= Z80_FLAG_H;
                if (v == 0x7F) f |= Z80_FLAG_PV;
                cpu->f = f;
            }
        }
        break;

    case Z80_OP_INC_RR:
        if (dec.reg1 == 3) cpu->sp++;
        else write_rr(cpu, dec.reg1, read_rr(cpu, dec.reg1) + 1);
        break;

    case Z80_OP_DEC_RR:
        if (dec.reg1 == 3) cpu->sp--;
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
            set_flags_sub(cpu, cpu->a, b, res);
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
            set_flags_sub(cpu, cpu->a, b, res);
        }
        break;

    case Z80_OP_CP_A_N:
        {
            uint8_t res = cpu->a - dec.imm8;
            set_flags_sub(cpu, cpu->a, dec.imm8, res);
        }
        break;

    case Z80_OP_JP_NN:
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
        break;

    case Z80_OP_JP_CC_NN:
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
        break;

    case Z80_OP_JR_CC_E:
        if (cond_true(cpu, dec.cc))
            cpu->pc = (old_pc + dec.bytes + dec.disp) & 0xFFFF;
        break;

    case Z80_OP_CALL_NN:
        /* push return address */
        cpu->sp = (cpu->sp - 2) & 0xFFFF;
        cpu->mem[cpu->sp]     = (cpu->pc) & 0xFF;
        cpu->mem[cpu->sp + 1] = (cpu->pc >> 8) & 0xFF;
        cpu->pc = dec.imm16;

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
        if (cond_true(cpu, dec.cc)) {
            cpu->sp = (cpu->sp - 2) & 0xFFFF;
            cpu->mem[cpu->sp]     = (cpu->pc) & 0xFF;
            cpu->mem[cpu->sp + 1] = (cpu->pc >> 8) & 0xFF;
            cpu->pc = dec.imm16;
        }
        break;

    case Z80_OP_RET:
        cpu->pc = cpu->mem[cpu->sp] | (cpu->mem[(cpu->sp + 1) & 0xFFFF] << 8);
        cpu->sp = (cpu->sp + 2) & 0xFFFF;
        if (cpu->pc == 0x0000) {
            /* Classic CP/M .COM final RET to warm boot */
            return 1;   /* clean exit */
        }
        break;

    case Z80_OP_RET_CC:
        if (cond_true(cpu, dec.cc)) {
            cpu->pc = cpu->mem[cpu->sp] | (cpu->mem[(cpu->sp + 1) & 0xFFFF] << 8);
            cpu->sp = (cpu->sp + 2) & 0xFFFF;
        }
        break;

    case Z80_OP_PUSH_RR:
        {
            uint16_t v = (dec.reg1 == 3) ? cpu->af : read_rr(cpu, dec.reg1);
            cpu->sp = (cpu->sp - 2) & 0xFFFF;
            cpu->mem[cpu->sp]     = v & 0xFF;
            cpu->mem[(cpu->sp + 1) & 0xFFFF] = v >> 8;
        }
        break;

    case Z80_OP_POP_RR:
        {
            uint16_t v = cpu->mem[cpu->sp] | (cpu->mem[(cpu->sp + 1) & 0xFFFF] << 8);
            cpu->sp = (cpu->sp + 2) & 0xFFFF;
            if (dec.reg1 == 3) cpu->af = v;
            else write_rr(cpu, dec.reg1, v);
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
        if (cpu->b != 0)
            cpu->pc = (old_pc + dec.bytes + dec.disp) & 0xFFFF;
        break;

    case Z80_OP_RST:
        cpu->sp = (cpu->sp - 2) & 0xFFFF;
        cpu->mem[cpu->sp]     = cpu->pc & 0xFF;
        cpu->mem[(cpu->sp + 1) & 0xFFFF] = cpu->pc >> 8;
        cpu->pc = dec.imm8;
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
        /* Extremely common in CP/M for screen and disk buffers */
        {
            while (cpu->bc != 0) {
                cpu->mem[cpu->de & 0xFFFF] = cpu->mem[cpu->hl & 0xFFFF];
                cpu->hl++; cpu->de++; cpu->bc--;
                /* LDIR sets flags in a weird way on real silicon, but most code ignores them */
            }
            /* On Z80, LDIR leaves PC pointing at the ED B0 instruction until BC==0,
               then continues after it. We already advanced PC, so we are correct
               for the "BC became zero" case. For the looping case we need to
               back up PC. */
            if (cpu->bc != 0) {
                cpu->pc = old_pc;   /* repeat the instruction */
            }
        }
        break;

    case Z80_OP_LDDR:
        /* similar, decrementing */
        while (cpu->bc != 0) {
            cpu->mem[cpu->de & 0xFFFF] = cpu->mem[cpu->hl & 0xFFFF];
            cpu->hl--; cpu->de--; cpu->bc--;
        }
        if (cpu->bc != 0) cpu->pc = old_pc;
        break;

    /* --------------------------------------------------------------------
     * 16-bit arithmetic — very common in CP/M address manipulation
     * ------------------------------------------------------------------ */
    case Z80_OP_ADD_HL_RR:
        {
            uint16_t src = (dec.reg1 == 3) ? cpu->sp : read_rr(cpu, dec.reg1);
            uint32_t res = (uint32_t)cpu->hl + src;
            /* Z80 16-bit ADD flags: H from bit 11 carry, C from bit 15, N=0, S/Z/PV not officially defined but many emulators copy from high byte */
            uint8_t f = cpu->f & (Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV); /* keep some */
            if (res & 0x10000) f |= Z80_FLAG_C;
            if (((cpu->hl & 0x0FFF) + (src & 0x0FFF)) & 0x1000) f |= Z80_FLAG_H;
            f &= ~Z80_FLAG_N;
            cpu->f = f;
            cpu->hl = (uint16_t)res;
        }
        break;

    case Z80_OP_LD_NN_HL:
        cpu->mem[dec.imm16 & 0xFFFF]       = cpu->l;
        cpu->mem[(dec.imm16 + 1) & 0xFFFF] = cpu->h;
        break;

    case Z80_OP_LD_HL_indNN:
        cpu->l = cpu->mem[dec.imm16 & 0xFFFF];
        cpu->h = cpu->mem[(dec.imm16 + 1) & 0xFFFF];
        break;

    case Z80_OP_CPL:
        cpu->a = ~cpu->a;
        cpu->f |= (Z80_FLAG_H | Z80_FLAG_N);
        break;

    case Z80_OP_SCF:
        cpu->f = (cpu->f & (Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV)) | Z80_FLAG_C;
        break;

    case Z80_OP_CCF:
        cpu->f ^= Z80_FLAG_C;
        cpu->f &= ~Z80_FLAG_N;
        /* H is set to old C in real Z80 — we approximate */
        if (cpu->f & Z80_FLAG_C) cpu->f |= Z80_FLAG_H; else cpu->f &= ~Z80_FLAG_H;
        break;

    case Z80_OP_DAA:
        /* Proper DAA is fiddly. For now a reasonable approximation that works
         * for the common BCD cases in CP/M tools. Real implementation needs
         * the full 100+ entry correction table or careful bit logic. */
        {
            uint8_t a = cpu->a;
            uint8_t corr = 0;
            if ((a & 0x0F) > 9 || (cpu->f & Z80_FLAG_H)) corr |= 0x06;
            if (a > 0x99 || (cpu->f & Z80_FLAG_C))     corr |= 0x60;
            if (a > 0x99) cpu->f |= Z80_FLAG_C;
            a += (cpu->f & Z80_FLAG_N) ? -corr : corr;
            cpu->a = a;
            /* update Z/S/PV (H is cleared or set per rules) */
            if (a == 0) cpu->f |= Z80_FLAG_Z; else cpu->f &= ~Z80_FLAG_Z;
            if (a & 0x80) cpu->f |= Z80_FLAG_S; else cpu->f &= ~Z80_FLAG_S;
            /* P/V = parity of result */
            int p = 0; for (int i = 0; i < 8; i++) p ^= (a >> i) & 1;
            if (p == 0) cpu->f |= Z80_FLAG_PV; else cpu->f &= ~Z80_FLAG_PV;
            cpu->f &= ~Z80_FLAG_H; /* simplified */
        }
        break;

    case Z80_OP_NEG:
        {
            uint8_t res = 0 - cpu->a;
            /* flags for NEG (same as SUB A, A with A as operand) */
            set_flags_sub(cpu, 0, cpu->a, res);
            cpu->a = res;
            cpu->f |= Z80_FLAG_N;   /* NEG always sets N */
            if (cpu->a == 0) cpu->f |= Z80_FLAG_Z; else cpu->f &= ~Z80_FLAG_Z;
        }
        break;

    case Z80_OP_LDI:
        /* one step of LDIR, no repeat */
        cpu->mem[cpu->de & 0xFFFF] = cpu->mem[cpu->hl & 0xFFFF];
        cpu->hl++; cpu->de++; cpu->bc--;
        /* P/V is set if BC != 0 after the decrement */
        if (cpu->bc != 0) cpu->f |= Z80_FLAG_PV; else cpu->f &= ~Z80_FLAG_PV;
        cpu->f &= ~Z80_FLAG_H;
        cpu->f &= ~Z80_FLAG_N;
        break;

    case Z80_OP_LDD:
        cpu->mem[cpu->de & 0xFFFF] = cpu->mem[cpu->hl & 0xFFFF];
        cpu->hl--; cpu->de--; cpu->bc--;
        if (cpu->bc != 0) cpu->f |= Z80_FLAG_PV; else cpu->f &= ~Z80_FLAG_PV;
        cpu->f &= ~Z80_FLAG_H;
        cpu->f &= ~Z80_FLAG_N;
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

        uint8_t val;
        bool mem = (r == 6);
        uint16_t addr = 0;

        if (mem) {
            /* For now plain (HL). DD/FD CB d xx will be improved when we
             * add proper indexed addressing support. */
            addr = cpu->hl;
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
            new_f &= ~(Z80_FLAG_Z | Z80_FLAG_H | Z80_FLAG_N | Z80_FLAG_PV);
            if (!b) new_f |= Z80_FLAG_Z;
            new_f |= Z80_FLAG_H;
            if (!b) new_f |= Z80_FLAG_PV;
            /* Undocumented 3/5 from the source byte */
            new_f = (new_f & 0xC7) | (val & 0x28);
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

        /* Write back */
        if (mem) {
            cpu->mem[addr & 0xFFFF] = result;
        } else {
            uint8_t *p = reg8_ptr(cpu, r);
            if (p) *p = result;
        }

        /* Common flag updates for non-BIT ops */
        if (!is_bit) {
            if (result == 0) new_f |= Z80_FLAG_Z; else new_f &= ~Z80_FLAG_Z;
            if (result & 0x80) new_f |= Z80_FLAG_S; else new_f &= ~Z80_FLAG_S;

            /* P/V = parity for most rotate/shift/RES/SET results */
            int parity = 0;
            for (int i = 0; i < 8; i++) parity ^= (result >> i) & 1;
            if (parity == 0) new_f |= Z80_FLAG_PV; else new_f &= ~Z80_FLAG_PV;
        }

        cpu->f = new_f;
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
