/* dbt_a64.c — AArch64 backend for the Z80 DBT.
 *
 * First cut. Translates a small whitelist of "boring" instructions
 * (NOP, LD r,n, LD r,r', LD rr,nn, JP nn, JR e). Anything else ends the
 * block; the run loop in dbt_common.c falls back to the interpreter for
 * that single instruction and retries the JIT at the new PC.
 *
 * Host register convention inside a translated block:
 *   X19 = z80_cpu_t *cpu        (callee-saved by trampoline)
 *   X20 = cpu->mem (uint8_t *)  (unused in first cut — no memory ops yet)
 *   X21 = block cache base      (unused in first cut — no chaining yet)
 *   X9  = scratch
 *
 * Block ABI: block updates cpu->pc and cpu->insn_count, then RETs back
 * into the trampoline. (See dbt_emit_trampoline below.)
 */
#include "dbt.h"
#include "../core/z80.h"
#include "../cpm/cpm.h"
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

    /* Frame: 48 bytes (must be 16-byte aligned).
     *   [SP+ 0] FP, LR
     *   [SP+16] X19, X20
     *   [SP+32] X21, X22       (X22 unused but STP wants a pair) */
    emit_stp_pre_sp (&e, A64_W29, A64_W30, -48);
    emit_stp_x64_off(&e, A64_W19, A64_W20, A64_SP, 16);
    emit_stp_x64_off(&e, A64_W21, A64_W22, A64_SP, 32);

    /* Bind host register convention. */
    emit_mov_x64_x64(&e, A64_W19, A64_W0);   /* X19 = cpu  */
    emit_mov_x64_x64(&e, A64_W20, A64_W1);   /* X20 = mem  */
    emit_mov_x64_x64(&e, A64_W21, A64_W3);   /* X21 = cache base */

    /* BLR into the block (pointer in X2). The block's RET returns here. */
    emit_blr(&e, A64_W2);

    /* Unwind. */
    emit_ldp_x64_off(&e, A64_W21, A64_W22, A64_SP, 32);
    emit_ldp_x64_off(&e, A64_W19, A64_W20, A64_SP, 16);
    emit_ldp_post_sp(&e, A64_W29, A64_W30, 48);
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
#define OFF_INSN_COUNT offsetof(z80_cpu_t, insn_count)

/* Map a Z80 reg code 0..7 (B,C,D,E,H,L,(HL),A) to its byte offset
 * within z80_cpu_t. (HL) and the half-index regs (IXH/IXL/IYH/IYL,
 * codes 8/9) are not handled here — they need memory or prefix logic
 * the first cut deliberately skips. Returns -1 on unsupported. */
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

/* Emit "W9 = imm16" using MOVZ (one instruction, since imm16 fits). */
static void emit_load_imm16(emit_t *e, uint16_t imm) {
    emit_movz_w32(e, A64_W9, imm, 0);
}

/* Emit the common block epilogue: write final PC + bump insn_count by n,
 * then RET back into the trampoline. */
static void emit_block_epilogue(emit_t *e, uint16_t final_pc, uint32_t insn_count_delta) {
    /* cpu->pc = final_pc */
    emit_movz_w32(e, A64_W9, final_pc, 0);
    emit_strh_imm(e, A64_W9, A64_W19, OFF_PC);

    /* cpu->insn_count += insn_count_delta */
    emit_ldr_x64_imm(e, A64_W9, A64_W19, OFF_INSN_COUNT);
    /* ADD X9, X9, #imm12  — delta fits in 12 bits as long as MAX_BLOCK_INSNS does. */
    emit_add_x64_imm(e, A64_W9, A64_W9, insn_count_delta);
    emit_str_x64_imm(e, A64_W9, A64_W19, OFF_INSN_COUNT);

    emit_ret(e);
}

/* A "trap target" is any PC the interpreter knows how to dispatch as a
 * host service (BDOS at 0x0005, BIOS vector range at CPM_BIOS_BASE..+0x80).
 * Translated control flow into one of these would skip the dispatch and
 * leave the JIT executing uninitialised memory, so we refuse to translate
 * any branch whose target lands here and let the interp handle it. */
static int is_trap_target(uint16_t pc) {
    return pc == CPM_BDOS_ENTRY
        || (pc >= CPM_BIOS_BASE && pc < CPM_BIOS_BASE + 0x80);
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
        return reg8_offset(dec->reg1) >= 0 && reg8_offset(dec->reg2) >= 0;
    case Z80_OP_LD_RR_NN:
        return rr_offset(dec->reg1) >= 0;
    case Z80_OP_JP_NN:
        return !is_trap_target(dec->imm16);
    case Z80_OP_JR_E: {
        uint16_t target = (uint16_t)(pc_after + (int16_t)dec->disp);
        return !is_trap_target(target);
    }
    default:
        return 0;
    }
}

/* Emit code for one (already-known-translatable) Z80 op. Returns 1 if the
 * op ends the block (control flow), 0 if execution flows through. */
static int emit_op(emit_t *e, const z80_decoded *dec, uint16_t pc_after) {
    switch (dec->type) {
    case Z80_OP_NOP:
        return 0;

    case Z80_OP_LD_R_N: {
        int off = reg8_offset(dec->reg1);
        emit_movz_w32(e, A64_W9, dec->imm8, 0);
        emit_strb_imm(e, A64_W9, A64_W19, (uint32_t)off);
        return 0;
    }

    case Z80_OP_LD_R_R: {
        int off_dst = reg8_offset(dec->reg1);
        int off_src = reg8_offset(dec->reg2);
        if (off_dst == off_src) return 0;   /* LD A,A and friends — no-op */
        emit_ldrb_imm(e, A64_W9, A64_W19, (uint32_t)off_src);
        emit_strb_imm(e, A64_W9, A64_W19, (uint32_t)off_dst);
        return 0;
    }

    case Z80_OP_LD_RR_NN: {
        int off = rr_offset(dec->reg1);
        emit_load_imm16(e, dec->imm16);
        emit_strh_imm(e, A64_W9, A64_W19, (uint32_t)off);
        return 0;
    }

    case Z80_OP_JP_NN: {
        /* Block ends; epilogue uses dec->imm16 as the final PC. */
        (void)pc_after;
        emit_movz_w32(e, A64_W9, dec->imm16, 0);
        emit_strh_imm(e, A64_W9, A64_W19, OFF_PC);
        return 1;
    }

    case Z80_OP_JR_E: {
        /* JR e: target = pc_after + (int8_t)disp.
         * The decoder stores the signed displacement in dec->disp and
         * has already added the instruction size to pc_after for us. */
        uint16_t target = (uint16_t)(pc_after + (int16_t)dec->disp);
        emit_movz_w32(e, A64_W9, target, 0);
        emit_strh_imm(e, A64_W9, A64_W19, OFF_PC);
        return 1;
    }

    default:
        /* can_translate() filters this; unreachable. */
        return 1;
    }
}

uint8_t *dbt_translate_block(z80_dbt_t *dbt, uint16_t guest_pc) {
    if (dbt->code_used + 4096 > CODE_BUF_SIZE) {
        /* Out of JIT space — blow away the cache and reset the cursor.
         * Cheap-and-cheerful; chained blocks would need patch-back here. */
        dbt_cache_invalidate_all(dbt);
        dbt->code_used = 0;
        dbt_jit_writable_begin();
        dbt_emit_trampoline(dbt);
        dbt_jit_writable_end();
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

        ended_by_branch = emit_op(&e, &dec, pc_after);
        insns++;
        pc = pc_after;

        if (ended_by_branch) break;
    }

    if (insns == 0) return NULL;

    /* If we ended on a branch, the op already wrote cpu->pc. Otherwise
     * the next PC is the straight-through fall-through value in `pc`. */
    if (ended_by_branch) {
        /* Only bump insn_count + RET; PC is already in place. */
        emit_ldr_x64_imm(&e, A64_W9, A64_W19, OFF_INSN_COUNT);
        emit_add_x64_imm(&e, A64_W9, A64_W9, insns);
        emit_str_x64_imm(&e, A64_W9, A64_W19, OFF_INSN_COUNT);
        emit_ret(&e);
    } else {
        emit_block_epilogue(&e, pc, insns);
    }

    dbt->code_used = e.offset;
    __builtin___clear_cache((char *)entry, (char *)(dbt->code_buf + e.offset));
    return entry;
}
