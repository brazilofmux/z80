/* dbt_x64.c — x86-64 backend for the Z80 DBT.
 *
 * A structural mirror of dbt_a64.c: same two-phase translator (decode
 * the whole block, then emit), same backward flag/memptr liveness, same
 * superblocks with out-of-line side exits, same direct-link edges and
 * SMC bitmap. Only the code emission differs — and x86-64 gives us two
 * things AArch64 doesn't: memory operands, and a FLAGS register that is
 * a lineal descendant of the 8080's. After an 8-bit ADD/SUB, LAHF yields
 *   AH = SF ZF 0 AF 0 PF 1 CF
 * which is S Z - H - P/V N C in exactly the Z80 bit positions (with AF
 * being the half carry and bit 1 conveniently set, i.e. N for SUB), so
 * the add/sub family builds F from native flags. Logic ops, INC/DEC and
 * the rotates keep using the result-indexed tables (parity, and the
 * INC/DEC H/PV/N rules are all baked into a row there).
 *
 * Host register convention inside translated code (pinned across blocks
 * AND across block chains):
 *   RBX = z80_cpu_t *cpu
 *   R12 = cpu->mem (uint8_t *)   guest byte access: [r12 + reg]
 *   R13 = JIT aux base           (&dbt->jit_ftables; flag tables at +0,
 *                                 code bitmap at +0x10000, block cache
 *                                 at +0x20000 — all one disp32 away)
 *   R14 = guest HL               (canonical: zero-extended 16-bit)
 *   R15 = guest BC               (canonical)
 *   RBP = guest DE               (canonical)
 *   R8  = guest SP               (canonical)
 *   RCX = guest A                (canonical: zero-extended 8-bit)
 *   RDX = guest F                (canonical)
 *   R9  = pending insn count     (flushed into cpu->insn_count on exit)
 *   RAX, RSI, RDI, R10, R11 = scratch. EAX = next guest PC at block
 *   tails (consumed by the chain probe / exit stub). RAX is also the
 *   LAHF target, so it never holds anything across a flag-building op.
 *
 * Canonical form is maintained cheaply: every write to a pinned guest
 * register is either a 32-bit op on a canonical input, a MOVZX, or an
 * 8/16-bit partial write (which leaves the zero upper bits alone). That
 * lets any pinned 16-bit register be used directly as a 64-bit index.
 *
 * A/F live in RCX/RDX (rather than in callee-saved registers) because
 * they're the hottest byte operands and RCX/RDX are the two registers
 * the LAHF dance can reach without a REX prefix. RCX/RDX/R8/R9 are
 * caller-saved, so the rare helper calls push/pop them; the callee-
 * saved set (RBX/RBP/R12-R15) is saved once by the trampoline.
 *
 * Block ABI: the trampoline saves the callee-saved registers, binds the
 * convention, loads the pinned guest state from the context and JMPs
 * into the block. Blocks chain to each other with the pinned state live.
 * On a chain miss the tail jumps to a shared exit stub that spills the
 * pinned state (including EAX -> cpu->pc), flushes R9 into
 * cpu->insn_count, restores the callee-saved registers and RETs to the
 * trampoline's caller. Blocks run with RSP 16-byte aligned so helper
 * CALLs need no extra adjustment beyond their own pushes.
 *
 * Helper-call sync contract: helpers read/write guest state through
 * cpu->*, so call sites spill the fields a helper READS and reload the
 * fields it WRITES (DAA: A/F; LDIR/LDDR: BC/DE/HL/A/F in, BC/DE/HL/F
 * out; post_store: nothing). The pinned caller-saved registers are
 * pushed/popped around the call by emit_call_helper itself.
 */
#include "dbt.h"
#include "../core/z80.h"
#include "../cpm/cpm.h"
#include "dbt_flags.h"
#include "emit_x64.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int dbt_jit_available(void) { return 1; }

/* See the note atop dbt_a64.c: the cap barely binds; kept as the
 * validated SMC-window hedge. */
#define SUPERBLOCK_BYTE_CAP 48

/* Pinned-register aliases (see convention above). */
#define R_CPU  X64_RBX
#define R_MEM  X64_R12
#define R_AUX  X64_R13
#define R_HL   X64_R14
#define R_BC   X64_R15
#define R_DE   X64_RBP
#define R_SP   X64_R8
#define R_A    X64_RCX
#define R_F    X64_RDX
#define R_CNT  X64_R9

/* Scratch aliases. T0 (RAX) doubles as the LAHF/SETcc target and as
 * "next pc" at block tails; T1 (RSI) is the ALU operand / value
 * register and the helper's second argument; T2 (RDI) usually holds an
 * effective address. */
#define T0     X64_RAX
#define T1     X64_RSI
#define T2     X64_RDI
#define T3     X64_R10
#define T4     X64_R11

/* JIT aux block segments (dbt.h layout). */
#define AUX_BITMAP_OFF 0x10000
#define AUX_CACHE_OFF  0x20000

/* z80_cpu_t offsets — referenced from emitted code. */
#define OFF_F          (int32_t)offsetof(z80_cpu_t, f)
#define OFF_A          (int32_t)offsetof(z80_cpu_t, a)
#define OFF_C          (int32_t)offsetof(z80_cpu_t, c)
#define OFF_B          (int32_t)offsetof(z80_cpu_t, b)
#define OFF_E          (int32_t)offsetof(z80_cpu_t, e)
#define OFF_D          (int32_t)offsetof(z80_cpu_t, d)
#define OFF_L          (int32_t)offsetof(z80_cpu_t, l)
#define OFF_H          (int32_t)offsetof(z80_cpu_t, h)
#define OFF_BC         (int32_t)offsetof(z80_cpu_t, bc)
#define OFF_DE         (int32_t)offsetof(z80_cpu_t, de)
#define OFF_HL         (int32_t)offsetof(z80_cpu_t, hl)
#define OFF_IX         (int32_t)offsetof(z80_cpu_t, ix)
#define OFF_IY         (int32_t)offsetof(z80_cpu_t, iy)
#define OFF_IXL        (OFF_IX + 0)
#define OFF_IXH        (OFF_IX + 1)
#define OFF_IYL        (OFF_IY + 0)
#define OFF_IYH        (OFF_IY + 1)
#define OFF_SP         (int32_t)offsetof(z80_cpu_t, sp)
#define OFF_PC         (int32_t)offsetof(z80_cpu_t, pc)
#define OFF_Q          (int32_t)offsetof(z80_cpu_t, q)
#define OFF_MEMPTR     (int32_t)offsetof(z80_cpu_t, memptr)
#define OFF_INSN_COUNT (int32_t)offsetof(z80_cpu_t, insn_count)
#define OFF_SP_BASE    (int32_t)offsetof(z80_cpu_t, jit_sp_base)
#define OFF_CALL_BUDGET (int32_t)offsetof(z80_cpu_t, jit_call_budget)

/* ----------------------------------------------------------------------
 * Hardware-paired return-address stack (RAS).
 *
 * Guest CALL pushes its guest return pc on the HOST stack and does a
 * native CALL into the target block; guest RET pops the guest pc from
 * guest memory, compares it with the slot on top of the host stack and,
 * on a match, native-RETs — riding the hardware return predictor
 * instead of the indirect probe. The host frame is {host_ret, guest_pc}
 * (16 bytes, so alignment is preserved). Anything that breaks the
 * pairing — a mismatch, a probe miss, a JIT exit, or the CALL budget
 * running out — resets RSP to the base recorded by the trampoline,
 * which sits above a sentinel frame whose guest_pc (-1) can never match.
 *
 * The budget is a counter in the context rather than a compare against
 * a limit because reading RSP explicitly costs a stack-engine sync uop
 * on Intel; measured, `cmp rsp,[limit]` per CALL turned a 3% win into a
 * 6% loss. One forced unwind per RAS_CALL_BUDGET calls is noise.
 *
 * Stale landing code is harmless: after a native RET the landing code
 * only edges to the fall-through pc, and edges are (un)linked through
 * the same registry as everything else; the code buffer itself is only
 * ever reset from C with the host stack fully unwound.
 *
 * Z80_NO_RAS=1 disables it (plain JMP edges for CALL, probe for RET)
 * so the two can be A/B measured. AArch64 measured a loss with this
 * scheme; see the note in dbt_a64.c.
 * ---------------------------------------------------------------------- */
static int s_use_ras = -1;

/* Z80_VERIFY_STRICT=1 under -V: every block returns to dbt_run — no
 * direct links, no inline cache probe, no RAS pairing — so the lockstep
 * comparison runs after every block instead of after every chained run.
 * Slow, and the way to localise a divergence to one block. Decided at
 * the first translation (dbt->verify is set after dbt_init). */
static int s_strict_exit = -1;

/* Guest CALLs between forced unwinds: bounds host-stack growth from
 * CALLs that never RET (16 bytes each) to RAS_CALL_BUDGET * 16 bytes. */
#define RAS_CALL_BUDGET (32u * 1024u)

/* ----------------------------------------------------------------------
 * Trampoline + exit stub.
 *
 * Called from C as:
 *   void trampoline(z80_cpu_t *cpu, uint8_t *mem, void *block, void *aux);
 * with the System V mapping cpu=RDI, mem=RSI, block=RDX, aux=RCX.
 * ---------------------------------------------------------------------- */
void dbt_emit_trampoline(z80_dbt_t *dbt) {
    emit_t e = { .buf = dbt->code_buf, .offset = 0, .capacity = CODE_BUF_SIZE };

    /* Frame: 6 callee-saved pushes (48 bytes) + 8 bytes of padding so
     * RSP is 16-byte aligned inside blocks (entry RSP is 8 mod 16). */
    emit_push_r64(&e, X64_RBX);
    emit_push_r64(&e, X64_RBP);
    emit_push_r64(&e, X64_R12);
    emit_push_r64(&e, X64_R13);
    emit_push_r64(&e, X64_R14);
    emit_push_r64(&e, X64_R15);
    emit_alu_r64_imm(&e, X64_ALU_SUB, X64_RSP, 8);

    if (s_use_ras < 0) s_use_ras = !getenv("Z80_NO_RAS");

    /* Bind host register convention. The block pointer is parked in RAX
     * because RDX is about to become F. */
    emit_mov_r64_r64(&e, T0, X64_RDX);
    emit_mov_r64_r64(&e, R_CPU, X64_RDI);
    emit_mov_r64_r64(&e, R_MEM, X64_RSI);
    emit_mov_r64_r64(&e, R_AUX, X64_RCX);
    emit_alu_r32_r32(&e, X64_ALU_XOR, R_CNT, R_CNT);

    if (s_use_ras) {
        /* Sentinel RAS frame {host_ret=0, guest_pc=-1}, then record the
         * base (RSP with no guest frames) and arm the CALL budget. */
        emit_push_imm32(&e, -1);
        emit_push_imm32(&e, 0);
        emit_mov_m64_r64(&e, R_CPU, X64_NOREG, OFF_SP_BASE, X64_RSP);
        emit_mov_m32_imm32(&e, R_CPU, X64_NOREG, OFF_CALL_BUDGET, RAS_CALL_BUDGET);
    }

    /* Load the pinned guest state. */
    emit_movzx_r32_m16(&e, R_BC, R_CPU, X64_NOREG, OFF_BC);
    emit_movzx_r32_m16(&e, R_DE, R_CPU, X64_NOREG, OFF_DE);
    emit_movzx_r32_m16(&e, R_HL, R_CPU, X64_NOREG, OFF_HL);
    emit_movzx_r32_m16(&e, R_SP, R_CPU, X64_NOREG, OFF_SP);
    emit_movzx_r32_m8 (&e, R_A,  R_CPU, X64_NOREG, OFF_A);
    emit_movzx_r32_m8 (&e, R_F,  R_CPU, X64_NOREG, OFF_F);

    /* Enter the block. Blocks exit via the stub below. */
    emit_jmp_r64(&e, T0);

    /* ---- Exit stub. Entered by JMP from block tails with EAX = next pc. */
    dbt->exit_stub_off = e.offset;

    emit_mov_m16_r16(&e, R_CPU, X64_NOREG, OFF_PC, T0);
    emit_mov_m16_r16(&e, R_CPU, X64_NOREG, OFF_BC, R_BC);
    emit_mov_m16_r16(&e, R_CPU, X64_NOREG, OFF_DE, R_DE);
    emit_mov_m16_r16(&e, R_CPU, X64_NOREG, OFF_HL, R_HL);
    emit_mov_m16_r16(&e, R_CPU, X64_NOREG, OFF_SP, R_SP);
    emit_mov_m8_r8  (&e, R_CPU, X64_NOREG, OFF_A,  R_A);
    emit_mov_m8_r8  (&e, R_CPU, X64_NOREG, OFF_F,  R_F);

    /* Flush the pending insn count. */
    emit_add_m64_r64(&e, R_CPU, X64_NOREG, OFF_INSN_COUNT, R_CNT);

    /* Unwind the trampoline frame and return to its caller. With the
     * RAS, RSP may be any number of guest frames deep: reset it to the
     * base first and drop the sentinel frame. */
    if (s_use_ras) {
        emit_mov_r64_m64(&e, X64_RSP, R_CPU, X64_NOREG, OFF_SP_BASE);
        emit_alu_r64_imm(&e, X64_ALU_ADD, X64_RSP, 16);
    }
    emit_alu_r64_imm(&e, X64_ALU_ADD, X64_RSP, 8);
    emit_pop_r64(&e, X64_R15);
    emit_pop_r64(&e, X64_R14);
    emit_pop_r64(&e, X64_R13);
    emit_pop_r64(&e, X64_R12);
    emit_pop_r64(&e, X64_RBP);
    emit_pop_r64(&e, X64_RBX);
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
 * A comes back as R_A itself (no code emitted); pinned low halves MOVZX
 * into `tmp`, high halves MOV+SHR; IX/IY halves MOVZX from the context.
 * The returned value is canonical (0..255). */
static int emit_read_r8(emit_t *e, int tmp, int r, uint8_t prefix) {
    int shift;
    int pair = r8_host_pair(r, &shift);
    if (r == 7) return R_A;
    if (pair >= 0) {
        if (shift == 0) {
            emit_movzx_r32_r8(e, tmp, pair);
        } else {
            emit_mov_r32_r32(e, tmp, pair);
            emit_shr_r32_imm(e, tmp, 8);
        }
        return tmp;
    }
    emit_movzx_r32_m8(e, tmp, R_CPU, X64_NOREG, reg8_offset_p(r, prefix));
    return tmp;
}

/* Write host register `src` (must be canonical 0..255) into guest 8-bit
 * reg `r`. Low halves are a partial byte move; high halves rotate the
 * high byte down, overwrite it, rotate back (the pair stays canonical
 * throughout); IX/IY halves store to the context. */
static void emit_write_r8(emit_t *e, int r, uint8_t prefix, int src) {
    int shift;
    int pair = r8_host_pair(r, &shift);
    if (r == 7) {
        if (src != R_A) emit_mov_r32_r32(e, R_A, src);
        return;
    }
    if (pair >= 0) {
        if (shift == 0) {
            emit_mov_r8_r8(e, pair, src);
        } else {
            emit_ror_r32_imm(e, pair, 8);
            emit_mov_r8_r8(e, pair, src);
            emit_rol_r32_imm(e, pair, 8);
        }
        return;
    }
    emit_mov_m8_r8(e, R_CPU, X64_NOREG, reg8_offset_p(r, prefix), src);
}

/* Guest 8-bit reg `r` = imm8. */
static void emit_write_r8_imm(emit_t *e, int r, uint8_t prefix, uint8_t imm) {
    int shift;
    int pair = r8_host_pair(r, &shift);
    if (r == 7) {
        emit_mov_r32_imm32(e, R_A, imm);
        return;
    }
    if (pair >= 0) {
        if (shift == 0) {
            emit_mov_r8_imm8(e, pair, imm);
        } else {
            emit_alu_r32_imm(e, X64_ALU_AND, pair, 0xFF);
            if (imm) emit_alu_r32_imm(e, X64_ALU_OR, pair, (int32_t)imm << 8);
        }
        return;
    }
    emit_mov_m8_imm8(e, R_CPU, X64_NOREG, reg8_offset_p(r, prefix), imm);
}

/* Offset of the IX or IY register (full 16-bit) for the given DD/FD
 * prefix. Caller must already know prefix is DD or FD. */
static int32_t idx_reg_offset(uint8_t prefix) {
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

/* ----------------------------------------------------------------------
 * Guest stores + SMC check.
 *
 * Every JIT store is followed by one `cmp byte [aux + addr + 0x10000], 0`
 * against the code bitmap. Non-SMC stores fall through the JNE; the
 * taken side lands in an out-of-line chunk after the block that calls
 * z80_jit_post_store and jumps back. The chunks are collected per block
 * in the list below (the translator resets it) so the hot path stays
 * two instructions and the cold code stays out of the I-cache.
 *
 * Stack pushes (CALL/PUSH/EX (SP),HL) bypass the check — the guest stack
 * essentially never overlaps code.
 * ---------------------------------------------------------------------- */
#define MAX_SLOW_PATHS 192
static struct {
    uint32_t jcc_imm;    /* rel32 field of the JNE to patch */
    uint32_t join;       /* offset to jump back to */
    int      addr_reg;   /* dynamic address register, or -1 for static */
    uint32_t addr_imm;   /* static address (first byte) */
    int      nbytes;     /* 1 or 2 (static word store) */
} s_slow[MAX_SLOW_PATHS];
static int s_nslow;

/* Emit "push caller-saved pinned; rdi = cpu; call helper; pop". RAX,
 * RSI, RDI, R10, R11 are clobbered. RSI must already hold the second
 * argument when the helper takes one. */
static void emit_call_helper(emit_t *e, void *helper_addr) {
    emit_push_r64(e, R_A);
    emit_push_r64(e, R_F);
    emit_push_r64(e, R_SP);
    emit_push_r64(e, R_CNT);
    emit_mov_r64_r64(e, X64_RDI, R_CPU);
    emit_mov_r64_imm64(e, T0, (uint64_t)(uintptr_t)helper_addr);
    emit_call_r64(e, T0);
    emit_pop_r64(e, R_CNT);
    emit_pop_r64(e, R_SP);
    emit_pop_r64(e, R_F);
    emit_pop_r64(e, R_A);
}

/* The slow-path body: post_store(cpu, addr) for each written byte. */
static void emit_smc_slow_body(emit_t *e, int addr_reg, uint32_t addr_imm, int nbytes) {
    if (addr_reg >= 0) {
        if (addr_reg != T1) emit_mov_r32_r32(e, T1, addr_reg);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_post_store);
        return;
    }
    for (int i = 0; i < nbytes; i++) {
        emit_mov_r32_imm32(e, T1, (addr_imm + (uint32_t)i) & 0xFFFF);
        emit_call_helper(e, (void *)(uintptr_t)z80_jit_post_store);
    }
}

/* Record (or, if the list is full, inline) the SMC slow path after the
 * bitmap compare that the caller has just emitted. */
static void emit_smc_branch(emit_t *e, int addr_reg, uint32_t addr_imm, int nbytes) {
    if (s_nslow < MAX_SLOW_PATHS) {
        s_slow[s_nslow].jcc_imm  = emit_jcc_rel32(e, X64_CC_NE);
        s_slow[s_nslow].join     = emit_pos(e);
        s_slow[s_nslow].addr_reg = addr_reg;
        s_slow[s_nslow].addr_imm = addr_imm;
        s_slow[s_nslow].nbytes   = nbytes;
        s_nslow++;
        return;
    }
    uint32_t skip = emit_jcc_rel32(e, X64_CC_E);
    emit_smc_slow_body(e, addr_reg, addr_imm, nbytes);
    emit_patch_rel32(e, skip, emit_pos(e));
}

/* Bitmap check for a byte just stored at dynamic address `addr`. */
static void emit_smc_check_dyn(emit_t *e, int addr) {
    emit_cmp_m8_imm8(e, R_AUX, addr, AUX_BITMAP_OFF, 0);
    emit_smc_branch(e, addr, 0, 1);
}
/* Bitmap check for `nbytes` (1 or 2) just stored at static address nn.
 * A 2-byte check must not straddle 0xFFFF (caller guarantees). */
static void emit_smc_check_imm(emit_t *e, uint16_t nn, int nbytes) {
    if (nbytes == 2)
        emit_cmp_m16_imm8(e, R_AUX, X64_NOREG, AUX_BITMAP_OFF + nn, 0);
    else
        emit_cmp_m8_imm8(e, R_AUX, X64_NOREG, AUX_BITMAP_OFF + nn, 0);
    emit_smc_branch(e, -1, nn, nbytes);
}

/* mem[addr] = val (byte register, canonical) + SMC check. `val` and
 * `addr` may be pinned registers; scratch is dead after this (the slow
 * path clobbers RAX/RSI/RDI/R10/R11). */
static void emit_guest_storeb_smc(emit_t *e, int val, int addr) {
    emit_mov_m8_r8(e, R_MEM, addr, 0, val);
    emit_smc_check_dyn(e, addr);
}
/* mem[addr] = imm8 + SMC check. */
static void emit_guest_storeb_imm_smc(emit_t *e, uint8_t imm, int addr) {
    emit_mov_m8_imm8(e, R_MEM, addr, 0, imm);
    emit_smc_check_dyn(e, addr);
}

/* dst = (src + delta) & 0xFFFF into a scratch register — for the
 * "SP+1" style second-byte addresses. Z80 addresses wrap at 0xFFFF and
 * the host buffer is exactly 64KB, so the mask is load-bearing. */
static void emit_addr_plus(emit_t *e, int dst, int src, int32_t delta) {
    emit_lea_r32(e, dst, src, X64_NOREG, delta);
    emit_movzx_r32_r16(e, dst, dst);
}

/* SP += delta (16-bit partial add keeps the register canonical). */
static void emit_sp_add(emit_t *e, int8_t delta) {
    emit_alu_r16_imm8(e, X64_ALU_ADD, R_SP, delta);
}

/* cpu->memptr = imm16. */
static void emit_set_memptr_imm(emit_t *e, uint16_t value) {
    emit_mov_m16_imm16(e, R_CPU, X64_NOREG, OFF_MEMPTR, value);
}

/* cpu->memptr = (A << 8) | low8_imm — the LD (nn),A memptr-quirk form. */
static void emit_set_memptr_quirk_imm(emit_t *e, uint8_t low8_imm) {
    emit_mov_r32_r32(e, T1, R_A);
    emit_shl_r32_imm(e, T1, 8);
    if (low8_imm) emit_alu_r32_imm(e, X64_ALU_OR, T1, low8_imm);
    emit_mov_m16_r16(e, R_CPU, X64_NOREG, OFF_MEMPTR, T1);
}

/* cpu->memptr = (A << 8) | ((pair + 1) & 0xFF) — dynamic LD (BC|DE),A. */
static void emit_set_memptr_quirk_rr(emit_t *e, int pair) {
    emit_lea_r32(e, T1, pair, X64_NOREG, 1);
    emit_movzx_r32_r8(e, T1, T1);
    emit_mov_r32_r32(e, T0, R_A);
    emit_shl_r32_imm(e, T0, 8);
    emit_alu_r32_r32(e, X64_ALU_OR, T1, T0);
    emit_mov_m16_r16(e, R_CPU, X64_NOREG, OFF_MEMPTR, T1);
}

/* cpu->memptr = (pair + 1) — LD A,(BC|DE). The 16-bit store truncates,
 * matching the interp's wrap. */
static void emit_set_memptr_rr_plus_one(emit_t *e, int pair) {
    emit_lea_r32(e, T1, pair, X64_NOREG, 1);
    emit_mov_m16_r16(e, R_CPU, X64_NOREG, OFF_MEMPTR, T1);
}

/* dst = ((IX|IY) + (int8)disp) & 0xFFFF. IX/IY stay context-resident.
 * Uses only `dst`. */
static void emit_idx_eff_addr(emit_t *e, int dst, uint8_t prefix, int8_t disp) {
    emit_movzx_r32_m16(e, dst, R_CPU, X64_NOREG, idx_reg_offset(prefix));
    if (disp == 0) return;
    emit_lea_r32(e, dst, dst, X64_NOREG, disp);
    emit_movzx_r32_r16(e, dst, dst);
}

/* Map a Z80 condition code (cc, 0..7) to (flag mask, host condition).
 * cc = (flag_select << 1) | sense; sense=0 is "take if flag clear"
 * (host Z after TEST) and sense=1 is "take if flag set" (host NZ). */
static uint8_t flag_mask_for_cc(int cc) {
    static const uint8_t masks[4] = {
        Z80_FLAG_Z, Z80_FLAG_C, Z80_FLAG_PV, Z80_FLAG_S
    };
    return masks[(cc >> 1) & 3];
}
static int host_cc_for_cc(int cc) {
    return (cc & 1) ? X64_CC_NZ : X64_CC_Z;
}

/* "TEST F, mask" against the pinned F. */
static void emit_test_z80_flag(emit_t *e, uint8_t mask) {
    emit_test_r8_imm8(e, R_F, mask);
}

/* What the block tail must do to cpu->q so it matches what the interp
 * would leave after the block's LAST instruction (see dbt_a64.c). */
enum { Q_CLEAR = 0, Q_KEEP = 1, Q_SET = 2 };

/* Tail prologue: fix up cpu->q per the mode above and bump the pending
 * insn count in R9. Runs exactly once per block exit, BEFORE any edge
 * split. NB: the ADD clobbers host flags, so conditional enders must
 * emit their TEST after this — they do. */
static void emit_tail_prologue(emit_t *e, uint32_t insn_count_delta, int q_mode) {
    if (q_mode == Q_CLEAR)      emit_mov_m8_imm8(e, R_CPU, X64_NOREG, OFF_Q, 0);
    else if (q_mode == Q_SET)   emit_mov_m8_imm8(e, R_CPU, X64_NOREG, OFF_Q, 1);
    emit_alu_r64_imm(e, X64_ALU_ADD, R_CNT, (int32_t)insn_count_delta);
}

/* Dynamic tail: inline cache probe against the aux base's cache segment.
 * Used when the next PC is only known at run time (RET, JP (HL)) and as
 * the fallback behind every unlinked static edge.
 *
 * Entry contract: EAX holds the next guest PC (0..0xFFFF).
 *   esi = pc * 16
 *   cmp dword [aux + rsi + 0x20000], eax   (entry.guest_pc — refused
 *                                            sentinels carry bit 30 and
 *                                            never match)
 *   jne miss
 *   jmp qword [aux + rsi + 0x20008]        (entry.native_code)
 * miss: jmp exit_stub
 * The cache index is exactly `pc` since BLOCK_CACHE_MASK == 0xFFFF. */
static void emit_dynamic_tail(emit_t *e, uint32_t exit_stub_off) {
    if (s_strict_exit > 0) {
        emit_jmp_rel32_to(e, exit_stub_off);
        return;
    }
    emit_mov_r32_r32(e, T1, T0);
    emit_shl_r32_imm(e, T1, 4);
    emit_cmp_m32_r32(e, R_AUX, T1, AUX_CACHE_OFF, T0);
    uint32_t jne_at = emit_pos(e);
    emit_jcc_rel8(e, X64_CC_NE, 0);                 /* patched below */
    emit_jmp_m64(e, R_AUX, T1, AUX_CACHE_OFF + 8);
    e->buf[jne_at + 1] = (uint8_t)(emit_pos(e) - (jne_at + 2));
    emit_jmp_rel32_to(e, exit_stub_off);
}

/* Static edge: the direct-link unit.
 *
 *   mov  eax, pc          (the probe/exit stub need it when unlinked;
 *                          one harmless insn when linked)
 *   jmp  <target | .+0>   (the patchable link site: E9 rel32)
 *   <probe + exit>        (fallback the unlink path re-aims the JMP at)
 *
 * If the target block already exists we link immediately; either way the
 * site is registered so dbt_cache_insert / SMC invalidation can (re)aim
 * it later. If the link pool is full the site stays permanently unlinked
 * — a direct link without a record could never be unpatched after SMC. */
static void emit_edge(z80_dbt_t *dbt, emit_t *e, uint16_t pc) {
    emit_mov_r32_imm32(e, T0, pc);
    if (s_strict_exit > 0) {
        emit_dynamic_tail(e, dbt->exit_stub_off);
        return;
    }
    uint32_t site = e->offset;
    int linked = 0;
    if (dbt_link_record(dbt, pc, site)) {
        z80_block_entry_t *be = &dbt->cache[pc];   /* index == pc */
        if (be->guest_pc == (uint32_t)pc && be->native_code) {
            emit_jmp_rel32_to(e, (uint32_t)(be->native_code - e->buf));
            linked = 1;
        }
    }
    if (!linked)
        emit_jmp_rel32_to(e, site + 5);   /* fall through to the probe */
    emit_dynamic_tail(e, dbt->exit_stub_off);
}

/* Byte sizes the CALL edge layout depends on (checked at emission). */
#define DYN_TAIL_BYTES     28   /* emit_dynamic_tail */
#define EDGE_BYTES         (5 + 5 + DYN_TAIL_BYTES)
#define CALL_LANDING_BYTES (4 + EDGE_BYTES)   /* add rsp,8 ; edge */

/* CALL edge (RAS form):
 *
 *   sub  dword [cpu->jit_call_budget], 1
 *   jnz  .ok
 *   mov  rsp, [cpu->jit_sp_base]      ; budget spent: forget the pairing
 *   mov  dword [cpu->jit_call_budget], RAS_CALL_BUDGET
 * .ok:
 *   push guest_ret_pc
 *   mov  eax, target
 *   call <target block | probe_stub>  ; the patchable link site (E8)
 * landing:
 *   add  rsp, 8                       ; drop the guest_pc slot
 *   <edge(pc_after)>                  ; continue after the CALL
 * probe_stub:
 *   <dynamic tail>                    ; unlinked fallback; a hit jumps
 *                                     ; into the block, which RETs here
 *
 * The site's fallback is probe_stub, a fixed CALL_LANDING_BYTES past
 * the instruction after the CALL; dbt_arch_patch_link tells the two
 * site kinds apart by opcode byte (E8 vs E9). */
static void emit_call_edge(z80_dbt_t *dbt, emit_t *e, uint16_t target, uint16_t pc_after) {
    if (!s_use_ras || s_strict_exit > 0) {
        emit_edge(dbt, e, target);
        return;
    }
    emit_alu_m32_imm8(e, X64_ALU_SUB, R_CPU, X64_NOREG, OFF_CALL_BUDGET, 1);
    uint32_t ok = emit_pos(e);
    emit_jcc_rel8(e, X64_CC_NZ, 0);
    emit_mov_r64_m64(e, X64_RSP, R_CPU, X64_NOREG, OFF_SP_BASE);
    emit_mov_m32_imm32(e, R_CPU, X64_NOREG, OFF_CALL_BUDGET, RAS_CALL_BUDGET);
    e->buf[ok + 1] = (uint8_t)(emit_pos(e) - (ok + 2));

    emit_push_imm32(e, (int32_t)pc_after);
    emit_mov_r32_imm32(e, T0, target);
    uint32_t site = e->offset;
    uint32_t probe_stub = site + 5 + CALL_LANDING_BYTES;
    int linked = 0;
    if (dbt_link_record(dbt, target, site)) {
        z80_block_entry_t *be = &dbt->cache[target];
        if (be->guest_pc == (uint32_t)target && be->native_code) {
            emit_call_rel32_to(e, (uint32_t)(be->native_code - e->buf));
            linked = 1;
        }
    }
    if (!linked)
        emit_call_rel32_to(e, probe_stub);

    /* landing */
    emit_alu_r64_imm(e, X64_ALU_ADD, X64_RSP, 8);
    emit_edge(dbt, e, pc_after);
    if (e->offset != probe_stub) {
        fprintf(stderr, "dbt_x64: CALL edge layout drift (%u vs %u)\n",
                e->offset - (site + 5), CALL_LANDING_BYTES);
        abort();
    }
    emit_dynamic_tail(e, dbt->exit_stub_off);
}

/* RET tail: EAX holds the popped guest pc.
 *   cmp  [rsp+8], eax ; jne .miss ; ret
 * .miss: mov rsp, [cpu->jit_sp_base] ; <dynamic tail> */
static void emit_ret_tail(emit_t *e, uint32_t exit_stub_off) {
    if (s_use_ras && s_strict_exit <= 0) {
        emit_cmp_m32_r32(e, X64_RSP, X64_NOREG, 8, T0);
        emit_jcc_rel8(e, X64_CC_NE, 1);
        emit_ret(e);
        emit_mov_r64_m64(e, X64_RSP, R_CPU, X64_NOREG, OFF_SP_BASE);
    }
    emit_dynamic_tail(e, exit_stub_off);
}

/* Rewrite the patchable JMP/CALL rel32 at site_off (see emit_edge and
 * emit_call_edge). target == NULL re-aims it at its own fallback probe:
 * the next instruction for a JMP, CALL_LANDING_BYTES further for a CALL.
 * x86 keeps instruction fetch coherent with same-thread stores, so no
 * flush. */
void dbt_arch_patch_link(z80_dbt_t *dbt, uint32_t site_off, uint8_t *target) {
    uint8_t *site = dbt->code_buf + site_off;
    uint8_t *fallback = site + 5 + (site[0] == 0xE8 ? CALL_LANDING_BYTES : 0);
    uint8_t *dst  = target ? target : fallback;
    int32_t  disp = (int32_t)(dst - (site + 5));
    int32_t  cur;
    memcpy(&cur, site + 1, 4);
    if (cur == disp) return;
    memcpy(site + 1, &disp, 4);
}

/* ----------------------------------------------------------------------
 * Inline 8-bit ALU — no helper call, pinned A and F.
 *
 * Operand: imm >= 0 means an immediate; otherwise the operand is in T1
 * (RSI), canonical, and must not alias R_A (callers copy A into T1 for
 * ADD A,A — the 8-bit op reads its source before writing CL, so this is
 * only about CP's XY-from-operand and the helper contract).
 *
 * Add/sub family: the native 8-bit op on CL sets CF/OF/SF/ZF/AF/PF
 * exactly as the Z80 would set C/V/S/Z/H/(PV); LAHF drops S Z H C into
 * bits 7 6 4 0 of AH with bit 1 set (= N for the subtract family),
 * SETO supplies V. Logic ops take their whole F from the FT_LOGIC row.
 * Clobbers T0, T1 (CP with a memory operand), T3.
 * ---------------------------------------------------------------------- */
enum {
    ALU_ADD, ALU_ADC, ALU_SUB, ALU_SBC, ALU_AND, ALU_OR, ALU_XOR, ALU_CP
};

/* Operand descriptor for emit_alu_inline. */
enum { SRC_IMM, SRC_REG, SRC_MEM };
typedef struct {
    int     kind;
    int     reg;    /* SRC_REG: byte-addressable host reg holding the operand
                       canonically in its low byte (may be R_A itself) */
    int     imm;    /* SRC_IMM */
    int     base;   /* SRC_MEM: [base + index + disp] */
    int     index;
    int32_t disp;
} alu_src_t;

static alu_src_t alu_src_imm(int imm) {
    alu_src_t s = { SRC_IMM, 0, imm, 0, 0, 0 }; return s;
}
static alu_src_t alu_src_reg(int reg) {
    alu_src_t s = { SRC_REG, reg, 0, 0, 0, 0 }; return s;
}
static alu_src_t alu_src_mem(int base, int index, int32_t disp) {
    alu_src_t s = { SRC_MEM, 0, 0, base, index, disp }; return s;
}

/* `x` cl, <src> */
static void emit_alu8_src(emit_t *e, int x, const alu_src_t *src) {
    switch (src->kind) {
    case SRC_IMM: emit_alu_r8_imm8(e, x, R_A, (uint8_t)src->imm); break;
    case SRC_REG: emit_alu_r8_r8(e, x, R_A, src->reg); break;
    default:      emit_alu_r8_m8(e, x, R_A, src->base, src->index, src->disp); break;
    }
}

static void emit_alu_inline(emit_t *e, int op, uint8_t fmask, alu_src_t src) {
    if (op == ALU_AND || op == ALU_OR || op == ALU_XOR) {
        int x = (op == ALU_AND) ? X64_ALU_AND : (op == ALU_OR) ? X64_ALU_OR : X64_ALU_XOR;
        emit_alu8_src(e, x, &src);
        if (fmask) {
            emit_movzx_r32_m8(e, R_F, R_AUX, R_A, FT_LOGIC);
            if (op == ALU_AND && (fmask & Z80_FLAG_H))
                emit_alu_r32_imm(e, X64_ALU_OR, R_F, Z80_FLAG_H);
        }
        return;
    }

    int is_sub = (op == ALU_SUB || op == ALU_SBC || op == ALU_CP);

    /* CP is flag-only: fully dead means nothing to do at all. */
    if (op == ALU_CP && !fmask) return;

    /* CP's XY come from the operand; a memory operand must be loaded
     * into a register so it survives for that. */
    int want_xy = (fmask & (Z80_FLAG_5 | Z80_FLAG_3)) != 0;
    if (op == ALU_CP && want_xy && src.kind == SRC_MEM) {
        emit_movzx_r32_m8(e, T1, src.base, src.index, src.disp);
        src = alu_src_reg(T1);
    }

    /* V rides in T3 (never an operand/address register here); it must be
     * zeroed BEFORE the op since XOR clobbers the flags we want. */
    int want_v = (fmask & Z80_FLAG_PV) != 0;
    if (want_v) emit_alu_r32_r32(e, X64_ALU_XOR, T3, T3);

    int x;
    switch (op) {
    case ALU_ADD: x = X64_ALU_ADD; break;
    case ALU_ADC: x = X64_ALU_ADC; emit_bt_r32_imm8(e, R_F, 0); break;
    case ALU_SUB: x = X64_ALU_SUB; break;
    case ALU_SBC: x = X64_ALU_SBB; emit_bt_r32_imm8(e, R_F, 0); break;
    default:      x = X64_ALU_CMP; break;
    }
    emit_alu8_src(e, x, &src);

    if (!fmask) return;

    if (fmask == Z80_FLAG_C) {
        /* Only C live (the op before an INC/DEC): F = carry, dead bits 0. */
        emit_setcc_r8(e, X64_CC_C, R_F);
        return;
    }

    if (want_v) emit_setcc_r8(e, X64_CC_O, T3);

    uint8_t lahf_bits = fmask & (Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_H | Z80_FLAG_C);
    uint8_t keep = lahf_bits | (is_sub ? Z80_FLAG_N : 0);
    if (lahf_bits) {
        emit_lahf(e);
        emit_movzx_r32_ah(e, R_F);
        emit_alu_r32_imm(e, X64_ALU_AND, R_F, keep);
    } else {
        emit_mov_r32_imm32(e, R_F, (is_sub && (fmask & Z80_FLAG_N)) ? Z80_FLAG_N : 0);
    }

    if (want_v) {
        emit_shl_r32_imm(e, T3, 2);
        emit_alu_r32_r32(e, X64_ALU_OR, R_F, T3);
    }

    if (want_xy) {
        if (op == ALU_CP) {
            /* CP quirk: XY comes from the OPERAND, not the result. */
            if (src.kind == SRC_IMM) {
                if (src.imm & (Z80_FLAG_5 | Z80_FLAG_3))
                    emit_alu_r32_imm(e, X64_ALU_OR, R_F, src.imm & (Z80_FLAG_5 | Z80_FLAG_3));
                return;
            }
            emit_movzx_r32_r8(e, T0, src.reg);
        } else {
            emit_mov_r32_r32(e, T0, R_A);
        }
        emit_alu_r32_imm(e, X64_ALU_AND, T0, Z80_FLAG_5 | Z80_FLAG_3);
        emit_alu_r32_r32(e, X64_ALU_OR, R_F, T0);
    }
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

/* Inline INC/DEC of an 8-bit value held (canonical) in byte register
 * `v`, updated in place. C is preserved from the pinned F; everything
 * else comes from the FT_INC / FT_DEC row. Clobbers T0 only. */
static void emit_incdec8_inline(emit_t *e, int v, int is_inc, uint8_t fmask) {
    int rest   = (fmask & (0xFF & ~Z80_FLAG_C)) != 0;
    int keep_c = rest && (fmask & Z80_FLAG_C);
    if (keep_c) {
        emit_mov_r32_r32(e, T0, R_F);
        emit_alu_r32_imm(e, X64_ALU_AND, T0, Z80_FLAG_C);
    }
    if (is_inc) emit_inc_r8(e, v);
    else        emit_dec_r8(e, v);
    if (!rest) return;   /* only C live (or nothing): F untouched */
    emit_movzx_r32_m8(e, R_F, R_AUX, v, is_inc ? FT_INC : FT_DEC);
    if (keep_c) emit_alu_r32_r32(e, X64_ALU_OR, R_F, T0);
}

/* Inline CB rotate/shift: value in byte register `v` (canonical), result
 * in place, F fully assembled into R_F (the rotate flag rule is
 * S|Z|parity|XY from the result — exactly the FT_LOGIC row — plus C from
 * the shifted-out bit, which the native rotate leaves in CF). `v` may be
 * R_A. Clobbers T0.
 * grp: 0=RLC 1=RRC 2=RL 3=RR 4=SLA 5=SRA 6=SLL 7=SRL. */
static void emit_cb_rotshift_inline(emit_t *e, int grp, int v, uint8_t fmask) {
    int c_live = (fmask & Z80_FLAG_C) != 0;
    if (c_live) emit_alu_r32_r32(e, X64_ALU_XOR, T0, T0);
    switch (grp) {
    case 0: emit_shift_r8_imm(e, X64_SH_ROL, v, 1); break;
    case 1: emit_shift_r8_imm(e, X64_SH_ROR, v, 1); break;
    case 2: emit_bt_r32_imm8(e, R_F, 0); emit_shift_r8_imm(e, X64_SH_RCL, v, 1); break;
    case 3: emit_bt_r32_imm8(e, R_F, 0); emit_shift_r8_imm(e, X64_SH_RCR, v, 1); break;
    case 4: emit_shift_r8_imm(e, X64_SH_SHL, v, 1); break;
    case 5: emit_shift_r8_imm(e, X64_SH_SAR, v, 1); break;
    case 6:  /* SLL (undocumented): res = (v<<1)|1, C = v.7 */
        emit_shift_r8_imm(e, X64_SH_SHL, v, 1);
        if (c_live) emit_setcc_r8(e, X64_CC_C, T0);
        emit_alu_r8_imm8(e, X64_ALU_OR, v, 1);
        break;
    default: emit_shift_r8_imm(e, X64_SH_SHR, v, 1); break;
    }
    if (c_live && grp != 6) emit_setcc_r8(e, X64_CC_C, T0);
    if (!fmask) return;
    emit_movzx_r32_m8(e, R_F, R_AUX, v, FT_LOGIC);
    if (c_live) emit_alu_r32_r32(e, X64_ALU_OR, R_F, T0);
}

/* Inline BIT n,<src>: value in `v`, XY source byte in `xy` (both
 * canonical; either may be R_A). C preserved; H=1, N=0; Z=PV=!bit;
 * S=(bit && n==7); XY from `xy`. Clobbers T0, T4. */
static void emit_cb_bit_inline(emit_t *e, int n, int v, int xy) {
    emit_alu_r32_imm(e, X64_ALU_AND, R_F, Z80_FLAG_C);       /* before the TEST */
    emit_test_r8_imm8(e, v, (uint8_t)(1u << n));
    emit_mov_r32_imm32(e, T0, Z80_FLAG_Z | Z80_FLAG_PV | Z80_FLAG_H);
    emit_mov_r32_imm32(e, T4, (uint32_t)((n == 7 ? Z80_FLAG_S : 0) | Z80_FLAG_H));
    emit_cmovcc_r32_r32(e, X64_CC_NZ, T0, T4);
    emit_alu_r32_r32(e, X64_ALU_OR, R_F, T0);
    emit_mov_r32_r32(e, T0, xy);
    emit_alu_r32_imm(e, X64_ALU_AND, T0, Z80_FLAG_5 | Z80_FLAG_3);
    emit_alu_r32_r32(e, X64_ALU_OR, R_F, T0);
}

/* Bitfield returned by emit_op so the translator knows what the op did. */
#define OP_FALL_THROUGH 0x0
#define OP_MODIFIES_F   0x2     /* helper set cpu->f and cpu->q=1 */
#define OP_SETS_F_INLINE 0x4    /* inline code wrote R_F; q=1 owed by tail */

/* A "trap target" is any PC the interpreter knows how to dispatch as a
 * host service. JP NN traps on BDOS (0x0005) and the BIOS vector range;
 * CALL NN additionally traps on the warm-boot entry (0x0000). */
static int is_jp_trap_target(uint16_t pc) {
    if (!cpm_traps_enabled) return 0;      /* native CP/M: the BDOS and BIOS are code */
    return pc == CPM_BDOS_ENTRY
        || (pc >= CPM_BIOS_BASE && pc < CPM_BIOS_BASE + 0x80);
}
static int is_call_trap_target(uint16_t pc) {
    return pc == CPM_WBOOT_ENTRY
        || is_jp_trap_target(pc);
}

/* Return 1 if this op type, in this prefix/register configuration, is
 * something the translator can emit. Identical to the AArch64 policy. */
static int can_translate(const z80_decoded *dec, uint16_t pc_after) {
    int idx = (dec->prefix == 0xDD || dec->prefix == 0xFD);

    switch (dec->type) {
    case Z80_OP_NOP:
        return !idx;

    case Z80_OP_LD_R_N:
        return reg8_offset_p(dec->reg1, dec->prefix) >= 0;

    case Z80_OP_LD_R_R:
        if (dec->reg1 == 6) return !idx && reg8_offset(dec->reg2) >= 0;
        if (dec->reg2 == 6) return !idx && reg8_offset(dec->reg1) >= 0;
        return reg8_offset_p(dec->reg1, dec->prefix) >= 0
            && reg8_offset_p(dec->reg2, dec->prefix) >= 0;

    case Z80_OP_LD_RR_NN:
        return dec->reg1 <= 3;

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
        return 1;
    case Z80_OP_EX_DE_HL:
        return !idx;

    case Z80_OP_INC_RR:
    case Z80_OP_DEC_RR:
        return dec->reg1 <= 3;

    case Z80_OP_ADD_HL_RR:
        return dec->reg1 <= 3;

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
        return 1;

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

    case Z80_OP_INC_R:
    case Z80_OP_DEC_R:
        if (dec->reg1 == 6) return !idx;
        return reg8_offset_p(dec->reg1, dec->prefix) >= 0;

    case Z80_OP_LD_A_HL_ind:
    case Z80_OP_LD_HL_A_ind:
    case Z80_OP_LD_HL_N_ind:
        return idx;
    case Z80_OP_LD_R_HL_ind:
        return idx && reg8_offset(dec->reg2) >= 0;
    case Z80_OP_LD_HL_R_ind:
        return idx && reg8_offset(dec->reg2) >= 0;
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
        return !idx;

    case Z80_OP_JP_NN:
        return !is_jp_trap_target(dec->imm16);
    case Z80_OP_JR_E: {
        uint16_t target = (uint16_t)(pc_after + (int16_t)dec->disp);
        return !is_jp_trap_target(target);
    }
    case Z80_OP_JP_CC_NN:
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
        return dec->prefix == 0xCB
            || dec->prefix == 0xDD
            || dec->prefix == 0xFD;

    case Z80_OP_LDIR:
    case Z80_OP_LDDR:
        return 1;

    case Z80_OP_PUSH_RR:
    case Z80_OP_POP_RR:
        if (dec->reg1 <= 2) return 1;
        if (dec->reg1 == 3) return !idx;
        if (dec->reg1 == 4) return idx;
        return 0;
    default:
        return 0;
    }
}

/* Operand descriptor for an ALU A,<src> op with a register-coded source.
 *   reg=6 -> (HL): a memory operand (HL is pinned — no address load).
 *   A and pinned low halves are used as byte registers directly; high
 *   halves are extracted into T1; IX/IY halves are context memory. */
static alu_src_t emit_alu_src_from_reg(emit_t *e, int reg, uint8_t prefix) {
    if (reg == 6) return alu_src_mem(R_MEM, R_HL, 0);
    if (reg == 7) return alu_src_reg(R_A);
    int shift, pair = r8_host_pair(reg, &shift);
    if (pair >= 0 && shift == 0) return alu_src_reg(pair);
    if (pair >= 0) {
        emit_mov_r32_r32(e, T1, pair);
        emit_shr_r32_imm(e, T1, 8);
        return alu_src_reg(T1);
    }
    return alu_src_mem(R_CPU, X64_NOREG, reg8_offset_p(reg, prefix));
}

/* Pop a 16-bit value from the guest stack into `dst` (any 32-bit reg
 * except T1) and advance SP. With mirrored guest memory this is a single
 * 16-bit load (the mirror page makes SP=0xFFFF wrap exactly); otherwise
 * byte-wise with masked addresses. */
static void emit_pop16_into(emit_t *e, int dst) {
    if (z80_mem_mirrored) {
        emit_movzx_r32_m16(e, dst, R_MEM, R_SP, 0);
    } else {
        emit_movzx_r32_m8(e, dst, R_MEM, R_SP, 0);
        emit_addr_plus(e, T1, R_SP, 1);
        emit_movzx_r32_m8(e, T1, R_MEM, T1, 0);
        emit_shl_r32_imm(e, T1, 8);
        emit_alu_r32_r32(e, X64_ALU_OR, dst, T1);
    }
    emit_sp_add(e, 2);
}

/* Push the (canonical 16-bit) value in `val` onto the guest stack.
 * `val` must not be T1/T2. */
static void emit_push16(emit_t *e, int val) {
    emit_sp_add(e, -2);
    if (z80_mem_mirrored) {
        emit_mov_m16_r16(e, R_MEM, R_SP, 0, val);
        return;
    }
    emit_mov_m8_r8(e, R_MEM, R_SP, 0, val);            /* mem[sp] = lo */
    emit_addr_plus(e, T1, R_SP, 1);
    emit_mov_r32_r32(e, T2, val);
    emit_shr_r32_imm(e, T2, 8);
    emit_mov_m8_r8(e, R_MEM, T1, 0, T2);               /* mem[sp+1] = hi */
}

/* Push a static 16-bit value (CALL's return address). */
static void emit_push16_imm(emit_t *e, uint16_t val) {
    emit_sp_add(e, -2);
    if (z80_mem_mirrored) {
        emit_mov_m16_imm16(e, R_MEM, R_SP, 0, val);
        return;
    }
    emit_mov_m8_imm8(e, R_MEM, R_SP, 0, (uint8_t)val);
    emit_addr_plus(e, T1, R_SP, 1);
    emit_mov_m8_imm8(e, R_MEM, T1, 0, (uint8_t)(val >> 8));
}

/* prev_q: 1/0 if the previous instruction in this block statically
 * did/didn't write F (SCF/CCF need it for the XY Q-quirk); -1 when this
 * is the first op of the block and the live cpu->q must be consulted. */
static unsigned emit_op(emit_t *e, const z80_decoded *dec, uint16_t pc_after,
                        int prev_q, uint8_t fmask, int store_memptr) {
    (void)pc_after;
    switch (dec->type) {
    case Z80_OP_NOP:
        return OP_FALL_THROUGH;

    case Z80_OP_LD_R_N:
        emit_write_r8_imm(e, dec->reg1, dec->prefix, dec->imm8);
        return OP_FALL_THROUGH;

    case Z80_OP_LD_R_R: {
        if (dec->reg1 == 6) {
            /* LD (HL), r : mem[HL] = reg2  (unprefixed only) */
            int src = emit_read_r8(e, T1, dec->reg2, 0);
            emit_guest_storeb_smc(e, src, R_HL);
            return OP_FALL_THROUGH;
        }
        if (dec->reg2 == 6) {
            /* LD r, (HL) : reg1 = mem[HL]  (unprefixed only) */
            if (dec->reg1 == 7) {
                emit_movzx_r32_m8(e, R_A, R_MEM, R_HL, 0);
            } else {
                emit_movzx_r32_m8(e, T1, R_MEM, R_HL, 0);
                emit_write_r8(e, dec->reg1, 0, T1);
            }
            return OP_FALL_THROUGH;
        }
        if (dec->reg1 == dec->reg2) return OP_FALL_THROUGH;  /* LD A,A etc. */
        if (dec->reg1 == 7) {
            /* LD A,r — read straight into the pinned A. */
            emit_read_r8(e, R_A, dec->reg2, dec->prefix);
            return OP_FALL_THROUGH;
        }
        int src = emit_read_r8(e, T1, dec->reg2, dec->prefix);
        emit_write_r8(e, dec->reg1, dec->prefix, src);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_LD_RR_NN: {
        int host = rr_host_p(dec->reg1, dec->prefix);
        if (host >= 0)
            emit_mov_r32_imm32(e, host, dec->imm16);
        else
            emit_mov_m16_imm16(e, R_CPU, X64_NOREG, idx_reg_offset(dec->prefix), dec->imm16);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_LD_HL_N:
        emit_guest_storeb_imm_smc(e, dec->imm8, R_HL);
        return OP_FALL_THROUGH;

    /* ---- DD/FD indexed memory ops. effective addr = (IX|IY) + disp.
     * Mirrors the interpreter: memptr is NOT updated for these. */
    case Z80_OP_LD_A_HL_ind:
        emit_idx_eff_addr(e, T2, dec->prefix, (int8_t)dec->disp);
        emit_movzx_r32_m8(e, R_A, R_MEM, T2, 0);
        return OP_FALL_THROUGH;
    case Z80_OP_LD_HL_A_ind:
        emit_idx_eff_addr(e, T2, dec->prefix, (int8_t)dec->disp);
        emit_guest_storeb_smc(e, R_A, T2);
        return OP_FALL_THROUGH;
    case Z80_OP_LD_R_HL_ind:
        emit_idx_eff_addr(e, T2, dec->prefix, (int8_t)dec->disp);
        if (dec->reg2 == 7) {
            emit_movzx_r32_m8(e, R_A, R_MEM, T2, 0);
        } else {
            emit_movzx_r32_m8(e, T1, R_MEM, T2, 0);
            emit_write_r8(e, dec->reg2, 0, T1);
        }
        return OP_FALL_THROUGH;
    case Z80_OP_LD_HL_R_ind: {
        emit_idx_eff_addr(e, T2, dec->prefix, (int8_t)dec->disp);
        int src = emit_read_r8(e, T1, dec->reg2, 0);
        emit_guest_storeb_smc(e, src, T2);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_HL_N_ind:
        emit_idx_eff_addr(e, T2, dec->prefix, (int8_t)dec->disp);
        emit_guest_storeb_imm_smc(e, dec->imm8, T2);
        return OP_FALL_THROUGH;
    case Z80_OP_INC_HL_ind:
    case Z80_OP_DEC_HL_ind: {
        int idx = (dec->prefix == 0xDD || dec->prefix == 0xFD);
        int addr;
        if (idx) {
            emit_idx_eff_addr(e, T2, dec->prefix, (int8_t)dec->disp);
            addr = T2;
        } else {
            addr = R_HL;
        }
        emit_movzx_r32_m8(e, T1, R_MEM, addr, 0);
        emit_incdec8_inline(e, T1, dec->type == Z80_OP_INC_HL_ind, fmask);
        emit_guest_storeb_smc(e, T1, addr);
        return OP_SETS_F_INLINE;
    }
    case Z80_OP_ADD_A_HL_ind:
    case Z80_OP_ADC_A_HL_ind:
    case Z80_OP_SUB_A_HL_ind:
    case Z80_OP_SBC_A_HL_ind:
    case Z80_OP_AND_A_HL_ind:
    case Z80_OP_OR_A_HL_ind:
    case Z80_OP_XOR_A_HL_ind:
    case Z80_OP_CP_A_HL_ind:
        emit_idx_eff_addr(e, T2, dec->prefix, (int8_t)dec->disp);
        emit_alu_inline(e, alu_op_for(dec->type), fmask, alu_src_mem(R_MEM, T2, 0));
        return OP_SETS_F_INLINE;

    case Z80_OP_LD_A_BC:
        emit_movzx_r32_m8(e, R_A, R_MEM, R_BC, 0);
        if (store_memptr) emit_set_memptr_rr_plus_one(e, R_BC);
        return OP_FALL_THROUGH;
    case Z80_OP_LD_A_DE:
        emit_movzx_r32_m8(e, R_A, R_MEM, R_DE, 0);
        if (store_memptr) emit_set_memptr_rr_plus_one(e, R_DE);
        return OP_FALL_THROUGH;
    case Z80_OP_LD_BC_A:
        emit_guest_storeb_smc(e, R_A, R_BC);
        if (store_memptr) emit_set_memptr_quirk_rr(e, R_BC);
        return OP_FALL_THROUGH;
    case Z80_OP_LD_DE_A:
        emit_guest_storeb_smc(e, R_A, R_DE);
        if (store_memptr) emit_set_memptr_quirk_rr(e, R_DE);
        return OP_FALL_THROUGH;

    case Z80_OP_LD_A_NN:
        /* Static address: one load with the address as displacement. */
        emit_movzx_r32_m8(e, R_A, R_MEM, X64_NOREG, dec->imm16);
        if (store_memptr) emit_set_memptr_imm(e, (uint16_t)(dec->imm16 + 1));
        return OP_FALL_THROUGH;
    case Z80_OP_LD_NN_A:
        emit_mov_m8_r8(e, R_MEM, X64_NOREG, dec->imm16, R_A);
        emit_smc_check_imm(e, dec->imm16, 1);
        if (store_memptr)
            emit_set_memptr_quirk_imm(e, (uint8_t)((dec->imm16 + 1) & 0xFF));
        return OP_FALL_THROUGH;

    case Z80_OP_LD_HL_indNN: {
        /* dst = mem16[nn]. A single 16-bit load unless nn == 0xFFFF,
         * where the high byte wraps to address 0 and the host buffer is
         * only 64K. Under DD/FD the target is IX/IY. */
        uint16_t nn  = dec->imm16;
        uint16_t nn1 = (uint16_t)(nn + 1);
        int idx = (dec->prefix == 0xDD || dec->prefix == 0xFD);
        int dst = idx ? T1 : R_HL;
        if (nn != 0xFFFF) {
            emit_movzx_r32_m16(e, dst, R_MEM, X64_NOREG, nn);
        } else {
            emit_movzx_r32_m8(e, dst, R_MEM, X64_NOREG, nn);
            emit_movzx_r32_m8(e, T0, R_MEM, X64_NOREG, nn1);
            emit_shl_r32_imm(e, T0, 8);
            emit_alu_r32_r32(e, X64_ALU_OR, dst, T0);
        }
        if (idx)
            emit_mov_m16_r16(e, R_CPU, X64_NOREG, idx_reg_offset(dec->prefix), T1);
        if (store_memptr) emit_set_memptr_imm(e, nn1);
        return OP_FALL_THROUGH;
    }
    case Z80_OP_LD_NN_HL: {
        uint16_t nn  = dec->imm16;
        uint16_t nn1 = (uint16_t)(nn + 1);
        int idx = (dec->prefix == 0xDD || dec->prefix == 0xFD);
        int src = R_HL;
        if (idx) {
            emit_movzx_r32_m16(e, T3, R_CPU, X64_NOREG, idx_reg_offset(dec->prefix));
            src = T3;
        }
        if (nn != 0xFFFF) {
            emit_mov_m16_r16(e, R_MEM, X64_NOREG, nn, src);
            emit_smc_check_imm(e, nn, 2);
        } else {
            emit_mov_m8_r8(e, R_MEM, X64_NOREG, nn, src);
            emit_smc_check_imm(e, nn, 1);
            emit_mov_r32_r32(e, T3, src);
            emit_shr_r32_imm(e, T3, 8);
            emit_mov_m8_r8(e, R_MEM, X64_NOREG, nn1, T3);
            emit_smc_check_imm(e, nn1, 1);
        }
        if (store_memptr) emit_set_memptr_imm(e, nn1);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_INC_RR:
    case Z80_OP_DEC_RR: {
        int is_inc = (dec->type == Z80_OP_INC_RR);
        int host = rr_host_p(dec->reg1, dec->prefix);
        if (host >= 0)
            emit_alu_r16_imm8(e, is_inc ? X64_ALU_ADD : X64_ALU_SUB, host, 1);
        else
            emit_incdec_m16(e, R_CPU, X64_NOREG, idx_reg_offset(dec->prefix), is_inc);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_ADD_HL_RR: {
        /* ADD HL,rr (or ADD IX,rr / ADD IY,rr under DD/FD).
         *   memptr = old dst + 1
         *   t      = dst + src        (32-bit: carry out of bit 15 lands
         *                              at bit 16, nothing is masked)
         *   dst    = t & 0xFFFF
         *   F: S/Z/PV preserved; N=0; C|H|XY from ONE table lookup —
         *      index = ((t ^ ((old ^ src) & 0x1000)) >> 8) is 9 bits:
         *      bit 8 = C, bit 4 = H (carry-recovery identity), bits 5/3
         *      = XY of the result high byte. See FT_ADD16. */
        int idx = (dec->prefix == 0xDD || dec->prefix == 0xFD);
        int dst = idx ? T1 : R_HL;
        if (idx) emit_movzx_r32_m16(e, T1, R_CPU, X64_NOREG, idx_reg_offset(dec->prefix));

        int self = (dec->reg1 == 2);                      /* ADD HL,HL */
        int src  = self ? dst : rr_host_p(dec->reg1, 0);
        int want = fmask & (Z80_FLAG_C | Z80_FLAG_H | Z80_FLAG_5 | Z80_FLAG_3);

        emit_lea_r32(e, T3, dst, src, 0);                 /* t = old + src */
        if (store_memptr) {
            emit_lea_r32(e, T2, dst, X64_NOREG, 1);       /* memptr = old + 1 */
            emit_mov_m16_r16(e, R_CPU, X64_NOREG, OFF_MEMPTR, T2);
        }
        if (want) {
            if ((fmask & Z80_FLAG_H) && !self) {
                /* old^old == 0 for ADD HL,HL, so the fix-up vanishes. */
                emit_mov_r32_r32(e, T0, dst);
                emit_alu_r32_r32(e, X64_ALU_XOR, T0, src);
                emit_alu_r32_imm(e, X64_ALU_AND, T0, 0x1000);
                emit_alu_r32_r32(e, X64_ALU_XOR, T0, T3);
            } else {
                emit_mov_r32_r32(e, T0, T3);
            }
            emit_shr_r32_imm(e, T0, 8);
        }
        emit_movzx_r32_r16(e, dst, T3);                   /* dst = t & 0xFFFF */
        if (idx)
            emit_mov_m16_r16(e, R_CPU, X64_NOREG, idx_reg_offset(dec->prefix), T1);

        if (fmask & 0x3B) {   /* writes C|H|N|XY; S/Z/PV pass through */
            emit_alu_r32_imm(e, X64_ALU_AND, R_F, Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV);
            if (want) {
                emit_movzx_r32_m8(e, T0, R_AUX, T0, FT_ADD16);
                emit_alu_r32_r32(e, X64_ALU_OR, R_F, T0);
            }
        }
        return OP_SETS_F_INLINE;
    }

    /* ---- Accumulator rotates. All four share the flag rule:
     *   F = (F & (S|Z|PV)) | carry_out | (A' & (5|3))    (H=0, N=0)
     * The native rotate leaves the shifted-out bit in CF. NB: the interp
     * does NOT set q for these, so we return plain OP_FALL_THROUGH. */
    case Z80_OP_RLCA:
    case Z80_OP_RRCA:
    case Z80_OP_RLA:
    case Z80_OP_RRA: {
        int c_live = (fmask & Z80_FLAG_C) != 0;
        if (c_live) emit_alu_r32_r32(e, X64_ALU_XOR, T0, T0);
        switch (dec->type) {
        case Z80_OP_RLCA: emit_shift_r8_imm(e, X64_SH_ROL, R_A, 1); break;
        case Z80_OP_RRCA: emit_shift_r8_imm(e, X64_SH_ROR, R_A, 1); break;
        case Z80_OP_RLA:  emit_bt_r32_imm8(e, R_F, 0); emit_shift_r8_imm(e, X64_SH_RCL, R_A, 1); break;
        default:          emit_bt_r32_imm8(e, R_F, 0); emit_shift_r8_imm(e, X64_SH_RCR, R_A, 1); break;
        }
        if (c_live) emit_setcc_r8(e, X64_CC_C, T0);
        if (fmask & 0x3B) {   /* writes C|H|N|XY; S/Z/PV pass through */
            emit_alu_r32_imm(e, X64_ALU_AND, R_F, Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV);
            if (c_live)
                emit_alu_r32_r32(e, X64_ALU_OR, R_F, T0);
            if (fmask & (Z80_FLAG_5 | Z80_FLAG_3)) {
                emit_mov_r32_r32(e, T0, R_A);
                emit_alu_r32_imm(e, X64_ALU_AND, T0, Z80_FLAG_5 | Z80_FLAG_3);
                emit_alu_r32_r32(e, X64_ALU_OR, R_F, T0);
            }
        }
        return OP_FALL_THROUGH;
    }

    case Z80_OP_DAA:
        /* Table lookup: entry = (A', F') at index ((F & (C|N|H)) << 8) | A.
         * COBOL's decimal runtime leans on DAA hard enough that the
         * helper call was measurable. */
        emit_mov_r32_r32(e, T0, R_F);
        emit_alu_r32_imm(e, X64_ALU_AND, T0, Z80_FLAG_C | Z80_FLAG_N | Z80_FLAG_H);
        emit_shl_r32_imm(e, T0, 8);
        emit_alu_r32_r32(e, X64_ALU_OR, T0, R_A);
        emit_movzx_r32_m16_sib(e, T0, R_AUX, T0, 1, FT_DAA);
        emit_movzx_r32_r8(e, R_A, T0);
        if (fmask) emit_movzx_r32_ah(e, R_F);
        return OP_SETS_F_INLINE;

    case Z80_OP_CPL:
        /* A = ~A. F: H and N set, XY from new A, S/Z/PV/C preserved. */
        emit_alu_r8_imm8(e, X64_ALU_XOR, R_A, 0xFF);
        if (fmask & 0x3A) {   /* writes H|N|XY; S/Z/PV/C pass through */
            emit_alu_r32_imm(e, X64_ALU_AND, R_F, 0xFF & ~(Z80_FLAG_5 | Z80_FLAG_3));
            emit_alu_r32_imm(e, X64_ALU_OR, R_F, Z80_FLAG_H | Z80_FLAG_N);
            emit_mov_r32_r32(e, T0, R_A);
            emit_alu_r32_imm(e, X64_ALU_AND, T0, Z80_FLAG_5 | Z80_FLAG_3);
            emit_alu_r32_r32(e, X64_ALU_OR, R_F, T0);
        }
        return OP_SETS_F_INLINE;

    /* SCF / CCF share the Q quirk: XY sources from A when the PREVIOUS
     * instruction modified F (prev_q), else from A|F. Mid-block the
     * translator knows prev_q statically; first-in-block it's the live
     * cpu->q value, so we select at runtime. Leaves xy_src in T3. */
    case Z80_OP_SCF:
    case Z80_OP_CCF: {
        if (!(fmask & 0x3B))   /* pure flag op; C|H|N|XY all dead → no-op */
            return OP_SETS_F_INLINE;
        if (prev_q > 0) {
            emit_mov_r32_r32(e, T3, R_A);
        } else if (prev_q == 0) {
            emit_mov_r32_r32(e, T3, R_A);
            emit_alu_r32_r32(e, X64_ALU_OR, T3, R_F);
        } else {
            emit_movzx_r32_m8(e, T0, R_CPU, X64_NOREG, OFF_Q);
            emit_mov_r32_r32(e, T3, R_A);
            emit_alu_r32_r32(e, X64_ALU_OR, T3, R_F);
            emit_test_r32_r32(e, T0, T0);
            emit_cmovcc_r32_r32(e, X64_CC_NZ, T3, R_A);
        }
        if (dec->type == Z80_OP_SCF) {
            /* F = (F & (S|Z|PV)) | C | XY(xy_src) */
            emit_alu_r32_imm(e, X64_ALU_AND, R_F, Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV);
            emit_alu_r32_imm(e, X64_ALU_OR, R_F, Z80_FLAG_C);
        } else {
            /* CCF: F = (F & (S|Z|PV)) | (old_c ? H : C) | XY(xy_src) */
            emit_mov_r32_r32(e, T0, R_F);
            emit_alu_r32_imm(e, X64_ALU_AND, T0, Z80_FLAG_C);       /* old_c */
            emit_mov_r32_r32(e, T2, T0);
            emit_alu_r32_imm(e, X64_ALU_XOR, T2, 1);                /* new C */
            emit_shl_r32_imm(e, T0, 4);                             /* old_c -> H */
            emit_alu_r32_imm(e, X64_ALU_AND, R_F, Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV);
            emit_alu_r32_r32(e, X64_ALU_OR, R_F, T2);
            emit_alu_r32_r32(e, X64_ALU_OR, R_F, T0);
        }
        emit_alu_r32_imm(e, X64_ALU_AND, T3, Z80_FLAG_5 | Z80_FLAG_3);
        emit_alu_r32_r32(e, X64_ALU_OR, R_F, T3);
        return OP_SETS_F_INLINE;
    }

    case Z80_OP_EX_SP_HL: {
        /* Exchange (SP) with HL (or IX/IY under DD/FD); memptr = new
         * value. Stack writes skip the SMC helper — same justification
         * as PUSH. */
        int idx = (dec->prefix == 0xDD || dec->prefix == 0xFD);
        int32_t off = idx ? idx_reg_offset(dec->prefix) : 0;
        int cur = R_HL;
        if (idx) {
            emit_movzx_r32_m16(e, T3, R_CPU, X64_NOREG, off);
            cur = T3;
        }

        if (z80_mem_mirrored) {
            emit_movzx_r32_m16(e, T0, R_MEM, R_SP, 0);     /* T0 = mem16[sp] */
            emit_mov_m16_r16(e, R_MEM, R_SP, 0, cur);      /* mem16[sp] = cur */
        } else {
            emit_movzx_r32_m8(e, T0, R_MEM, R_SP, 0);      /* lo = mem[sp] */
            emit_addr_plus(e, T1, R_SP, 1);
            emit_movzx_r32_m8(e, T2, R_MEM, T1, 0);        /* hi = mem[sp+1] */
            emit_shl_r32_imm(e, T2, 8);
            emit_alu_r32_r32(e, X64_ALU_OR, T0, T2);       /* T0 = new value */
            emit_mov_m8_r8(e, R_MEM, R_SP, 0, cur);        /* mem[sp] = lo(cur) */
            emit_mov_r32_r32(e, T2, cur);
            emit_shr_r32_imm(e, T2, 8);
            emit_mov_m8_r8(e, R_MEM, T1, 0, T2);           /* mem[sp+1] = hi(cur) */
        }

        if (idx) {
            emit_mov_m16_r16(e, R_CPU, X64_NOREG, off, T0);
        } else {
            emit_mov_r32_r32(e, R_HL, T0);
        }
        if (store_memptr)
            emit_mov_m16_r16(e, R_CPU, X64_NOREG, OFF_MEMPTR, T0);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_EX_DE_HL:
        emit_mov_r32_r32(e, T0, R_DE);
        emit_mov_r32_r32(e, R_DE, R_HL);
        emit_mov_r32_r32(e, R_HL, T0);
        return OP_FALL_THROUGH;

    case Z80_OP_LD_SP_HL: {
        int idx = (dec->prefix == 0xDD || dec->prefix == 0xFD);
        if (idx)
            emit_movzx_r32_m16(e, R_SP, R_CPU, X64_NOREG, idx_reg_offset(dec->prefix));
        else
            emit_mov_r32_r32(e, R_SP, R_HL);
        return OP_FALL_THROUGH;
    }

    /* ---- 8-bit ALU, emitted inline. Operand -> T1 (or immediate). */
    case Z80_OP_ADD_A_R: case Z80_OP_ADC_A_R: case Z80_OP_SUB_A_R:
    case Z80_OP_SBC_A_R: case Z80_OP_AND_A_R: case Z80_OP_OR_A_R:
    case Z80_OP_XOR_A_R: case Z80_OP_CP_A_R:
        emit_alu_inline(e, alu_op_for(dec->type), fmask,
                        emit_alu_src_from_reg(e, dec->reg1, dec->prefix));
        return OP_SETS_F_INLINE;
    case Z80_OP_ADD_A_N: case Z80_OP_ADC_A_N: case Z80_OP_SUB_A_N:
    case Z80_OP_SBC_A_N: case Z80_OP_AND_A_N: case Z80_OP_OR_A_N:
    case Z80_OP_XOR_A_N: case Z80_OP_CP_A_N:
        emit_alu_inline(e, alu_op_for(dec->type), fmask, alu_src_imm(dec->imm8));
        return OP_SETS_F_INLINE;

    case Z80_OP_INC_R:
    case Z80_OP_DEC_R: {
        int is_inc = (dec->type == Z80_OP_INC_R);
        if (dec->reg1 == 6) {
            /* INC/DEC (HL), unprefixed. */
            emit_movzx_r32_m8(e, T1, R_MEM, R_HL, 0);
            emit_incdec8_inline(e, T1, is_inc, fmask);
            emit_guest_storeb_smc(e, T1, R_HL);
            return OP_SETS_F_INLINE;
        }
        if (dec->reg1 == 7) {
            emit_incdec8_inline(e, R_A, is_inc, fmask);
            return OP_SETS_F_INLINE;
        }
        int shift, pair = r8_host_pair(dec->reg1, &shift);
        if (pair >= 0 && shift == 8) {
            /* High half of a pinned pair: bump the whole pair by 0x100
             * and re-canonicalize — cheaper than extract/inc/insert. */
            int rest   = (fmask & (0xFF & ~Z80_FLAG_C)) != 0;
            int keep_c = rest && (fmask & Z80_FLAG_C);
            if (keep_c) {
                emit_mov_r32_r32(e, T0, R_F);
                emit_alu_r32_imm(e, X64_ALU_AND, T0, Z80_FLAG_C);
            }
            emit_alu_r32_imm(e, is_inc ? X64_ALU_ADD : X64_ALU_SUB, pair, 0x100);
            emit_movzx_r32_r16(e, pair, pair);
            if (rest) {
                emit_mov_r32_r32(e, T1, pair);
                emit_shr_r32_imm(e, T1, 8);
                emit_movzx_r32_m8(e, R_F, R_AUX, T1, is_inc ? FT_INC : FT_DEC);
                if (keep_c) emit_alu_r32_r32(e, X64_ALU_OR, R_F, T0);
            }
            return OP_SETS_F_INLINE;
        }
        int v = emit_read_r8(e, T1, dec->reg1, dec->prefix);
        emit_incdec8_inline(e, v, is_inc, fmask);
        emit_write_r8(e, dec->reg1, dec->prefix, v);
        return OP_SETS_F_INLINE;
    }

    /* ---- CB-prefix: rotate/shift (sub<0x40), BIT (0x40..0x7F),
     *      RES (0x80..0xBF), SET (0xC0..0xFF). See dbt_a64.c for the
     *      three prefix forms and the dual-writeback rule. */
    case Z80_OP_CB: {
        uint8_t sub = dec->imm8;
        int     r   = sub & 7;
        int     grp = (sub >> 3) & 7;
        unsigned family = sub >> 6;  /* 0=rot/shift, 1=BIT, 2=RES, 3=SET */
        int     indexed = (dec->prefix == 0xDD || dec->prefix == 0xFD);
        int     is_mem = indexed || (r == 6);
        int     dual_wb = indexed && r != 6 && family != 1;

        /* Operand value -> `val` (T1 or R_A); memory forms keep the
         * address in T2 (indexed) or use R_HL directly. */
        int val, addr = -1;
        if (indexed) {
            emit_idx_eff_addr(e, T2, dec->prefix, (int8_t)dec->disp);
            addr = T2;
        } else if (r == 6) {
            addr = R_HL;
        }

        if (family == 2 || family == 3) {
            /* RES / SET: single-bit masks, no flag change. */
            uint8_t mask = (uint8_t)(1u << grp);
            int op = (family == 2) ? X64_ALU_AND : X64_ALU_OR;
            uint8_t m8 = (family == 2) ? (uint8_t)~mask : mask;
            if (!is_mem) {
                int shift, pair = r8_host_pair(r, &shift);
                if (r == 7) {
                    emit_alu_r8_imm8(e, op, R_A, m8);
                } else if (pair >= 0 && shift == 0) {
                    emit_alu_r8_imm8(e, op, pair, m8);
                } else if (pair >= 0) {
                    if (family == 2) emit_alu_r32_imm(e, X64_ALU_AND, pair, (int32_t)~((uint32_t)mask << 8));
                    else             emit_alu_r32_imm(e, X64_ALU_OR,  pair, (int32_t)mask << 8);
                } else {
                    emit_alu_m8_imm8(e, op, R_CPU, X64_NOREG, reg8_offset_p(r, dec->prefix), m8);
                }
                return OP_FALL_THROUGH;
            }
            if (!dual_wb) {
                /* Pure memory form: read-modify-write in place. */
                emit_alu_m8_imm8(e, op, R_MEM, addr, 0, m8);
                emit_smc_check_dyn(e, addr);
                return OP_FALL_THROUGH;
            }
            emit_movzx_r32_m8(e, T1, R_MEM, addr, 0);
            emit_alu_r8_imm8(e, op, T1, m8);
            emit_write_r8(e, r, 0, T1);
            emit_guest_storeb_smc(e, T1, addr);
            return OP_FALL_THROUGH;
        }

        if (is_mem) {
            emit_movzx_r32_m8(e, T1, R_MEM, addr, 0);
            val = T1;
        } else {
            val = emit_read_r8(e, T1, r, 0);
        }

        if (family == 0) {
            /* Rotate / shift, inline: result in place + F in R_F. */
            emit_cb_rotshift_inline(e, grp, val, fmask);
            if (is_mem) {
                if (dual_wb) emit_write_r8(e, r, 0, val);
                emit_guest_storeb_smc(e, val, addr);
            } else {
                emit_write_r8(e, r, 0, val);
            }
            return OP_SETS_F_INLINE;
        }

        /* BIT n,<src>. No write-back — flag-only, so a dead result
         * means the whole op vanishes. */
        if (!(fmask & (0xFF & ~Z80_FLAG_C)))
            return OP_SETS_F_INLINE;
        int xy;
        if (indexed) {
            emit_mov_r32_r32(e, T3, addr);
            emit_shr_r32_imm(e, T3, 8);                     /* addr.hi */
            xy = T3;
        } else if (r == 6) {
            emit_movzx_r32_m8(e, T3, R_CPU, X64_NOREG, OFF_MEMPTR + 1);
            xy = T3;
        } else {
            xy = val;
        }
        emit_cb_bit_inline(e, grp, val, xy);
        return OP_SETS_F_INLINE;
    }

    case Z80_OP_LDIR:
    case Z80_OP_LDDR: {
        /* Helper does the entire block copy, updates HL/DE/BC/F, and
         * performs the SMC sweep. Sync both directions. */
        void *helper = (dec->type == Z80_OP_LDIR)
                           ? (void *)(uintptr_t)z80_jit_ldir
                           : (void *)(uintptr_t)z80_jit_lddr;
        emit_mov_m16_r16(e, R_CPU, X64_NOREG, OFF_BC, R_BC);
        emit_mov_m16_r16(e, R_CPU, X64_NOREG, OFF_DE, R_DE);
        emit_mov_m16_r16(e, R_CPU, X64_NOREG, OFF_HL, R_HL);
        emit_mov_m8_r8  (e, R_CPU, X64_NOREG, OFF_A,  R_A);
        emit_mov_m8_r8  (e, R_CPU, X64_NOREG, OFF_F,  R_F);
        emit_call_helper(e, helper);
        emit_movzx_r32_m16(e, R_BC, R_CPU, X64_NOREG, OFF_BC);
        emit_movzx_r32_m16(e, R_DE, R_CPU, X64_NOREG, OFF_DE);
        emit_movzx_r32_m16(e, R_HL, R_CPU, X64_NOREG, OFF_HL);
        emit_movzx_r32_m8 (e, R_F,  R_CPU, X64_NOREG, OFF_F);
        return OP_MODIFIES_F;
    }

    /* ---- PUSH rr / POP rr. SP-relative stack accesses don't go through
     * the SMC store helper (see dbt_a64.c). */
    case Z80_OP_PUSH_RR: {
        int val;
        if (dec->reg1 <= 2) {
            val = rr_host_p(dec->reg1, 0);
        } else if (dec->reg1 == 3) {
            emit_mov_r32_r32(e, T0, R_A);                  /* AF = A:F */
            emit_shl_r32_imm(e, T0, 8);
            emit_alu_r32_r32(e, X64_ALU_OR, T0, R_F);
            val = T0;
        } else {
            emit_movzx_r32_m16(e, T0, R_CPU, X64_NOREG, idx_reg_offset(dec->prefix));
            val = T0;
        }
        emit_push16(e, val);
        return OP_FALL_THROUGH;
    }

    case Z80_OP_POP_RR: {
        if (dec->reg1 <= 2) {
            emit_pop16_into(e, rr_host_p(dec->reg1, 0));
        } else if (dec->reg1 == 3) {
            emit_movzx_r32_m8(e, R_F, R_MEM, R_SP, 0);     /* F = mem[sp] */
            if (z80_mem_mirrored) {
                emit_movzx_r32_m8(e, R_A, R_MEM, R_SP, 1); /* A = mem[sp+1] */
            } else {
                emit_addr_plus(e, T1, R_SP, 1);
                emit_movzx_r32_m8(e, R_A, R_MEM, T1, 0);
            }
            emit_sp_add(e, 2);
        } else {
            emit_pop16_into(e, T0);
            emit_mov_m16_r16(e, R_CPU, X64_NOREG, idx_reg_offset(dec->prefix), T0);
        }
        return OP_FALL_THROUGH;
    }

    default:
        /* Block enders live in emit_branch_ender; can_translate filters
         * everything else. Unreachable. */
        return OP_FALL_THROUGH;
    }
}

/* Static F data-flow of one translatable op, for the backward liveness
 * pass (identical to dbt_a64.c — see the discussion there). */
static void op_flag_effects(const z80_decoded *dec, uint8_t *rd, uint8_t *wr) {
    const uint8_t XY = Z80_FLAG_5 | Z80_FLAG_3;
    const uint8_t SZP = Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV;
    *rd = 0; *wr = 0;
    switch (dec->type) {
    case Z80_OP_ADD_A_R: case Z80_OP_ADD_A_N: case Z80_OP_ADD_A_HL_ind:
    case Z80_OP_SUB_A_R: case Z80_OP_SUB_A_N: case Z80_OP_SUB_A_HL_ind:
    case Z80_OP_AND_A_R: case Z80_OP_AND_A_N: case Z80_OP_AND_A_HL_ind:
    case Z80_OP_OR_A_R:  case Z80_OP_OR_A_N:  case Z80_OP_OR_A_HL_ind:
    case Z80_OP_XOR_A_R: case Z80_OP_XOR_A_N: case Z80_OP_XOR_A_HL_ind:
    case Z80_OP_CP_A_R:  case Z80_OP_CP_A_N:  case Z80_OP_CP_A_HL_ind:
        *wr = 0xFF;
        break;
    case Z80_OP_ADC_A_R: case Z80_OP_ADC_A_N: case Z80_OP_ADC_A_HL_ind:
    case Z80_OP_SBC_A_R: case Z80_OP_SBC_A_N: case Z80_OP_SBC_A_HL_ind:
        *wr = 0xFF; *rd = Z80_FLAG_C;
        break;
    case Z80_OP_INC_R: case Z80_OP_DEC_R:
    case Z80_OP_INC_HL_ind: case Z80_OP_DEC_HL_ind:
        *wr = 0xFF & ~Z80_FLAG_C; *rd = Z80_FLAG_C;
        break;
    case Z80_OP_ADD_HL_RR:
        *wr = Z80_FLAG_C | Z80_FLAG_H | Z80_FLAG_N | XY; *rd = SZP;
        break;
    case Z80_OP_RLCA: case Z80_OP_RRCA:
        *wr = Z80_FLAG_C | Z80_FLAG_H | Z80_FLAG_N | XY; *rd = SZP;
        break;
    case Z80_OP_RLA: case Z80_OP_RRA:
        *wr = Z80_FLAG_C | Z80_FLAG_H | Z80_FLAG_N | XY;
        *rd = SZP | Z80_FLAG_C;
        break;
    case Z80_OP_DAA:
        *wr = 0xFF; *rd = Z80_FLAG_C | Z80_FLAG_H | Z80_FLAG_N;
        break;
    case Z80_OP_CPL:
        *wr = Z80_FLAG_H | Z80_FLAG_N | XY; *rd = SZP | Z80_FLAG_C;
        break;
    case Z80_OP_SCF: case Z80_OP_CCF:
        *wr = Z80_FLAG_C | Z80_FLAG_H | Z80_FLAG_N | XY; *rd = 0xFF;
        break;
    case Z80_OP_CB: {
        unsigned family = dec->imm8 >> 6;
        int grp = (dec->imm8 >> 3) & 7;
        if (family == 0) {
            *wr = 0xFF;
            if (grp == 2 || grp == 3) *rd = Z80_FLAG_C;
        } else if (family == 1) {
            *wr = 0xFF & ~Z80_FLAG_C; *rd = Z80_FLAG_C;
        }
        break;
    }
    case Z80_OP_LDIR: case Z80_OP_LDDR:
        *wr = Z80_FLAG_H | Z80_FLAG_PV | Z80_FLAG_N | XY; *rd = 0xFF;
        break;
    case Z80_OP_PUSH_RR:
        if (dec->reg1 == 3) *rd = 0xFF;
        break;
    case Z80_OP_POP_RR:
        if (dec->reg1 == 3) *wr = 0xFF;
        break;
    case Z80_OP_JP_CC_NN: case Z80_OP_JR_CC_E: case Z80_OP_DJNZ:
    case Z80_OP_CALL_CC_NN: case Z80_OP_RET_CC:
        *rd = 0xFF;
        break;
    default:
        break;
    }
}

/* Memptr data-flow class of one translatable op (see dbt_a64.c). */
enum { MPTR_NONE = 0, MPTR_WRITE, MPTR_WRITE_FIXED, MPTR_READ };
static int op_memptr_effect(const z80_decoded *dec) {
    switch (dec->type) {
    case Z80_OP_LD_A_BC: case Z80_OP_LD_A_DE:
    case Z80_OP_LD_BC_A: case Z80_OP_LD_DE_A:
    case Z80_OP_LD_A_NN: case Z80_OP_LD_NN_A:
    case Z80_OP_LD_HL_indNN: case Z80_OP_LD_NN_HL:
    case Z80_OP_ADD_HL_RR: case Z80_OP_EX_SP_HL:
        return MPTR_WRITE;
    case Z80_OP_JP_CC_NN: case Z80_OP_CALL_CC_NN:
        return MPTR_WRITE_FIXED;
    case Z80_OP_CB:
        if (dec->prefix == 0xCB && (dec->imm8 >> 6) == 1
            && (dec->imm8 & 7) == 6)
            return MPTR_READ;
        return MPTR_NONE;
    default:
        return MPTR_NONE;
    }
}

/* Unconditional control flow always ends a superblock. */
static int is_uncond_ender(int type) {
    switch (type) {
    case Z80_OP_JP_NN: case Z80_OP_JP_HL: case Z80_OP_JR_E:
    case Z80_OP_CALL_NN: case Z80_OP_RET:
        return 1;
    default:
        return 0;
    }
}

/* Conditional control flow becomes a mid-block SIDE EXIT. */
static int is_cond_ender(int type) {
    switch (type) {
    case Z80_OP_JP_CC_NN: case Z80_OP_JR_CC_E: case Z80_OP_DJNZ:
    case Z80_OP_CALL_CC_NN: case Z80_OP_RET_CC:
        return 1;
    default:
        return 0;
    }
}

/* Emit a conditional op's inline part — always-executed guts (memptr for
 * JP cc/CALL cc, the B decrement for DJNZ), the flag/counter test, and
 * the Jcc toward the taken arm. Returns the Jcc's rel32 offset for the
 * caller to patch at the taken chunk. */
static uint32_t emit_cond_side_branch(emit_t *e, const z80_decoded *dec) {
    switch (dec->type) {
    case Z80_OP_JP_CC_NN:
    case Z80_OP_CALL_CC_NN:
        /* memptr = nn on BOTH paths (interp sets it regardless). */
        emit_set_memptr_imm(e, dec->imm16);
        /* fall through */
    case Z80_OP_JR_CC_E:
    case Z80_OP_RET_CC:
        emit_test_z80_flag(e, flag_mask_for_cc(dec->cc));
        return emit_jcc_rel32(e, host_cc_for_cc(dec->cc));
    default:
        /* DJNZ: B = (B-1) & 0xFF, taken when B != 0. Decrement the
         * whole pair by 0x100, re-canonicalize, test the high byte. */
        emit_alu_r32_imm(e, X64_ALU_SUB, R_BC, 0x100);
        emit_movzx_r32_r16(e, R_BC, R_BC);
        emit_test_r32_imm32(e, R_BC, 0xFF00);
        return emit_jcc_rel32(e, X64_CC_NZ);
    }
}

/* Emit a conditional op's taken-arm tail: memptr quirks, CALL's push /
 * RET's pop, then the outgoing edge (static targets) or dynamic probe
 * (RET cc). The caller has already emitted the tail prologue. */
static void emit_cond_taken_tail(z80_dbt_t *dbt, emit_t *e,
                                 const z80_decoded *dec, uint16_t pc_after) {
    switch (dec->type) {
    case Z80_OP_JP_CC_NN:
        emit_edge(dbt, e, dec->imm16);     /* memptr stored inline */
        break;
    case Z80_OP_JR_CC_E:
    case Z80_OP_DJNZ: {
        uint16_t target = (uint16_t)(pc_after + (int16_t)dec->disp);
        emit_set_memptr_imm(e, target);
        emit_edge(dbt, e, target);
        break;
    }
    case Z80_OP_CALL_CC_NN:
        emit_push16_imm(e, pc_after);
        emit_call_edge(dbt, e, dec->imm16, pc_after);
        break;
    default:   /* RET cc: pop a run-time pc -> RAS-paired return / probe */
        emit_pop16_into(e, T0);
        emit_mov_m16_r16(e, R_CPU, X64_NOREG, OFF_MEMPTR, T0);
        emit_ret_tail(e, dbt->exit_stub_off);
        break;
    }
}

/* Emit a block-ending control-flow op, including the block tail(s).
 * The caller has already emitted the tail prologue. */
static void emit_branch_ender(z80_dbt_t *dbt, emit_t *e,
                              const z80_decoded *dec, uint16_t pc_after) {
    switch (dec->type) {
    case Z80_OP_JP_NN:
        emit_set_memptr_imm(e, dec->imm16);
        emit_edge(dbt, e, dec->imm16);
        return;

    case Z80_OP_JP_HL:
        /* pc = HL. memptr unchanged (matches interp). */
        emit_mov_r32_r32(e, T0, R_HL);
        emit_dynamic_tail(e, dbt->exit_stub_off);
        return;

    case Z80_OP_JR_E: {
        uint16_t target = (uint16_t)(pc_after + (int16_t)dec->disp);
        emit_set_memptr_imm(e, target);
        emit_edge(dbt, e, target);
        return;
    }

    case Z80_OP_CALL_NN:
        emit_push16_imm(e, pc_after);
        emit_set_memptr_imm(e, dec->imm16);
        emit_call_edge(dbt, e, dec->imm16, pc_after);
        return;

    case Z80_OP_RET:
        /* RET: pop pc into EAX, sp += 2, memptr = popped pc. */
        emit_pop16_into(e, T0);
        emit_mov_m16_r16(e, R_CPU, X64_NOREG, OFF_MEMPTR, T0);
        emit_ret_tail(e, dbt->exit_stub_off);
        return;

    case Z80_OP_JP_CC_NN:
    case Z80_OP_JR_CC_E:
    case Z80_OP_DJNZ:
    case Z80_OP_CALL_CC_NN:
    case Z80_OP_RET_CC: {
        /* Conditional in final position: classic two-edge ender —
         * not-taken edge first (straight-line likely path). */
        uint32_t patch_taken = emit_cond_side_branch(e, dec);
        emit_edge(dbt, e, pc_after);                       /* not taken */
        emit_patch_rel32(e, patch_taken, emit_pos(e));
        emit_cond_taken_tail(dbt, e, dec, pc_after);
        return;
    }

    default:
        return;
    }
}

uint8_t *dbt_translate_block(z80_dbt_t *dbt, uint16_t guest_pc) {
    if (s_strict_exit < 0)
        s_strict_exit = dbt->verify && getenv("Z80_VERIFY_STRICT") != NULL;
    if (dbt->code_used + 65536 > CODE_BUF_SIZE) {
        /* Out of JIT space — blow away the cache and reset the cursor. */
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
    /* Align block entries to 16 bytes (the padding is never executed).
     * Measured: without this, unrelated code-size changes shuffle every
     * later block's fetch-window alignment and swing SQUARO by +-4% —
     * enough to invert the verdict on a real optimization. With it, the
     * run-to-run spread on SQUARO drops to ~1% and zexdoc gains ~5%. */
    while (e.offset & 15) emit_int3(&e);
    uint8_t *entry = dbt->code_buf + e.offset;
    s_nslow = 0;

    /* ---- Phase 1: decode the whole block. ---- */
    z80_decoded decs[MAX_BLOCK_INSNS];
    uint16_t    pc_afters[MAX_BLOCK_INSNS];
    uint16_t pc = guest_pc;
    uint32_t n_ops = 0;

    while (n_ops < MAX_BLOCK_INSNS) {
        z80_decoded *dec = &decs[n_ops];
        int n = z80_decode_one(cpu->mem, pc, dec);
        if (n == 0) break;

        uint16_t pc_after = (uint16_t)(pc + dec->bytes);
        if (!can_translate(dec, pc_after)) {
            if (n_ops == 0) return NULL;
            break;
        }
        pc_afters[n_ops] = pc_after;
        n_ops++;
        pc = pc_after;
        if (is_uncond_ender(dec->type))
            break;
        if (is_cond_ender(dec->type) &&
            (uint16_t)(pc - guest_pc) >= SUPERBLOCK_BYTE_CAP)
            break;
    }

    if (n_ops == 0) return NULL;

    /* ---- Phase 2: backward F-bit liveness + memptr dead-store elision
     * (see dbt_a64.c for the invariants). ---- */
    uint8_t fmask[MAX_BLOCK_INSNS];
    uint8_t mstore[MAX_BLOCK_INSNS];
    {
        uint8_t live = 0xFF;
        uint8_t mlive = 1;
        for (int i = (int)n_ops - 1; i >= 0; i--) {
            uint8_t rd, wr;
            op_flag_effects(&decs[i], &rd, &wr);
            fmask[i] = live;
            live = (uint8_t)((live & (uint8_t)~wr) | rd);

            switch (op_memptr_effect(&decs[i])) {
            case MPTR_WRITE:       mstore[i] = mlive; mlive = 0; break;
            case MPTR_WRITE_FIXED: mstore[i] = 1;     mlive = 0; break;
            case MPTR_READ:        mstore[i] = 0;     mlive = 1; break;
            default:               mstore[i] = 1;                break;
            }
        }
    }

    /* ---- Phase 3: emit. ---- */
    struct { uint32_t patch_off; uint32_t insns; uint32_t op; } sides[MAX_BLOCK_INSNS];
    uint32_t n_sides = 0;
    int q_mode = Q_CLEAR;
    int prev_q = -1;
    int final_by_branch = 0;

    for (uint32_t i = 0; i < n_ops; i++) {
        const z80_decoded *dec = &decs[i];

        if (is_uncond_ender(dec->type) ||
            (is_cond_ender(dec->type) && i == n_ops - 1)) {
            emit_tail_prologue(&e, n_ops, Q_CLEAR);
            emit_branch_ender(dbt, &e, dec, pc_afters[i]);
            final_by_branch = 1;
            break;
        }

        if (is_cond_ender(dec->type)) {
            sides[n_sides].patch_off = emit_cond_side_branch(&e, dec);
            sides[n_sides].insns = i + 1;
            sides[n_sides].op = i;
            n_sides++;
            q_mode = Q_CLEAR;
            prev_q = 0;
            continue;
        }

        unsigned r = emit_op(&e, dec, pc_afters[i], prev_q, fmask[i], mstore[i]);
        if      (r & OP_MODIFIES_F)    q_mode = Q_KEEP;
        else if (r & OP_SETS_F_INLINE) q_mode = Q_SET;
        else                           q_mode = Q_CLEAR;
        prev_q = (q_mode != Q_CLEAR);
    }

    if (!final_by_branch) {
        emit_tail_prologue(&e, n_ops, q_mode);
        emit_edge(dbt, &e, pc);
    }

    /* ---- Side-exit chunks: the taken arms of mid-block conditionals. */
    for (uint32_t k = 0; k < n_sides; k++) {
        emit_patch_rel32(&e, sides[k].patch_off, emit_pos(&e));
        emit_tail_prologue(&e, sides[k].insns, Q_CLEAR);
        emit_cond_taken_tail(dbt, &e, &decs[sides[k].op],
                             pc_afters[sides[k].op]);
    }

    /* ---- SMC slow paths: call post_store and rejoin the block. */
    for (int k = 0; k < s_nslow; k++) {
        emit_patch_rel32(&e, s_slow[k].jcc_imm, emit_pos(&e));
        emit_smc_slow_body(&e, s_slow[k].addr_reg, s_slow[k].addr_imm, s_slow[k].nbytes);
        emit_jmp_rel32_to(&e, s_slow[k].join);
    }
    s_nslow = 0;

    dbt->code_used = e.offset;
    __builtin___clear_cache((char *)entry, (char *)(dbt->code_buf + e.offset));

    dbt_mark_block_bytes(dbt, guest_pc, pc);
    return entry;
}
