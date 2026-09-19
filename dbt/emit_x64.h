/* emit_x64.h — x86-64 code emission helpers for Z80 blocks.
 *
 * Same single-header static-inline style as emit_a64.h. Variable-length
 * encoding: every emitter writes prefix(es) + REX + opcode + ModRM/SIB
 * + displacement + immediate. Register operands are the architectural
 * numbers 0..15; the operand width is part of the emitter name
 * (`_r8_`, `_r16_`, `_r32_`, `_r64_`, `_m8_` ...).
 *
 * Byte registers: we never use AH/CH/DH/BH. Any 8-bit operand numbered
 * 4..7 forces a REX prefix so it means SPL/BPL/SIL/DIL.
 *
 * Memory operands are [base + index*scale + disp32]; index may be
 * X64_NOREG. The encoder handles the two x86 special cases: a base whose
 * low three bits are 100 (RSP/R12) needs a SIB byte, and a base whose
 * low bits are 101 (RBP/R13) cannot use mod=00, so a zero displacement
 * is encoded as disp8=0.
 */
#ifndef EMIT_X64_H
#define EMIT_X64_H

#include <stdint.h>
#include <string.h>

typedef struct {
    uint8_t *buf;
    uint32_t offset;
    uint32_t capacity;
} emit_t;

typedef enum {
    X64_RAX = 0, X64_RCX = 1, X64_RDX = 2,  X64_RBX = 3,
    X64_RSP = 4, X64_RBP = 5, X64_RSI = 6,  X64_RDI = 7,
    X64_R8  = 8, X64_R9  = 9, X64_R10 = 10, X64_R11 = 11,
    X64_R12 = 12, X64_R13 = 13, X64_R14 = 14, X64_R15 = 15
} x64_reg_t;

#define X64_NOREG (-1)

/* Condition codes as used in Jcc / SETcc / CMOVcc low nibble. */
typedef enum {
    X64_CC_O  = 0x0, X64_CC_NO = 0x1,
    X64_CC_B  = 0x2, X64_CC_AE = 0x3,   /* CF=1 / CF=0 */
    X64_CC_E  = 0x4, X64_CC_NE = 0x5,   /* ZF=1 / ZF=0 */
    X64_CC_BE = 0x6, X64_CC_A  = 0x7,
    X64_CC_S  = 0x8, X64_CC_NS = 0x9,
    X64_CC_P  = 0xA, X64_CC_NP = 0xB,
    X64_CC_L  = 0xC, X64_CC_GE = 0xD,
    X64_CC_LE = 0xE, X64_CC_G  = 0xF
} x64_cc_t;
#define X64_CC_C  X64_CC_B
#define X64_CC_NC X64_CC_AE
#define X64_CC_Z  X64_CC_E
#define X64_CC_NZ X64_CC_NE

/* ALU opcode groups: `op r/m, r` base opcode (r32 form; r8 form is
 * base - 1) and the /digit for the immediate forms (80/81/83). */
enum {
    X64_ALU_ADD = 0, X64_ALU_OR = 1, X64_ALU_ADC = 2, X64_ALU_SBB = 3,
    X64_ALU_AND = 4, X64_ALU_SUB = 5, X64_ALU_XOR = 6, X64_ALU_CMP = 7
};

/* Shift/rotate /digit for C0/C1/D0/D1. */
enum {
    X64_SH_ROL = 0, X64_SH_ROR = 1, X64_SH_RCL = 2, X64_SH_RCR = 3,
    X64_SH_SHL = 4, X64_SH_SHR = 5, X64_SH_SAR = 7
};

/* ---- Raw emit + cursor helpers ---- */

static inline void emit_byte(emit_t *e, uint8_t b) {
    if (e->offset < e->capacity) e->buf[e->offset] = b;
    e->offset++;
}
static inline void emit_u16(emit_t *e, uint16_t v) {
    emit_byte(e, (uint8_t)v); emit_byte(e, (uint8_t)(v >> 8));
}
static inline void emit_u32(emit_t *e, uint32_t v) {
    emit_u16(e, (uint16_t)v); emit_u16(e, (uint16_t)(v >> 16));
}
static inline void emit_u64(emit_t *e, uint64_t v) {
    emit_u32(e, (uint32_t)v); emit_u32(e, (uint32_t)(v >> 32));
}
static inline uint32_t emit_pos(emit_t *e) { return e->offset; }

/* Patch the rel32 field at `imm_off` so the branch lands on target_off.
 * (The displacement is relative to the end of the 4-byte field.) */
static inline void emit_patch_rel32(emit_t *e, uint32_t imm_off, uint32_t target_off) {
    int32_t disp = (int32_t)(target_off - (imm_off + 4));
    memcpy(e->buf + imm_off, &disp, 4);
}

/* ---- Encoding primitives ---- */

static inline int x64_byte_reg_needs_rex(int r) { return r >= 4 && r <= 7; }

static inline void x64_rex(emit_t *e, int w, int reg, int index, int base, int force) {
    uint8_t rex = 0x40;
    if (w)                 rex |= 0x08;
    if ((reg   >> 3) & 1)  rex |= 0x04;
    if (index >= 0 && ((index >> 3) & 1)) rex |= 0x02;
    if ((base  >> 3) & 1)  rex |= 0x01;
    if (rex != 0x40 || force) emit_byte(e, rex);
}

static inline void x64_modrm_mem(emit_t *e, int reg, int base, int index,
                                 int scale_log2, int32_t disp) {
    int need_sib = (index >= 0) || ((base & 7) == 4);
    int mod;
    if (disp == 0 && (base & 7) != 5) mod = 0;
    else if (disp >= -128 && disp <= 127) mod = 1;
    else mod = 2;
    if (need_sib) {
        emit_byte(e, (uint8_t)((mod << 6) | ((reg & 7) << 3) | 4));
        int idx = (index >= 0) ? (index & 7) : 4;
        emit_byte(e, (uint8_t)((scale_log2 << 6) | (idx << 3) | (base & 7)));
    } else {
        emit_byte(e, (uint8_t)((mod << 6) | ((reg & 7) << 3) | (base & 7)));
    }
    if (mod == 1)      emit_byte(e, (uint8_t)disp);
    else if (mod == 2) emit_u32(e, (uint32_t)disp);
}

/* opcode(s) reg, [base + index + disp] */
static inline void x64_mem(emit_t *e, int w, int pfx66, int force, int op0, int op1,
                           int reg, int base, int index, int32_t disp) {
    if (pfx66) emit_byte(e, 0x66);
    x64_rex(e, w, reg, index, base, force);
    emit_byte(e, (uint8_t)op0);
    if (op1 >= 0) emit_byte(e, (uint8_t)op1);
    x64_modrm_mem(e, reg, base, index, 0, disp);
}

/* opcode(s) reg, rm  (register-direct) */
static inline void x64_rr(emit_t *e, int w, int pfx66, int force, int op0, int op1,
                          int reg, int rm) {
    if (pfx66) emit_byte(e, 0x66);
    x64_rex(e, w, reg, -1, rm, force);
    emit_byte(e, (uint8_t)op0);
    if (op1 >= 0) emit_byte(e, (uint8_t)op1);
    emit_byte(e, (uint8_t)(0xC0 | ((reg & 7) << 3) | (rm & 7)));
}

/* ---- Moves ---- */

static inline void emit_mov_r32_r32(emit_t *e, int dst, int src) {
    x64_rr(e, 0, 0, 0, 0x89, -1, src, dst);
}
static inline void emit_mov_r64_r64(emit_t *e, int dst, int src) {
    x64_rr(e, 1, 0, 0, 0x89, -1, src, dst);
}
static inline void emit_mov_r8_r8(emit_t *e, int dst, int src) {
    x64_rr(e, 0, 0, x64_byte_reg_needs_rex(dst) || x64_byte_reg_needs_rex(src),
           0x88, -1, src, dst);
}
static inline void emit_mov_r32_imm32(emit_t *e, int dst, uint32_t imm) {
    x64_rex(e, 0, 0, -1, dst, 0);
    emit_byte(e, (uint8_t)(0xB8 | (dst & 7)));
    emit_u32(e, imm);
}
static inline void emit_mov_r64_imm64(emit_t *e, int dst, uint64_t imm) {
    x64_rex(e, 1, 0, -1, dst, 0);
    emit_byte(e, (uint8_t)(0xB8 | (dst & 7)));
    emit_u64(e, imm);
}
static inline void emit_mov_r8_imm8(emit_t *e, int dst, uint8_t imm) {
    x64_rex(e, 0, 0, -1, dst, x64_byte_reg_needs_rex(dst));
    emit_byte(e, (uint8_t)(0xB0 | (dst & 7)));
    emit_byte(e, imm);
}
static inline void emit_movzx_r32_r8(emit_t *e, int dst, int src) {
    x64_rr(e, 0, 0, x64_byte_reg_needs_rex(src), 0x0F, 0xB6, dst, src);
}
static inline void emit_movzx_r32_r16(emit_t *e, int dst, int src) {
    x64_rr(e, 0, 0, 0, 0x0F, 0xB7, dst, src);
}

/* Loads */
static inline void emit_movzx_r32_m8(emit_t *e, int dst, int base, int index, int32_t disp) {
    x64_mem(e, 0, 0, 0, 0x0F, 0xB6, dst, base, index, disp);
}
static inline void emit_movzx_r32_m16(emit_t *e, int dst, int base, int index, int32_t disp) {
    x64_mem(e, 0, 0, 0, 0x0F, 0xB7, dst, base, index, disp);
}
/* movzx r32, word [base + index*2^scale + disp] */
static inline void emit_movzx_r32_m16_sib(emit_t *e, int dst, int base, int index,
                                          int scale_log2, int32_t disp) {
    x64_rex(e, 0, dst, index, base, 0);
    emit_byte(e, 0x0F); emit_byte(e, 0xB7);
    x64_modrm_mem(e, dst, base, index, scale_log2, disp);
}
static inline void emit_mov_r32_m32(emit_t *e, int dst, int base, int index, int32_t disp) {
    x64_mem(e, 0, 0, 0, 0x8B, -1, dst, base, index, disp);
}
static inline void emit_mov_r64_m64(emit_t *e, int dst, int base, int index, int32_t disp) {
    x64_mem(e, 1, 0, 0, 0x8B, -1, dst, base, index, disp);
}

/* Stores */
static inline void emit_mov_m8_r8(emit_t *e, int base, int index, int32_t disp, int src) {
    x64_mem(e, 0, 0, x64_byte_reg_needs_rex(src), 0x88, -1, src, base, index, disp);
}
static inline void emit_mov_m16_r16(emit_t *e, int base, int index, int32_t disp, int src) {
    x64_mem(e, 0, 1, 0, 0x89, -1, src, base, index, disp);
}
static inline void emit_mov_m32_r32(emit_t *e, int base, int index, int32_t disp, int src) {
    x64_mem(e, 0, 0, 0, 0x89, -1, src, base, index, disp);
}
static inline void emit_mov_m64_r64(emit_t *e, int base, int index, int32_t disp, int src) {
    x64_mem(e, 1, 0, 0, 0x89, -1, src, base, index, disp);
}
static inline void emit_mov_m8_imm8(emit_t *e, int base, int index, int32_t disp, uint8_t imm) {
    x64_mem(e, 0, 0, 0, 0xC6, -1, 0, base, index, disp);
    emit_byte(e, imm);
}
static inline void emit_mov_m16_imm16(emit_t *e, int base, int index, int32_t disp, uint16_t imm) {
    x64_mem(e, 0, 1, 0, 0xC7, -1, 0, base, index, disp);
    emit_u16(e, imm);
}

/* ---- Memory compares (for the SMC bitmap check and cache probe) ---- */

static inline void emit_cmp_m8_imm8(emit_t *e, int base, int index, int32_t disp, uint8_t imm) {
    x64_mem(e, 0, 0, 0, 0x80, -1, 7, base, index, disp);
    emit_byte(e, imm);
}
static inline void emit_cmp_m16_imm8(emit_t *e, int base, int index, int32_t disp, int8_t imm) {
    x64_mem(e, 0, 1, 0, 0x83, -1, 7, base, index, disp);
    emit_byte(e, (uint8_t)imm);
}
static inline void emit_cmp_m32_r32(emit_t *e, int base, int index, int32_t disp, int reg) {
    x64_mem(e, 0, 0, 0, 0x39, -1, reg, base, index, disp);
}

/* ---- ALU ---- */

/* op r32dst, r32src */
static inline void emit_alu_r32_r32(emit_t *e, int op, int dst, int src) {
    x64_rr(e, 0, 0, 0, (op << 3) | 0x01, -1, src, dst);
}
static inline void emit_alu_r64_r64(emit_t *e, int op, int dst, int src) {
    x64_rr(e, 1, 0, 0, (op << 3) | 0x01, -1, src, dst);
}
/* op r8dst, r8src */
static inline void emit_alu_r8_r8(emit_t *e, int op, int dst, int src) {
    x64_rr(e, 0, 0, x64_byte_reg_needs_rex(dst) || x64_byte_reg_needs_rex(src),
           (op << 3) | 0x00, -1, src, dst);
}
/* op r8dst, [mem] */
static inline void emit_alu_r8_m8(emit_t *e, int op, int dst, int base, int index, int32_t disp) {
    x64_mem(e, 0, 0, x64_byte_reg_needs_rex(dst), (op << 3) | 0x02, -1, dst, base, index, disp);
}
/* op r32, imm (imm8 sign-extended form when it fits) */
static inline void emit_alu_r32_imm(emit_t *e, int op, int dst, int32_t imm) {
    if (imm >= -128 && imm <= 127) {
        x64_rr(e, 0, 0, 0, 0x83, -1, op, dst);
        emit_byte(e, (uint8_t)imm);
    } else {
        x64_rr(e, 0, 0, 0, 0x81, -1, op, dst);
        emit_u32(e, (uint32_t)imm);
    }
}
static inline void emit_alu_r64_imm(emit_t *e, int op, int dst, int32_t imm) {
    if (imm >= -128 && imm <= 127) {
        x64_rr(e, 1, 0, 0, 0x83, -1, op, dst);
        emit_byte(e, (uint8_t)imm);
    } else {
        x64_rr(e, 1, 0, 0, 0x81, -1, op, dst);
        emit_u32(e, (uint32_t)imm);
    }
}
/* op r16, imm8 (sign-extended) — 16-bit partial op, keeps upper bits */
static inline void emit_alu_r16_imm8(emit_t *e, int op, int dst, int8_t imm) {
    x64_rr(e, 0, 1, 0, 0x83, -1, op, dst);
    emit_byte(e, (uint8_t)imm);
}
/* op r16dst, r16src */
static inline void emit_alu_r16_r16(emit_t *e, int op, int dst, int src) {
    x64_rr(e, 0, 1, 0, (op << 3) | 0x01, -1, src, dst);
}
/* op r8, imm8 */
static inline void emit_alu_r8_imm8(emit_t *e, int op, int dst, uint8_t imm) {
    x64_rr(e, 0, 0, x64_byte_reg_needs_rex(dst), 0x80, -1, op, dst);
    emit_byte(e, imm);
}
/* add [mem64], r64 */
static inline void emit_add_m64_r64(emit_t *e, int base, int index, int32_t disp, int src) {
    x64_mem(e, 1, 0, 0, 0x01, -1, src, base, index, disp);
}

static inline void emit_test_r32_r32(emit_t *e, int a, int b) {
    x64_rr(e, 0, 0, 0, 0x85, -1, b, a);
}
static inline void emit_test_r32_imm32(emit_t *e, int r, uint32_t imm) {
    x64_rr(e, 0, 0, 0, 0xF7, -1, 0, r);
    emit_u32(e, imm);
}
static inline void emit_test_r8_imm8(emit_t *e, int r, uint8_t imm) {
    x64_rr(e, 0, 0, x64_byte_reg_needs_rex(r), 0xF6, -1, 0, r);
    emit_byte(e, imm);
}

static inline void emit_shift_r32_imm(emit_t *e, int sh, int r, uint8_t n) {
    if (n == 1) {
        x64_rr(e, 0, 0, 0, 0xD1, -1, sh, r);
    } else {
        x64_rr(e, 0, 0, 0, 0xC1, -1, sh, r);
        emit_byte(e, n);
    }
}
static inline void emit_shift_r8_imm(emit_t *e, int sh, int r, uint8_t n) {
    if (n == 1) {
        x64_rr(e, 0, 0, x64_byte_reg_needs_rex(r), 0xD0, -1, sh, r);
    } else {
        x64_rr(e, 0, 0, x64_byte_reg_needs_rex(r), 0xC0, -1, sh, r);
        emit_byte(e, n);
    }
}
#define emit_shl_r32_imm(e, r, n) emit_shift_r32_imm(e, X64_SH_SHL, r, n)
#define emit_shr_r32_imm(e, r, n) emit_shift_r32_imm(e, X64_SH_SHR, r, n)
#define emit_ror_r32_imm(e, r, n) emit_shift_r32_imm(e, X64_SH_ROR, r, n)
#define emit_rol_r32_imm(e, r, n) emit_shift_r32_imm(e, X64_SH_ROL, r, n)

static inline void emit_not_r32(emit_t *e, int r) { x64_rr(e, 0, 0, 0, 0xF7, -1, 2, r); }
static inline void emit_neg_r32(emit_t *e, int r) { x64_rr(e, 0, 0, 0, 0xF7, -1, 3, r); }
static inline void emit_inc_r8(emit_t *e, int r) {
    x64_rr(e, 0, 0, x64_byte_reg_needs_rex(r), 0xFE, -1, 0, r);
}
static inline void emit_dec_r8(emit_t *e, int r) {
    x64_rr(e, 0, 0, x64_byte_reg_needs_rex(r), 0xFE, -1, 1, r);
}
static inline void emit_inc_r32(emit_t *e, int r) { x64_rr(e, 0, 0, 0, 0xFF, -1, 0, r); }
static inline void emit_dec_r32(emit_t *e, int r) { x64_rr(e, 0, 0, 0, 0xFF, -1, 1, r); }

/* lea r32, [base + index + disp] */
static inline void emit_lea_r32(emit_t *e, int dst, int base, int index, int32_t disp) {
    x64_mem(e, 0, 0, 0, 0x8D, -1, dst, base, index, disp);
}
static inline void emit_lea_r64(emit_t *e, int dst, int base, int index, int32_t disp) {
    x64_mem(e, 1, 0, 0, 0x8D, -1, dst, base, index, disp);
}

static inline void emit_setcc_r8(emit_t *e, int cc, int r) {
    x64_rr(e, 0, 0, x64_byte_reg_needs_rex(r), 0x0F, 0x90 | cc, 0, r);
}
static inline void emit_cmovcc_r32_r32(emit_t *e, int cc, int dst, int src) {
    x64_rr(e, 0, 0, 0, 0x0F, 0x40 | cc, dst, src);
}
/* bt r32, imm8 — CF = bit */
static inline void emit_bt_r32_imm8(emit_t *e, int r, uint8_t bit) {
    x64_rr(e, 0, 0, 0, 0x0F, 0xBA, 4, r);
    emit_byte(e, bit);
}
static inline void emit_lahf(emit_t *e) { emit_byte(e, 0x9F); }

/* movzx r32, AH — the one legacy high-byte form we use (after LAHF).
 * dst must be one of RAX..RDI (no REX possible with AH). */
static inline void emit_movzx_r32_ah(emit_t *e, int dst) {
    emit_byte(e, 0x0F); emit_byte(e, 0xB6);
    emit_byte(e, (uint8_t)(0xC0 | ((dst & 7) << 3) | 4));
}
/* op byte [mem], imm8 */
static inline void emit_alu_m8_imm8(emit_t *e, int op, int base, int index, int32_t disp, uint8_t imm) {
    x64_mem(e, 0, 0, 0, 0x80, -1, op, base, index, disp);
    emit_byte(e, imm);
}
/* inc/dec word [mem] */
static inline void emit_incdec_m16(emit_t *e, int base, int index, int32_t disp, int is_inc) {
    x64_mem(e, 0, 1, 0, 0xFF, -1, is_inc ? 0 : 1, base, index, disp);
}
/* Jcc rel8 (forward skip of a few bytes; caller supplies the exact rel8). */
static inline void emit_jcc_rel8(emit_t *e, int cc, int8_t rel8) {
    emit_byte(e, (uint8_t)(0x70 | cc));
    emit_byte(e, (uint8_t)rel8);
}

/* ---- Control flow ---- */

/* Jcc rel32 with a zero placeholder; returns the offset of the rel32
 * field for emit_patch_rel32. */
static inline uint32_t emit_jcc_rel32(emit_t *e, int cc) {
    emit_byte(e, 0x0F);
    emit_byte(e, (uint8_t)(0x80 | cc));
    uint32_t at = emit_pos(e);
    emit_u32(e, 0);
    return at;
}
static inline uint32_t emit_jmp_rel32(emit_t *e) {
    emit_byte(e, 0xE9);
    uint32_t at = emit_pos(e);
    emit_u32(e, 0);
    return at;
}
/* jmp rel32 to a known buffer offset. */
static inline void emit_jmp_rel32_to(emit_t *e, uint32_t target_off) {
    uint32_t at = emit_jmp_rel32(e);
    emit_patch_rel32(e, at, target_off);
}
static inline void emit_jcc_rel32_to(emit_t *e, int cc, uint32_t target_off) {
    uint32_t at = emit_jcc_rel32(e, cc);
    emit_patch_rel32(e, at, target_off);
}
static inline void emit_jmp_r64(emit_t *e, int r)  { x64_rr(e, 0, 0, 0, 0xFF, -1, 4, r); }
static inline void emit_call_r64(emit_t *e, int r) { x64_rr(e, 0, 0, 0, 0xFF, -1, 2, r); }
static inline void emit_jmp_m64(emit_t *e, int base, int index, int32_t disp) {
    x64_mem(e, 0, 0, 0, 0xFF, -1, 4, base, index, disp);
}
static inline void emit_ret(emit_t *e) { emit_byte(e, 0xC3); }
static inline void emit_push_r64(emit_t *e, int r) {
    x64_rex(e, 0, 0, -1, r, 0);
    emit_byte(e, (uint8_t)(0x50 | (r & 7)));
}
static inline void emit_pop_r64(emit_t *e, int r) {
    x64_rex(e, 0, 0, -1, r, 0);
    emit_byte(e, (uint8_t)(0x58 | (r & 7)));
}
static inline void emit_int3(emit_t *e) { emit_byte(e, 0xCC); }
static inline void emit_nop(emit_t *e)  { emit_byte(e, 0x90); }

#endif /* EMIT_X64_H */
