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
#define OFF_AF         offsetof(z80_cpu_t, af)   /* PUSH/POP AF as a halfword */
#define OFF_C          offsetof(z80_cpu_t, c)
#define OFF_B          offsetof(z80_cpu_t, b)
#define OFF_E          offsetof(z80_cpu_t, e)
#define OFF_D          offsetof(z80_cpu_t, d)
#define OFF_L          offsetof(z80_cpu_t, l)
#define OFF_H          offsetof(z80_cpu_t, h)
#define OFF_BC         offsetof(z80_cpu_t, bc)
#define OFF_DE         offsetof(z80_cpu_t, de)
#define OFF_HL         offsetof(z80_cpu_t, hl)
#define OFF_IX         offsetof(z80_cpu_t, ix)
#define OFF_IY         offsetof(z80_cpu_t, iy)
/* The struct stores ix/iy as plain uint16_t, no union halves. We index
 * the high/low byte via offsetof + 0/1 since the host is little-endian. */
#define OFF_IXL        (offsetof(z80_cpu_t, ix) + 0)
#define OFF_IXH        (offsetof(z80_cpu_t, ix) + 1)
#define OFF_IYL        (offsetof(z80_cpu_t, iy) + 0)
#define OFF_IYH        (offsetof(z80_cpu_t, iy) + 1)
#define OFF_SP         offsetof(z80_cpu_t, sp)
#define OFF_PC         offsetof(z80_cpu_t, pc)
#define OFF_Q          offsetof(z80_cpu_t, q)
#define OFF_MEMPTR     offsetof(z80_cpu_t, memptr)
#define OFF_INSN_COUNT offsetof(z80_cpu_t, insn_count)

/* Map a Z80 reg code 0..7 (B,C,D,E,H,L,(HL),A) to its byte offset
 * within z80_cpu_t. Code 6 is (HL), handled by the memory-op cases
 * via X20 + HL. Codes 8/9 — IXH/IXL or IYH/IYL — are the DD/FD
 * half-register remap; the prefix selects IX vs IY. Returns -1 on
 * unsupported (here that's just (HL)). */
static int reg8_offset_p(int r, uint8_t prefix) {
    switch (r) {
    case 0: return OFF_B;
    case 1: return OFF_C;
    case 2: return OFF_D;
    case 3: return OFF_E;
    case 4: return OFF_H;
    case 5: return OFF_L;
    case 7: return OFF_A;
    case 8: return (prefix == 0xFD) ? OFF_IYH : OFF_IXH;
    case 9: return (prefix == 0xFD) ? OFF_IYL : OFF_IXL;
    default: return -1;
    }
}
static int reg8_offset(int r) { return reg8_offset_p(r, 0); }

/* Offset of the IX or IY register (full 16-bit) for the given DD/FD
 * prefix. Caller must already know prefix is DD or FD. */
static int idx_reg_offset(uint8_t prefix) {
    return (prefix == 0xFD) ? OFF_IY : OFF_IX;
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

/* Prefix-aware rr_offset: under DD/FD, the HL slot (code 2) actually
 * names IX or IY. SP/BC/DE unaffected. */
static int rr_offset_p(int rr, uint8_t prefix) {
    if (rr == 2 && (prefix == 0xDD || prefix == 0xFD))
        return idx_reg_offset(prefix);
    return rr_offset(rr);
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

/* cpu->memptr = imm16. Compiles to MOVZ + STRH (2 host insns). Used by
 * the "memptr = nn+1" / "memptr = target" cases where the value is
 * known at codegen time. */
static void emit_set_memptr_imm(emit_t *e, uint16_t value) {
    emit_movz_w32(e, A64_W9, value, 0);
    emit_strh_imm(e, A64_W9, A64_W19, OFF_MEMPTR);
}

/* cpu->memptr = (A << 8) | (low8_imm)  — the LD (nn),A / LD (BC|DE),A
 * memptr-quirk form. low8_imm is the static "(addr + 1) & 0xFF" part.
 *
 * Goes via a scratch reg + ORR-reg rather than ORR-imm because most
 * 8-bit constants aren't encodable as an AArch64 logical immediate
 * (0x66 = 0110_0110 was the original casualty here). */
static void emit_set_memptr_quirk_imm(emit_t *e, uint8_t low8_imm) {
    emit_ldrb_imm(e, A64_W9, A64_W19, OFF_A);
    emit_lsl_w32_imm(e, A64_W9, A64_W9, 8);
    if (low8_imm != 0) {
        emit_movz_w32(e, A64_W12, low8_imm, 0);
        emit_orr_w32(e, A64_W9, A64_W9, A64_W12);
    }
    emit_strh_imm(e, A64_W9, A64_W19, OFF_MEMPTR);
}

/* cpu->memptr = (A << 8) | ((rr_value + 1) & 0xFF). Used for the
 * dynamic LD (BC),A / LD (DE),A — the address comes from a register
 * pair, so the +1-and-mask must run at execution time. */
static void emit_set_memptr_quirk_rr(emit_t *e, uint32_t off_rr) {
    emit_ldrh_imm(e, A64_W12, A64_W19, off_rr);
    emit_add_w32_imm(e, A64_W12, A64_W12, 1);
    (void)emit_and_w32_imm(e, A64_W12, A64_W12, 0xFF);
    emit_ldrb_imm(e, A64_W9, A64_W19, OFF_A);
    emit_lsl_w32_imm(e, A64_W9, A64_W9, 8);
    emit_orr_w32(e, A64_W9, A64_W9, A64_W12);
    emit_strh_imm(e, A64_W9, A64_W19, OFF_MEMPTR);
}

/* Wdst = ((IX|IY) + (int8)disp) & 0xFFFF. Loads the 16-bit IX/IY,
 * adds the sign-extended displacement, masks to 16 bits. The mask is
 * required because the host uses UXTW on the result as a memory index,
 * and (IX+d) is allowed to wrap across the 64KB boundary. Uses only
 * `dst`; no other scratch. */
static void emit_idx_eff_addr(emit_t *e, a64_reg_t dst, uint8_t prefix, int8_t disp) {
    emit_ldrh_imm(e, dst, A64_W19, (uint32_t)idx_reg_offset(prefix));
    if (disp == 0) {
        (void)emit_and_w32_imm(e, dst, dst, 0xFFFF);
        return;
    }
    /* Sign-extend the 8-bit disp into a 32-bit value. MOVZ + sign-fix:
     * for negative disp use the equivalent SUB; for positive use ADD. */
    if (disp > 0) {
        emit_add_w32_imm(e, dst, dst, (uint32_t)disp);
    } else {
        emit_sub_w32_imm(e, dst, dst, (uint32_t)(-disp));
    }
    (void)emit_and_w32_imm(e, dst, dst, 0xFFFF);
}

/* cpu->memptr = (rr + 1) & 0xFFFF. Used by LD A,(BC|DE) — straight
 * "addr+1" memptr semantics, no XY quirk. */
static void emit_set_memptr_rr_plus_one(emit_t *e, uint32_t off_rr) {
    emit_ldrh_imm(e, A64_W9, A64_W19, off_rr);
    emit_add_w32_imm(e, A64_W9, A64_W9, 1);
    emit_strh_imm(e, A64_W9, A64_W19, OFF_MEMPTR);
}

/* Map a Z80 condition code (cc, 0..7) to (flag mask, host condition).
 * Layout: cc = (flag_select << 1) | sense, where sense=0 is "take if
 * flag clear" (host EQ after TST) and sense=1 is "take if flag set"
 * (host NE). */
static uint8_t flag_mask_for_cc(int cc) {
    static const uint8_t masks[4] = {
        Z80_FLAG_Z, Z80_FLAG_C, Z80_FLAG_PV, Z80_FLAG_S
    };
    return masks[(cc >> 1) & 3];
}
static a64_cond_t host_cond_for_cc(int cc) {
    /* Even cc: take-if-clear  → EQ. Odd cc: take-if-set → NE. */
    return (cc & 1) ? A64_COND_NE : A64_COND_EQ;
}

/* Emit "TST F, #mask" via a scratch register, sets host flags so the
 * caller can immediately follow with CSEL Wd, Wtaken, Wfall, host_cond.
 * Uses W12 as the mask scratch. */
static void emit_test_z80_flag(emit_t *e, uint8_t mask) {
    emit_ldrb_imm(e, A64_W10, A64_W19, OFF_F);
    emit_movz_w32(e, A64_W12, mask, 0);
    emit_tst_w32(e, A64_W10, A64_W12);
}

/* Per-block prologue. The trampoline BLRs in with X30 (LR) pointing at
 * the trampoline's resume site. Any BLR we make inside the block (e.g.
 * to a flag helper) would clobber X30, so we stash it in X22 — which is
 * AAPCS64 callee-saved and already preserved across the trampoline.
 * Emitted unconditionally; even ALU-free blocks pay 1 host insn for it. */
static void emit_block_prologue(emit_t *e) {
    emit_mov_x64_x64(e, A64_W22, A64_W30);
}

/* What the block tail must do to cpu->q so it matches what the interp
 * would leave after the block's LAST instruction (z80_step zeroes q,
 * then F-writing ops set it to 1):
 *   Q_CLEAR — last op does not write F (or is an interp quirk like the
 *             accumulator rotates, which don't set q): store 0.
 *   Q_KEEP  — last op called a flag helper, which stored q=1 itself.
 *   Q_SET   — last op wrote F with inline code (no helper): store 1. */
enum { Q_CLEAR = 0, Q_KEEP = 1, Q_SET = 2 };

/* Per-block tail: fix up cpu->q per the mode above,
 * bump insn_count by delta, restore LR from X22, then attempt to chain
 * directly to the next block via an inline cache probe. On a probe miss
 * (no cached translation for the next PC, or a collision), fall through
 * to RET so the trampoline can take the slow path.
 *
 * Block chaining ABI: chained blocks share the trampoline's stack frame
 * and the original LR (which lives in X22 across the chain — each block
 * re-saves X30 → X22 in its prologue, but since the chain entry does a
 * plain BR, X30 still holds the trampoline-return that block A's tail
 * restored). RET in the miss path therefore lands back in the trampoline
 * as if no chaining had happened. */
static void emit_block_tail(emit_t *e, uint32_t insn_count_delta, int q_mode) {
    if (q_mode == Q_CLEAR) {
        emit_strb_imm(e, A64_WZR, A64_W19, OFF_Q);
    } else if (q_mode == Q_SET) {
        emit_movz_w32(e, A64_W9, 1, 0);
        emit_strb_imm(e, A64_W9, A64_W19, OFF_Q);
    }

    emit_ldr_x64_imm(e, A64_W9, A64_W19, OFF_INSN_COUNT);
    emit_add_x64_imm(e, A64_W9, A64_W9, insn_count_delta);
    emit_str_x64_imm(e, A64_W9, A64_W19, OFF_INSN_COUNT);

    emit_mov_x64_x64(e, A64_W30, A64_W22);

    /* ---- Inline cache probe (block chaining) ----
     *   W12 = cpu->pc             (0..0xFFFF, zero-ext to W)
     *   W13 = pc * 16             (cache entry byte offset)
     *   W15 = cache[idx].guest_pc (32-bit field at +0)
     *   if W15 != W12 → RET (fall through to trampoline)
     *   X16 = cache[idx].native_code (8-byte field at +8)
     *   BR X16
     *
     * The cache index is exactly `pc` since BLOCK_CACHE_MASK == 0xFFFF
     * and pc is uint16_t, so no AND is needed.
     */
    emit_ldrh_imm(e, A64_W12, A64_W19, OFF_PC);
    emit_lsl_w32_imm(e, A64_W13, A64_W12, 4);                /* pc * 16 */
    emit_ldr_w32_reg_uxtw(e, A64_W15, A64_W21, A64_W13);     /* guest_pc */
    emit_cmp_w32_w32(e, A64_W15, A64_W12);

    uint32_t patch_miss = emit_pos(e);
    emit_b_cond(e, A64_COND_NE, 0);

    emit_add_w32_imm(e, A64_W13, A64_W13, 8);                /* offset to native_code */
    emit_ldr_x64_reg_uxtw(e, A64_W16, A64_W21, A64_W13);
    emit_br(e, A64_W16);

    emit_patch_cond19(e, patch_miss, emit_pos(e));
    emit_ret(e);
}

/* Convenience: write cpu->pc = final_pc, then emit tail. Used by blocks
 * that fall through (no branch op already wrote pc). */
static void emit_block_epilogue(emit_t *e, uint16_t final_pc,
                                uint32_t insn_count_delta, int q_mode) {
    emit_movz_w32(e, A64_W9, final_pc, 0);
    emit_strh_imm(e, A64_W9, A64_W19, OFF_PC);
    emit_block_tail(e, insn_count_delta, q_mode);
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
#define OP_SETS_F_INLINE 0x4    /* inline code wrote cpu->f; q=1 owed by tail */

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
    /* ED is a separate ISA — accepted only on the per-op cases that
     * opt in (LDIR/LDDR as host intrinsics). CB-prefix is accepted on
     * the Z80_OP_CB case below; DD CB / FD CB (which decode with
     * prefix==DD or FD AND type==Z80_OP_CB) we still skip — the dual
     * register+memory writeback for the indexed form needs its own
     * codegen pass. */
    int idx = (dec->prefix == 0xDD || dec->prefix == 0xFD);

    switch (dec->type) {
    case Z80_OP_NOP:
        return !idx;   /* DD/FD NOP is reserved; let interp handle. */

    case Z80_OP_LD_R_N:
        return reg8_offset_p(dec->reg1, dec->prefix) >= 0;

    case Z80_OP_LD_R_R:
        /* (HL) on either side under no prefix is the indirect form;
         * under DD/FD the decoder routes those to LD_*_HL_ind so we
         * only see pure register forms here. */
        if (dec->reg1 == 6) return !idx && reg8_offset(dec->reg2) >= 0;
        if (dec->reg2 == 6) return !idx && reg8_offset(dec->reg1) >= 0;
        return reg8_offset_p(dec->reg1, dec->prefix) >= 0
            && reg8_offset_p(dec->reg2, dec->prefix) >= 0;

    case Z80_OP_LD_RR_NN:
        /* DD/FD LD HL,nn is really LD IX,nn / LD IY,nn — accept it. */
        return rr_offset(dec->reg1) >= 0;

    case Z80_OP_LD_HL_N:
    case Z80_OP_LD_A_BC:
    case Z80_OP_LD_A_DE:
    case Z80_OP_LD_BC_A:
    case Z80_OP_LD_DE_A:
    case Z80_OP_LD_A_NN:
    case Z80_OP_LD_NN_A:
        return !idx;
    case Z80_OP_LD_HL_indNN:
    case Z80_OP_LD_NN_HL:
    case Z80_OP_LD_SP_HL:
        return 1;    /* DD/FD variants land on IX/IY */
    case Z80_OP_EX_DE_HL:
        return !idx; /* DD/FD EX DE,HL is a no-op on real silicon; rare */

    case Z80_OP_INC_RR:
    case Z80_OP_DEC_RR:
        return rr_offset(dec->reg1) >= 0;

    /* ADD HL,rr — and under DD/FD, ADD IX,rr / ADD IY,rr. reg1==2 names
     * the destination register itself (ADD HL,HL / ADD IX,IX). */
    case Z80_OP_ADD_HL_RR:
        return dec->reg1 >= 0 && dec->reg1 <= 3;

    /* Accumulator rotates + DAA. DD/FD-prefixed forms are decode quirks;
     * leave those to the interp. */
    case Z80_OP_RLCA:
    case Z80_OP_RRCA:
    case Z80_OP_RLA:
    case Z80_OP_RRA:
    case Z80_OP_DAA:
    case Z80_OP_CPL:
    case Z80_OP_SCF:
    case Z80_OP_CCF:
        return !idx;

    case Z80_OP_EX_SP_HL:
        return 1;   /* DD/FD forms exchange IX/IY */

    /* 8-bit ALU A,<src>. reg=6 means (HL) / (IX+d) / (IY+d); the helper
     * path takes a byte either way. Codes 8/9 require DD/FD prefix —
     * reg8_offset_p resolves them. */
    case Z80_OP_ADD_A_R:
    case Z80_OP_ADC_A_R:
    case Z80_OP_SUB_A_R:
    case Z80_OP_SBC_A_R:
    case Z80_OP_AND_A_R:
    case Z80_OP_OR_A_R:
    case Z80_OP_XOR_A_R:
    case Z80_OP_CP_A_R:
        return dec->reg1 == 6 ? !idx
                              : reg8_offset_p(dec->reg1, dec->prefix) >= 0;

    case Z80_OP_ADD_A_N:
    case Z80_OP_ADC_A_N:
    case Z80_OP_SUB_A_N:
    case Z80_OP_SBC_A_N:
    case Z80_OP_AND_A_N:
    case Z80_OP_OR_A_N:
    case Z80_OP_XOR_A_N:
    case Z80_OP_CP_A_N:
        return !idx;

    /* INC/DEC r — reg=6 is unprefixed (HL); the (IX+d)/(IY+d) forms
     * decode as INC_HL_ind / DEC_HL_ind instead. Half-index regs land
     * here under DD/FD via reg8_offset_p(8|9). */
    case Z80_OP_INC_R:
    case Z80_OP_DEC_R:
        if (dec->reg1 == 6) return !idx;
        return reg8_offset_p(dec->reg1, dec->prefix) >= 0;

    /* DD/FD-prefixed indexed memory ops. (Unprefixed LD r,(HL) / (HL),r
     * still come through LD_R_R with reg=6.) */
    case Z80_OP_LD_A_HL_ind:
    case Z80_OP_LD_HL_A_ind:
    case Z80_OP_LD_HL_N_ind:
        return idx;
    case Z80_OP_LD_R_HL_ind:
        return idx && reg8_offset(dec->reg2) >= 0;
    case Z80_OP_LD_HL_R_ind:
        return idx && reg8_offset(dec->reg2) >= 0;
    /* INC/DEC (HL) — also the unprefixed form; the emit case picks HL
     * vs (IX|IY)+d by prefix. */
    case Z80_OP_INC_HL_ind:
    case Z80_OP_DEC_HL_ind:
        return 1;
    case Z80_OP_ADD_A_HL_ind:
    case Z80_OP_ADC_A_HL_ind:
    case Z80_OP_SUB_A_HL_ind:
    case Z80_OP_SBC_A_HL_ind:
    case Z80_OP_AND_A_HL_ind:
    case Z80_OP_OR_A_HL_ind:
    case Z80_OP_XOR_A_HL_ind:
    case Z80_OP_CP_A_HL_ind:
        return idx;
    case Z80_OP_JP_HL:
        /* Plain JP (HL) only. JP (IX)/JP (IY) under DD/FD is rare and
         * the interp has a latent bug (always reads cpu->hl); leave it
         * to interp until the interp side is straightened out. */
        return !idx;

    case Z80_OP_JP_NN:
        return !is_jp_trap_target(dec->imm16);
    case Z80_OP_JR_E: {
        uint16_t target = (uint16_t)(pc_after + (int16_t)dec->disp);
        return !is_jp_trap_target(target);
    }
    case Z80_OP_JP_CC_NN:
        /* JP cc: only one branch (the conditional one) could trap. The
         * fall-through pc_after is straight-line code we can always
         * resolve via the cache, so we only need to refuse when the
         * conditional target is a trap. */
        return !is_jp_trap_target(dec->imm16);
    case Z80_OP_JR_CC_E: {
        uint16_t target = (uint16_t)(pc_after + (int16_t)dec->disp);
        return !is_jp_trap_target(target);
    }
    case Z80_OP_CALL_NN:
        return !is_call_trap_target(dec->imm16);
    case Z80_OP_CALL_CC_NN:
        return !is_call_trap_target(dec->imm16);
    case Z80_OP_RET:
        return 1;
    case Z80_OP_RET_CC:
        return 1;
    case Z80_OP_DJNZ:
        return 1;
    case Z80_OP_CB:
        /* Plain CB, DD CB, and FD CB. DD CB / FD CB operate on (IX|IY)+d
         * with the documented dual writeback (the result goes to memory
         * AND to register r when r != 6). */
        return dec->prefix == 0xCB
            || dec->prefix == 0xDD
            || dec->prefix == 0xFD;

    /* ED-prefix block-copy intrinsics. The interp also runs these
     * atomically, so insn_count parity is preserved. */
    case Z80_OP_LDIR:
    case Z80_OP_LDDR:
        return 1;

    /* PUSH/POP rr. reg1 encoding:
     *   0/1/2  → BC/DE/HL (rr_offset)
     *   3      → AF (special)
     *   4      → IX or IY per DD/FD prefix (idx_reg_offset)
     * Flags untouched (the F byte is just data for PUSH AF / POP AF). */
    case Z80_OP_PUSH_RR:
    case Z80_OP_POP_RR:
        if (dec->reg1 <= 2) return 1;
        if (dec->reg1 == 3) return !idx;     /* PUSH AF has no DD/FD form */
        if (dec->reg1 == 4) return idx;
        return 0;
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
static void emit_alu_src_from_reg(emit_t *e, int reg, uint8_t prefix) {
    if (reg == 6) {
        emit_ldrh_imm(e, A64_W10, A64_W19, OFF_HL);
        emit_ldrb_reg_uxtw(e, A64_W1, A64_W20, A64_W10);
    } else {
        int off = reg8_offset_p(reg, prefix);
        emit_ldrb_imm(e, A64_W1, A64_W19, (uint32_t)off);
    }
}

/* prev_q: 1/0 if the previous instruction in this block statically
 * did/didn't write F (SCF/CCF need it for the XY Q-quirk); -1 when this
 * is the first op of the block and the live cpu->q must be consulted. */
static unsigned emit_op(emit_t *e, const z80_decoded *dec, uint16_t pc_after,
                        int prev_q) {
    switch (dec->type) {
    case Z80_OP_NOP:
        return OP_FALL_THROUGH;

    case Z80_OP_LD_R_N: {
        int off = reg8_offset_p(dec->reg1, dec->prefix);
        emit_movz_w32(e, A64_W9, dec->imm8, 0);
        emit_strb_imm(e, A64_W9, A64_W19, (uint32_t)off);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_LD_R_R: {
        if (dec->reg1 == 6) {
            /* LD (HL), r : mem[HL] = reg2  (unprefixed only — can_translate gate) */
            int off_src = reg8_offset(dec->reg2);
            emit_ldrh_imm(e, A64_W10, A64_W19, OFF_HL);
            emit_ldrb_imm(e, A64_W9,  A64_W19, (uint32_t)off_src);
            emit_guest_storeb_w9_at_w10_smc(e);
            return OP_FALL_THROUGH;
        }
        if (dec->reg2 == 6) {
            /* LD r, (HL) : reg1 = mem[HL]  (unprefixed only) */
            int off_dst = reg8_offset(dec->reg1);
            emit_ldrh_imm(e, A64_W10, A64_W19, OFF_HL);
            emit_guest_loadb_w9_at_w10(e);
            emit_strb_imm(e, A64_W9, A64_W19, (uint32_t)off_dst);
            return OP_FALL_THROUGH;
        }
        int off_dst = reg8_offset_p(dec->reg1, dec->prefix);
        int off_src = reg8_offset_p(dec->reg2, dec->prefix);
        if (off_dst == off_src) return OP_FALL_THROUGH;   /* LD A,A and friends — no-op */
        emit_ldrb_imm(e, A64_W9, A64_W19, (uint32_t)off_src);
        emit_strb_imm(e, A64_W9, A64_W19, (uint32_t)off_dst);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_LD_RR_NN: {
        int off = rr_offset_p(dec->reg1, dec->prefix);
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

    /* ---- DD/FD indexed memory ops. effective addr = (IX|IY) + disp.
     * Mirrors the interpreter: memptr is NOT updated for these (the
     * indexed form's memptr semantics only matter for BIT n,(IX+d),
     * which is DD-CB-prefix territory we don't translate yet). */
    case Z80_OP_LD_A_HL_ind: {
        emit_idx_eff_addr(e, A64_W10, dec->prefix, (int8_t)dec->disp);
        emit_guest_loadb_w9_at_w10(e);
        emit_strb_imm(e, A64_W9, A64_W19, OFF_A);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_HL_A_ind: {
        emit_idx_eff_addr(e, A64_W10, dec->prefix, (int8_t)dec->disp);
        emit_ldrb_imm(e, A64_W9, A64_W19, OFF_A);
        emit_guest_storeb_w9_at_w10_smc(e);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_R_HL_ind: {
        /* reg2 is the destination GPR — main H/L, not IXH/IXL. */
        int off_dst = reg8_offset(dec->reg2);
        emit_idx_eff_addr(e, A64_W10, dec->prefix, (int8_t)dec->disp);
        emit_guest_loadb_w9_at_w10(e);
        emit_strb_imm(e, A64_W9, A64_W19, (uint32_t)off_dst);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_HL_R_ind: {
        int off_src = reg8_offset(dec->reg2);
        emit_idx_eff_addr(e, A64_W10, dec->prefix, (int8_t)dec->disp);
        emit_ldrb_imm(e, A64_W9, A64_W19, (uint32_t)off_src);
        emit_guest_storeb_w9_at_w10_smc(e);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_HL_N_ind: {
        emit_idx_eff_addr(e, A64_W10, dec->prefix, (int8_t)dec->disp);
        emit_movz_w32(e, A64_W9, dec->imm8, 0);
        emit_guest_storeb_w9_at_w10_smc(e);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_INC_HL_ind:
    case Z80_OP_DEC_HL_ind: {
        /* mem[eff] = INC8/DEC8(mem[eff]). eff = HL unprefixed, or
         * (IX|IY)+d under DD/FD. The store goes through the SMC helper
         * since the byte we just modified might be code. */
        int idx = (dec->prefix == 0xDD || dec->prefix == 0xFD);
        void *helper = (dec->type == Z80_OP_INC_HL_ind)
                           ? (void *)(uintptr_t)z80_jit_inc8
                           : (void *)(uintptr_t)z80_jit_dec8;
        if (idx) emit_idx_eff_addr(e, A64_W10, dec->prefix, (int8_t)dec->disp);
        else     emit_ldrh_imm(e, A64_W10, A64_W19, OFF_HL);
        emit_guest_loadb_w9_at_w10(e);          /* W9 = old byte */
        emit_mov_w32_w32(e, A64_W1, A64_W9);
        emit_call_helper(e, helper);
        /* helper returns new value in W0; addr W10 was clobbered. Reload. */
        if (idx) emit_idx_eff_addr(e, A64_W10, dec->prefix, (int8_t)dec->disp);
        else     emit_ldrh_imm(e, A64_W10, A64_W19, OFF_HL);
        emit_mov_w32_w32(e, A64_W9, A64_W0);
        emit_guest_storeb_w9_at_w10_smc(e);
        return OP_MODIFIES_F;
    }
    case Z80_OP_ADD_A_HL_ind:
    case Z80_OP_ADC_A_HL_ind:
    case Z80_OP_SUB_A_HL_ind:
    case Z80_OP_SBC_A_HL_ind:
    case Z80_OP_AND_A_HL_ind:
    case Z80_OP_OR_A_HL_ind:
    case Z80_OP_XOR_A_HL_ind:
    case Z80_OP_CP_A_HL_ind:  {
        emit_idx_eff_addr(e, A64_W10, dec->prefix, (int8_t)dec->disp);
        emit_ldrb_reg_uxtw(e, A64_W1, A64_W20, A64_W10);
        void *helper;
        switch (dec->type) {
        case Z80_OP_ADD_A_HL_ind: helper = (void *)z80_jit_add; break;
        case Z80_OP_ADC_A_HL_ind: helper = (void *)z80_jit_adc; break;
        case Z80_OP_SUB_A_HL_ind: helper = (void *)z80_jit_sub; break;
        case Z80_OP_SBC_A_HL_ind: helper = (void *)z80_jit_sbc; break;
        case Z80_OP_AND_A_HL_ind: helper = (void *)z80_jit_and; break;
        case Z80_OP_OR_A_HL_ind:  helper = (void *)z80_jit_or;  break;
        case Z80_OP_XOR_A_HL_ind: helper = (void *)z80_jit_xor; break;
        case Z80_OP_CP_A_HL_ind:  helper = (void *)z80_jit_cp;  break;
        default: helper = NULL;
        }
        emit_call_helper(e, helper);
        return OP_MODIFIES_F;
    }

    case Z80_OP_LD_A_BC: {
        emit_ldrh_imm(e, A64_W10, A64_W19, OFF_BC);
        emit_guest_loadb_w9_at_w10(e);
        emit_strb_imm(e, A64_W9, A64_W19, OFF_A);
        emit_set_memptr_rr_plus_one(e, OFF_BC);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_A_DE: {
        emit_ldrh_imm(e, A64_W10, A64_W19, OFF_DE);
        emit_guest_loadb_w9_at_w10(e);
        emit_strb_imm(e, A64_W9, A64_W19, OFF_A);
        emit_set_memptr_rr_plus_one(e, OFF_DE);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_BC_A: {
        emit_ldrh_imm(e, A64_W10, A64_W19, OFF_BC);
        emit_ldrb_imm(e, A64_W9,  A64_W19, OFF_A);
        emit_guest_storeb_w9_at_w10_smc(e);
        emit_set_memptr_quirk_rr(e, OFF_BC);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_DE_A: {
        emit_ldrh_imm(e, A64_W10, A64_W19, OFF_DE);
        emit_ldrb_imm(e, A64_W9,  A64_W19, OFF_A);
        emit_guest_storeb_w9_at_w10_smc(e);
        emit_set_memptr_quirk_rr(e, OFF_DE);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_LD_A_NN: {
        emit_load_imm16_into(e, A64_W10, dec->imm16);
        emit_guest_loadb_w9_at_w10(e);
        emit_strb_imm(e, A64_W9, A64_W19, OFF_A);
        emit_set_memptr_imm(e, (uint16_t)(dec->imm16 + 1));
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_NN_A: {
        emit_load_imm16_into(e, A64_W10, dec->imm16);
        emit_ldrb_imm(e, A64_W9, A64_W19, OFF_A);
        emit_guest_storeb_w9_at_w10_smc(e);
        emit_set_memptr_quirk_imm(e, (uint8_t)((dec->imm16 + 1) & 0xFF));
        return OP_FALL_THROUGH;
    }

    case Z80_OP_LD_HL_indNN: {
        /* HL.lo = mem[nn] ; HL.hi = mem[(nn+1) & 0xFFFF] — byte-by-byte
         * because nn+1 must wrap at 0x10000 and the host buffer is only
         * 64K, so a 16-bit halfword load at nn=0xFFFF would walk off.
         * Under DD/FD prefix, target is IX or IY instead of HL. */
        uint16_t nn  = dec->imm16;
        uint16_t nn1 = (uint16_t)(nn + 1);
        int idx = (dec->prefix == 0xDD || dec->prefix == 0xFD);
        uint32_t off_lo = idx ? (uint32_t)idx_reg_offset(dec->prefix)
                              : OFF_L;
        uint32_t off_hi = idx ? (uint32_t)idx_reg_offset(dec->prefix) + 1
                              : OFF_H;
        emit_load_imm16_into(e, A64_W10, nn);
        emit_guest_loadb_w9_at_w10(e);
        emit_strb_imm(e, A64_W9, A64_W19, off_lo);
        emit_load_imm16_into(e, A64_W10, nn1);
        emit_guest_loadb_w9_at_w10(e);
        emit_strb_imm(e, A64_W9, A64_W19, off_hi);
        emit_set_memptr_imm(e, nn1);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_NN_HL: {
        uint16_t nn  = dec->imm16;
        uint16_t nn1 = (uint16_t)(nn + 1);
        int idx = (dec->prefix == 0xDD || dec->prefix == 0xFD);
        uint32_t off_lo = idx ? (uint32_t)idx_reg_offset(dec->prefix)
                              : OFF_L;
        uint32_t off_hi = idx ? (uint32_t)idx_reg_offset(dec->prefix) + 1
                              : OFF_H;
        emit_load_imm16_into(e, A64_W10, nn);
        emit_ldrb_imm(e, A64_W9, A64_W19, off_lo);
        emit_guest_storeb_w9_at_w10_smc(e);
        emit_load_imm16_into(e, A64_W10, nn1);
        emit_ldrb_imm(e, A64_W9, A64_W19, off_hi);
        emit_guest_storeb_w9_at_w10_smc(e);
        emit_set_memptr_imm(e, nn1);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_INC_RR: {
        int off = rr_offset_p(dec->reg1, dec->prefix);
        emit_ldrh_imm(e, A64_W9, A64_W19, (uint32_t)off);
        emit_add_w32_imm(e, A64_W9, A64_W9, 1);
        emit_strh_imm(e, A64_W9, A64_W19, (uint32_t)off);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_DEC_RR: {
        int off = rr_offset_p(dec->reg1, dec->prefix);
        emit_ldrh_imm(e, A64_W9, A64_W19, (uint32_t)off);
        emit_sub_w32_imm(e, A64_W9, A64_W9, 1);
        emit_strh_imm(e, A64_W9, A64_W19, (uint32_t)off);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_ADD_HL_RR: {
        /* ADD HL,rr (or ADD IX,rr / ADD IY,rr under DD/FD).
         *   memptr = old dst + 1
         *   dst   += src
         *   F: S/Z/PV preserved; N=0; C = carry out of bit 15;
         *      H = carry out of bit 11; XY from result high byte.
         * Fully inline — no helper call. H uses the carry-recovery
         * identity: bit 12 of (a ^ b ^ (a+b)) is the carry INTO bit 12,
         * i.e. the half-carry. */
        int idx = (dec->prefix == 0xDD || dec->prefix == 0xFD);
        uint32_t off_dst = idx ? (uint32_t)idx_reg_offset(dec->prefix) : OFF_HL;
        uint32_t off_src = (dec->reg1 == 2) ? off_dst      /* ADD HL,HL etc. */
                                            : (uint32_t)rr_offset(dec->reg1);

        emit_ldrh_imm(e, A64_W9, A64_W19, off_dst);        /* W9 = old dst */
        if (off_src == off_dst)
            emit_mov_w32_w32(e, A64_W11, A64_W9);
        else
            emit_ldrh_imm(e, A64_W11, A64_W19, off_src);   /* W11 = src */
        emit_add_w32(e, A64_W13, A64_W9, A64_W11);         /* W13 = sum, C at bit 16 */

        emit_add_mask16(e, A64_W12, A64_W9, 1);            /* memptr = old dst + 1 */
        emit_strh_imm(e, A64_W12, A64_W19, OFF_MEMPTR);
        emit_strh_imm(e, A64_W13, A64_W19, off_dst);       /* STRH keeps low 16 */

        emit_ldrb_imm(e, A64_W10, A64_W19, OFF_F);
        emit_movz_w32(e, A64_W12, Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV, 0);
        emit_and_w32(e, A64_W10, A64_W10, A64_W12);
        emit_lsr_w32_imm(e, A64_W12, A64_W13, 16);         /* C: sum bit 16 -> bit 0 */
        emit_orr_w32(e, A64_W10, A64_W10, A64_W12);
        emit_eor_w32(e, A64_W12, A64_W9, A64_W11);         /* H: (a^b^sum) bit 12 */
        emit_eor_w32(e, A64_W12, A64_W12, A64_W13);
        emit_lsr_w32_imm(e, A64_W12, A64_W12, 8);          /*   -> bit 4 (FLAG_H) */
        (void)emit_and_w32_imm(e, A64_W12, A64_W12, Z80_FLAG_H);
        emit_orr_w32(e, A64_W10, A64_W10, A64_W12);
        emit_lsr_w32_imm(e, A64_W12, A64_W13, 8);          /* XY from result.hi */
        emit_movz_w32(e, A64_W14, Z80_FLAG_5 | Z80_FLAG_3, 0);
        emit_and_w32(e, A64_W12, A64_W12, A64_W14);
        emit_orr_w32(e, A64_W10, A64_W10, A64_W12);
        emit_strb_imm(e, A64_W10, A64_W19, OFF_F);
        return OP_SETS_F_INLINE;
    }

    /* ---- Accumulator rotates. All four share the flag rule:
     *   F = (F & (S|Z|PV)) | carry_out | (A' & (5|3))    (H=0, N=0)
     * NB: the interp does NOT set q for these (unlike every other
     * F-writing op), so we return plain OP_FALL_THROUGH — if one of
     * these is the last op in a block the tail stores q=0, matching. */
    case Z80_OP_RLCA:
    case Z80_OP_RRCA:
    case Z80_OP_RLA:
    case Z80_OP_RRA: {
        emit_ldrb_imm(e, A64_W9,  A64_W19, OFF_A);
        emit_ldrb_imm(e, A64_W11, A64_W19, OFF_F);
        switch (dec->type) {
        case Z80_OP_RLCA:                      /* A = A<<1 | A.7 ; C = A.7 */
            emit_lsr_w32_imm(e, A64_W10, A64_W9, 7);
            emit_lsl_w32_imm(e, A64_W9, A64_W9, 1);
            emit_orr_w32(e, A64_W9, A64_W9, A64_W10);
            (void)emit_and_w32_imm(e, A64_W9, A64_W9, 0xFF);
            break;
        case Z80_OP_RRCA:                      /* A = A>>1 | A.0<<7 ; C = A.0 */
            (void)emit_and_w32_imm(e, A64_W10, A64_W9, 1);
            emit_lsr_w32_imm(e, A64_W9, A64_W9, 1);
            emit_lsl_w32_imm(e, A64_W12, A64_W10, 7);
            emit_orr_w32(e, A64_W9, A64_W9, A64_W12);
            break;
        case Z80_OP_RLA:                       /* A = A<<1 | C_in ; C = A.7 */
            (void)emit_and_w32_imm(e, A64_W13, A64_W11, 1);
            emit_lsr_w32_imm(e, A64_W10, A64_W9, 7);
            emit_lsl_w32_imm(e, A64_W9, A64_W9, 1);
            emit_orr_w32(e, A64_W9, A64_W9, A64_W13);
            (void)emit_and_w32_imm(e, A64_W9, A64_W9, 0xFF);
            break;
        default:                               /* RRA: A = A>>1 | C_in<<7 ; C = A.0 */
            (void)emit_and_w32_imm(e, A64_W13, A64_W11, 1);
            (void)emit_and_w32_imm(e, A64_W10, A64_W9, 1);
            emit_lsr_w32_imm(e, A64_W9, A64_W9, 1);
            emit_lsl_w32_imm(e, A64_W12, A64_W13, 7);
            emit_orr_w32(e, A64_W9, A64_W9, A64_W12);
            break;
        }
        emit_strb_imm(e, A64_W9, A64_W19, OFF_A);
        emit_movz_w32(e, A64_W12, Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV, 0);
        emit_and_w32(e, A64_W11, A64_W11, A64_W12);
        emit_orr_w32(e, A64_W11, A64_W11, A64_W10);        /* carry out */
        emit_movz_w32(e, A64_W12, Z80_FLAG_5 | Z80_FLAG_3, 0);
        emit_and_w32(e, A64_W12, A64_W9, A64_W12);
        emit_orr_w32(e, A64_W11, A64_W11, A64_W12);        /* XY from A' */
        emit_strb_imm(e, A64_W11, A64_W19, OFF_F);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_DAA:
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_daa);
        return OP_MODIFIES_F;

    case Z80_OP_CPL: {
        /* A = ~A. F: H and N set, XY from new A, S/Z/PV/C preserved. */
        emit_ldrb_imm(e, A64_W9, A64_W19, OFF_A);
        emit_mvn_w32(e, A64_W9, A64_W9);
        (void)emit_and_w32_imm(e, A64_W9, A64_W9, 0xFF);
        emit_strb_imm(e, A64_W9, A64_W19, OFF_A);
        emit_ldrb_imm(e, A64_W11, A64_W19, OFF_F);
        emit_movz_w32(e, A64_W12, 0xFF & ~(Z80_FLAG_5 | Z80_FLAG_3), 0);
        emit_and_w32(e, A64_W11, A64_W11, A64_W12);
        emit_movz_w32(e, A64_W12, Z80_FLAG_H | Z80_FLAG_N, 0);
        emit_orr_w32(e, A64_W11, A64_W11, A64_W12);
        emit_movz_w32(e, A64_W12, Z80_FLAG_5 | Z80_FLAG_3, 0);
        emit_and_w32(e, A64_W12, A64_W9, A64_W12);
        emit_orr_w32(e, A64_W11, A64_W11, A64_W12);
        emit_strb_imm(e, A64_W11, A64_W19, OFF_F);
        return OP_SETS_F_INLINE;
    }

    /* SCF / CCF share the Q quirk: XY sources from A when the PREVIOUS
     * instruction modified F (prev_q), else from A|F. Mid-block the
     * translator knows prev_q statically; first-in-block it's the live
     * cpu->q value, so we select at runtime. Leaves xy_src in W13;
     * expects A in W9 and F in W11. */
    case Z80_OP_SCF:
    case Z80_OP_CCF: {
        emit_ldrb_imm(e, A64_W9,  A64_W19, OFF_A);
        emit_ldrb_imm(e, A64_W11, A64_W19, OFF_F);
        if (prev_q > 0) {
            emit_mov_w32_w32(e, A64_W13, A64_W9);
        } else if (prev_q == 0) {
            emit_orr_w32(e, A64_W13, A64_W9, A64_W11);
        } else {
            emit_ldrb_imm(e, A64_W12, A64_W19, OFF_Q);
            emit_orr_w32(e, A64_W13, A64_W9, A64_W11);
            emit_cmp_w32_imm(e, A64_W12, 0);
            emit_csel_w32(e, A64_W13, A64_W9, A64_W13, A64_COND_NE);
        }
        if (dec->type == Z80_OP_SCF) {
            /* F = (F & (S|Z|PV)) | C | XY(xy_src) */
            emit_movz_w32(e, A64_W12, Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV, 0);
            emit_and_w32(e, A64_W11, A64_W11, A64_W12);
            (void)emit_orr_w32_imm(e, A64_W11, A64_W11, Z80_FLAG_C);
        } else {
            /* CCF: F = (F & (S|Z|PV)) | (old_c ? H : C) | XY(xy_src) */
            (void)emit_and_w32_imm(e, A64_W12, A64_W11, Z80_FLAG_C);  /* old_c */
            (void)emit_eor_w32_imm(e, A64_W14, A64_W12, 1);           /* new C */
            emit_lsl_w32_imm(e, A64_W12, A64_W12, 4);                 /* old_c -> H */
            emit_movz_w32(e, A64_W10, Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV, 0);
            emit_and_w32(e, A64_W11, A64_W11, A64_W10);
            emit_orr_w32(e, A64_W11, A64_W11, A64_W14);
            emit_orr_w32(e, A64_W11, A64_W11, A64_W12);
        }
        emit_movz_w32(e, A64_W12, Z80_FLAG_5 | Z80_FLAG_3, 0);
        emit_and_w32(e, A64_W12, A64_W13, A64_W12);
        emit_orr_w32(e, A64_W11, A64_W11, A64_W12);
        emit_strb_imm(e, A64_W11, A64_W19, OFF_F);
        return OP_SETS_F_INLINE;
    }

    case Z80_OP_EX_SP_HL: {
        /* Exchange (SP) with HL (or IX/IY under DD/FD); memptr = new
         * value. Stack writes skip the SMC helper — same justification
         * as PUSH: the guest stack essentially never overlaps code. */
        int idx = (dec->prefix == 0xDD || dec->prefix == 0xFD);
        uint32_t off_dst = idx ? (uint32_t)idx_reg_offset(dec->prefix) : OFF_HL;

        emit_ldrh_imm(e, A64_W11, A64_W19, OFF_SP);
        emit_ldrb_reg_uxtw(e, A64_W9, A64_W20, A64_W11);    /* lo = mem[sp] */
        emit_add_mask16(e, A64_W12, A64_W11, 1);
        emit_ldrb_reg_uxtw(e, A64_W10, A64_W20, A64_W12);   /* hi = mem[sp+1] */

        emit_ldrh_imm(e, A64_W13, A64_W19, off_dst);
        emit_strb_reg_uxtw(e, A64_W13, A64_W20, A64_W11);   /* mem[sp] = dst.lo */
        emit_lsr_w32_imm(e, A64_W14, A64_W13, 8);
        emit_strb_reg_uxtw(e, A64_W14, A64_W20, A64_W12);   /* mem[sp+1] = dst.hi */

        emit_lsl_w32_imm(e, A64_W10, A64_W10, 8);
        emit_orr_w32(e, A64_W9, A64_W9, A64_W10);           /* new dst */
        emit_strh_imm(e, A64_W9, A64_W19, off_dst);
        emit_strh_imm(e, A64_W9, A64_W19, OFF_MEMPTR);
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
        int src_off = (dec->prefix == 0xDD || dec->prefix == 0xFD)
                          ? idx_reg_offset(dec->prefix) : OFF_HL;
        emit_ldrh_imm(e, A64_W9, A64_W19, (uint32_t)src_off);
        emit_strh_imm(e, A64_W9, A64_W19, OFF_SP);
        return OP_FALL_THROUGH;
    }

    /* ---- 8-bit ALU. Helpers in dbt_flags.c do the actual flag math. */
    case Z80_OP_ADD_A_R: {
        emit_alu_src_from_reg(e, dec->reg1, dec->prefix);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_add);
        return OP_MODIFIES_F;
    }
    case Z80_OP_ADD_A_N: {
        emit_movz_w32(e, A64_W1, dec->imm8, 0);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_add);
        return OP_MODIFIES_F;
    }
    case Z80_OP_ADC_A_R: {
        emit_alu_src_from_reg(e, dec->reg1, dec->prefix);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_adc);
        return OP_MODIFIES_F;
    }
    case Z80_OP_ADC_A_N: {
        emit_movz_w32(e, A64_W1, dec->imm8, 0);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_adc);
        return OP_MODIFIES_F;
    }
    case Z80_OP_SUB_A_R: {
        emit_alu_src_from_reg(e, dec->reg1, dec->prefix);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_sub);
        return OP_MODIFIES_F;
    }
    case Z80_OP_SUB_A_N: {
        emit_movz_w32(e, A64_W1, dec->imm8, 0);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_sub);
        return OP_MODIFIES_F;
    }
    case Z80_OP_SBC_A_R: {
        emit_alu_src_from_reg(e, dec->reg1, dec->prefix);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_sbc);
        return OP_MODIFIES_F;
    }
    case Z80_OP_SBC_A_N: {
        emit_movz_w32(e, A64_W1, dec->imm8, 0);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_sbc);
        return OP_MODIFIES_F;
    }
    case Z80_OP_AND_A_R: {
        emit_alu_src_from_reg(e, dec->reg1, dec->prefix);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_and);
        return OP_MODIFIES_F;
    }
    case Z80_OP_AND_A_N: {
        emit_movz_w32(e, A64_W1, dec->imm8, 0);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_and);
        return OP_MODIFIES_F;
    }
    case Z80_OP_OR_A_R: {
        emit_alu_src_from_reg(e, dec->reg1, dec->prefix);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_or);
        return OP_MODIFIES_F;
    }
    case Z80_OP_OR_A_N: {
        emit_movz_w32(e, A64_W1, dec->imm8, 0);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_or);
        return OP_MODIFIES_F;
    }
    case Z80_OP_XOR_A_R: {
        emit_alu_src_from_reg(e, dec->reg1, dec->prefix);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_xor);
        return OP_MODIFIES_F;
    }
    case Z80_OP_XOR_A_N: {
        emit_movz_w32(e, A64_W1, dec->imm8, 0);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_xor);
        return OP_MODIFIES_F;
    }
    case Z80_OP_CP_A_R: {
        emit_alu_src_from_reg(e, dec->reg1, dec->prefix);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_cp);
        return OP_MODIFIES_F;
    }
    case Z80_OP_CP_A_N: {
        emit_movz_w32(e, A64_W1, dec->imm8, 0);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_cp);
        return OP_MODIFIES_F;
    }

    case Z80_OP_INC_R:
    case Z80_OP_DEC_R: {
        void *helper = (dec->type == Z80_OP_INC_R)
                           ? (void *)(uintptr_t)z80_jit_inc8
                           : (void *)(uintptr_t)z80_jit_dec8;
        if (dec->reg1 == 6) {
            /* INC/DEC (HL), unprefixed — same shape as the indexed
             * *_HL_ind case but the address is just HL. */
            emit_ldrh_imm(e, A64_W10, A64_W19, OFF_HL);
            emit_guest_loadb_w9_at_w10(e);          /* W9 = old byte */
            emit_mov_w32_w32(e, A64_W1, A64_W9);
            emit_call_helper(e, helper);
            emit_ldrh_imm(e, A64_W10, A64_W19, OFF_HL);  /* helper clobbered W10 */
            emit_mov_w32_w32(e, A64_W9, A64_W0);
            emit_guest_storeb_w9_at_w10_smc(e);
            return OP_MODIFIES_F;
        }
        int off = reg8_offset_p(dec->reg1, dec->prefix);
        emit_ldrb_imm(e, A64_W1, A64_W19, (uint32_t)off);   /* W1 = old value */
        emit_call_helper(e, helper);
        emit_strb_imm(e, A64_W0, A64_W19, (uint32_t)off);   /* W0 = new value */
        return OP_MODIFIES_F;
    }

    /* ---- CB-prefix: rotate/shift (sub<0x40), BIT (0x40..0x7F),
     *      RES (0x80..0xBF), SET (0xC0..0xFF).
     *
     * Sub-byte layout: family in bits 6..7, n (bit number or rotate kind)
     * in bits 3..5, reg encoding in bits 0..2 (6 = (HL)).
     *
     * Three forms by prefix:
     *   CB     — operand is r or (HL).
     *   DD CB  — operand is (IX+d); for non-BIT ops, result also writes
     *            to register r when r != 6 (the documented "dual
     *            writeback" undocumented behaviour zex* tests).
     *   FD CB  — same as DD CB but using IY.
     *
     * BIT n,<src> XY source byte:
     *   register form : the operand value itself
     *   (HL) form     : memptr.high
     *   (IX+d) form   : high byte of the computed addr
     */
    case Z80_OP_CB: {
        uint8_t sub = dec->imm8;
        int     r   = sub & 7;
        int     grp = (sub >> 3) & 7;
        unsigned family = sub >> 6;  /* 0=rot/shift, 1=BIT, 2=RES, 3=SET */
        int     indexed = (dec->prefix == 0xDD || dec->prefix == 0xFD);
        int     is_mem = indexed || (r == 6);
        /* Dual writeback applies to indexed RES/SET/rot/shift when
         * r != 6 (r == 6 is the pure memory form). BIT never writes. */
        int     dual_wb = indexed && r != 6 && family != 1;

        /* Load val into W2. For mem ops, keep the addr in W10. */
        if (indexed) {
            emit_idx_eff_addr(e, A64_W10, dec->prefix, (int8_t)dec->disp);
            emit_ldrb_reg_uxtw(e, A64_W2, A64_W20, A64_W10);
        } else if (r == 6) {
            emit_ldrh_imm(e, A64_W10, A64_W19, OFF_HL);
            emit_ldrb_reg_uxtw(e, A64_W2, A64_W20, A64_W10);
        } else {
            int off = reg8_offset(r);
            emit_ldrb_imm(e, A64_W2, A64_W19, (uint32_t)off);
        }

        if (family == 0) {
            /* Rotate / shift. Call helper(cpu, val); helper returns new
             * value in W0 and sets cpu->f. */
            static void *const helpers[8] = {
                (void *)z80_jit_rlc, (void *)z80_jit_rrc,
                (void *)z80_jit_rl,  (void *)z80_jit_rr,
                (void *)z80_jit_sla, (void *)z80_jit_sra,
                (void *)z80_jit_sll, (void *)z80_jit_srl,
            };
            emit_mov_w32_w32(e, A64_W1, A64_W2);     /* W1 = val (helper arg) */
            emit_call_helper(e, helpers[grp]);
            if (is_mem) {
                /* Helper clobbered W10; reload the addr. */
                if (indexed) {
                    emit_idx_eff_addr(e, A64_W10, dec->prefix, (int8_t)dec->disp);
                } else {
                    emit_ldrh_imm(e, A64_W10, A64_W19, OFF_HL);
                }
                /* Dual-writeback to register FIRST: the SMC helper inside
                 * the store sequence is allowed to clobber W0, so we
                 * stash the result to the register slot before that
                 * window opens. */
                if (dual_wb) {
                    int off = reg8_offset(r);
                    emit_strb_imm(e, A64_W0, A64_W19, (uint32_t)off);
                }
                emit_mov_w32_w32(e, A64_W9, A64_W0);
                emit_guest_storeb_w9_at_w10_smc(e);
            } else {
                int off = reg8_offset(r);
                emit_strb_imm(e, A64_W0, A64_W19, (uint32_t)off);
            }
            return OP_MODIFIES_F;
        }

        if (family == 1) {
            /* BIT n,<src>. No write-back. */
            emit_mov_w32_w32(e, A64_W1, A64_W2);         /* val */
            emit_movz_w32(e, A64_W2, (uint8_t)grp, 0);   /* bit number */
            if (indexed) {
                /* xy_byte = high byte of addr (in W10). */
                emit_lsr_w32_imm(e, A64_W3, A64_W10, 8);
            } else if (r == 6) {
                /* xy_byte = memptr.high */
                emit_ldrb_imm(e, A64_W3, A64_W19, OFF_MEMPTR + 1);
            } else {
                emit_mov_w32_w32(e, A64_W3, A64_W1);     /* xy = val */
            }
            emit_call_helper(e, (void *)(uintptr_t)z80_jit_bit);
            return OP_MODIFIES_F;
        }

        /* RES / SET: inline bit mask. No flag change.
         *
         * MOVZ-to-scratch + AND/ORR-register because the AArch64 logical-
         * immediate encoding can't express many of the 8-bit masks we
         * need (e.g. 0xFB = ~0x04 has a single zero bit and isn't a
         * valid bitmask immediate). */
        uint8_t mask = (uint8_t)(1u << grp);
        if (family == 2) {
            /* RES n,<src> : val &= ~mask */
            emit_movz_w32(e, A64_W11, (uint8_t)~mask, 0);
            emit_and_w32(e, A64_W2, A64_W2, A64_W11);
        } else {
            /* SET n,<src> : val |= mask */
            emit_movz_w32(e, A64_W11, mask, 0);
            emit_orr_w32(e, A64_W2, A64_W2, A64_W11);
        }
        if (is_mem) {
            /* Dual writeback to register BEFORE the SMC store (same
             * reason as the rot/shift path). For inline RES/SET no helper
             * has run yet, so W2 is also still valid here — but order
             * the writes the same way for consistency. */
            if (dual_wb) {
                int off = reg8_offset(r);
                emit_strb_imm(e, A64_W2, A64_W19, (uint32_t)off);
            }
            emit_mov_w32_w32(e, A64_W9, A64_W2);
            emit_guest_storeb_w9_at_w10_smc(e);
        } else {
            int off = reg8_offset(r);
            emit_strb_imm(e, A64_W2, A64_W19, (uint32_t)off);
        }
        return OP_FALL_THROUGH;
    }

    case Z80_OP_LDIR:
    case Z80_OP_LDDR: {
        /* Helper does the entire block copy, updates HL/DE/BC/F, and
         * performs the SMC sweep. Helper bumps cpu->q itself. */
        void *helper = (dec->type == Z80_OP_LDIR)
                           ? (void *)(uintptr_t)z80_jit_ldir
                           : (void *)(uintptr_t)z80_jit_lddr;
        emit_call_helper(e, helper);
        return OP_MODIFIES_F;
    }

    /* ---- PUSH rr / POP rr.
     * SP-relative stack accesses don't go through the SMC store helper:
     * the guest stack and the code segment essentially never overlap on
     * CP/M, and the interp-side SMC tracker (z80_mem_w) still catches
     * any pathological case via interp fallback for an immediate retry.
     * Same justification as the CALL push path. */
    case Z80_OP_PUSH_RR: {
        uint32_t off_rr;
        if      (dec->reg1 <= 2) off_rr = (uint32_t)rr_offset(dec->reg1);
        else if (dec->reg1 == 3) off_rr = OFF_AF;
        else                     off_rr = (uint32_t)idx_reg_offset(dec->prefix);

        emit_ldrh_imm(e, A64_W11, A64_W19, OFF_SP);
        emit_sub_mask16(e, A64_W11, A64_W11, 2);
        emit_strh_imm(e, A64_W11, A64_W19, OFF_SP);

        emit_ldrh_imm(e, A64_W9, A64_W19, off_rr);
        emit_strb_reg_uxtw(e, A64_W9, A64_W20, A64_W11);    /* mem[sp] = lo(rr) */
        emit_add_mask16(e, A64_W12, A64_W11, 1);
        emit_lsr_w32_imm(e, A64_W10, A64_W9, 8);
        emit_strb_reg_uxtw(e, A64_W10, A64_W20, A64_W12);   /* mem[sp+1] = hi(rr) */
        return OP_FALL_THROUGH;
    }

    case Z80_OP_POP_RR: {
        uint32_t off_rr;
        if      (dec->reg1 <= 2) off_rr = (uint32_t)rr_offset(dec->reg1);
        else if (dec->reg1 == 3) off_rr = OFF_AF;
        else                     off_rr = (uint32_t)idx_reg_offset(dec->prefix);

        emit_ldrh_imm(e, A64_W11, A64_W19, OFF_SP);
        emit_ldrb_reg_uxtw(e, A64_W9, A64_W20, A64_W11);    /* W9 = mem[sp] */
        emit_add_mask16(e, A64_W12, A64_W11, 1);
        emit_ldrb_reg_uxtw(e, A64_W10, A64_W20, A64_W12);   /* W10 = mem[sp+1] */
        emit_lsl_w32_imm(e, A64_W10, A64_W10, 8);
        emit_orr_w32(e, A64_W9, A64_W9, A64_W10);           /* W9 = hi:lo */
        emit_strh_imm(e, A64_W9, A64_W19, off_rr);

        emit_add_mask16(e, A64_W11, A64_W11, 2);
        emit_strh_imm(e, A64_W11, A64_W19, OFF_SP);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_JP_NN: {
        /* Block ends; epilogue uses dec->imm16 as the final PC.
         * memptr = nn (the jump target). */
        (void)pc_after;
        emit_movz_w32(e, A64_W9, dec->imm16, 0);
        emit_strh_imm(e, A64_W9, A64_W19, OFF_PC);
        emit_strh_imm(e, A64_W9, A64_W19, OFF_MEMPTR);
        return OP_ENDS_BLOCK;
    }

    case Z80_OP_JP_HL: {
        /* pc = HL. memptr unchanged (matches interp). */
        emit_ldrh_imm(e, A64_W9, A64_W19, OFF_HL);
        emit_strh_imm(e, A64_W9, A64_W19, OFF_PC);
        return OP_ENDS_BLOCK;
    }

    case Z80_OP_JR_E: {
        /* JR e: target = pc_after + (int8_t)disp.
         * memptr = target (always set on JR, unconditional). */
        uint16_t target = (uint16_t)(pc_after + (int16_t)dec->disp);
        emit_movz_w32(e, A64_W9, target, 0);
        emit_strh_imm(e, A64_W9, A64_W19, OFF_PC);
        emit_strh_imm(e, A64_W9, A64_W19, OFF_MEMPTR);
        return OP_ENDS_BLOCK;
    }

    case Z80_OP_CALL_NN: {
        /* CALL nn:
         *   sp = (sp - 2) & 0xFFFF
         *   mem[sp]     = pc_after & 0xFF
         *   mem[sp + 1] = (pc_after >> 8) & 0xFF        (sp+1 also masked)
         *   pc          = target                          (block ends)
         *   memptr      = target
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

        /* pc = target; memptr = target. */
        emit_movz_w32(e, A64_W9, dec->imm16, 0);
        emit_strh_imm(e, A64_W9, A64_W19, OFF_PC);
        emit_strh_imm(e, A64_W9, A64_W19, OFF_MEMPTR);
        return OP_ENDS_BLOCK;
    }

    case Z80_OP_RET: {
        /* RET:
         *   pc.lo = mem[sp]
         *   pc.hi = mem[(sp + 1) & 0xFFFF]
         *   sp    = (sp + 2) & 0xFFFF
         *   memptr = popped pc
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
        emit_strh_imm(e, A64_W9, A64_W19, OFF_MEMPTR);

        /* sp += 2, masked */
        emit_add_mask16(e, A64_W11, A64_W11, 2);
        emit_strh_imm(e, A64_W11, A64_W19, OFF_SP);
        return OP_ENDS_BLOCK;
    }

    case Z80_OP_JP_CC_NN: {
        /* JP cc, nn:
         *   memptr = nn          (always — interp sets it regardless)
         *   pc = (cc) ? nn : pc_after
         * No memory side effects, so CSEL handles both branches cleanly. */
        uint16_t target = dec->imm16;
        emit_test_z80_flag(e, flag_mask_for_cc(dec->cc));
        emit_movz_w32(e, A64_W9,  target,   0);     /* W9 = target */
        emit_movz_w32(e, A64_W11, pc_after, 0);     /* W11 = fall-through */
        emit_csel_w32(e, A64_W13, A64_W9, A64_W11, host_cond_for_cc(dec->cc));
        emit_strh_imm(e, A64_W13, A64_W19, OFF_PC);
        emit_strh_imm(e, A64_W9,  A64_W19, OFF_MEMPTR);
        return OP_ENDS_BLOCK;
    }

    case Z80_OP_JR_CC_E: {
        /* JR cc, e:
         *   if (cc) { pc = target ; memptr = target } else { pc = pc_after }
         * memptr is conditional; CSEL it against the existing memptr. */
        uint16_t target = (uint16_t)(pc_after + (int16_t)dec->disp);
        emit_test_z80_flag(e, flag_mask_for_cc(dec->cc));
        emit_movz_w32(e, A64_W9,  target,   0);
        emit_movz_w32(e, A64_W11, pc_after, 0);
        a64_cond_t hc = host_cond_for_cc(dec->cc);
        emit_csel_w32(e, A64_W13, A64_W9, A64_W11, hc);
        emit_strh_imm(e, A64_W13, A64_W19, OFF_PC);
        /* memptr: only updated on the taken path. */
        emit_ldrh_imm(e, A64_W14, A64_W19, OFF_MEMPTR);
        emit_csel_w32(e, A64_W14, A64_W9, A64_W14, hc);
        emit_strh_imm(e, A64_W14, A64_W19, OFF_MEMPTR);
        return OP_ENDS_BLOCK;
    }

    case Z80_OP_DJNZ: {
        /* DJNZ e:
         *   B = (B - 1) & 0xFF
         *   if (B != 0) { pc = target ; memptr = target }
         *   else        { pc = pc_after }   (memptr unchanged) */
        uint16_t target = (uint16_t)(pc_after + (int16_t)dec->disp);

        emit_ldrb_imm(e, A64_W9, A64_W19, OFF_B);
        emit_sub_w32_imm(e, A64_W9, A64_W9, 1);
        (void)emit_and_w32_imm(e, A64_W9, A64_W9, 0xFF);
        emit_strb_imm(e, A64_W9, A64_W19, OFF_B);

        /* CMP W9, #0 — host Z = (W9 == 0). Use NE for "taken when B != 0". */
        emit_cmp_w32_imm(e, A64_W9, 0);
        emit_movz_w32(e, A64_W10, target,   0);
        emit_movz_w32(e, A64_W11, pc_after, 0);
        emit_csel_w32(e, A64_W13, A64_W10, A64_W11, A64_COND_NE);
        emit_strh_imm(e, A64_W13, A64_W19, OFF_PC);

        /* memptr: only on taken path. */
        emit_ldrh_imm(e, A64_W14, A64_W19, OFF_MEMPTR);
        emit_csel_w32(e, A64_W14, A64_W10, A64_W14, A64_COND_NE);
        emit_strh_imm(e, A64_W14, A64_W19, OFF_MEMPTR);
        return OP_ENDS_BLOCK;
    }

    case Z80_OP_CALL_CC_NN: {
        /* CALL cc, nn:
         *   memptr = nn (always)
         *   if (cc) { sp -= 2 ; mem[sp..sp+1] = pc_after ; pc = nn }
         *   else    { pc = pc_after }
         *
         * Forward-branch over the taken path because the push is
         * irreducibly conditional — we can't CSEL a memory write. */
        uint16_t target = dec->imm16;
        emit_movz_w32(e, A64_W9, target, 0);
        emit_strh_imm(e, A64_W9, A64_W19, OFF_MEMPTR);       /* memptr = target */

        emit_test_z80_flag(e, flag_mask_for_cc(dec->cc));
        /* B.cond skip_to_not_taken — fires when cc is FALSE. EQ when cc
         * is odd (take-if-set, so not-taken is flag-clear → host EQ
         * after TST means flag-clear → not taken). Mirror for even. */
        a64_cond_t skip_cond = (dec->cc & 1) ? A64_COND_EQ : A64_COND_NE;
        uint32_t patch_skip = emit_pos(e);
        emit_b_cond(e, skip_cond, 0);

        /* Taken path: push pc_after, set pc = target. */
        emit_ldrh_imm(e, A64_W11, A64_W19, OFF_SP);
        emit_sub_mask16(e, A64_W11, A64_W11, 2);
        emit_strh_imm(e, A64_W11, A64_W19, OFF_SP);
        emit_movz_w32(e, A64_W9, pc_after & 0xFF, 0);
        emit_strb_reg_uxtw(e, A64_W9, A64_W20, A64_W11);
        emit_add_mask16(e, A64_W12, A64_W11, 1);
        emit_movz_w32(e, A64_W9, (pc_after >> 8) & 0xFF, 0);
        emit_strb_reg_uxtw(e, A64_W9, A64_W20, A64_W12);
        emit_movz_w32(e, A64_W9, target, 0);
        emit_strh_imm(e, A64_W9, A64_W19, OFF_PC);

        /* Skip past the not-taken path. */
        uint32_t patch_done = emit_pos(e);
        emit_b_cond(e, A64_COND_AL, 0);

        /* Not-taken path: pc = pc_after. */
        emit_patch_cond19(e, patch_skip, emit_pos(e));
        emit_movz_w32(e, A64_W9, pc_after, 0);
        emit_strh_imm(e, A64_W9, A64_W19, OFF_PC);

        /* Land here. */
        emit_patch_cond19(e, patch_done, emit_pos(e));
        return OP_ENDS_BLOCK;
    }

    case Z80_OP_RET_CC: {
        /* RET cc:
         *   if (cc) { pc.lo = mem[sp] ; pc.hi = mem[(sp+1)&0xFFFF] ;
         *              sp += 2 ; memptr = pc }
         *   else    { pc = pc_after }
         *
         * Forward-branch like CALL cc since the load+sp update is
         * irreducibly conditional. */
        emit_test_z80_flag(e, flag_mask_for_cc(dec->cc));
        a64_cond_t skip_cond = (dec->cc & 1) ? A64_COND_EQ : A64_COND_NE;
        uint32_t patch_skip = emit_pos(e);
        emit_b_cond(e, skip_cond, 0);

        /* Taken path: pop pc, sp += 2, memptr = pc. */
        emit_ldrh_imm(e, A64_W11, A64_W19, OFF_SP);
        emit_ldrb_reg_uxtw(e, A64_W9, A64_W20, A64_W11);
        emit_add_mask16(e, A64_W12, A64_W11, 1);
        emit_ldrb_reg_uxtw(e, A64_W10, A64_W20, A64_W12);
        emit_lsl_w32_imm(e, A64_W10, A64_W10, 8);
        emit_orr_w32(e, A64_W9, A64_W9, A64_W10);
        emit_strh_imm(e, A64_W9, A64_W19, OFF_PC);
        emit_strh_imm(e, A64_W9, A64_W19, OFF_MEMPTR);
        emit_add_mask16(e, A64_W11, A64_W11, 2);
        emit_strh_imm(e, A64_W11, A64_W19, OFF_SP);

        uint32_t patch_done = emit_pos(e);
        emit_b_cond(e, A64_COND_AL, 0);

        /* Not-taken path: pc = pc_after. */
        emit_patch_cond19(e, patch_skip, emit_pos(e));
        emit_movz_w32(e, A64_W9, pc_after, 0);
        emit_strh_imm(e, A64_W9, A64_W19, OFF_PC);

        emit_patch_cond19(e, patch_done, emit_pos(e));
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
    int q_mode = Q_CLEAR;
    int prev_q = -1;   /* first op: prev insn's q is the live cpu->q */

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

        unsigned r = emit_op(&e, &dec, pc_after, prev_q);
        if      (r & OP_MODIFIES_F)    q_mode = Q_KEEP;
        else if (r & OP_SETS_F_INLINE) q_mode = Q_SET;
        else                           q_mode = Q_CLEAR;
        prev_q = (q_mode != Q_CLEAR);
        ended_by_branch = (r & OP_ENDS_BLOCK) != 0;
        insns++;
        pc = pc_after;

        if (ended_by_branch) break;
    }

    if (insns == 0) return NULL;

    /* If we ended on a branch, the op already wrote cpu->pc. Otherwise
     * the next PC is the straight-through fall-through value in `pc`. */
    if (ended_by_branch) {
        emit_block_tail(&e, insns, q_mode);
    } else {
        emit_block_epilogue(&e, pc, insns, q_mode);
    }

    dbt->code_used = e.offset;
    __builtin___clear_cache((char *)entry, (char *)(dbt->code_buf + e.offset));

    /* Tag every guest byte this block covered so a JIT-emitted store
     * that lands on any of them triggers SMC invalidation. */
    dbt_mark_block_bytes(dbt, guest_pc, pc);
    return entry;
}
