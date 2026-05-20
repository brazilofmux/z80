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

    /* Handle CB (possibly DD CB d xx or FD CB d xx) */
    if (op == 0xCB) {
        out->prefix = prefix ? prefix : 0xCB;

        if (prefix) {
            /* DD CB d op or FD CB d op */
            out->disp = (int8_t)mem[pc & 0xFFFF];
            pc++; out->bytes++;
        }

        out->imm8 = mem[pc & 0xFFFF];   /* the CB sub-opcode */
        pc++; out->bytes++;

        out->type = Z80_OP_CB;
        out->opcode = out->imm8;
        return out->bytes;
    }

    /* Handle ED (rarely prefixed by DD/FD in practice) */
    if (op == 0xED) {
        out->prefix = 0xED;
        pc++; out->bytes++;
        op = mem[pc & 0xFFFF];
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
        out->disp = (int8_t)mem[pc & 0xFFFF];
        pc++;
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
        case 0x96: out->type = Z80_OP_SUB_A_HL_ind; out->reg1 = 6; return out->bytes;
        case 0xA6: out->type = Z80_OP_AND_A_HL_ind; out->reg1 = 6; return out->bytes;
        case 0xAE: out->type = Z80_OP_XOR_A_HL_ind; out->reg1 = 6; return out->bytes;
        case 0xB6: out->type = Z80_OP_OR_A_HL_ind;  out->reg1 = 6; return out->bytes;
        case 0xBE: out->type = Z80_OP_CP_A_HL_ind;  out->reg1 = 6; return out->bytes;

        /* Half-registers */
        case 0x7C: out->type = Z80_OP_LD_A_IXH; return out->bytes;
        case 0x7D: out->type = Z80_OP_LD_A_IXL; return out->bytes;
        }
    }

    /* -----------------------------------------------------------------------
     * Main dispatch table (only for non-prefix-handled instructions).
     * -------------------------------------------------------------------- */
    if (!handled_by_prefix) {
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
    case 0x07: /* RLCA */ /* TODO */ break;
    case 0x08: out->type = Z80_OP_EX_AF_AF; break;
    case 0x09: out->type = Z80_OP_ADD_HL_RR; out->reg1 = RR_BC; break;
    case 0x0A: out->type = Z80_OP_LD_A_BC; break;
    case 0x0B: out->type = Z80_OP_DEC_RR; out->reg1 = RR_BC; break;
    case 0x0C: out->type = Z80_OP_INC_R; out->reg1 = R_C; break;
    case 0x0D: out->type = Z80_OP_DEC_R; out->reg1 = R_C; break;
    case 0x0E: /* LD C, n */
        out->type = Z80_OP_LD_R_N; out->reg1 = R_C;
        out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0x0F: /* RRCA */ break;

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
    case 0x17: /* RLA */ break;
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
    case 0x1F: /* RRA */ break;

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
    case 0x34: /* INC (HL) */ /* TODO */ break;
    case 0x35: /* DEC (HL) */ /* TODO */ break;
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
    case 0x76: /* HALT */ /* TODO */ break;
    default:
        if (op >= 0x40 && op <= 0x7F) {
            out->type = Z80_OP_LD_R_R;
            out->reg1 = (op >> 3) & 7;
            out->reg2 = op & 7;
        } else if (op >= 0x80 && op <= 0x87) {
            out->type = Z80_OP_ADD_A_R; out->reg1 = op & 7;
        } else if (op == 0xC6) {
            out->type = Z80_OP_ADD_A_N; out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++;
        } else if (op >= 0x90 && op <= 0x97) {
            out->type = Z80_OP_SUB_A_R; out->reg1 = op & 7;
        } else if (op == 0xD6) {
            out->type = Z80_OP_SUB_A_N; out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++;
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
    case 0xC6: /* already handled above */ break;
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
    case 0xCE: /* ADC A, n */ /* TODO */ break;
    case 0xCF: out->type = Z80_OP_RST; out->imm8 = 0x08; break;

    /* 0xD0-DF */
    case 0xD0: out->type = Z80_OP_RET_CC; out->cc = CC_NC; break;
    case 0xD1: out->type = Z80_OP_POP_RR; out->reg1 = RR_DE; break;
    case 0xD2: /* JP NC, nn */ /* TODO pattern */ break;
    case 0xD3: /* OUT (n), A */
        out->type = Z80_OP_OUT_N_A;
        out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0xD4: /* CALL NC, nn */ break;
    case 0xD5: out->type = Z80_OP_PUSH_RR; out->reg1 = RR_DE; break;
    case 0xD6: /* SUB n - handled above */ break;
    case 0xD7: out->type = Z80_OP_RST; out->imm8 = 0x10; break;
    case 0xD8: out->type = Z80_OP_RET_CC; out->cc = CC_C; break;
    case 0xD9: out->type = Z80_OP_EXX; break;
    case 0xDA: /* JP C, nn */ break;
    case 0xDB: /* IN A, (n) */
        out->type = Z80_OP_IN_A_N;
        out->imm8 = mem[(pc+1)&0xFFFF]; out->bytes++; break;
    case 0xDC: /* CALL C, nn */ break;
    case 0xDE: /* SBC A, n */ break;
    case 0xDF: out->type = Z80_OP_RST; out->imm8 = 0x18; break;

    /* 0xE0-EF */
    case 0xE0: out->type = Z80_OP_RET_CC; out->cc = CC_PO; break;
    case 0xE1: out->type = Z80_OP_POP_RR; out->reg1 = RR_HL; break;
    case 0xE2: /* JP PO, nn */ break;
    case 0xE3: /* EX (SP), HL */ /* TODO */ break;
    case 0xE4: /* CALL PO, nn */ break;
    case 0xE5: out->type = Z80_OP_PUSH_RR; out->reg1 = RR_HL; break;
    case 0xE6: /* AND n - handled */ break;
    case 0xE7: out->type = Z80_OP_RST; out->imm8 = 0x20; break;
    case 0xE8: out->type = Z80_OP_RET_CC; out->cc = CC_PE; break;
    case 0xE9: /* JP (HL) */ /* TODO */ break;
    case 0xEA: /* JP PE, nn */ break;
    case 0xEB: out->type = Z80_OP_EX_DE_HL; break;
    case 0xEC: /* CALL PE, nn */ break;
    case 0xEE: /* XOR n - handled */ break;
    case 0xEF: out->type = Z80_OP_RST; out->imm8 = 0x28; break;

    /* 0xF0-FF */
    case 0xF0: out->type = Z80_OP_RET_CC; out->cc = CC_P; break;
    case 0xF1: out->type = Z80_OP_POP_RR; out->reg1 = RR_AF; break;
    case 0xF2: /* JP P, nn */ break;
    case 0xF3: /* DI */ /* TODO */ break;
    case 0xF4: /* CALL P, nn */ break;
    case 0xF5: out->type = Z80_OP_PUSH_RR; out->reg1 = RR_AF; break;
    case 0xF6: /* OR n - handled */ break;
    case 0xF7: out->type = Z80_OP_RST; out->imm8 = 0x30; break;
    case 0xF8: out->type = Z80_OP_RET_CC; out->cc = CC_M; break;
    case 0xF9: out->type = Z80_OP_LD_SP_HL; break;
    case 0xFA: /* JP M, nn */ break;
    case 0xFB: /* EI */ /* TODO */ break;
    case 0xFC: /* CALL M, nn */ break;
    case 0xFE: /* CP n - handled */ break;
    case 0xFF: out->type = Z80_OP_RST; out->imm8 = 0x38; break;
    }

    /* ED prefix block instructions (very common in CP/M for I/O and block moves) */
    if (prefix == 0xED) {
        switch (op) {
        case 0x44: out->type = Z80_OP_NEG; break;
        case 0xA0: out->type = Z80_OP_LDI; break;
        case 0xA8: out->type = Z80_OP_LDD; break;
        case 0xB0: out->type = Z80_OP_LDIR; break;
        case 0xB8: out->type = Z80_OP_LDDR; break;
        /* Many more: CPI, CPIR, INI, OUTI, etc. — added as encountered */
        default:
            /* Unknown ED xx — interpreter will loudly reject when executed */
            break;
        }
    }

    /* DD/FD (IX/IY) handling is incomplete in this first pass.
     * Real implementation will need a second decode pass for the
     * "main opcode after DD/FD" and special (IX+d) forms.
     */
    if (prefix == 0xDD || prefix == 0xFD) {
        /* For now we just record that we saw it; execution will be sad. */
    }

    if (prefix == 0xCB) {
        /* Bit, rotate, and shift group — huge table.
         * For the first bring-up we will implement the most common ones
         * (BIT 7, RES 0-7, SET 0-7, RLC/RRC/RL/RR/SLA etc.) as we hit them.
         */
        out->type = Z80_OP_UNKNOWN; /* force interpreter to complain loudly */
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
