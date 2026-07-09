/* dbt_a64.c — AArch64 backend for the Z80 DBT.
 *
 * Translates loads/stores/16-bit boring ops/control flow/8-bit ALU/CB
 * ops. Anything else (ED-prefix beyond LDIR/LDDR, EX AF, EXX, I/O, etc.)
 * ends the block; the run loop in dbt_common.c falls back to the
 * interpreter for that single instruction.
 *
 * Host register convention inside translated code (pinned across blocks
 * AND across block chains — the whole point):
 *   X19 = z80_cpu_t *cpu
 *   X20 = cpu->mem (uint8_t *)   (guest LDRB/STRB via UXTW index)
 *   X21 = guest BC               (canonical: zero-extended 16-bit)
 *   X22 = guest DE               (canonical)
 *   X23 = guest HL               (canonical)
 *   X24 = JIT aux base           (&dbt->jit_ftables; flag tables at +0,
 *                                 code bitmap at +0x10000, block cache
 *                                 at +0x20000 — both big offsets reach
 *                                 via a single ADD #imm12, LSL#12)
 *   X25 = pending insn count     (flushed into cpu->insn_count on exit)
 *   X26 = guest SP               (canonical)
 *   X27 = guest A                (canonical: zero-extended 8-bit)
 *   X28 = guest F                (canonical)
 *   W9..W16 = scratch; W0..W3 near helper calls; W0 = next guest PC at
 *   block tails (consumed by the chain probe / exit stub).
 *
 * Everything else guest-visible (IX, IY, memptr, q, alternate set, I/R)
 * stays context-resident.
 *
 * Block ABI: the trampoline loads the pinned set from the context and
 * BRs into the block. Blocks chain to each other with the pinned state
 * live. On a chain miss the tail branches to a shared exit stub that
 * spills the pinned state (including W0 -> cpu->pc), flushes X25 into
 * cpu->insn_count, restores the AAPCS64 callee-saved registers from the
 * trampoline frame, and RETs to the trampoline's caller. Blocks never
 * RET themselves, so helper BLRs are free to clobber X30.
 *
 * Helper-call sync contract: the pinned registers are AAPCS64 callee-
 * saved, so C helpers preserve them — but helpers read/write guest state
 * through cpu->*, so call sites must spill the fields a helper READS and
 * reload the fields it WRITES (DAA: A/F; LDIR/LDDR: BC/DE/HL/A/F in,
 * BC/DE/HL/F out; post_store: nothing).
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

/* Pinned-register aliases (see convention above). */
#define R_CPU  A64_W19
#define R_MEM  A64_W20
#define R_BC   A64_W21
#define R_DE   A64_W22
#define R_HL   A64_W23
#define R_AUX  A64_W24
#define R_CNT  A64_W25
#define R_SP   A64_W26
#define R_A    A64_W27
#define R_F    A64_W28

/* JIT aux block segments, in ADD #imm12, LSL#12 units (dbt.h layout). */
#define AUX_BITMAP_SEG 16   /* +0x10000 */
#define AUX_CACHE_SEG  32   /* +0x20000 */

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

/* ----------------------------------------------------------------------
 * Trampoline + exit stub.
 *
 * Called from C as:
 *   void trampoline(z80_cpu_t *cpu, uint8_t *mem, void *block, void *aux);
 * with the AAPCS64 mapping cpu=X0, mem=X1, block=X2, aux=X3.
 *
 * Saves the callee-saved registers we pin, binds the host register
 * convention, loads the pinned guest state from the context, and BRs
 * into `block`. Blocks exit by branching to the exit stub emitted right
 * after, which spills everything back and unwinds the frame directly —
 * the BR (not BLR) into the block means X30 still holds the trampoline's
 * caller, but we restore it from the frame anyway since helper calls
 * inside blocks clobber it.
 * ---------------------------------------------------------------------- */
void dbt_emit_trampoline(z80_dbt_t *dbt) {
    emit_t e = { .buf = dbt->code_buf, .offset = 0, .capacity = CODE_BUF_SIZE };

    /* Frame: 96 bytes (16-byte aligned).
     *   [SP+ 0] FP, LR      [SP+16] X19, X20    [SP+32] X21, X22
     *   [SP+48] X23, X24    [SP+64] X25, X26    [SP+80] X27, X28 */
    emit_stp_pre_sp (&e, A64_W29, A64_W30, -96);
    emit_stp_x64_off(&e, A64_W19, A64_W20, A64_SP, 16);
    emit_stp_x64_off(&e, A64_W21, A64_W22, A64_SP, 32);
    emit_stp_x64_off(&e, A64_W23, A64_W24, A64_SP, 48);
    emit_stp_x64_off(&e, A64_W25, A64_W26, A64_SP, 64);
    emit_stp_x64_off(&e, A64_W27, A64_W28, A64_SP, 80);

    /* Bind host register convention. */
    emit_mov_x64_x64(&e, R_CPU, A64_W0);
    emit_mov_x64_x64(&e, R_MEM, A64_W1);
    emit_mov_x64_x64(&e, R_AUX, A64_W3);
    emit_movz_x64(&e, R_CNT, 0, 0);

    /* Load the pinned guest state. */
    emit_ldrh_imm(&e, R_BC, R_CPU, OFF_BC);
    emit_ldrh_imm(&e, R_DE, R_CPU, OFF_DE);
    emit_ldrh_imm(&e, R_HL, R_CPU, OFF_HL);
    emit_ldrh_imm(&e, R_SP, R_CPU, OFF_SP);
    emit_ldrb_imm(&e, R_A,  R_CPU, OFF_A);
    emit_ldrb_imm(&e, R_F,  R_CPU, OFF_F);

    /* Enter the block (pointer in X2). Blocks exit via the stub below. */
    emit_br(&e, A64_W2);

    /* ---- Exit stub. Entered by B from block tails with W0 = next pc. */
    dbt->exit_stub_off = e.offset;

    emit_strh_imm(&e, A64_W0, R_CPU, OFF_PC);
    emit_strh_imm(&e, R_BC,   R_CPU, OFF_BC);
    emit_strh_imm(&e, R_DE,   R_CPU, OFF_DE);
    emit_strh_imm(&e, R_HL,   R_CPU, OFF_HL);
    emit_strh_imm(&e, R_SP,   R_CPU, OFF_SP);
    emit_strb_imm(&e, R_A,    R_CPU, OFF_A);
    emit_strb_imm(&e, R_F,    R_CPU, OFF_F);

    /* Flush the pending insn count. */
    emit_ldr_x64_imm(&e, A64_W9, R_CPU, OFF_INSN_COUNT);
    emit_add_x64(&e, A64_W9, A64_W9, R_CNT);
    emit_str_x64_imm(&e, A64_W9, R_CPU, OFF_INSN_COUNT);

    /* Unwind the trampoline frame and return to its caller. */
    emit_ldp_x64_off(&e, A64_W27, A64_W28, A64_SP, 80);
    emit_ldp_x64_off(&e, A64_W25, A64_W26, A64_SP, 64);
    emit_ldp_x64_off(&e, A64_W23, A64_W24, A64_SP, 48);
    emit_ldp_x64_off(&e, A64_W21, A64_W22, A64_SP, 32);
    emit_ldp_x64_off(&e, A64_W19, A64_W20, A64_SP, 16);
    emit_ldp_post_sp(&e, A64_W29, A64_W30, 96);
    emit_ret(&e);

    dbt->code_used = e.offset;
    __builtin___clear_cache((char *)dbt->code_buf,
                            (char *)dbt->code_buf + e.offset);
}

/* ----------------------------------------------------------------------
 * Guest register access helpers.
 * ---------------------------------------------------------------------- */

/* Map a Z80 reg code 0..7 (B,C,D,E,H,L,(HL),A) to its byte offset
 * within z80_cpu_t — used by can_translate for validity and by the
 * IX/IY half-register forms (codes 8/9) for their context offsets.
 * Returns -1 on unsupported (here that's just (HL)). */
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

/* Pinned-pair location of an 8-bit reg code: host pair register and the
 * bit shift of the half (0 = low, 8 = high). -1 if not pinned-pair
 * (A lives whole in R_A; codes 8/9 are context bytes). */
static int r8_host_pair(int r, int *shift) {
    switch (r) {
    case 0: *shift = 8; return R_BC;
    case 1: *shift = 0; return R_BC;
    case 2: *shift = 8; return R_DE;
    case 3: *shift = 0; return R_DE;
    case 4: *shift = 8; return R_HL;
    case 5: *shift = 0; return R_HL;
    default: return -1;
    }
}

/* Materialize guest 8-bit reg `r` into a host register and return it.
 * A comes back as R_A itself (no code emitted); pinned halves UBFX into
 * `tmp`; IX/IY halves LDRB into `tmp`. The returned value is canonical
 * (0..255). */
static a64_reg_t emit_read_r8(emit_t *e, a64_reg_t tmp, int r, uint8_t prefix) {
    int shift;
    int pair = r8_host_pair(r, &shift);
    if (r == 7) return R_A;
    if (pair >= 0) {
        emit_ubfx_w32(e, tmp, (a64_reg_t)pair, (uint32_t)shift, 8);
        return tmp;
    }
    emit_ldrb_imm(e, tmp, R_CPU, (uint32_t)reg8_offset_p(r, prefix));
    return tmp;
}

/* Write host register `src` (must be canonical 0..255) into guest 8-bit
 * reg `r`. Pinned halves BFI; A is a plain MOV; IX/IY halves STRB. */
static void emit_write_r8(emit_t *e, int r, uint8_t prefix, a64_reg_t src) {
    int shift;
    int pair = r8_host_pair(r, &shift);
    if (r == 7) {
        if (src != R_A) emit_mov_w32_w32(e, R_A, src);
        return;
    }
    if (pair >= 0) {
        emit_bfi_w32(e, (a64_reg_t)pair, src, (uint32_t)shift, 8);
        return;
    }
    emit_strb_imm(e, src, R_CPU, (uint32_t)reg8_offset_p(r, prefix));
}

/* Offset of the IX or IY register (full 16-bit) for the given DD/FD
 * prefix. Caller must already know prefix is DD or FD. */
static int idx_reg_offset(uint8_t prefix) {
    return (prefix == 0xFD) ? OFF_IY : OFF_IX;
}

/* Pinned host register for 16-bit pair code 0..3 (BC/DE/HL/SP), or -1
 * when the pair is IX/IY under DD/FD (context-resident). */
static int rr_host_p(int rr, uint8_t prefix) {
    if (rr == 2 && (prefix == 0xDD || prefix == 0xFD)) return -1;
    switch (rr) {
    case 0: return R_BC;
    case 1: return R_DE;
    case 2: return R_HL;
    case 3: return R_SP;
    default: return -1;
    }
}

/* ---- Tiny emit helpers used by the per-op cases. W9 is the canonical
 *      scratch for values, W10 for addresses. ---- */

static inline void emit_load_imm16_into(emit_t *e, a64_reg_t rd, uint16_t imm) {
    emit_movz_w32(e, rd, imm, 0);
}

/* mem[addr] = val — guest byte store with inlined SMC fast-path check.
 *
 * The check loads dbt->code_bitmap[addr] through the aux base (bitmap
 * segment at +0x10000, reached with one ADD #16,LSL#12). If the byte is
 * zero (no cached block covered this address), the CBZ skips the BLR —
 * non-SMC stores cost ADD + LDRB + CBZ. Real SMC stores trip into
 * z80_jit_post_store, which invalidates the affected cache entries.
 *
 * `val` and `addr` may be pinned registers (they survive the helper —
 * callee-saved — and the fast path doesn't touch them). Scratch W0/W1/
 * W9/W15 (and everything caller-saved, on the slow path) is clobbered,
 * so callers must not keep values there across this call.
 *
 * Stack pushes (CALL/PUSH/EX (SP),HL) bypass this helper — the guest
 * stack essentially never overlaps code, so the check would be wasted
 * work there. */
static void emit_guest_storeb_smc(emit_t *e, a64_reg_t val, a64_reg_t addr) {
    emit_strb_reg_uxtw(e, val, R_MEM, addr);
    emit_add_w32_imm_lsl12(e, A64_W15, addr, AUX_BITMAP_SEG);
    emit_ldrb_reg_uxtw(e, A64_W15, R_AUX, A64_W15);

    /* CBZ W15, .skip — placeholder, patched below. */
    uint32_t cbz_at = e->offset;
    emit_cbz_w32(e, A64_W15, 0);

    /* SMC path: post_store(cpu, addr). Touches no guest registers. */
    emit_mov_x64_x64(e, A64_W0, R_CPU);
    emit_mov_w32_w32(e, A64_W1, addr);
    emit_mov_x64_imm64(e, A64_W9, (uint64_t)(uintptr_t)z80_jit_post_store);
    emit_blr(e, A64_W9);

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

/* cpu->memptr = imm16. */
static void emit_set_memptr_imm(emit_t *e, uint16_t value) {
    emit_movz_w32(e, A64_W9, value, 0);
    emit_strh_imm(e, A64_W9, R_CPU, OFF_MEMPTR);
}

/* cpu->memptr = (A << 8) | low8_imm — the LD (nn),A memptr-quirk form.
 * low8_imm is the static "(addr + 1) & 0xFF" part. */
static void emit_set_memptr_quirk_imm(emit_t *e, uint8_t low8_imm) {
    if (low8_imm != 0) {
        emit_movz_w32(e, A64_W9, low8_imm, 0);
        emit_orr_w32_lsl(e, A64_W9, A64_W9, R_A, 8);
    } else {
        emit_lsl_w32_imm(e, A64_W9, R_A, 8);
    }
    emit_strh_imm(e, A64_W9, R_CPU, OFF_MEMPTR);
}

/* cpu->memptr = (A << 8) | ((pair + 1) & 0xFF) — dynamic LD (BC|DE),A. */
static void emit_set_memptr_quirk_rr(emit_t *e, a64_reg_t pair) {
    emit_add_w32_imm(e, A64_W12, pair, 1);
    (void)emit_and_w32_imm(e, A64_W12, A64_W12, 0xFF);
    emit_orr_w32_lsl(e, A64_W12, A64_W12, R_A, 8);
    emit_strh_imm(e, A64_W12, R_CPU, OFF_MEMPTR);
}

/* cpu->memptr = (pair + 1) — LD A,(BC|DE) straight "addr+1" semantics.
 * STRH truncates to 16 bits, matching the interp's wrap. */
static void emit_set_memptr_rr_plus_one(emit_t *e, a64_reg_t pair) {
    emit_add_w32_imm(e, A64_W9, pair, 1);
    emit_strh_imm(e, A64_W9, R_CPU, OFF_MEMPTR);
}

/* Wdst = ((IX|IY) + (int8)disp) & 0xFFFF. IX/IY stay context-resident.
 * The mask is required because the result is used as a UXTW memory
 * index and (IX+d) may wrap across the 64KB boundary. Uses only `dst`. */
static void emit_idx_eff_addr(emit_t *e, a64_reg_t dst, uint8_t prefix, int8_t disp) {
    emit_ldrh_imm(e, dst, R_CPU, (uint32_t)idx_reg_offset(prefix));
    if (disp == 0) return;   /* LDRH result is already canonical */
    if (disp > 0) {
        emit_add_w32_imm(e, dst, dst, (uint32_t)disp);
    } else {
        emit_sub_w32_imm(e, dst, dst, (uint32_t)(-disp));
    }
    (void)emit_and_w32_imm(e, dst, dst, 0xFFFF);
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

/* Emit "TST F, #mask" against the pinned F, setting host flags so the
 * caller can immediately CSEL / B.cond. All four cc flag masks are
 * single bits and encode as logical immediates. */
static void emit_test_z80_flag(emit_t *e, uint8_t mask) {
    if (!emit_tst_w32_imm(e, R_F, mask)) {
        emit_movz_w32(e, A64_W12, mask, 0);
        emit_tst_w32(e, R_F, A64_W12);
    }
}

/* What the block tail must do to cpu->q so it matches what the interp
 * would leave after the block's LAST instruction (z80_step zeroes q,
 * then F-writing ops set it to 1):
 *   Q_CLEAR — last op does not write F (or is an interp quirk like the
 *             accumulator rotates, which don't set q): store 0.
 *   Q_KEEP  — last op called a flag helper, which stored q=1 itself.
 *   Q_SET   — last op wrote F with inline code (no helper): store 1. */
enum { Q_CLEAR = 0, Q_KEEP = 1, Q_SET = 2 };

/* Tail prologue: fix up cpu->q per the mode above and bump the pending
 * insn count in X25. Runs exactly once per block exit, BEFORE any edge
 * split — neither insn touches NZCV, so conditional enders can TST
 * before or after it. */
static void emit_tail_prologue(emit_t *e, uint32_t insn_count_delta, int q_mode) {
    if (q_mode == Q_CLEAR) {
        emit_strb_imm(e, A64_WZR, R_CPU, OFF_Q);
    } else if (q_mode == Q_SET) {
        emit_movz_w32(e, A64_W9, 1, 0);
        emit_strb_imm(e, A64_W9, R_CPU, OFF_Q);
    }
    emit_add_x64_imm(e, R_CNT, R_CNT, insn_count_delta);
}

/* Dynamic tail: inline cache probe against the aux base's cache segment.
 * Used when the next PC is only known at run time (RET, JP (HL)) and as
 * the fallback behind every unlinked static edge.
 *
 * Entry contract: W0 holds the next guest PC (0..0xFFFF).
 *   X13 = aux + pc * 16 + 0x20000    (cache entry address)
 *   X15, X16 = LDP entry             (guest_pc+pad, native_code)
 *   if W15 != W0 → B exit stub
 *   BR X16
 * One LDP fetches the whole 16-byte entry — the pad bytes ride in
 * X15's high half, which the 32-bit compare ignores. The cache index
 * is exactly `pc` since BLOCK_CACHE_MASK == 0xFFFF. */
static void emit_dynamic_tail(emit_t *e, uint32_t exit_stub_off) {
    emit_lsl_w32_imm(e, A64_W13, A64_W0, 4);
    emit_add_w32_imm_lsl12(e, A64_W13, A64_W13, AUX_CACHE_SEG);
    emit_add_x64_w32_uxtw(e, A64_W13, R_AUX, A64_W13);
    emit_ldp_x64_off(e, A64_W15, A64_W16, A64_W13, 0);
    emit_cmp_w32_w32(e, A64_W15, A64_W0);

    uint32_t patch_miss = emit_pos(e);
    emit_b_cond(e, A64_COND_NE, 0);
    emit_br(e, A64_W16);

    /* Chain exit (probe miss): the stub spills pinned state + W0 and
     * unwinds the trampoline frame. */
    emit_patch_cond19(e, patch_miss, emit_pos(e));
    emit_b(e, (int32_t)exit_stub_off - (int32_t)emit_pos(e));
}

/* Static edge: the direct-link unit.
 *
 *   MOVZ W0, #pc          (the probe/exit stub need it when unlinked;
 *                          one harmless insn when linked)
 *   B    <target | .+4>   (the patchable link site)
 *   <probe + exit>        (fallback the unlink path re-aims the B at)
 *
 * If the target block already exists we link immediately; either way the
 * site is registered so dbt_cache_insert / SMC invalidation can (re)aim
 * it later. If the link pool is full the site stays permanently unlinked
 * — a direct link without a record could never be unpatched after SMC. */
static void emit_edge(z80_dbt_t *dbt, emit_t *e, uint16_t pc) {
    emit_movz_w32(e, A64_W0, pc, 0);
    uint32_t site = e->offset;
    int linked = 0;
    if (dbt_link_record(dbt, pc, site)) {
        z80_block_entry_t *be = &dbt->cache[pc];   /* index == pc */
        if (be->guest_pc == (uint32_t)pc && be->native_code) {
            emit_b(e, (int32_t)(be->native_code - (e->buf + site)));
            linked = 1;
        }
    }
    if (!linked)
        emit_b(e, 4);   /* fall through to the probe below */
    emit_dynamic_tail(e, dbt->exit_stub_off);
}

/* NOTE on a road not taken: a shadow return-address stack (guest CALL
 * does a host BL + pushes {guest_ret_pc, landing_pad} on the host
 * stack; guest RET validates and host-RETs, riding the hardware return
 * predictor) was built and benchmarked here — and measured a consistent
 * 3-6% LOSS on both SQUARO and zexdoc. Apple Silicon's indirect-branch
 * predictor already handles the probe's per-site BR essentially
 * perfectly, so the prediction win never materializes while the
 * CALL-side push (~6 insns) is paid every time. The LDP single-load
 * probe below is the useful thing that analysis left behind. */

/* Rewrite the patchable B at site_off (see emit_edge). target == NULL
 * re-aims it at its own fallback probe (site + 4). Caller holds the
 * W^X writable bracket; this can run while the JIT is mid-execution
 * (post_store → invalidate → unlink), which is safe because we're in C
 * code — not executing from the JIT region — and sys_icache_invalidate
 * (via __builtin___clear_cache) handles same-thread code modification. */
void dbt_arch_patch_link(z80_dbt_t *dbt, uint32_t site_off, uint8_t *target) {
    uint8_t *site = dbt->code_buf + site_off;
    uint8_t *dst  = target ? target : site + 4;
    int64_t  disp = dst - site;
    uint32_t inst = 0x14000000u | (((uint32_t)((int32_t)disp >> 2)) & 0x03FFFFFFu);

    /* Skip the write AND the icache flush when the site already holds
     * the desired branch — unlinking a never-linked (pending) site is
     * the common case on SMC-heavy workloads, and sys_icache_invalidate
     * per 4-byte patch is what turned zexdoc's invalidation storms into
     * a meltdown before this check. */
    uint32_t cur;
    memcpy(&cur, site, 4);
    if (cur == inst) return;

    site[0] = (uint8_t)(inst >>  0);
    site[1] = (uint8_t)(inst >>  8);
    site[2] = (uint8_t)(inst >> 16);
    site[3] = (uint8_t)(inst >> 24);
    __builtin___clear_cache((char *)site, (char *)site + 4);
}

/* Emit "MOV X0, X19 ; MOVZ/K X9, <helper> ; BLR X9". Result lands in W0.
 * X30 is clobbered — harmless, blocks never RET (see exit stub). */
static void emit_call_helper(emit_t *e, void *helper_addr) {
    emit_mov_x64_x64(e, A64_W0, R_CPU);
    emit_mov_x64_imm64(e, A64_W9, (uint64_t)(uintptr_t)helper_addr);
    emit_blr(e, A64_W9);
}

/* ----------------------------------------------------------------------
 * Inline 8-bit ALU — no helper call, pinned A and F.
 *
 * Contract: operand b is in W1 and MUST be canonical (0..255) and must
 * not alias R_A (callers copy A to W1 for ADD A,A etc. — the identity-
 * based H/V computation needs the OLD operand after A is rewritten).
 * F is fully assembled into R_F: the result-only bits (S/Z/XY, +parity
 * for logic ops) come from one LDRB off the aux flag tables; H rides the
 * carry-recovery identity (bit 4 of a^b^res holds the carry into bit 4
 * for add AND the borrow for sub, with or without carry-in); C is bit 8
 * of the 32-bit result; V is the classic sign-xor formula.
 * Clobbers W9, W11-W15.
 * ---------------------------------------------------------------------- */
enum {
    ALU_ADD, ALU_ADC, ALU_SUB, ALU_SBC, ALU_AND, ALU_OR, ALU_XOR, ALU_CP
};

static void emit_alu_inline(emit_t *e, int op) {
    if (op == ALU_AND || op == ALU_OR || op == ALU_XOR) {
        if (op == ALU_AND)      emit_and_w32(e, R_A, R_A, A64_W1);
        else if (op == ALU_OR)  emit_orr_w32(e, R_A, R_A, A64_W1);
        else                    emit_eor_w32(e, R_A, R_A, A64_W1);
        emit_ldrb_reg_uxtw(e, R_F, R_AUX, R_A);   /* FT_LOGIC == +0 */
        if (op == ALU_AND)
            (void)emit_orr_w32_imm(e, R_F, R_F, Z80_FLAG_H);
        return;
    }

    int is_sub = (op == ALU_SUB || op == ALU_SBC || op == ALU_CP);

    emit_mov_w32_w32(e, A64_W9, R_A);                      /* W9 = old a */

    /* W13 = wide result (carry/borrow lives at bit 8). Carry-in is
     * extracted from R_F BEFORE R_F is overwritten below. */
    if (op == ALU_ADC || op == ALU_SBC) {
        (void)emit_and_w32_imm(e, A64_W11, R_F, Z80_FLAG_C);
        if (is_sub) {
            emit_sub_w32(e, A64_W13, A64_W9, A64_W1);
            emit_sub_w32(e, A64_W13, A64_W13, A64_W11);
        } else {
            emit_add_w32(e, A64_W13, A64_W9, A64_W1);
            emit_add_w32(e, A64_W13, A64_W13, A64_W11);
        }
    } else if (is_sub) {
        emit_sub_w32(e, A64_W13, A64_W9, A64_W1);
    } else {
        emit_add_w32(e, A64_W13, A64_W9, A64_W1);
    }
    (void)emit_and_w32_imm(e, A64_W14, A64_W13, 0xFF);     /* W14 = res8 */
    if (op != ALU_CP)
        emit_mov_w32_w32(e, R_A, A64_W14);

    /* F skeleton: S|Z|XY from the result byte. */
    emit_add_w32_imm(e, A64_W12, A64_W14, FT_SZXY);
    emit_ldrb_reg_uxtw(e, R_F, R_AUX, A64_W12);
    if (op == ALU_CP) {
        /* CP quirk: XY comes from the OPERAND, not the result. */
        emit_movz_w32(e, A64_W12, 0xFF & ~(Z80_FLAG_5 | Z80_FLAG_3), 0);
        emit_and_w32(e, R_F, R_F, A64_W12);
        emit_movz_w32(e, A64_W12, Z80_FLAG_5 | Z80_FLAG_3, 0);
        emit_and_w32(e, A64_W12, A64_W1, A64_W12);
        emit_orr_w32(e, R_F, R_F, A64_W12);
    }

    /* H: bit 4 of (a ^ b ^ wide_res). */
    emit_eor_w32(e, A64_W12, A64_W9, A64_W1);
    emit_eor_w32(e, A64_W12, A64_W12, A64_W13);
    (void)emit_and_w32_imm(e, A64_W12, A64_W12, Z80_FLAG_H);
    emit_orr_w32(e, R_F, R_F, A64_W12);

    /* C: bit 8 of the wide result (borrow sign-extends, so mask). */
    emit_lsr_w32_imm(e, A64_W12, A64_W13, 8);
    (void)emit_and_w32_imm(e, A64_W12, A64_W12, 1);
    emit_orr_w32(e, R_F, R_F, A64_W12);

    /* V -> PV (bit 2): add: (~(a^b) & (a^res)).7 ; sub: ((a^b) & (a^res)).7 */
    emit_eor_w32(e, A64_W12, A64_W9, A64_W1);
    if (!is_sub)
        emit_mvn_w32(e, A64_W12, A64_W12);   /* high garbage ANDed away below */
    emit_eor_w32(e, A64_W15, A64_W9, A64_W14);
    emit_and_w32(e, A64_W12, A64_W12, A64_W15);
    emit_lsr_w32_imm(e, A64_W12, A64_W12, 7);
    emit_lsl_w32_imm(e, A64_W12, A64_W12, 2);
    emit_orr_w32(e, R_F, R_F, A64_W12);

    if (is_sub)
        (void)emit_orr_w32_imm(e, R_F, R_F, Z80_FLAG_N);
}

/* Map a decoded ALU op type (any of the _R / _N / _HL_ind forms) to
 * its ALU_* code. */
static int alu_op_for(int type) {
    switch (type) {
    case Z80_OP_ADD_A_R: case Z80_OP_ADD_A_N: case Z80_OP_ADD_A_HL_ind: return ALU_ADD;
    case Z80_OP_ADC_A_R: case Z80_OP_ADC_A_N: case Z80_OP_ADC_A_HL_ind: return ALU_ADC;
    case Z80_OP_SUB_A_R: case Z80_OP_SUB_A_N: case Z80_OP_SUB_A_HL_ind: return ALU_SUB;
    case Z80_OP_SBC_A_R: case Z80_OP_SBC_A_N: case Z80_OP_SBC_A_HL_ind: return ALU_SBC;
    case Z80_OP_AND_A_R: case Z80_OP_AND_A_N: case Z80_OP_AND_A_HL_ind: return ALU_AND;
    case Z80_OP_OR_A_R:  case Z80_OP_OR_A_N:  case Z80_OP_OR_A_HL_ind:  return ALU_OR;
    case Z80_OP_XOR_A_R: case Z80_OP_XOR_A_N: case Z80_OP_XOR_A_HL_ind: return ALU_XOR;
    default:                                                            return ALU_CP;
    }
}

/* Inline INC/DEC of an 8-bit value: W2 in (old value), W9 out (new
 * value). C is preserved from the pinned F; everything else comes from
 * the FT_INC / FT_DEC table row. W10 is NOT touched, so memory forms
 * can keep the effective address live across the flag update.
 * Clobbers W12, W13. */
static void emit_incdec8_inline(emit_t *e, int is_inc) {
    if (is_inc) emit_add_w32_imm(e, A64_W9, A64_W2, 1);
    else        emit_sub_w32_imm(e, A64_W9, A64_W2, 1);
    (void)emit_and_w32_imm(e, A64_W9, A64_W9, 0xFF);
    (void)emit_and_w32_imm(e, A64_W13, R_F, Z80_FLAG_C);   /* old C first */
    emit_add_w32_imm(e, A64_W12, A64_W9, is_inc ? FT_INC : FT_DEC);
    emit_ldrb_reg_uxtw(e, R_F, R_AUX, A64_W12);
    emit_orr_w32(e, R_F, R_F, A64_W13);
}

/* Inline CB rotate/shift: value in `v` (canonical), result -> W9, the
 * shifted-out carry bit -> W10, F fully assembled into R_F (the rotate
 * flag rule is S|Z|parity|XY from the result — exactly the FT_LOGIC
 * row — plus C). `v` may be R_A. Clobbers W9, W10, W11.
 * grp: 0=RLC 1=RRC 2=RL 3=RR 4=SLA 5=SRA 6=SLL 7=SRL. */
static void emit_cb_rotshift_inline(emit_t *e, int grp, a64_reg_t v) {
    switch (grp) {
    case 0:  /* RLC: res = (v<<1)|(v>>7), C = v.7 */
        emit_lsr_w32_imm(e, A64_W10, v, 7);
        emit_lsl_w32_imm(e, A64_W9, v, 1);
        emit_orr_w32(e, A64_W9, A64_W9, A64_W10);
        (void)emit_and_w32_imm(e, A64_W9, A64_W9, 0xFF);
        break;
    case 1:  /* RRC: res = (v>>1)|(v.0<<7), C = v.0 */
        (void)emit_and_w32_imm(e, A64_W10, v, 1);
        emit_lsr_w32_imm(e, A64_W9, v, 1);
        emit_orr_w32_lsl(e, A64_W9, A64_W9, A64_W10, 7);
        break;
    case 2:  /* RL: res = (v<<1)|C_in, C = v.7 */
        (void)emit_and_w32_imm(e, A64_W11, R_F, 1);
        emit_lsr_w32_imm(e, A64_W10, v, 7);
        emit_lsl_w32_imm(e, A64_W9, v, 1);
        emit_orr_w32(e, A64_W9, A64_W9, A64_W11);
        (void)emit_and_w32_imm(e, A64_W9, A64_W9, 0xFF);
        break;
    case 3:  /* RR: res = (v>>1)|(C_in<<7), C = v.0 */
        (void)emit_and_w32_imm(e, A64_W11, R_F, 1);
        (void)emit_and_w32_imm(e, A64_W10, v, 1);
        emit_lsr_w32_imm(e, A64_W9, v, 1);
        emit_orr_w32_lsl(e, A64_W9, A64_W9, A64_W11, 7);
        break;
    case 4:  /* SLA: res = (v<<1)&0xFF, C = v.7 */
        emit_lsr_w32_imm(e, A64_W10, v, 7);
        emit_lsl_w32_imm(e, A64_W9, v, 1);
        (void)emit_and_w32_imm(e, A64_W9, A64_W9, 0xFF);
        break;
    case 5:  /* SRA: res = (v>>1)|(v&0x80), C = v.0 */
        (void)emit_and_w32_imm(e, A64_W10, v, 1);
        (void)emit_and_w32_imm(e, A64_W11, v, 0x80);
        emit_lsr_w32_imm(e, A64_W9, v, 1);
        emit_orr_w32(e, A64_W9, A64_W9, A64_W11);
        break;
    case 6:  /* SLL (undocumented): res = (v<<1)|1, C = v.7 */
        emit_lsr_w32_imm(e, A64_W10, v, 7);
        emit_lsl_w32_imm(e, A64_W9, v, 1);
        (void)emit_orr_w32_imm(e, A64_W9, A64_W9, 1);
        (void)emit_and_w32_imm(e, A64_W9, A64_W9, 0xFF);
        break;
    default: /* SRL: res = v>>1, C = v.0 */
        (void)emit_and_w32_imm(e, A64_W10, v, 1);
        emit_lsr_w32_imm(e, A64_W9, v, 1);
        break;
    }
    emit_ldrb_reg_uxtw(e, R_F, R_AUX, A64_W9);   /* FT_LOGIC row */
    emit_orr_w32(e, R_F, R_F, A64_W10);
}

/* Inline BIT n,<src>: value in `v`, XY source byte in `xy` (both
 * canonical; either may be R_A). C preserved; H=1, N=0; Z=PV=!bit;
 * S=(bit && n==7); XY from `xy`. Clobbers W9-W12. */
static void emit_cb_bit_inline(emit_t *e, int n, a64_reg_t v, a64_reg_t xy) {
    (void)emit_and_w32_imm(e, A64_W9, v, 1u << n);
    (void)emit_and_w32_imm(e, A64_W10, R_F, Z80_FLAG_C);
    emit_movz_w32(e, A64_W11, Z80_FLAG_Z | Z80_FLAG_PV | Z80_FLAG_H, 0);
    emit_movz_w32(e, A64_W12,
                  (uint16_t)((n == 7 ? Z80_FLAG_S : 0) | Z80_FLAG_H), 0);
    emit_cmp_w32_imm(e, A64_W9, 0);
    emit_csel_w32(e, A64_W11, A64_W11, A64_W12, A64_COND_EQ);
    emit_orr_w32(e, A64_W10, A64_W10, A64_W11);
    emit_movz_w32(e, A64_W12, Z80_FLAG_5 | Z80_FLAG_3, 0);
    emit_and_w32(e, A64_W12, xy, A64_W12);
    emit_orr_w32(e, R_F, A64_W10, A64_W12);
}

/* Bitfield returned by emit_op so the translator knows what the op did. */
#define OP_FALL_THROUGH 0x0
/* (block-ending control flow is handled by emit_branch_ender, not emit_op) */
#define OP_MODIFIES_F   0x2     /* helper set cpu->f and cpu->q=1 */
#define OP_SETS_F_INLINE 0x4    /* inline code wrote R_F; q=1 owed by tail */

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
 * something the translator can emit. */
static int can_translate(const z80_decoded *dec, uint16_t pc_after) {
    /* ED is a separate ISA — accepted only on the per-op cases that
     * opt in (LDIR/LDDR as host intrinsics). CB-prefix is accepted on
     * the Z80_OP_CB case below, including DD CB / FD CB. */
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
        return dec->reg1 >= 0 && dec->reg1 <= 3;

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
        return dec->reg1 >= 0 && dec->reg1 <= 3;

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

    /* 8-bit ALU A,<src>. reg=6 means (HL) / (IX+d) / (IY+d). Codes 8/9
     * require DD/FD prefix — reg8_offset_p resolves them. */
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
     *   0/1/2  → BC/DE/HL
     *   3      → AF (special)
     *   4      → IX or IY per DD/FD prefix
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

/* Emit "W1 = source operand" for an ALU A,<src> op.
 *   reg=6 -> (HL): LDRB mem[HL] into W1 (HL is pinned — no address load).
 *   otherwise    : read the register; A is copied (emit_alu_inline needs
 *                  the operand to survive A's rewrite).
 */
static void emit_alu_src_from_reg(emit_t *e, int reg, uint8_t prefix) {
    if (reg == 6) {
        emit_ldrb_reg_uxtw(e, A64_W1, R_MEM, R_HL);
        return;
    }
    a64_reg_t src = emit_read_r8(e, A64_W1, reg, prefix);
    if (src != A64_W1)
        emit_mov_w32_w32(e, A64_W1, src);
}

/* prev_q: 1/0 if the previous instruction in this block statically
 * did/didn't write F (SCF/CCF need it for the XY Q-quirk); -1 when this
 * is the first op of the block and the live cpu->q must be consulted. */
static unsigned emit_op(emit_t *e, const z80_decoded *dec, uint16_t pc_after,
                        int prev_q) {
    (void)pc_after;   /* block enders (the only users) live in emit_branch_ender */
    switch (dec->type) {
    case Z80_OP_NOP:
        return OP_FALL_THROUGH;

    case Z80_OP_LD_R_N: {
        if (dec->reg1 == 7) {
            emit_movz_w32(e, R_A, dec->imm8, 0);
        } else {
            emit_movz_w32(e, A64_W9, dec->imm8, 0);
            emit_write_r8(e, dec->reg1, dec->prefix, A64_W9);
        }
        return OP_FALL_THROUGH;
    }

    case Z80_OP_LD_R_R: {
        if (dec->reg1 == 6) {
            /* LD (HL), r : mem[HL] = reg2  (unprefixed only) */
            a64_reg_t src = emit_read_r8(e, A64_W9, dec->reg2, 0);
            emit_guest_storeb_smc(e, src, R_HL);
            return OP_FALL_THROUGH;
        }
        if (dec->reg2 == 6) {
            /* LD r, (HL) : reg1 = mem[HL]  (unprefixed only) */
            if (dec->reg1 == 7) {
                emit_ldrb_reg_uxtw(e, R_A, R_MEM, R_HL);
            } else {
                emit_ldrb_reg_uxtw(e, A64_W9, R_MEM, R_HL);
                emit_write_r8(e, dec->reg1, 0, A64_W9);
            }
            return OP_FALL_THROUGH;
        }
        if (dec->reg1 == dec->reg2) return OP_FALL_THROUGH;  /* LD A,A etc. */
        if (dec->reg1 == 7) {
            /* LD A,r — read straight into the pinned A. */
            emit_read_r8(e, R_A, dec->reg2, dec->prefix);
            return OP_FALL_THROUGH;
        }
        a64_reg_t src = emit_read_r8(e, A64_W9, dec->reg2, dec->prefix);
        emit_write_r8(e, dec->reg1, dec->prefix, src);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_LD_RR_NN: {
        int host = rr_host_p(dec->reg1, dec->prefix);
        if (host >= 0) {
            emit_load_imm16_into(e, (a64_reg_t)host, dec->imm16);
        } else {
            emit_load_imm16_into(e, A64_W9, dec->imm16);
            emit_strh_imm(e, A64_W9, R_CPU, (uint32_t)idx_reg_offset(dec->prefix));
        }
        return OP_FALL_THROUGH;
    }

    case Z80_OP_LD_HL_N: {
        /* mem[HL] = imm8 */
        emit_movz_w32(e, A64_W9, dec->imm8, 0);
        emit_guest_storeb_smc(e, A64_W9, R_HL);
        return OP_FALL_THROUGH;
    }

    /* ---- DD/FD indexed memory ops. effective addr = (IX|IY) + disp.
     * Mirrors the interpreter: memptr is NOT updated for these. */
    case Z80_OP_LD_A_HL_ind: {
        emit_idx_eff_addr(e, A64_W10, dec->prefix, (int8_t)dec->disp);
        emit_ldrb_reg_uxtw(e, R_A, R_MEM, A64_W10);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_HL_A_ind: {
        emit_idx_eff_addr(e, A64_W10, dec->prefix, (int8_t)dec->disp);
        emit_guest_storeb_smc(e, R_A, A64_W10);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_R_HL_ind: {
        /* reg2 is the destination GPR — main H/L, not IXH/IXL. */
        emit_idx_eff_addr(e, A64_W10, dec->prefix, (int8_t)dec->disp);
        if (dec->reg2 == 7) {
            emit_ldrb_reg_uxtw(e, R_A, R_MEM, A64_W10);
        } else {
            emit_ldrb_reg_uxtw(e, A64_W9, R_MEM, A64_W10);
            emit_write_r8(e, dec->reg2, 0, A64_W9);
        }
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_HL_R_ind: {
        emit_idx_eff_addr(e, A64_W10, dec->prefix, (int8_t)dec->disp);
        a64_reg_t src = emit_read_r8(e, A64_W9, dec->reg2, 0);
        emit_guest_storeb_smc(e, src, A64_W10);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_HL_N_ind: {
        emit_idx_eff_addr(e, A64_W10, dec->prefix, (int8_t)dec->disp);
        emit_movz_w32(e, A64_W9, dec->imm8, 0);
        emit_guest_storeb_smc(e, A64_W9, A64_W10);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_INC_HL_ind:
    case Z80_OP_DEC_HL_ind: {
        /* mem[eff] = INC8/DEC8(mem[eff]). eff = HL unprefixed, or
         * (IX|IY)+d under DD/FD. */
        int idx = (dec->prefix == 0xDD || dec->prefix == 0xFD);
        a64_reg_t addr;
        if (idx) {
            emit_idx_eff_addr(e, A64_W10, dec->prefix, (int8_t)dec->disp);
            addr = A64_W10;
        } else {
            addr = R_HL;
        }
        emit_ldrb_reg_uxtw(e, A64_W2, R_MEM, addr);        /* W2 = old byte */
        emit_incdec8_inline(e, dec->type == Z80_OP_INC_HL_ind);
        emit_guest_storeb_smc(e, A64_W9, addr);
        return OP_SETS_F_INLINE;
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
        emit_ldrb_reg_uxtw(e, A64_W1, R_MEM, A64_W10);
        emit_alu_inline(e, alu_op_for(dec->type));
        return OP_SETS_F_INLINE;
    }

    case Z80_OP_LD_A_BC: {
        emit_ldrb_reg_uxtw(e, R_A, R_MEM, R_BC);
        emit_set_memptr_rr_plus_one(e, R_BC);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_A_DE: {
        emit_ldrb_reg_uxtw(e, R_A, R_MEM, R_DE);
        emit_set_memptr_rr_plus_one(e, R_DE);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_BC_A: {
        emit_guest_storeb_smc(e, R_A, R_BC);
        emit_set_memptr_quirk_rr(e, R_BC);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_DE_A: {
        emit_guest_storeb_smc(e, R_A, R_DE);
        emit_set_memptr_quirk_rr(e, R_DE);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_LD_A_NN: {
        emit_load_imm16_into(e, A64_W10, dec->imm16);
        emit_ldrb_reg_uxtw(e, R_A, R_MEM, A64_W10);
        emit_set_memptr_imm(e, (uint16_t)(dec->imm16 + 1));
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_NN_A: {
        emit_load_imm16_into(e, A64_W10, dec->imm16);
        emit_guest_storeb_smc(e, R_A, A64_W10);
        emit_set_memptr_quirk_imm(e, (uint8_t)((dec->imm16 + 1) & 0xFF));
        return OP_FALL_THROUGH;
    }

    case Z80_OP_LD_HL_indNN: {
        /* dst.lo = mem[nn] ; dst.hi = mem[(nn+1) & 0xFFFF] — byte-by-byte
         * because nn+1 must wrap at 0x10000 and the host buffer is only
         * 64K. Under DD/FD prefix, target is IX or IY instead of HL. */
        uint16_t nn  = dec->imm16;
        uint16_t nn1 = (uint16_t)(nn + 1);
        int idx = (dec->prefix == 0xDD || dec->prefix == 0xFD);
        emit_load_imm16_into(e, A64_W10, nn);
        emit_ldrb_reg_uxtw(e, A64_W9, R_MEM, A64_W10);
        emit_load_imm16_into(e, A64_W10, nn1);
        emit_ldrb_reg_uxtw(e, A64_W11, R_MEM, A64_W10);
        if (idx) {
            emit_orr_w32_lsl(e, A64_W9, A64_W9, A64_W11, 8);
            emit_strh_imm(e, A64_W9, R_CPU, (uint32_t)idx_reg_offset(dec->prefix));
        } else {
            emit_orr_w32_lsl(e, R_HL, A64_W9, A64_W11, 8);
        }
        emit_strh_imm(e, A64_W10, R_CPU, OFF_MEMPTR);      /* memptr = nn1 */
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_NN_HL: {
        uint16_t nn  = dec->imm16;
        uint16_t nn1 = (uint16_t)(nn + 1);
        int idx = (dec->prefix == 0xDD || dec->prefix == 0xFD);
        if (idx) {
            /* Source is context-resident IX/IY. */
            uint32_t off = (uint32_t)idx_reg_offset(dec->prefix);
            emit_load_imm16_into(e, A64_W10, nn);
            emit_ldrb_imm(e, A64_W9, R_CPU, off);
            emit_guest_storeb_smc(e, A64_W9, A64_W10);
            emit_load_imm16_into(e, A64_W10, nn1);
            emit_ldrb_imm(e, A64_W9, R_CPU, off + 1);
            emit_guest_storeb_smc(e, A64_W9, A64_W10);
        } else {
            emit_load_imm16_into(e, A64_W10, nn);
            emit_guest_storeb_smc(e, R_HL, A64_W10);       /* STRB = low byte */
            emit_load_imm16_into(e, A64_W10, nn1);
            emit_lsr_w32_imm(e, A64_W9, R_HL, 8);
            emit_guest_storeb_smc(e, A64_W9, A64_W10);
        }
        emit_set_memptr_imm(e, nn1);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_INC_RR:
    case Z80_OP_DEC_RR: {
        int is_inc = (dec->type == Z80_OP_INC_RR);
        int host = rr_host_p(dec->reg1, dec->prefix);
        if (host >= 0) {
            if (is_inc) emit_add_mask16(e, (a64_reg_t)host, (a64_reg_t)host, 1);
            else        emit_sub_mask16(e, (a64_reg_t)host, (a64_reg_t)host, 1);
        } else {
            uint32_t off = (uint32_t)idx_reg_offset(dec->prefix);
            emit_ldrh_imm(e, A64_W9, R_CPU, off);
            if (is_inc) emit_add_w32_imm(e, A64_W9, A64_W9, 1);
            else        emit_sub_w32_imm(e, A64_W9, A64_W9, 1);
            emit_strh_imm(e, A64_W9, R_CPU, off);          /* STRH truncates */
        }
        return OP_FALL_THROUGH;
    }

    case Z80_OP_ADD_HL_RR: {
        /* ADD HL,rr (or ADD IX,rr / ADD IY,rr under DD/FD).
         *   memptr = old dst + 1
         *   dst   += src
         *   F: S/Z/PV preserved; N=0; C = carry out of bit 15;
         *      H = carry out of bit 11; XY from result high byte.
         * H uses the carry-recovery identity: bit 12 of (a ^ b ^ (a+b))
         * is the carry INTO bit 12, i.e. the half-carry. */
        int idx = (dec->prefix == 0xDD || dec->prefix == 0xFD);
        int dst_host = idx ? -1 : R_HL;
        uint32_t dst_off = idx ? (uint32_t)idx_reg_offset(dec->prefix) : 0;

        /* W9 = old dst. */
        if (dst_host >= 0) emit_mov_w32_w32(e, A64_W9, (a64_reg_t)dst_host);
        else               emit_ldrh_imm(e, A64_W9, R_CPU, dst_off);

        /* W11 = src (reg1==2 names the destination itself). */
        a64_reg_t src;
        if (dec->reg1 == 2) {
            src = A64_W9;
        } else {
            int sh = rr_host_p(dec->reg1, 0);   /* BC/DE/SP — always pinned */
            src = (a64_reg_t)sh;
        }

        emit_add_w32(e, A64_W13, A64_W9, src);             /* sum, C at bit 16 */

        emit_add_mask16(e, A64_W12, A64_W9, 1);            /* memptr = old dst + 1 */
        emit_strh_imm(e, A64_W12, R_CPU, OFF_MEMPTR);

        if (dst_host >= 0)
            (void)emit_and_w32_imm(e, (a64_reg_t)dst_host, A64_W13, 0xFFFF);
        else
            emit_strh_imm(e, A64_W13, R_CPU, dst_off);     /* STRH keeps low 16 */

        /* Flags — build into R_F (src/W9/W13 already consumed as needed). */
        emit_movz_w32(e, A64_W12, Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV, 0);
        emit_and_w32(e, R_F, R_F, A64_W12);
        emit_eor_w32(e, A64_W12, A64_W9, src);             /* H: (a^b^sum).12 */
        emit_eor_w32(e, A64_W12, A64_W12, A64_W13);
        emit_lsr_w32_imm(e, A64_W12, A64_W12, 8);          /*   -> bit 4 */
        (void)emit_and_w32_imm(e, A64_W12, A64_W12, Z80_FLAG_H);
        emit_orr_w32(e, R_F, R_F, A64_W12);
        emit_lsr_w32_imm(e, A64_W12, A64_W13, 16);         /* C: sum bit 16 */
        emit_orr_w32(e, R_F, R_F, A64_W12);
        emit_lsr_w32_imm(e, A64_W12, A64_W13, 8);          /* XY from result.hi */
        emit_movz_w32(e, A64_W14, Z80_FLAG_5 | Z80_FLAG_3, 0);
        emit_and_w32(e, A64_W12, A64_W12, A64_W14);
        emit_orr_w32(e, R_F, R_F, A64_W12);
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
        /* Carry-out lands in W10; new A built in place. */
        switch (dec->type) {
        case Z80_OP_RLCA:                      /* A = A<<1 | A.7 ; C = A.7 */
            emit_lsr_w32_imm(e, A64_W10, R_A, 7);
            emit_lsl_w32_imm(e, A64_W9, R_A, 1);
            emit_orr_w32(e, A64_W9, A64_W9, A64_W10);
            (void)emit_and_w32_imm(e, R_A, A64_W9, 0xFF);
            break;
        case Z80_OP_RRCA:                      /* A = A>>1 | A.0<<7 ; C = A.0 */
            (void)emit_and_w32_imm(e, A64_W10, R_A, 1);
            emit_lsr_w32_imm(e, A64_W9, R_A, 1);
            emit_orr_w32_lsl(e, R_A, A64_W9, A64_W10, 7);
            break;
        case Z80_OP_RLA:                       /* A = A<<1 | C_in ; C = A.7 */
            (void)emit_and_w32_imm(e, A64_W13, R_F, 1);
            emit_lsr_w32_imm(e, A64_W10, R_A, 7);
            emit_lsl_w32_imm(e, A64_W9, R_A, 1);
            emit_orr_w32(e, A64_W9, A64_W9, A64_W13);
            (void)emit_and_w32_imm(e, R_A, A64_W9, 0xFF);
            break;
        default:                               /* RRA: A = A>>1 | C_in<<7 ; C = A.0 */
            (void)emit_and_w32_imm(e, A64_W13, R_F, 1);
            (void)emit_and_w32_imm(e, A64_W10, R_A, 1);
            emit_lsr_w32_imm(e, A64_W9, R_A, 1);
            emit_orr_w32_lsl(e, R_A, A64_W9, A64_W13, 7);
            break;
        }
        emit_movz_w32(e, A64_W12, Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV, 0);
        emit_and_w32(e, R_F, R_F, A64_W12);
        emit_orr_w32(e, R_F, R_F, A64_W10);                /* carry out */
        emit_movz_w32(e, A64_W12, Z80_FLAG_5 | Z80_FLAG_3, 0);
        emit_and_w32(e, A64_W12, R_A, A64_W12);
        emit_orr_w32(e, R_F, R_F, A64_W12);                /* XY from A' */
        return OP_FALL_THROUGH;
    }

    case Z80_OP_DAA:
        /* Helper reads cpu->a/f and writes both — sync around the call. */
        emit_strb_imm(e, R_A, R_CPU, OFF_A);
        emit_strb_imm(e, R_F, R_CPU, OFF_F);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_daa);
        emit_ldrb_imm(e, R_A, R_CPU, OFF_A);
        emit_ldrb_imm(e, R_F, R_CPU, OFF_F);
        return OP_MODIFIES_F;

    case Z80_OP_CPL: {
        /* A = ~A. F: H and N set, XY from new A, S/Z/PV/C preserved. */
        emit_mvn_w32(e, A64_W9, R_A);
        (void)emit_and_w32_imm(e, R_A, A64_W9, 0xFF);
        emit_movz_w32(e, A64_W12, 0xFF & ~(Z80_FLAG_5 | Z80_FLAG_3), 0);
        emit_and_w32(e, R_F, R_F, A64_W12);
        emit_movz_w32(e, A64_W12, Z80_FLAG_H | Z80_FLAG_N, 0);
        emit_orr_w32(e, R_F, R_F, A64_W12);
        emit_movz_w32(e, A64_W12, Z80_FLAG_5 | Z80_FLAG_3, 0);
        emit_and_w32(e, A64_W12, R_A, A64_W12);
        emit_orr_w32(e, R_F, R_F, A64_W12);
        return OP_SETS_F_INLINE;
    }

    /* SCF / CCF share the Q quirk: XY sources from A when the PREVIOUS
     * instruction modified F (prev_q), else from A|F. Mid-block the
     * translator knows prev_q statically; first-in-block it's the live
     * cpu->q value, so we select at runtime. Leaves xy_src in W13. */
    case Z80_OP_SCF:
    case Z80_OP_CCF: {
        if (prev_q > 0) {
            emit_mov_w32_w32(e, A64_W13, R_A);
        } else if (prev_q == 0) {
            emit_orr_w32(e, A64_W13, R_A, R_F);
        } else {
            emit_ldrb_imm(e, A64_W12, R_CPU, OFF_Q);
            emit_orr_w32(e, A64_W13, R_A, R_F);
            emit_cmp_w32_imm(e, A64_W12, 0);
            emit_csel_w32(e, A64_W13, R_A, A64_W13, A64_COND_NE);
        }
        if (dec->type == Z80_OP_SCF) {
            /* F = (F & (S|Z|PV)) | C | XY(xy_src) */
            emit_movz_w32(e, A64_W12, Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV, 0);
            emit_and_w32(e, R_F, R_F, A64_W12);
            (void)emit_orr_w32_imm(e, R_F, R_F, Z80_FLAG_C);
        } else {
            /* CCF: F = (F & (S|Z|PV)) | (old_c ? H : C) | XY(xy_src) */
            (void)emit_and_w32_imm(e, A64_W12, R_F, Z80_FLAG_C);  /* old_c */
            (void)emit_eor_w32_imm(e, A64_W14, A64_W12, 1);       /* new C */
            emit_lsl_w32_imm(e, A64_W12, A64_W12, 4);             /* old_c -> H */
            emit_movz_w32(e, A64_W10, Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV, 0);
            emit_and_w32(e, R_F, R_F, A64_W10);
            emit_orr_w32(e, R_F, R_F, A64_W14);
            emit_orr_w32(e, R_F, R_F, A64_W12);
        }
        emit_movz_w32(e, A64_W12, Z80_FLAG_5 | Z80_FLAG_3, 0);
        emit_and_w32(e, A64_W12, A64_W13, A64_W12);
        emit_orr_w32(e, R_F, R_F, A64_W12);
        return OP_SETS_F_INLINE;
    }

    case Z80_OP_EX_SP_HL: {
        /* Exchange (SP) with HL (or IX/IY under DD/FD); memptr = new
         * value. Stack writes skip the SMC helper — same justification
         * as PUSH: the guest stack essentially never overlaps code. */
        int idx = (dec->prefix == 0xDD || dec->prefix == 0xFD);

        emit_ldrb_reg_uxtw(e, A64_W9, R_MEM, R_SP);        /* lo = mem[sp] */
        emit_add_mask16(e, A64_W12, R_SP, 1);
        emit_ldrb_reg_uxtw(e, A64_W10, R_MEM, A64_W12);    /* hi = mem[sp+1] */

        if (idx) {
            uint32_t off = (uint32_t)idx_reg_offset(dec->prefix);
            emit_ldrh_imm(e, A64_W13, R_CPU, off);
            emit_strb_reg_uxtw(e, A64_W13, R_MEM, R_SP);
            emit_lsr_w32_imm(e, A64_W14, A64_W13, 8);
            emit_strb_reg_uxtw(e, A64_W14, R_MEM, A64_W12);
            emit_orr_w32_lsl(e, A64_W9, A64_W9, A64_W10, 8);
            emit_strh_imm(e, A64_W9, R_CPU, off);
            emit_strh_imm(e, A64_W9, R_CPU, OFF_MEMPTR);
        } else {
            emit_strb_reg_uxtw(e, R_HL, R_MEM, R_SP);      /* mem[sp] = L */
            emit_lsr_w32_imm(e, A64_W14, R_HL, 8);
            emit_strb_reg_uxtw(e, A64_W14, R_MEM, A64_W12);/* mem[sp+1] = H */
            emit_orr_w32_lsl(e, R_HL, A64_W9, A64_W10, 8); /* HL = hi:lo */
            emit_strh_imm(e, R_HL, R_CPU, OFF_MEMPTR);
        }
        return OP_FALL_THROUGH;
    }

    case Z80_OP_EX_DE_HL: {
        emit_mov_w32_w32(e, A64_W9, R_DE);
        emit_mov_w32_w32(e, R_DE, R_HL);
        emit_mov_w32_w32(e, R_HL, A64_W9);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_LD_SP_HL: {
        int idx = (dec->prefix == 0xDD || dec->prefix == 0xFD);
        if (idx)
            emit_ldrh_imm(e, R_SP, R_CPU, (uint32_t)idx_reg_offset(dec->prefix));
        else
            emit_mov_w32_w32(e, R_SP, R_HL);
        return OP_FALL_THROUGH;
    }

    /* ---- 8-bit ALU, emitted inline. Operand -> W1 first. */
    case Z80_OP_ADD_A_R: case Z80_OP_ADC_A_R: case Z80_OP_SUB_A_R:
    case Z80_OP_SBC_A_R: case Z80_OP_AND_A_R: case Z80_OP_OR_A_R:
    case Z80_OP_XOR_A_R: case Z80_OP_CP_A_R: {
        emit_alu_src_from_reg(e, dec->reg1, dec->prefix);
        emit_alu_inline(e, alu_op_for(dec->type));
        return OP_SETS_F_INLINE;
    }
    case Z80_OP_ADD_A_N: case Z80_OP_ADC_A_N: case Z80_OP_SUB_A_N:
    case Z80_OP_SBC_A_N: case Z80_OP_AND_A_N: case Z80_OP_OR_A_N:
    case Z80_OP_XOR_A_N: case Z80_OP_CP_A_N: {
        emit_movz_w32(e, A64_W1, dec->imm8, 0);
        emit_alu_inline(e, alu_op_for(dec->type));
        return OP_SETS_F_INLINE;
    }

    case Z80_OP_INC_R:
    case Z80_OP_DEC_R: {
        int is_inc = (dec->type == Z80_OP_INC_R);
        if (dec->reg1 == 6) {
            /* INC/DEC (HL), unprefixed. */
            emit_ldrb_reg_uxtw(e, A64_W2, R_MEM, R_HL);
            emit_incdec8_inline(e, is_inc);
            emit_guest_storeb_smc(e, A64_W9, R_HL);
            return OP_SETS_F_INLINE;
        }
        a64_reg_t old = emit_read_r8(e, A64_W2, dec->reg1, dec->prefix);
        if (old != A64_W2) emit_mov_w32_w32(e, A64_W2, old);
        emit_incdec8_inline(e, is_inc);
        emit_write_r8(e, dec->reg1, dec->prefix, A64_W9);
        return OP_SETS_F_INLINE;
    }

    /* ---- CB-prefix: rotate/shift (sub<0x40), BIT (0x40..0x7F),
     *      RES (0x80..0xBF), SET (0xC0..0xFF). All inline now.
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

        /* Operand value -> `val`; memory forms keep the address in W14
         * (clear of the W9-W11 the rotate/BIT bodies use). */
        a64_reg_t val, addr = A64_WZR;
        if (indexed) {
            emit_idx_eff_addr(e, A64_W14, dec->prefix, (int8_t)dec->disp);
            emit_ldrb_reg_uxtw(e, A64_W2, R_MEM, A64_W14);
            val = A64_W2; addr = A64_W14;
        } else if (r == 6) {
            emit_ldrb_reg_uxtw(e, A64_W2, R_MEM, R_HL);
            val = A64_W2; addr = R_HL;
        } else {
            val = emit_read_r8(e, A64_W2, r, 0);
        }

        if (family == 0) {
            /* Rotate / shift, inline: result W9 + F in R_F. */
            emit_cb_rotshift_inline(e, grp, val);
            if (is_mem) {
                if (dual_wb) emit_write_r8(e, r, 0, A64_W9);
                emit_guest_storeb_smc(e, A64_W9, addr);
            } else {
                emit_write_r8(e, r, 0, A64_W9);
            }
            return OP_SETS_F_INLINE;
        }

        if (family == 1) {
            /* BIT n,<src>. No write-back. XY source per form. */
            a64_reg_t xy;
            if (indexed) {
                emit_lsr_w32_imm(e, A64_W13, addr, 8);     /* addr.hi */
                xy = A64_W13;
            } else if (r == 6) {
                emit_ldrb_imm(e, A64_W13, R_CPU, OFF_MEMPTR + 1);
                xy = A64_W13;
            } else {
                xy = val;
            }
            emit_cb_bit_inline(e, grp, val, xy);
            return OP_SETS_F_INLINE;
        }

        /* RES / SET: single-bit masks — both the clear form (~mask has
         * one zero bit) and the set form encode as logical immediates.
         * No flag change. */
        uint32_t mask = 1u << grp;
        if (!is_mem) {
            /* Pure register form: flip the bit in place — one insn for
             * pinned pairs (mask shifted to the half's position). */
            int shift, pair = r8_host_pair(r, &shift);
            if (r == 7) {
                if (family == 2) (void)emit_and_w32_imm(e, R_A, R_A, ~mask);
                else             (void)emit_orr_w32_imm(e, R_A, R_A, mask);
            } else if (pair >= 0) {
                uint32_t m = mask << shift;
                if (family == 2) (void)emit_and_w32_imm(e, (a64_reg_t)pair, (a64_reg_t)pair, ~m);
                else             (void)emit_orr_w32_imm(e, (a64_reg_t)pair, (a64_reg_t)pair, m);
            } else {
                /* IX/IY half (context byte) — val is already in W2. */
                if (family == 2) (void)emit_and_w32_imm(e, A64_W2, A64_W2, ~mask);
                else             (void)emit_orr_w32_imm(e, A64_W2, A64_W2, mask);
                emit_write_r8(e, r, dec->prefix, A64_W2);
            }
            return OP_FALL_THROUGH;
        }
        if (family == 2) (void)emit_and_w32_imm(e, A64_W2, A64_W2, ~mask);
        else             (void)emit_orr_w32_imm(e, A64_W2, A64_W2, mask);
        if (dual_wb) emit_write_r8(e, r, 0, A64_W2);
        emit_guest_storeb_smc(e, A64_W2, addr);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_LDIR:
    case Z80_OP_LDDR: {
        /* Helper does the entire block copy, updates HL/DE/BC/F, and
         * performs the SMC sweep. It reads BC/DE/HL/A/F from the context
         * and writes BC/DE/HL/F (+q) back — sync both directions. */
        void *helper = (dec->type == Z80_OP_LDIR)
                           ? (void *)(uintptr_t)z80_jit_ldir
                           : (void *)(uintptr_t)z80_jit_lddr;
        emit_strh_imm(e, R_BC, R_CPU, OFF_BC);
        emit_strh_imm(e, R_DE, R_CPU, OFF_DE);
        emit_strh_imm(e, R_HL, R_CPU, OFF_HL);
        emit_strb_imm(e, R_A,  R_CPU, OFF_A);
        emit_strb_imm(e, R_F,  R_CPU, OFF_F);
        emit_call_helper(e, helper);
        emit_ldrh_imm(e, R_BC, R_CPU, OFF_BC);
        emit_ldrh_imm(e, R_DE, R_CPU, OFF_DE);
        emit_ldrh_imm(e, R_HL, R_CPU, OFF_HL);
        emit_ldrb_imm(e, R_F,  R_CPU, OFF_F);
        return OP_MODIFIES_F;
    }

    /* ---- PUSH rr / POP rr.
     * SP-relative stack accesses don't go through the SMC store helper:
     * the guest stack and the code segment essentially never overlap on
     * CP/M, and the interp-side SMC tracker (z80_mem_w) still catches
     * any pathological case via interp fallback for an immediate retry. */
    case Z80_OP_PUSH_RR: {
        a64_reg_t val;
        if (dec->reg1 <= 2) {
            val = (a64_reg_t)rr_host_p(dec->reg1, 0);
        } else if (dec->reg1 == 3) {
            emit_orr_w32_lsl(e, A64_W9, R_F, R_A, 8);      /* AF = A:F */
            val = A64_W9;
        } else {
            emit_ldrh_imm(e, A64_W9, R_CPU, (uint32_t)idx_reg_offset(dec->prefix));
            val = A64_W9;
        }
        emit_sub_mask16(e, R_SP, R_SP, 2);
        emit_strb_reg_uxtw(e, val, R_MEM, R_SP);           /* mem[sp] = lo */
        emit_add_mask16(e, A64_W12, R_SP, 1);
        emit_lsr_w32_imm(e, A64_W10, val, 8);
        emit_strb_reg_uxtw(e, A64_W10, R_MEM, A64_W12);    /* mem[sp+1] = hi */
        return OP_FALL_THROUGH;
    }

    case Z80_OP_POP_RR: {
        emit_ldrb_reg_uxtw(e, A64_W9, R_MEM, R_SP);        /* lo */
        emit_add_mask16(e, A64_W12, R_SP, 1);
        emit_ldrb_reg_uxtw(e, A64_W10, R_MEM, A64_W12);    /* hi */
        if (dec->reg1 <= 2) {
            a64_reg_t host = (a64_reg_t)rr_host_p(dec->reg1, 0);
            emit_orr_w32_lsl(e, host, A64_W9, A64_W10, 8);
        } else if (dec->reg1 == 3) {
            emit_mov_w32_w32(e, R_F, A64_W9);              /* F = mem[sp] */
            emit_mov_w32_w32(e, R_A, A64_W10);             /* A = mem[sp+1] */
        } else {
            emit_orr_w32_lsl(e, A64_W9, A64_W9, A64_W10, 8);
            emit_strh_imm(e, A64_W9, R_CPU, (uint32_t)idx_reg_offset(dec->prefix));
        }
        emit_add_mask16(e, R_SP, R_SP, 2);
        return OP_FALL_THROUGH;
    }

    default:
        /* Block enders live in emit_branch_ender; can_translate filters
         * everything else. Unreachable. */
        return OP_FALL_THROUGH;
    }
}

/* Is this op a block ender (handled by emit_branch_ender, not emit_op)? */
static int is_block_ender(int type) {
    switch (type) {
    case Z80_OP_JP_NN:   case Z80_OP_JP_HL:     case Z80_OP_JR_E:
    case Z80_OP_CALL_NN: case Z80_OP_RET:       case Z80_OP_JP_CC_NN:
    case Z80_OP_JR_CC_E: case Z80_OP_DJNZ:      case Z80_OP_CALL_CC_NN:
    case Z80_OP_RET_CC:
        return 1;
    default:
        return 0;
    }
}

/* Emit a block-ending control-flow op, including the block tail(s).
 *
 * The caller has already emitted the tail prologue (q store + insn-count
 * bump) — legal because branch ops never write F (q ends 0) and the
 * prologue doesn't touch NZCV, so the TST/CMP emitted here still governs
 * the split. Each statically-known successor gets its own direct-link
 * edge (emit_edge); run-time targets (RET, JP (HL), RET cc taken) use
 * the dynamic probe with W0. Conditionals emit the not-taken edge first
 * (straight-line likely path). */
static void emit_branch_ender(z80_dbt_t *dbt, emit_t *e,
                              const z80_decoded *dec, uint16_t pc_after) {
    switch (dec->type) {
    case Z80_OP_JP_NN: {
        /* memptr = nn (the jump target). */
        emit_movz_w32(e, A64_W9, dec->imm16, 0);
        emit_strh_imm(e, A64_W9, R_CPU, OFF_MEMPTR);
        emit_edge(dbt, e, dec->imm16);
        return;
    }

    case Z80_OP_JP_HL: {
        /* pc = HL. memptr unchanged (matches interp). */
        emit_mov_w32_w32(e, A64_W0, R_HL);
        emit_dynamic_tail(e, dbt->exit_stub_off);
        return;
    }

    case Z80_OP_JR_E: {
        /* JR e: target = pc_after + disp. memptr = target (always). */
        uint16_t target = (uint16_t)(pc_after + (int16_t)dec->disp);
        emit_movz_w32(e, A64_W9, target, 0);
        emit_strh_imm(e, A64_W9, R_CPU, OFF_MEMPTR);
        emit_edge(dbt, e, target);
        return;
    }

    case Z80_OP_CALL_NN: {
        /* CALL nn: push pc_after, memptr = target.
         * Trap targets (BDOS/WBOOT/BIOS) were filtered by can_translate. */
        emit_sub_mask16(e, R_SP, R_SP, 2);
        emit_movz_w32(e, A64_W9, pc_after & 0xFF, 0);
        emit_strb_reg_uxtw(e, A64_W9, R_MEM, R_SP);
        emit_add_mask16(e, A64_W12, R_SP, 1);
        emit_movz_w32(e, A64_W9, (pc_after >> 8) & 0xFF, 0);
        emit_strb_reg_uxtw(e, A64_W9, R_MEM, A64_W12);
        emit_movz_w32(e, A64_W9, dec->imm16, 0);
        emit_strh_imm(e, A64_W9, R_CPU, OFF_MEMPTR);
        emit_edge(dbt, e, dec->imm16);
        return;
    }

    case Z80_OP_RET: {
        /* RET: pop pc into W0, sp += 2, memptr = popped pc.
         * Popped pc == 0 is the classic CP/M warm-boot termination;
         * dbt_run's top-of-loop pc==0 && insn_count>4 check catches it. */
        emit_ldrb_reg_uxtw(e, A64_W9, R_MEM, R_SP);        /* lo */
        emit_add_mask16(e, A64_W12, R_SP, 1);
        emit_ldrb_reg_uxtw(e, A64_W10, R_MEM, A64_W12);    /* hi */
        emit_orr_w32_lsl(e, A64_W0, A64_W9, A64_W10, 8);
        emit_strh_imm(e, A64_W0, R_CPU, OFF_MEMPTR);
        emit_add_mask16(e, R_SP, R_SP, 2);
        emit_dynamic_tail(e, dbt->exit_stub_off);
        return;
    }

    case Z80_OP_JP_CC_NN: {
        /* memptr = nn (always — interp sets it regardless). */
        uint16_t target = dec->imm16;
        emit_movz_w32(e, A64_W9, target, 0);
        emit_strh_imm(e, A64_W9, R_CPU, OFF_MEMPTR);

        emit_test_z80_flag(e, flag_mask_for_cc(dec->cc));
        uint32_t patch_taken = emit_pos(e);
        emit_b_cond(e, host_cond_for_cc(dec->cc), 0);
        emit_edge(dbt, e, pc_after);                       /* not taken */
        emit_patch_cond19(e, patch_taken, emit_pos(e));
        emit_edge(dbt, e, target);                         /* taken */
        return;
    }

    case Z80_OP_JR_CC_E: {
        /* memptr = target only on the taken path. */
        uint16_t target = (uint16_t)(pc_after + (int16_t)dec->disp);
        emit_test_z80_flag(e, flag_mask_for_cc(dec->cc));
        uint32_t patch_taken = emit_pos(e);
        emit_b_cond(e, host_cond_for_cc(dec->cc), 0);
        emit_edge(dbt, e, pc_after);                       /* not taken */
        emit_patch_cond19(e, patch_taken, emit_pos(e));
        emit_movz_w32(e, A64_W9, target, 0);
        emit_strh_imm(e, A64_W9, R_CPU, OFF_MEMPTR);
        emit_edge(dbt, e, target);                         /* taken */
        return;
    }

    case Z80_OP_DJNZ: {
        /* B = (B - 1) & 0xFF; taken when B != 0; memptr only on taken. */
        uint16_t target = (uint16_t)(pc_after + (int16_t)dec->disp);

        emit_ubfx_w32(e, A64_W9, R_BC, 8, 8);
        emit_sub_w32_imm(e, A64_W9, A64_W9, 1);
        (void)emit_and_w32_imm(e, A64_W9, A64_W9, 0xFF);
        emit_bfi_w32(e, R_BC, A64_W9, 8, 8);

        emit_cmp_w32_imm(e, A64_W9, 0);
        uint32_t patch_taken = emit_pos(e);
        emit_b_cond(e, A64_COND_NE, 0);
        emit_edge(dbt, e, pc_after);                       /* B == 0: exit loop */
        emit_patch_cond19(e, patch_taken, emit_pos(e));
        emit_movz_w32(e, A64_W9, target, 0);
        emit_strh_imm(e, A64_W9, R_CPU, OFF_MEMPTR);
        emit_edge(dbt, e, target);                         /* loop back */
        return;
    }

    case Z80_OP_CALL_CC_NN: {
        /* memptr = nn (always); push only on the taken path. */
        uint16_t target = dec->imm16;
        emit_movz_w32(e, A64_W9, target, 0);
        emit_strh_imm(e, A64_W9, R_CPU, OFF_MEMPTR);

        emit_test_z80_flag(e, flag_mask_for_cc(dec->cc));
        uint32_t patch_taken = emit_pos(e);
        emit_b_cond(e, host_cond_for_cc(dec->cc), 0);
        emit_edge(dbt, e, pc_after);                       /* not taken */

        emit_patch_cond19(e, patch_taken, emit_pos(e));
        emit_sub_mask16(e, R_SP, R_SP, 2);
        emit_movz_w32(e, A64_W9, pc_after & 0xFF, 0);
        emit_strb_reg_uxtw(e, A64_W9, R_MEM, R_SP);
        emit_add_mask16(e, A64_W12, R_SP, 1);
        emit_movz_w32(e, A64_W9, (pc_after >> 8) & 0xFF, 0);
        emit_strb_reg_uxtw(e, A64_W9, R_MEM, A64_W12);
        emit_edge(dbt, e, target);                         /* taken */
        return;
    }

    case Z80_OP_RET_CC: {
        /* Taken path pops a run-time pc → dynamic tail; not-taken is a
         * static edge to pc_after. */
        emit_test_z80_flag(e, flag_mask_for_cc(dec->cc));
        uint32_t patch_taken = emit_pos(e);
        emit_b_cond(e, host_cond_for_cc(dec->cc), 0);
        emit_edge(dbt, e, pc_after);                       /* not taken */

        emit_patch_cond19(e, patch_taken, emit_pos(e));
        emit_ldrb_reg_uxtw(e, A64_W9, R_MEM, R_SP);
        emit_add_mask16(e, A64_W12, R_SP, 1);
        emit_ldrb_reg_uxtw(e, A64_W10, R_MEM, A64_W12);
        emit_orr_w32_lsl(e, A64_W0, A64_W9, A64_W10, 8);
        emit_strh_imm(e, A64_W0, R_CPU, OFF_MEMPTR);
        emit_add_mask16(e, R_SP, R_SP, 2);
        emit_dynamic_tail(e, dbt->exit_stub_off);
        return;
    }

    default:
        /* is_block_ender filters this; unreachable. */
        return;
    }
}

uint8_t *dbt_translate_block(z80_dbt_t *dbt, uint16_t guest_pc) {
    if (dbt->code_used + 16384 > CODE_BUF_SIZE) {
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

        if (is_block_ender(dec.type)) {
            /* Enders never write F, so the block-final q is 0 regardless
             * of q_mode so far. Prologue first (q + insn count, once for
             * all paths), then the branch guts + per-successor edges. */
            insns++;
            pc = pc_after;
            emit_tail_prologue(&e, insns, Q_CLEAR);
            emit_branch_ender(dbt, &e, &dec, pc_after);
            ended_by_branch = 1;
            break;
        }

        unsigned r = emit_op(&e, &dec, pc_after, prev_q);
        if      (r & OP_MODIFIES_F)    q_mode = Q_KEEP;
        else if (r & OP_SETS_F_INLINE) q_mode = Q_SET;
        else                           q_mode = Q_CLEAR;
        prev_q = (q_mode != Q_CLEAR);
        insns++;
        pc = pc_after;
    }

    if (insns == 0) return NULL;

    /* Fall-through end (block cap or untranslatable next op): the next
     * pc is the static straight-line successor — a linkable edge. */
    if (!ended_by_branch) {
        emit_tail_prologue(&e, insns, q_mode);
        emit_edge(dbt, &e, pc);
    }

    dbt->code_used = e.offset;
    __builtin___clear_cache((char *)entry, (char *)(dbt->code_buf + e.offset));

    /* Tag every guest byte this block covered so a JIT-emitted store
     * that lands on any of them triggers SMC invalidation. */
    dbt_mark_block_bytes(dbt, guest_pc, pc);
    return entry;
}
