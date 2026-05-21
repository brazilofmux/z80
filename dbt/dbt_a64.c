/* dbt_a64.c — AArch64 backend for the Z80 DBT.
 *
 * Translates loads/stores/16-bit boring ops/control flow/8-bit ALU.
 * Anything else (CB/ED/DD/FD prefixes, conditional branches, PUSH/POP,
 * block ops, etc.) ends the block; the run loop in dbt_common.c falls
 * back to the interpreter for that single instruction.
 *
 * Host register convention inside a translated block:
 *   X19 = z80_cpu_t *cpu        (callee-saved by trampoline)
 *   X20 = cpu->mem (uint8_t *)  (mem base for guest LDRB/STRB UXTW)
 *   X21 = block cache base      (unused until chaining)
 *   X22 = saved LR              (BLR to flag helpers clobbers X30)
 *   X23 = code_bitmap base      (inlined SMC fast-path check on stores)
 *   X9 / X10 / X11 / X12 = scratch
 *
 * Block ABI: block updates cpu->pc and cpu->insn_count, then RETs back
 * into the trampoline. (See dbt_emit_trampoline below.)
 */
#include "dbt.h"
#include "../core/z80.h"
#include "../cpm/cpm.h"
#include "dbt_flags.h"
#include "emit_a64.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

int dbt_jit_available(void) { return 1; }

/* ----------------------------------------------------------------------
 * Trampoline.
 *
 * Called from C as:
 *   void trampoline(z80_cpu_t *cpu, uint8_t *mem, void *block, void *cache);
 * with the AAPCS64 mapping cpu=X0, mem=X1, block=X2, cache=X3.
 *
 * Saves the AAPCS64 callee-saved regs we touch (FP/LR + X19..X22), binds
 * the host register convention, BLRs into `block`, and unwinds.
 * ---------------------------------------------------------------------- */
void dbt_emit_trampoline(z80_dbt_t *dbt) {
    emit_t e = { .buf = dbt->code_buf, .offset = 0, .capacity = CODE_BUF_SIZE };

    /* Frame: 64 bytes (16-byte aligned).
     *   [SP+ 0] FP, LR
     *   [SP+16] X19, X20
     *   [SP+32] X21, X22
     *   [SP+48] X23, X24       (X24 unused but STP wants a pair) */
    emit_stp_pre_sp (&e, A64_W29, A64_W30, -64);
    emit_stp_x64_off(&e, A64_W19, A64_W20, A64_SP, 16);
    emit_stp_x64_off(&e, A64_W21, A64_W22, A64_SP, 32);
    emit_stp_x64_off(&e, A64_W23, A64_W24, A64_SP, 48);

    /* Bind host register convention. */
    emit_mov_x64_x64(&e, A64_W19, A64_W0);   /* X19 = cpu  */
    emit_mov_x64_x64(&e, A64_W20, A64_W1);   /* X20 = mem  */
    emit_mov_x64_x64(&e, A64_W21, A64_W3);   /* X21 = cache base */
    emit_mov_x64_x64(&e, A64_W23, A64_W4);   /* X23 = code_bitmap base */

    /* BLR into the block (pointer in X2). The block's RET returns here. */
    emit_blr(&e, A64_W2);

    /* Unwind. */
    emit_ldp_x64_off(&e, A64_W23, A64_W24, A64_SP, 48);
    emit_ldp_x64_off(&e, A64_W21, A64_W22, A64_SP, 32);
    emit_ldp_x64_off(&e, A64_W19, A64_W20, A64_SP, 16);
    emit_ldp_post_sp(&e, A64_W29, A64_W30, 64);
    emit_ret(&e);

    dbt->code_used = e.offset;
    __builtin___clear_cache((char *)dbt->code_buf,
                            (char *)dbt->code_buf + e.offset);
}

/* ----------------------------------------------------------------------
 * Translator.
 * ---------------------------------------------------------------------- */

/* z80_cpu_t offsets — referenced from emitted code. We compute these
 * with offsetof at codegen time so the layout stays soft. */
#define OFF_F          offsetof(z80_cpu_t, f)
#define OFF_A          offsetof(z80_cpu_t, a)
#define OFF_C          offsetof(z80_cpu_t, c)
#define OFF_B          offsetof(z80_cpu_t, b)
#define OFF_E          offsetof(z80_cpu_t, e)
#define OFF_D          offsetof(z80_cpu_t, d)
#define OFF_L          offsetof(z80_cpu_t, l)
#define OFF_H          offsetof(z80_cpu_t, h)
#define OFF_BC         offsetof(z80_cpu_t, bc)
#define OFF_DE         offsetof(z80_cpu_t, de)
#define OFF_HL         offsetof(z80_cpu_t, hl)
#define OFF_SP         offsetof(z80_cpu_t, sp)
#define OFF_PC         offsetof(z80_cpu_t, pc)
#define OFF_Q          offsetof(z80_cpu_t, q)
#define OFF_INSN_COUNT offsetof(z80_cpu_t, insn_count)

/* Map a Z80 reg code 0..7 (B,C,D,E,H,L,(HL),A) to its byte offset
 * within z80_cpu_t. Code 6 is (HL), handled by the memory-op cases
 * via X20 + HL. Codes 8/9 (IXH/IXL/IYH/IYL with DD/FD prefix) are not
 * handled here — first cut skips them. Returns -1 on unsupported. */
static int reg8_offset(int r) {
    switch (r) {
    case 0: return OFF_B;
    case 1: return OFF_C;
    case 2: return OFF_D;
    case 3: return OFF_E;
    case 4: return OFF_H;
    case 5: return OFF_L;
    case 7: return OFF_A;
    default: return -1;
    }
}

/* Map a 16-bit pair code 0..3 to the union field's offset. Code 3 is
 * SP here (LD rr,nn uses SP; PUSH/POP uses AF — not in the first cut). */
static int rr_offset(int rr) {
    switch (rr) {
    case 0: return OFF_BC;
    case 1: return OFF_DE;
    case 2: return OFF_HL;
    case 3: return OFF_SP;
    default: return -1;
    }
}

/* ---- Tiny emit helpers used by the per-op cases. W9 is the canonical
 *      scratch for values, W10 for addresses. ---- */

static inline void emit_load_imm16_into(emit_t *e, a64_reg_t rd, uint16_t imm) {
    emit_movz_w32(e, rd, imm, 0);
}

/* W9 = mem[W10] — guest byte load via the X20 mem base. */
static inline void emit_guest_loadb_w9_at_w10(emit_t *e) {
    emit_ldrb_reg_uxtw(e, A64_W9, A64_W20, A64_W10);
}

/* mem[W10] = W9 — guest byte store via X20, with inlined SMC check.
 *
 * The check loads dbt->code_bitmap[addr] via X23 (already bound by the
 * trampoline). If the byte is zero (no cached block covered this
 * address), the CBZ skips the BLR entirely — non-SMC stores cost just
 * one extra LDRB + CBZ. Real SMC stores trip into z80_jit_post_store
 * which invalidates the affected cache entries.
 *
 * The BLR clobbers W9/W10/W11/W12 (caller-saved) so every caller must
 * reload anything it still needs afterwards.
 *
 * CALL's stack pushes go through emit_strb_reg_uxtw directly — the
 * guest stack essentially never overlaps code so the SMC check would
 * just be wasted work there. */
static void emit_guest_storeb_w9_at_w10_smc(emit_t *e) {
    emit_strb_reg_uxtw(e, A64_W9,  A64_W20, A64_W10);
    emit_ldrb_reg_uxtw(e, A64_W11, A64_W23, A64_W10);

    /* CBZ Wtmp, .skip   — placeholder offset, patched once we know the
     * SMC handler's emitted length. */
    uint32_t cbz_at = e->offset;
    emit_cbz_w32(e, A64_W11, 0);

    /* SMC path: post_store(cpu, addr). */
    emit_mov_x64_x64(e, A64_W0, A64_W19);
    emit_mov_w32_w32(e, A64_W1, A64_W10);
    emit_mov_x64_imm64(e, A64_W9, (uint64_t)(uintptr_t)z80_jit_post_store);
    emit_blr(e, A64_W9);

    /* Patch the CBZ to skip past the helper-call block. */
    emit_patch_cond19(e, cbz_at, e->offset);
}

/* Wdst = (Wsrc + delta) & 0xFFFF — keep an address register in canonical
 * 16-bit form before using it as an LDRB/STRB UXTW index. Necessary
 * because Z80 SP+1 / SP-2 can wrap at 0xFFFF while the host buffer is
 * exactly 64KB and UXTW would zero-extend an out-of-range value. */
static void emit_add_mask16(emit_t *e, a64_reg_t dst, a64_reg_t src, uint32_t delta) {
    emit_add_w32_imm(e, dst, src, delta);
    (void)emit_and_w32_imm(e, dst, dst, 0xFFFF);
}
static void emit_sub_mask16(emit_t *e, a64_reg_t dst, a64_reg_t src, uint32_t delta) {
    emit_sub_w32_imm(e, dst, src, delta);
    (void)emit_and_w32_imm(e, dst, dst, 0xFFFF);
}

/* Per-block prologue. The trampoline BLRs in with X30 (LR) pointing at
 * the trampoline's resume site. Any BLR we make inside the block (e.g.
 * to a flag helper) would clobber X30, so we stash it in X22 — which is
 * AAPCS64 callee-saved and already preserved across the trampoline.
 * Emitted unconditionally; even ALU-free blocks pay 1 host insn for it. */
static void emit_block_prologue(emit_t *e) {
    emit_mov_x64_x64(e, A64_W22, A64_W30);
}

/* Per-block tail: optionally clear cpu->q (when the last emitted op did
 * NOT modify F — helpers set q=1 themselves, so leave their write alone),
 * bump insn_count by delta, restore LR from X22, RET to trampoline. */
static void emit_block_tail(emit_t *e, uint32_t insn_count_delta, int clear_q) {
    if (clear_q) {
        emit_strb_imm(e, A64_WZR, A64_W19, OFF_Q);
    }

    emit_ldr_x64_imm(e, A64_W9, A64_W19, OFF_INSN_COUNT);
    emit_add_x64_imm(e, A64_W9, A64_W9, insn_count_delta);
    emit_str_x64_imm(e, A64_W9, A64_W19, OFF_INSN_COUNT);

    emit_mov_x64_x64(e, A64_W30, A64_W22);
    emit_ret(e);
}

/* Convenience: write cpu->pc = final_pc, then emit tail. Used by blocks
 * that fall through (no branch op already wrote pc). */
static void emit_block_epilogue(emit_t *e, uint16_t final_pc,
                                uint32_t insn_count_delta, int clear_q) {
    emit_movz_w32(e, A64_W9, final_pc, 0);
    emit_strh_imm(e, A64_W9, A64_W19, OFF_PC);
    emit_block_tail(e, insn_count_delta, clear_q);
}

/* Emit "MOV X0, X19 ; MOVZ/K X9, <helper> ; BLR X9". Result lands in W0
 * (helpers either return void or a uint8 return value). */
static void emit_call_helper(emit_t *e, void *helper_addr) {
    emit_mov_x64_x64(e, A64_W0, A64_W19);
    emit_mov_x64_imm64(e, A64_W9, (uint64_t)(uintptr_t)helper_addr);
    emit_blr(e, A64_W9);
}

/* Bitfield returned by emit_op so the translator knows what the op did. */
#define OP_FALL_THROUGH 0x0
#define OP_ENDS_BLOCK   0x1     /* op wrote cpu->pc and the block must end */
#define OP_MODIFIES_F   0x2     /* helper set cpu->f and cpu->q=1 */

/* A "trap target" is any PC the interpreter knows how to dispatch as a
 * host service. JP NN traps on BDOS (0x0005) and the BIOS vector range;
 * CALL NN additionally traps on the warm-boot entry (0x0000). Refuse
 * to translate control flow whose target lands here and let interp do
 * the dispatch. */
static int is_jp_trap_target(uint16_t pc) {
    return pc == CPM_BDOS_ENTRY
        || (pc >= CPM_BIOS_BASE && pc < CPM_BIOS_BASE + 0x80);
}
static int is_call_trap_target(uint16_t pc) {
    return pc == CPM_WBOOT_ENTRY
        || is_jp_trap_target(pc);
}

/* Return 1 if this op type, in this prefix/register configuration, is
 * something the first-cut translator can emit. */
static int can_translate(const z80_decoded *dec, uint16_t pc_after) {
    /* DD/FD prefixes redirect HL→IX/IY and introduce half-register
     * encodings; first cut skips them entirely. CB/ED prefixes are
     * full alternate ISAs and also skipped. */
    if (dec->prefix != 0) return 0;

    switch (dec->type) {
    case Z80_OP_NOP:
        return 1;

    case Z80_OP_LD_R_N:
        return reg8_offset(dec->reg1) >= 0;

    case Z80_OP_LD_R_R:
        /* (HL) on either side is the unprefixed indirect form; we handle
         * it as a memory op below. (HL)/(HL) is HALT (filtered by the
         * decoder), so we never see both = 6. */
        if (dec->reg1 == 6) return reg8_offset(dec->reg2) >= 0;
        if (dec->reg2 == 6) return reg8_offset(dec->reg1) >= 0;
        return reg8_offset(dec->reg1) >= 0 && reg8_offset(dec->reg2) >= 0;

    case Z80_OP_LD_RR_NN:
        return rr_offset(dec->reg1) >= 0;

    case Z80_OP_LD_HL_N:
    case Z80_OP_LD_A_BC:
    case Z80_OP_LD_A_DE:
    case Z80_OP_LD_BC_A:
    case Z80_OP_LD_DE_A:
    case Z80_OP_LD_A_NN:
    case Z80_OP_LD_NN_A:
    case Z80_OP_LD_HL_indNN:
    case Z80_OP_LD_NN_HL:
    case Z80_OP_EX_DE_HL:
    case Z80_OP_LD_SP_HL:
        return 1;

    case Z80_OP_INC_RR:
    case Z80_OP_DEC_RR:
        return rr_offset(dec->reg1) >= 0;

    /* 8-bit ALU A,<src>. reg=6 means (HL); the helper path takes a byte
     * either way. Half-index regs (codes 8/9) need the DD/FD prefix,
     * which we've already rejected up top. */
    case Z80_OP_ADD_A_R:
    case Z80_OP_ADC_A_R:
    case Z80_OP_SUB_A_R:
    case Z80_OP_SBC_A_R:
    case Z80_OP_AND_A_R:
    case Z80_OP_OR_A_R:
    case Z80_OP_XOR_A_R:
    case Z80_OP_CP_A_R:
        return dec->reg1 == 6 || reg8_offset(dec->reg1) >= 0;

    case Z80_OP_ADD_A_N:
    case Z80_OP_ADC_A_N:
    case Z80_OP_SUB_A_N:
    case Z80_OP_SBC_A_N:
    case Z80_OP_AND_A_N:
    case Z80_OP_OR_A_N:
    case Z80_OP_XOR_A_N:
    case Z80_OP_CP_A_N:
        return 1;

    /* INC/DEC r — reg=6 is (HL), needs a memory load+store sequence
     * that we don't emit yet. Same with half-index regs. */
    case Z80_OP_INC_R:
    case Z80_OP_DEC_R:
        return reg8_offset(dec->reg1) >= 0;

    case Z80_OP_JP_NN:
        return !is_jp_trap_target(dec->imm16);
    case Z80_OP_JR_E: {
        uint16_t target = (uint16_t)(pc_after + (int16_t)dec->disp);
        return !is_jp_trap_target(target);
    }
    case Z80_OP_CALL_NN:
        return !is_call_trap_target(dec->imm16);
    case Z80_OP_RET:
        return 1;
    default:
        return 0;
    }
}

/* Emit code for one (already-known-translatable) Z80 op. Returns 1 if the
 * op ends the block (control flow), 0 if execution flows through. */
/* Emit "W1 = source operand" for an ALU A,<src> op.
 *   reg=6 -> (HL): LDRH HL into W10, LDRB mem[HL] into W1.
 *   reg=0..5,7    : LDRB cpu->r into W1.
 *   imm path is caller's job (just MOVZ W1, #imm).
 */
static void emit_alu_src_from_reg(emit_t *e, int reg) {
    if (reg == 6) {
        emit_ldrh_imm(e, A64_W10, A64_W19, OFF_HL);
        emit_ldrb_reg_uxtw(e, A64_W1, A64_W20, A64_W10);
    } else {
        int off = reg8_offset(reg);
        emit_ldrb_imm(e, A64_W1, A64_W19, (uint32_t)off);
    }
}

static unsigned emit_op(emit_t *e, const z80_decoded *dec, uint16_t pc_after) {
    switch (dec->type) {
    case Z80_OP_NOP:
        return OP_FALL_THROUGH;

    case Z80_OP_LD_R_N: {
        int off = reg8_offset(dec->reg1);
        emit_movz_w32(e, A64_W9, dec->imm8, 0);
        emit_strb_imm(e, A64_W9, A64_W19, (uint32_t)off);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_LD_R_R: {
        if (dec->reg1 == 6) {
            /* LD (HL), r : mem[HL] = reg2 */
            int off_src = reg8_offset(dec->reg2);
            emit_ldrh_imm(e, A64_W10, A64_W19, OFF_HL);
            emit_ldrb_imm(e, A64_W9,  A64_W19, (uint32_t)off_src);
            emit_guest_storeb_w9_at_w10_smc(e);
            return OP_FALL_THROUGH;
        }
        if (dec->reg2 == 6) {
            /* LD r, (HL) : reg1 = mem[HL] */
            int off_dst = reg8_offset(dec->reg1);
            emit_ldrh_imm(e, A64_W10, A64_W19, OFF_HL);
            emit_guest_loadb_w9_at_w10(e);
            emit_strb_imm(e, A64_W9, A64_W19, (uint32_t)off_dst);
            return OP_FALL_THROUGH;
        }
        int off_dst = reg8_offset(dec->reg1);
        int off_src = reg8_offset(dec->reg2);
        if (off_dst == off_src) return OP_FALL_THROUGH;   /* LD A,A and friends — no-op */
        emit_ldrb_imm(e, A64_W9, A64_W19, (uint32_t)off_src);
        emit_strb_imm(e, A64_W9, A64_W19, (uint32_t)off_dst);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_LD_RR_NN: {
        int off = rr_offset(dec->reg1);
        emit_load_imm16_into(e, A64_W9, dec->imm16);
        emit_strh_imm(e, A64_W9, A64_W19, (uint32_t)off);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_LD_HL_N: {
        /* mem[HL] = imm8 */
        emit_ldrh_imm(e, A64_W10, A64_W19, OFF_HL);
        emit_movz_w32(e, A64_W9, dec->imm8, 0);
        emit_guest_storeb_w9_at_w10_smc(e);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_LD_A_BC: {
        emit_ldrh_imm(e, A64_W10, A64_W19, OFF_BC);
        emit_guest_loadb_w9_at_w10(e);
        emit_strb_imm(e, A64_W9, A64_W19, OFF_A);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_A_DE: {
        emit_ldrh_imm(e, A64_W10, A64_W19, OFF_DE);
        emit_guest_loadb_w9_at_w10(e);
        emit_strb_imm(e, A64_W9, A64_W19, OFF_A);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_BC_A: {
        emit_ldrh_imm(e, A64_W10, A64_W19, OFF_BC);
        emit_ldrb_imm(e, A64_W9,  A64_W19, OFF_A);
        emit_guest_storeb_w9_at_w10_smc(e);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_DE_A: {
        emit_ldrh_imm(e, A64_W10, A64_W19, OFF_DE);
        emit_ldrb_imm(e, A64_W9,  A64_W19, OFF_A);
        emit_guest_storeb_w9_at_w10_smc(e);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_LD_A_NN: {
        emit_load_imm16_into(e, A64_W10, dec->imm16);
        emit_guest_loadb_w9_at_w10(e);
        emit_strb_imm(e, A64_W9, A64_W19, OFF_A);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_NN_A: {
        emit_load_imm16_into(e, A64_W10, dec->imm16);
        emit_ldrb_imm(e, A64_W9, A64_W19, OFF_A);
        emit_guest_storeb_w9_at_w10_smc(e);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_LD_HL_indNN: {
        /* HL.lo = mem[nn] ; HL.hi = mem[(nn+1) & 0xFFFF] — byte-by-byte
         * because nn+1 must wrap at 0x10000 and the host buffer is only
         * 64K, so a 16-bit halfword load at nn=0xFFFF would walk off. */
        uint16_t nn  = dec->imm16;
        uint16_t nn1 = (uint16_t)(nn + 1);
        emit_load_imm16_into(e, A64_W10, nn);
        emit_guest_loadb_w9_at_w10(e);
        emit_strb_imm(e, A64_W9, A64_W19, OFF_L);
        emit_load_imm16_into(e, A64_W10, nn1);
        emit_guest_loadb_w9_at_w10(e);
        emit_strb_imm(e, A64_W9, A64_W19, OFF_H);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_NN_HL: {
        uint16_t nn  = dec->imm16;
        uint16_t nn1 = (uint16_t)(nn + 1);
        emit_load_imm16_into(e, A64_W10, nn);
        emit_ldrb_imm(e, A64_W9, A64_W19, OFF_L);
        emit_guest_storeb_w9_at_w10_smc(e);
        emit_load_imm16_into(e, A64_W10, nn1);
        emit_ldrb_imm(e, A64_W9, A64_W19, OFF_H);
        emit_guest_storeb_w9_at_w10_smc(e);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_INC_RR: {
        int off = rr_offset(dec->reg1);
        emit_ldrh_imm(e, A64_W9, A64_W19, (uint32_t)off);
        emit_add_w32_imm(e, A64_W9, A64_W9, 1);
        emit_strh_imm(e, A64_W9, A64_W19, (uint32_t)off);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_DEC_RR: {
        int off = rr_offset(dec->reg1);
        emit_ldrh_imm(e, A64_W9, A64_W19, (uint32_t)off);
        emit_sub_w32_imm(e, A64_W9, A64_W9, 1);
        emit_strh_imm(e, A64_W9, A64_W19, (uint32_t)off);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_EX_DE_HL: {
        emit_ldrh_imm(e, A64_W9,  A64_W19, OFF_DE);
        emit_ldrh_imm(e, A64_W10, A64_W19, OFF_HL);
        emit_strh_imm(e, A64_W10, A64_W19, OFF_DE);
        emit_strh_imm(e, A64_W9,  A64_W19, OFF_HL);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_LD_SP_HL: {
        emit_ldrh_imm(e, A64_W9, A64_W19, OFF_HL);
        emit_strh_imm(e, A64_W9, A64_W19, OFF_SP);
        return OP_FALL_THROUGH;
    }

    /* ---- 8-bit ALU. Helpers in dbt_flags.c do the actual flag math. */
    case Z80_OP_ADD_A_R: {
        emit_alu_src_from_reg(e, dec->reg1);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_add);
        return OP_MODIFIES_F;
    }
    case Z80_OP_ADD_A_N: {
        emit_movz_w32(e, A64_W1, dec->imm8, 0);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_add);
        return OP_MODIFIES_F;
    }
    case Z80_OP_ADC_A_R: {
        emit_alu_src_from_reg(e, dec->reg1);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_adc);
        return OP_MODIFIES_F;
    }
    case Z80_OP_ADC_A_N: {
        emit_movz_w32(e, A64_W1, dec->imm8, 0);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_adc);
        return OP_MODIFIES_F;
    }
    case Z80_OP_SUB_A_R: {
        emit_alu_src_from_reg(e, dec->reg1);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_sub);
        return OP_MODIFIES_F;
    }
    case Z80_OP_SUB_A_N: {
        emit_movz_w32(e, A64_W1, dec->imm8, 0);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_sub);
        return OP_MODIFIES_F;
    }
    case Z80_OP_SBC_A_R: {
        emit_alu_src_from_reg(e, dec->reg1);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_sbc);
        return OP_MODIFIES_F;
    }
    case Z80_OP_SBC_A_N: {
        emit_movz_w32(e, A64_W1, dec->imm8, 0);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_sbc);
        return OP_MODIFIES_F;
    }
    case Z80_OP_AND_A_R: {
        emit_alu_src_from_reg(e, dec->reg1);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_and);
        return OP_MODIFIES_F;
    }
    case Z80_OP_AND_A_N: {
        emit_movz_w32(e, A64_W1, dec->imm8, 0);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_and);
        return OP_MODIFIES_F;
    }
    case Z80_OP_OR_A_R: {
        emit_alu_src_from_reg(e, dec->reg1);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_or);
        return OP_MODIFIES_F;
    }
    case Z80_OP_OR_A_N: {
        emit_movz_w32(e, A64_W1, dec->imm8, 0);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_or);
        return OP_MODIFIES_F;
    }
    case Z80_OP_XOR_A_R: {
        emit_alu_src_from_reg(e, dec->reg1);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_xor);
        return OP_MODIFIES_F;
    }
    case Z80_OP_XOR_A_N: {
        emit_movz_w32(e, A64_W1, dec->imm8, 0);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_xor);
        return OP_MODIFIES_F;
    }
    case Z80_OP_CP_A_R: {
        emit_alu_src_from_reg(e, dec->reg1);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_cp);
        return OP_MODIFIES_F;
    }
    case Z80_OP_CP_A_N: {
        emit_movz_w32(e, A64_W1, dec->imm8, 0);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_cp);
        return OP_MODIFIES_F;
    }

    case Z80_OP_INC_R: {
        /* INC (HL) and the half-index regs aren't translated yet. */
        int off = reg8_offset(dec->reg1);
        emit_ldrb_imm(e, A64_W1, A64_W19, (uint32_t)off);   /* W1 = old value */
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_inc8);
        emit_strb_imm(e, A64_W0, A64_W19, (uint32_t)off);   /* W0 = new value */
        return OP_MODIFIES_F;
    }
    case Z80_OP_DEC_R: {
        int off = reg8_offset(dec->reg1);
        emit_ldrb_imm(e, A64_W1, A64_W19, (uint32_t)off);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_dec8);
        emit_strb_imm(e, A64_W0, A64_W19, (uint32_t)off);
        return OP_MODIFIES_F;
    }

    case Z80_OP_JP_NN: {
        /* Block ends; epilogue uses dec->imm16 as the final PC. */
        (void)pc_after;
        emit_movz_w32(e, A64_W9, dec->imm16, 0);
        emit_strh_imm(e, A64_W9, A64_W19, OFF_PC);
        return OP_ENDS_BLOCK;
    }

    case Z80_OP_JR_E: {
        /* JR e: target = pc_after + (int8_t)disp.
         * The decoder stores the signed displacement in dec->disp and
         * has already added the instruction size to pc_after for us. */
        uint16_t target = (uint16_t)(pc_after + (int16_t)dec->disp);
        emit_movz_w32(e, A64_W9, target, 0);
        emit_strh_imm(e, A64_W9, A64_W19, OFF_PC);
        return OP_ENDS_BLOCK;
    }

    case Z80_OP_CALL_NN: {
        /* CALL nn:
         *   sp = (sp - 2) & 0xFFFF
         *   mem[sp]     = pc_after & 0xFF
         *   mem[sp + 1] = (pc_after >> 8) & 0xFF        (sp+1 also masked)
         *   pc          = target                          (block ends)
         *
         * Trap targets (BDOS/WBOOT/BIOS) were filtered by can_translate
         * — this case only runs for "regular" callees. */
        emit_ldrh_imm(e, A64_W11, A64_W19, OFF_SP);
        emit_sub_mask16(e, A64_W11, A64_W11, 2);
        emit_strh_imm(e, A64_W11, A64_W19, OFF_SP);

        /* mem[W11] = pc_after_lo */
        emit_movz_w32(e, A64_W9, pc_after & 0xFF, 0);
        emit_strb_reg_uxtw(e, A64_W9, A64_W20, A64_W11);

        /* mem[(W11+1)&0xFFFF] = pc_after_hi */
        emit_add_mask16(e, A64_W12, A64_W11, 1);
        emit_movz_w32(e, A64_W9, (pc_after >> 8) & 0xFF, 0);
        emit_strb_reg_uxtw(e, A64_W9, A64_W20, A64_W12);

        /* pc = target */
        emit_movz_w32(e, A64_W9, dec->imm16, 0);
        emit_strh_imm(e, A64_W9, A64_W19, OFF_PC);
        return OP_ENDS_BLOCK;
    }

    case Z80_OP_RET: {
        /* RET:
         *   pc.lo = mem[sp]
         *   pc.hi = mem[(sp + 1) & 0xFFFF]
         *   sp    = (sp + 2) & 0xFFFF
         *
         * Popped pc == 0 is the classic CP/M warm-boot termination;
         * dbt_run's top-of-loop pc==0 && insn_count>4 check catches it,
         * so we don't need a runtime branch here. */
        (void)pc_after;
        emit_ldrh_imm(e, A64_W11, A64_W19, OFF_SP);

        /* W9 = mem[sp] (pc low byte) */
        emit_ldrb_reg_uxtw(e, A64_W9, A64_W20, A64_W11);

        /* W10 = mem[(sp+1)&0xFFFF] (pc high byte) */
        emit_add_mask16(e, A64_W12, A64_W11, 1);
        emit_ldrb_reg_uxtw(e, A64_W10, A64_W20, A64_W12);

        /* W9 = pc = lo | (hi << 8) */
        emit_lsl_w32_imm(e, A64_W10, A64_W10, 8);
        emit_orr_w32(e, A64_W9, A64_W9, A64_W10);
        emit_strh_imm(e, A64_W9, A64_W19, OFF_PC);

        /* sp += 2, masked */
        emit_add_mask16(e, A64_W11, A64_W11, 2);
        emit_strh_imm(e, A64_W11, A64_W19, OFF_SP);
        return OP_ENDS_BLOCK;
    }

    default:
        /* can_translate() filters this; unreachable. */
        return OP_ENDS_BLOCK;
    }
}

uint8_t *dbt_translate_block(z80_dbt_t *dbt, uint16_t guest_pc) {
    if (dbt->code_used + 4096 > CODE_BUF_SIZE) {
        /* Out of JIT space — blow away the cache and reset the cursor.
         * Cheap-and-cheerful; chained blocks would need patch-back here.
         *
         * Already inside dbt_jit_writable_begin/end (the run-loop wraps
         * our caller), so DO NOT nest another pair — pthread_jit_write_
         * protect_np isn't a counted lock and a stray inner end() would
         * drop the page back to read-only mid-emission. */
        dbt_cache_invalidate_all(dbt);
        dbt->code_used = 0;
        dbt_emit_trampoline(dbt);
    }

    z80_cpu_t *cpu = dbt->cpu;
    emit_t e = {
        .buf      = dbt->code_buf,
        .offset   = dbt->code_used,
        .capacity = CODE_BUF_SIZE,
    };
    uint8_t *entry = dbt->code_buf + e.offset;

    /* Stash LR before any BLR-to-helper clobbers it. Emitted always;
     * blocks with no helper calls pay one MOV but no helper-saturated
     * block pays per-call save/restore. */
    emit_block_prologue(&e);

    uint16_t pc = guest_pc;
    uint32_t insns = 0;
    int ended_by_branch = 0;
    int last_modifies_f = 0;

    while (insns < MAX_BLOCK_INSNS) {
        z80_decoded dec;
        int n = z80_decode_one(cpu->mem, pc, &dec);
        if (n == 0) break;   /* decode failed — let interp report it */

        uint16_t pc_after = (uint16_t)(pc + dec.bytes);
        if (!can_translate(&dec, pc_after)) {
            if (insns == 0) {
                /* Refuse the block — caller falls back to interp. */
                return NULL;
            }
            break;
        }

        unsigned r = emit_op(&e, &dec, pc_after);
        last_modifies_f = (r & OP_MODIFIES_F) != 0;
        ended_by_branch = (r & OP_ENDS_BLOCK) != 0;
        insns++;
        pc = pc_after;

        if (ended_by_branch) break;
    }

    if (insns == 0) return NULL;

    /* If the last op was a flag-helper call, leave cpu->q at the helper's
     * value (1). Otherwise clear it so the next interp step's prev_q is 0. */
    int clear_q = !last_modifies_f;

    /* If we ended on a branch, the op already wrote cpu->pc. Otherwise
     * the next PC is the straight-through fall-through value in `pc`. */
    if (ended_by_branch) {
        emit_block_tail(&e, insns, clear_q);
    } else {
        emit_block_epilogue(&e, pc, insns, clear_q);
    }

    dbt->code_used = e.offset;
    __builtin___clear_cache((char *)entry, (char *)(dbt->code_buf + e.offset));

    /* Tag every guest byte this block covered so a JIT-emitted store
     * that lands on any of them triggers SMC invalidation. */
    dbt_mark_block_bytes(dbt, guest_pc, pc);
    return entry;
}
