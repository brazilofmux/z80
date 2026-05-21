/* z80_decode.c — Z80 instruction decoder
 *
 * Handles all four prefix bytes (CB, DD, ED, FD) and their combinations.
 * The decoder is deliberately table-light for the first version so we can
 * see every opcode we hit while bringing up real CP/M binaries.
 *
 * We return a small decoded structure that both the interpreter and the
 * future DBT can consume.
 */

#include "z80.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* Condition codes (for JP cc, CALL cc, RET cc, JR cc) */
enum {
    CC_NZ = 0, CC_Z, CC_NC, CC_C, CC_PO, CC_PE, CC_P, CC_M
};

/* Register encoding (for 8-bit ops) */
enum {
    R_B = 0, R_C, R_D, R_E, R_H, R_L, R_HL_IND, R_A
};

/* 16-bit register pairs for PUSH/POP etc. */
enum {
    RR_BC = 0, RR_DE, RR_HL, RR_AF
};

/* Decode one instruction at mem[pc].
 * Returns the number of bytes consumed, or 0 on error.
 * Fills *out with the decoded form.
 */
int z80_decode_one(const uint8_t *mem, uint16_t pc, z80_decoded *out) {
    memset(out, 0, sizeof(*out));
    out->bytes = 1;

    uint8_t op = mem[pc & 0xFFFF];

    /* =======================================================================
     * Prefix handling for DD/FD (IX/IY), CB, ED
     * This version properly consumes displacement bytes for indexed ops
     * and handles the DD CB / FD CB forms.
     * ==================================================================== */
    uint8_t prefix = 0;
    int max_prefix = 4;

    /* Consume leading DD/FD prefixes (last one wins) */
    while ((op == 0xDD || op == 0xFD) && max_prefix--) {
        prefix = op;
        out->prefix = prefix;
        pc++;
        out->bytes++;
        op = mem[pc & 0xFFFF];
    }

    /* Handle CB (possibly DD CB d xx or FD CB d xx).
     *
     * Convention: pc points at the op byte (here, CB). bytes already
     * accounts for everything up to and including the op. Subsequent
     * bytes are at mem[pc+1], mem[pc+2], ... */
    if (op == 0xCB) {
        out->prefix = prefix ? prefix : 0xCB;
        if (prefix) {
            /* DD CB d sub  /  FD CB d sub */
            out->disp = (int8_t)mem[(pc + 1) & 0xFFFF];
            out->imm8 = mem[(pc + 2) & 0xFFFF];
            out->bytes += 2;
        } else {
            /* CB sub */
            out->imm8 = mem[(pc + 1) & 0xFFFF];
            out->bytes++;
        }
        out->type = Z80_OP_CB;
        out->opcode = out->imm8;
        return out->bytes;
    }

    /* Handle ED (rarely prefixed by DD/FD in practice).
     *
     * ED-prefixed opcodes are a separate ISA from the main one — 0x42 here
     * is SBC HL,BC, not LD B,D. Decode entirely in this block and return so
     * the main switch never gets a chance to misclassify them. */
    if (op == 0xED) {
        prefix = 0xED;
        out->prefix = 0xED;
        pc++; out->bytes++;
        op = mem[pc & 0xFFFF];
        out->opcode = op;

        switch (op) {
        case 0x42: out->type = Z80_OP_SBC_HL_RR; out->reg1 = RR_BC; return out->bytes;
        case 0x52: out->type = Z80_OP_SBC_HL_RR; out->reg1 = RR_DE; return out->bytes;
        case 0x62: out->type = Z80_OP_SBC_HL_RR; out->reg1 = RR_HL; return out->bytes;
        case 0x72: out->type = Z80_OP_SBC_HL_RR; out->reg1 = 3;     return out->bytes;  /* SP */
        case 0x4A: out->type = Z80_OP_ADC_HL_RR; out->reg1 = RR_BC; return out->bytes;
        case 0x5A: out->type = Z80_OP_ADC_HL_RR; out->reg1 = RR_DE; return out->bytes;
        case 0x6A: out->type = Z80_OP_ADC_HL_RR; out->reg1 = RR_HL; return out->bytes;
        case 0x7A: out->type = Z80_OP_ADC_HL_RR; out->reg1 = 3;     return out->bytes;  /* SP */
        case 0x43: case 0x53: case 0x63: case 0x73:  /* LD (nn),rr  */
            out->type = Z80_OP_LD_NN_RR;
            out->reg1 = (op >> 4) - 4;   /* 0x43->0, 0x53->1, 0x63->2, 0x73->3 */
            out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
            out->bytes += 2;
            return out->bytes;
        case 0x4B: case 0x5B: case 0x6B: case 0x7B:  /* LD rr,(nn)  */
            out->type = Z80_OP_LD_RR_NN_IND;
            out->reg1 = (op >> 4) - 4;
            out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
            out->bytes += 2;
            return out->bytes;
        case 0x44: out->type = Z80_OP_NEG;  return out->bytes;
        case 0xA0: out->type = Z80_OP_LDI;  return out->bytes;
        case 0xA1: out->type = Z80_OP_CPI;  return out->bytes;
        case 0xA8: out->type = Z80_OP_LDD;  return out->bytes;
        case 0xA9: out->type = Z80_OP_CPD;  return out->bytes;
        case 0xB0: out->type = Z80_OP_LDIR; return out->bytes;
        case 0xB1: out->type = Z80_OP_CPIR; return out->bytes;
        case 0xB8: out->type = Z80_OP_LDDR; return out->bytes;
        case 0xB9: out->type = Z80_OP_CPDR; return out->bytes;
        default:
            out->type = Z80_OP_UNKNOWN;
            return out->bytes;
        }
    }

    /* At this point we have a "main" opcode byte.
     * If we have a DD or FD prefix, many opcodes will require a displacement. */
    out->opcode = op;

    /* Determine if this opcode (under DD/FD) needs a displacement byte */
    bool needs_disp = false;

    if (prefix == 0xDD || prefix == 0xFD) {
        switch (op) {
        /* Memory forms that use (IX+d) or (IY+d) */
        case 0x34: case 0x35:           /* INC/DEC (IX+d) */
        case 0x36:                      /* LD (IX+d), n */
        case 0x46: case 0x4E: case 0x56: case 0x5E:
        case 0x66: case 0x6E: case 0x7E: /* LD r, (IX+d) */
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x77: /* LD (IX+d), r */
        case 0x86: case 0x8E: case 0x96: case 0x9E:
        case 0xA6: case 0xAE: case 0xB6: case 0xBE: /* ALU A, (IX+d) */
            needs_disp = true;
            break;

        /* JP (IX) / JP (IY) — no displacement */
        case 0xE9:
            break;

        /* Default: some opcodes use IXH/IXL instead of H/L (no disp) */
        default:
            break;
        }
    }

    if (needs_disp) {
        /* pc currently points at the op byte under the DD/FD prefix; the
         * displacement is the byte right after it. */
        out->disp = (int8_t)mem[(pc + 1) & 0xFFFF];
        pc += 2;
        out->bytes++;
    }

    /* -----------------------------------------------------------------------
     * Prefix-aware remapping for common DD/FD (IX/IY) indexed ops.
     * Short-circuit return so the plain main switch doesn't overwrite the type.
     * -------------------------------------------------------------------- */
    if (prefix == 0xDD || prefix == 0xFD) {
        switch (op) {
        case 0x7E:  out->type = Z80_OP_LD_A_HL_ind; out->reg1 = 6; return out->bytes;
        case 0x77:  out->type = Z80_OP_LD_HL_A_ind; out->reg1 = 6; return out->bytes;
        case 0x46: case 0x4E: case 0x56: case 0x5E: case 0x66: case 0x6E:
            out->type = Z80_OP_LD_R_HL_ind; out->reg1 = 6; out->reg2 = (op >> 3) & 7; return out->bytes;
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75:
            out->type = Z80_OP_LD_HL_R_ind; out->reg1 = 6; out->reg2 = op & 7; return out->bytes;
        case 0x34: out->type = Z80_OP_INC_HL_ind; out->reg1 = 6; return out->bytes;
        case 0x35: out->type = Z80_OP_DEC_HL_ind; out->reg1 = 6; return out->bytes;
        case 0x36: out->type = Z80_OP_LD_HL_N_ind; out->reg1 = 6;
                   out->imm8 = mem[pc & 0xFFFF]; pc++; out->bytes++; return out->bytes;

        /* ALU A, (IX+d) */
        case 0x86: out->type = Z80_OP_ADD_A_HL_ind; out->reg1 = 6; return out->bytes;
        case 0x8E: out->type = Z80_OP_ADC_A_HL_ind; out->reg1 = 6; return out->bytes;
        case 0x96: out->type = Z80_OP_SUB_A_HL_ind; out->reg1 = 6; return out->bytes;
        case 0x9E: out->type = Z80_OP_SBC_A_HL_ind; out->reg1 = 6; return out->bytes;
        case 0xA6: out->type = Z80_OP_AND_A_HL_ind; out->reg1 = 6; return out->bytes;
        case 0xAE: out->type = Z80_OP_XOR_A_HL_ind; out->reg1 = 6; return out->bytes;
        case 0xB6: out->type = Z80_OP_OR_A_HL_ind;  out->reg1 = 6; return out->bytes;
        case 0xBE: out->type = Z80_OP_CP_A_HL_ind;  out->reg1 = 6; return out->bytes;

        /* Half-registers */
        case 0x7C: out->type = Z80_OP_LD_A_IXH; return out->bytes;
        case 0x7D: out->type = Z80_OP_LD_A_IXL; return out->bytes;

        /* DD/FD E1/E5 — POP/PUSH IX/IY.  reg1=0 means "IX or IY per dec.prefix". */
        case 0xE1: out->type = Z80_OP_POP_RR;  out->reg1 = 4; return out->bytes;
        case 0xE5: out->type = Z80_OP_PUSH_RR; out->reg1 = 4; return out->bytes;
        }
    }

    /* -----------------------------------------------------------------------
     * Main dispatch table.
     * (Indexed DD/FD cases short-circuit earlier with return.)
     * -------------------------------------------------------------------- */
    switch (op) {
        case 0x00: out->type = Z80_OP_NOP; break;
    case 0x01: /* LD BC, nn */
        out->type = Z80_OP_LD_RR_NN; out->reg1 = RR_BC;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0x02: out->type = Z80_OP_LD_BC_A; break;
    case 0x03: out->type = Z80_OP_INC_RR; out->reg1 = RR_BC; break;
    case 0x04: out->type = Z80_OP_INC_R; out->reg1 = R_B; break;
    case 0x05: out->type = Z80_OP_DEC_R; out->reg1 = R_B; break;
    case 0x06: /* LD B, n */
        out->type = Z80_OP_LD_R_N; out->reg1 = R_B;
        out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0x07: out->type = Z80_OP_RLCA; break;  /* RLCA */
    case 0x08: out->type = Z80_OP_EX_AF_AF; break;
    case 0x09: out->type = Z80_OP_ADD_HL_RR; out->reg1 = RR_BC; break;
    case 0x0A: out->type = Z80_OP_LD_A_BC; break;
    case 0x0B: out->type = Z80_OP_DEC_RR; out->reg1 = RR_BC; break;
    case 0x0C: out->type = Z80_OP_INC_R; out->reg1 = R_C; break;
    case 0x0D: out->type = Z80_OP_DEC_R; out->reg1 = R_C; break;
    case 0x0E: /* LD C, n */
        out->type = Z80_OP_LD_R_N; out->reg1 = R_C;
        out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0x0F: out->type = Z80_OP_RRCA; break;  /* RRCA */

    case 0x10: /* DJNZ d */
        out->type = Z80_OP_DJNZ;
        out->disp = (int8_t)mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0x11: /* LD DE, nn */
        out->type = Z80_OP_LD_RR_NN; out->reg1 = RR_DE;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0x12: out->type = Z80_OP_LD_DE_A; break;
    case 0x13: out->type = Z80_OP_INC_RR; out->reg1 = RR_DE; break;
    case 0x14: out->type = Z80_OP_INC_R; out->reg1 = R_D; break;
    case 0x15: out->type = Z80_OP_DEC_R; out->reg1 = R_D; break;
    case 0x16: /* LD D, n */
        out->type = Z80_OP_LD_R_N; out->reg1 = R_D;
        out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0x17: out->type = Z80_OP_RLA; break;  /* RLA */
    case 0x18: /* JR d */
        out->type = Z80_OP_JR_E;
        out->disp = (int8_t)mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0x19: out->type = Z80_OP_ADD_HL_RR; out->reg1 = RR_DE; break;
    case 0x1A: out->type = Z80_OP_LD_A_DE; break;
    case 0x1B: out->type = Z80_OP_DEC_RR; out->reg1 = RR_DE; break;
    case 0x1C: out->type = Z80_OP_INC_R; out->reg1 = R_E; break;
    case 0x1D: out->type = Z80_OP_DEC_R; out->reg1 = R_E; break;
    case 0x1E: /* LD E, n */
        out->type = Z80_OP_LD_R_N; out->reg1 = R_E;
        out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0x1F: out->type = Z80_OP_RRA; break;  /* RRA */

    case 0x20: /* JR NZ, d */
        out->type = Z80_OP_JR_CC_E; out->cc = CC_NZ;
        out->disp = (int8_t)mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0x21: /* LD HL, nn */
        out->type = Z80_OP_LD_RR_NN; out->reg1 = RR_HL;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0x22: /* LD (nn), HL */
        out->type = Z80_OP_LD_NN_HL;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0x23: out->type = Z80_OP_INC_RR; out->reg1 = RR_HL; break;
    case 0x24: out->type = Z80_OP_INC_R; out->reg1 = R_H; break;
    case 0x25: out->type = Z80_OP_DEC_R; out->reg1 = R_H; break;
    case 0x26: /* LD H, n */
        out->type = Z80_OP_LD_R_N; out->reg1 = R_H;
        out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0x27: out->type = Z80_OP_DAA; break;
    case 0x28: /* JR Z, d */
        out->type = Z80_OP_JR_CC_E; out->cc = CC_Z;
        out->disp = (int8_t)mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0x29: out->type = Z80_OP_ADD_HL_RR; out->reg1 = RR_HL; break;
    case 0x2A: /* LD HL, (nn) */
        out->type = Z80_OP_LD_HL_indNN;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0x2B: out->type = Z80_OP_DEC_RR; out->reg1 = RR_HL; break;
    case 0x2C: out->type = Z80_OP_INC_R; out->reg1 = R_L; break;
    case 0x2D: out->type = Z80_OP_DEC_R; out->reg1 = R_L; break;
    case 0x2E: /* LD L, n */
        out->type = Z80_OP_LD_R_N; out->reg1 = R_L;
        out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0x2F: out->type = Z80_OP_CPL; break;

    case 0x30: /* JR NC, d */
        out->type = Z80_OP_JR_CC_E; out->cc = CC_NC;
        out->disp = (int8_t)mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0x31: /* LD SP, nn */
        out->type = Z80_OP_LD_RR_NN; out->reg1 = 3; /* special: SP */
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0x32: /* LD (nn), A */
        out->type = Z80_OP_LD_NN_A;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0x33: out->type = Z80_OP_INC_RR; out->reg1 = 3; /* SP */ break;
    case 0x34: out->type = Z80_OP_INC_R; out->reg1 = 6; break;  /* INC (HL) */
    case 0x35: out->type = Z80_OP_DEC_R; out->reg1 = 6; break;  /* DEC (HL) */
    case 0x36: /* LD (HL), n */
        out->type = Z80_OP_LD_HL_N;
        out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0x37: out->type = Z80_OP_SCF; break;
    case 0x38: /* JR C, d */
        out->type = Z80_OP_JR_CC_E; out->cc = CC_C;
        out->disp = (int8_t)mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0x39: out->type = Z80_OP_ADD_HL_RR; out->reg1 = 3; /* SP */ break;
    case 0x3A: /* LD A, (nn) */
        out->type = Z80_OP_LD_A_NN;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0x3B: out->type = Z80_OP_DEC_RR; out->reg1 = 3; /* SP */ break;
    case 0x3C: out->type = Z80_OP_INC_R; out->reg1 = R_A; break;
    case 0x3D: out->type = Z80_OP_DEC_R; out->reg1 = R_A; break;
    case 0x3E: /* LD A, n */
        out->type = Z80_OP_LD_R_N; out->reg1 = R_A;
        out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0x3F: out->type = Z80_OP_CCF; break;

    /* 0x40-0x7F : LD r, r'  (including HALT at 0x76) */
    case 0x76: out->type = Z80_OP_HALT; break;
    default:
        if (op >= 0x40 && op <= 0x7F) {
            out->type = Z80_OP_LD_R_R;
            out->reg1 = (op >> 3) & 7;
            out->reg2 = op & 7;
        } else if (op >= 0x80 && op <= 0x87) {
            out->type = Z80_OP_ADD_A_R; out->reg1 = op & 7;
        } else if (op >= 0x88 && op <= 0x8F) {
            out->type = Z80_OP_ADC_A_R; out->reg1 = op & 7;
        } else if (op == 0xC6) {
            out->type = Z80_OP_ADD_A_N; out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++;
        } else if (op == 0xCE) {
            out->type = Z80_OP_ADC_A_N; out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++;
        } else if (op >= 0x90 && op <= 0x97) {
            out->type = Z80_OP_SUB_A_R; out->reg1 = op & 7;
        } else if (op >= 0x98 && op <= 0x9F) {
            out->type = Z80_OP_SBC_A_R; out->reg1 = op & 7;
        } else if (op == 0xD6) {
            out->type = Z80_OP_SUB_A_N; out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++;
        } else if (op == 0xDE) {
            out->type = Z80_OP_SBC_A_N; out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++;
        } else if (op >= 0xA0 && op <= 0xA7) {
            out->type = Z80_OP_AND_A_R; out->reg1 = op & 7;
        } else if (op == 0xE6) {
            out->type = Z80_OP_AND_A_N; out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++;
        } else if (op >= 0xB0 && op <= 0xB7) {
            out->type = Z80_OP_OR_A_R; out->reg1 = op & 7;
        } else if (op == 0xF6) {
            out->type = Z80_OP_OR_A_N; out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++;
        } else if (op >= 0xA8 && op <= 0xAF) {
            out->type = Z80_OP_XOR_A_R; out->reg1 = op & 7;
        } else if (op == 0xEE) {
            out->type = Z80_OP_XOR_A_N; out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++;
        } else if (op >= 0xB8 && op <= 0xBF) {
            out->type = Z80_OP_CP_A_R; out->reg1 = op & 7;
        } else if (op == 0xFE) {
            out->type = Z80_OP_CP_A_N; out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++;
        } else {
            /* Many more to implement: 0xC0-CF, 0xD0-DF, 0xE0-EF, 0xF0-FF */
        }
        break;

    /* 0xC0-CF group */
    case 0xC0: out->type = Z80_OP_RET_CC; out->cc = CC_NZ; break;
    case 0xC1: out->type = Z80_OP_POP_RR; out->reg1 = RR_BC; break;
    case 0xC2: /* JP NZ, nn */
        out->type = Z80_OP_JP_CC_NN; out->cc = CC_NZ;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0xC3: /* JP nn */
        out->type = Z80_OP_JP_NN;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0xC4: /* CALL NZ, nn */
        out->type = Z80_OP_CALL_CC_NN; out->cc = CC_NZ;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0xC5: out->type = Z80_OP_PUSH_RR; out->reg1 = RR_BC; break;
    case 0xC6: /* ADD A, n */
        out->type = Z80_OP_ADD_A_N;
        out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0xC7: out->type = Z80_OP_RST; out->imm8 = 0x00; break;
    case 0xC8: out->type = Z80_OP_RET_CC; out->cc = CC_Z; break;
    case 0xC9: out->type = Z80_OP_RET; break;
    case 0xCA: /* JP Z, nn */
        out->type = Z80_OP_JP_CC_NN; out->cc = CC_Z;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0xCC: /* CALL Z, nn */
        out->type = Z80_OP_CALL_CC_NN; out->cc = CC_Z;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0xCD: /* CALL nn */
        out->type = Z80_OP_CALL_NN;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0xCE: /* ADC A, n */
        out->type = Z80_OP_ADC_A_N;
        out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0xCF: out->type = Z80_OP_RST; out->imm8 = 0x08; break;

    /* 0xD0-DF */
    case 0xD0: out->type = Z80_OP_RET_CC; out->cc = CC_NC; break;
    case 0xD1: out->type = Z80_OP_POP_RR; out->reg1 = RR_DE; break;
    case 0xD2: /* JP NC, nn */
        out->type = Z80_OP_JP_CC_NN; out->cc = CC_NC;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0xD3: /* OUT (n), A */
        out->type = Z80_OP_OUT_N_A;
        out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0xD4: /* CALL NC, nn */
        out->type = Z80_OP_CALL_CC_NN; out->cc = CC_NC;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0xD5: out->type = Z80_OP_PUSH_RR; out->reg1 = RR_DE; break;
    case 0xD6: /* SUB n */
        out->type = Z80_OP_SUB_A_N;
        out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0xD7: out->type = Z80_OP_RST; out->imm8 = 0x10; break;
    case 0xD8: out->type = Z80_OP_RET_CC; out->cc = CC_C; break;
    case 0xD9: out->type = Z80_OP_EXX; break;
    case 0xDA: /* JP C, nn */
        out->type = Z80_OP_JP_CC_NN; out->cc = CC_C;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0xDB: /* IN A, (n) */
        out->type = Z80_OP_IN_A_N;
        out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0xDC: /* CALL C, nn */
        out->type = Z80_OP_CALL_CC_NN; out->cc = CC_C;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0xDE: /* SBC A, n */
        out->type = Z80_OP_SBC_A_N;
        out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0xDF: out->type = Z80_OP_RST; out->imm8 = 0x18; break;

    /* 0xE0-EF */
    case 0xE0: out->type = Z80_OP_RET_CC; out->cc = CC_PO; break;
    case 0xE1: out->type = Z80_OP_POP_RR; out->reg1 = RR_HL; break;
    case 0xE2: /* JP PO, nn */
        out->type = Z80_OP_JP_CC_NN; out->cc = CC_PO;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0xE3: out->type = Z80_OP_EX_SP_HL; break;
    case 0xE4: /* CALL PO, nn */
        out->type = Z80_OP_CALL_CC_NN; out->cc = CC_PO;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0xE5: out->type = Z80_OP_PUSH_RR; out->reg1 = RR_HL; break;
    case 0xE6: out->type = Z80_OP_AND_A_N; out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++; break;  /* AND n */
    case 0xE7: out->type = Z80_OP_RST; out->imm8 = 0x20; break;
    case 0xE8: out->type = Z80_OP_RET_CC; out->cc = CC_PE; break;
    case 0xE9: out->type = Z80_OP_JP_HL; break;  /* JP (HL) */
    case 0xEA: /* JP PE, nn */
        out->type = Z80_OP_JP_CC_NN; out->cc = CC_PE;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0xEB: out->type = Z80_OP_EX_DE_HL; break;
    case 0xEC: /* CALL PE, nn */
        out->type = Z80_OP_CALL_CC_NN; out->cc = CC_PE;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0xEE: /* XOR n */
        out->type = Z80_OP_XOR_A_N;
        out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0xEF: out->type = Z80_OP_RST; out->imm8 = 0x28; break;

    /* 0xF0-FF */
    case 0xF0: out->type = Z80_OP_RET_CC; out->cc = CC_P; break;
    case 0xF1: out->type = Z80_OP_POP_RR; out->reg1 = RR_AF; break;
    case 0xF2: /* JP P, nn */
        out->type = Z80_OP_JP_CC_NN; out->cc = CC_P;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0xF3: out->type = Z80_OP_DI; break;
    case 0xF4: /* CALL P, nn */
        out->type = Z80_OP_CALL_CC_NN; out->cc = CC_P;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0xF5: out->type = Z80_OP_PUSH_RR; out->reg1 = RR_AF; break;
    case 0xF6: /* OR n */
        out->type = Z80_OP_OR_A_N;
        out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0xF7: out->type = Z80_OP_RST; out->imm8 = 0x30; break;
    case 0xF8: out->type = Z80_OP_RET_CC; out->cc = CC_M; break;
    case 0xF9: out->type = Z80_OP_LD_SP_HL; break;
    case 0xFA: /* JP M, nn */
        out->type = Z80_OP_JP_CC_NN; out->cc = CC_M;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0xFB: out->type = Z80_OP_EI; break;
    case 0xFC: /* CALL M, nn */
        out->type = Z80_OP_CALL_CC_NN; out->cc = CC_M;
        out->imm16 = mem[(pc+1)&0xFFFF] | (mem[(pc+2)&0xFFFF] << 8);
        out->bytes += 2; break;
    case 0xFE: out->type = Z80_OP_CP_A_N; out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++; break;  /* CP n */
    case 0xFF: out->type = Z80_OP_RST; out->imm8 = 0x38; break;
    }

    /* ED prefix is now fully handled at the top of this function and returns
     * before reaching here, so no late-pass override is needed. */

    /* DD/FD half-register remap: under DD/FD prefix, H (4) and L (5) refer
     * to IXH/IXL or IYH/IYL — but only for pure register forms (the memory
     * forms have already short-circuited above with their own op types).
     * The interpreter resolves codes 8/9 to IXH/IXL or IYH/IYL based on
     * out->prefix at execution time. */
    if (prefix == 0xDD || prefix == 0xFD) {
        switch (out->type) {
        case Z80_OP_LD_R_R:
        case Z80_OP_LD_R_N:
        case Z80_OP_INC_R:
        case Z80_OP_DEC_R:
        case Z80_OP_ADD_A_R:
        case Z80_OP_ADC_A_R:
        case Z80_OP_SUB_A_R:
        case Z80_OP_SBC_A_R:
        case Z80_OP_AND_A_R:
        case Z80_OP_OR_A_R:
        case Z80_OP_XOR_A_R:
        case Z80_OP_CP_A_R:
            if (out->reg1 == 4) out->reg1 = 8;
            else if (out->reg1 == 5) out->reg1 = 9;
            if (out->reg2 == 4) out->reg2 = 8;
            else if (out->reg2 == 5) out->reg2 = 9;
            break;
        default:
            break;
        }
    }

    /* Legacy placeholder cleanup — safe to ignore for now */
    if (prefix == 0xCB && out->type == 0) {
        out->type = Z80_OP_CB;
    }

    return out->bytes;
}

/* Tiny helper for debug printing */
const char *z80_op_name(z80_op_type t) {
    switch (t) {
    case Z80_OP_NOP: return "NOP";
    case Z80_OP_LD_RR_NN: return "LD rr,nn";
    case Z80_OP_LD_R_N: return "LD r,n";
    case Z80_OP_CALL_NN: return "CALL nn";
    case Z80_OP_RET: return "RET";
    case Z80_OP_JP_NN: return "JP nn";
    case Z80_OP_LD_NN_A: return "LD (nn),A";
    case Z80_OP_LD_A_NN: return "LD A,(nn)";
    case Z80_OP_OUT_N_A: return "OUT (n),A";
    default: return "???";
    }
}
