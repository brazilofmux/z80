/* z80.h — Z80 CPU state, flags, and public interfaces
 *
 * This is the heart of the beast. Every DBT block and the interpreter
 * will operate on (or against) this structure.
 *
 * Layout is chosen for:
 *  - Easy access from generated native code (fixed offsets)
 *  - Good cache behavior when we have hot blocks
 */
#ifndef Z80_H
#define Z80_H

#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * Flag bits (F register)
 * ========================================================================= */
#define Z80_FLAG_S   0x80   /* Sign */
#define Z80_FLAG_Z   0x40   /* Zero */
#define Z80_FLAG_5   0x20   /* Undocumented (copy of bit 5 of result) */
#define Z80_FLAG_H   0x10   /* Half-carry */
#define Z80_FLAG_3   0x08   /* Undocumented (copy of bit 3 of result) */
#define Z80_FLAG_PV  0x04   /* Parity/Overflow */
#define Z80_FLAG_N   0x02   /* Subtract */
#define Z80_FLAG_C   0x01   /* Carry */

/* Convenient masks */
#define Z80_FLAG_SZHPNC  (Z80_FLAG_S|Z80_FLAG_Z|Z80_FLAG_H|Z80_FLAG_PV|Z80_FLAG_N|Z80_FLAG_C)
#define Z80_FLAG_35      (Z80_FLAG_5|Z80_FLAG_3)

/* ============================================================================
 * Z80 CPU Context
 *
 * This struct will be pointed to by a host register in translated code
 * (e.g. X19 on AArch64, RBX on x86-64, like the RV32 ctx).
 *
 * We keep both the "official" 8/16-bit view and some expanded forms that
 * the DBT can use for fast paths (e.g. pre-computed A and F separately).
 * ========================================================================= */
typedef struct z80_cpu {
    /* Primary register set (the one most instructions see) */
    union {
        uint16_t af;
        struct { uint8_t f, a; };   /* little-endian: f is low byte */
    };
    union {
        uint16_t bc;
        struct { uint8_t c, b; };
    };
    union {
        uint16_t de;
        struct { uint8_t e, d; };
    };
    union {
        uint16_t hl;
        struct { uint8_t l, h; };
    };

    /* Alternate register set (EX AF,AF' / EXX) */
    union {
        uint16_t af_;
        struct { uint8_t f_, a_; };
    };
    union {
        uint16_t bc_;
        struct { uint8_t c_, b_; };
    };
    union {
        uint16_t de_;
        struct { uint8_t e_, d_; };
    };
    union {
        uint16_t hl_;
        struct { uint8_t l_, h_; };
    };

    /* Index registers + stack + program counter */
    uint16_t ix;
    uint16_t iy;
    uint16_t sp;
    uint16_t pc;

    /* Interrupt / refresh */
    uint8_t  i;          /* interrupt vector */
    uint8_t  r;          /* refresh counter (bit 7 often not incremented) */
    uint8_t  iff1;       /* interrupt flip-flop 1 */
    uint8_t  iff2;       /* interrupt flip-flop 2 */
    uint8_t  im;         /* interrupt mode (0,1,2) */

    /* ========================================================================
     * DBT / execution engine extensions (not part of the Z80 ISA)
     * These live at the end so the offsets of the real registers stay small.
     * ====================================================================== */

    /* Pointer to the 64KB guest memory (or larger if we do banking later) */
    uint8_t *mem;

    /* Size of the memory region (usually 65536) */
    uint32_t mem_size;

    /* Next PC to execute after the current translated block finishes.
     * Set by every block exit (direct jump, conditional, CALL, RET, etc.).
     */
    uint16_t next_pc;

    /* Pointer to the block cache base (for inline lookup from generated code) */
    void     *block_cache;

    /* Return Address Stack for fast CALL/RET prediction (like the RV32 one) */
    uint16_t ras[32];
    uint8_t  ras_top;

    /* Statistics / profiling (optional, enabled with -s) */
    uint64_t insn_count;
    uint64_t block_count;
    uint64_t cache_hits;
    uint64_t cache_misses;

    /* For lazy flag handling in the DBT (see CLAUDE.md) */
    uint32_t flag_state;   /* opaque descriptor of what last set the flags */
    uint16_t flag_op1;
    uint16_t flag_op2;
    uint8_t  flag_result;
} z80_cpu_t;

/* Size assertion so we can keep the struct cache-friendly and JIT-friendly */
_Static_assert(sizeof(z80_cpu_t) <= 256, "z80_cpu_t grew too big — reconsider layout");

/* ============================================================================
 * Decoded instruction (shared between decoder, interpreter, and future DBT)
 * ========================================================================= */
typedef enum {
    Z80_OP_UNKNOWN = 0,
    Z80_OP_NOP,
    Z80_OP_LD_R_N,
    Z80_OP_LD_R_R,
    Z80_OP_LD_RR_NN,
    Z80_OP_LD_HL_N,
    Z80_OP_LD_A_BC,
    Z80_OP_LD_A_DE,
    Z80_OP_LD_A_NN,
    Z80_OP_LD_BC_A,
    Z80_OP_LD_DE_A,
    Z80_OP_LD_NN_A,
    Z80_OP_LD_SP_HL,
    Z80_OP_PUSH_RR,
    Z80_OP_POP_RR,
    Z80_OP_ADD_A_R,
    Z80_OP_ADD_A_N,
    Z80_OP_SUB_A_R,
    Z80_OP_SUB_A_N,
    Z80_OP_AND_A_R,
    Z80_OP_AND_A_N,
    Z80_OP_OR_A_R,
    Z80_OP_OR_A_N,
    Z80_OP_XOR_A_R,
    Z80_OP_XOR_A_N,
    Z80_OP_CP_A_R,
    Z80_OP_CP_A_N,
    Z80_OP_INC_R,
    Z80_OP_DEC_R,
    Z80_OP_INC_RR,
    Z80_OP_DEC_RR,
    Z80_OP_JP_NN,
    Z80_OP_JP_CC_NN,
    Z80_OP_JR_E,
    Z80_OP_JR_CC_E,
    Z80_OP_CALL_NN,
    Z80_OP_CALL_CC_NN,
    Z80_OP_RET,
    Z80_OP_RET_CC,
    Z80_OP_RST,
    Z80_OP_OUT_N_A,
    Z80_OP_IN_A_N,
    Z80_OP_EX_DE_HL,
    Z80_OP_EX_AF_AF,
    Z80_OP_EXX,
    Z80_OP_DJNZ,
    Z80_OP_LDIR,
    Z80_OP_LDDR,
    Z80_OP_ADD_HL_RR,   /* ADD HL, BC/DE/HL/SP */
    Z80_OP_LD_HL_indNN, /* LD HL, (nn) */
    Z80_OP_LD_NN_HL,    /* LD (nn), HL */
    Z80_OP_CPL,
    Z80_OP_SCF,
    Z80_OP_CCF,
    Z80_OP_DAA,
    Z80_OP_NEG,         /* ED 44 */
    Z80_OP_LDI,
    Z80_OP_LDD,
    Z80_OP_CB,          /* CB prefix group: rotates, shifts, BIT/RES/SET */
} z80_op_type;

typedef struct {
    z80_op_type type;
    uint8_t     prefix;     /* 0, 0xCB, 0xDD, 0xED, 0xFD */
    uint8_t     opcode;
    uint8_t     reg1, reg2, cc;
    uint16_t    imm16;
    uint8_t     imm8;
    int8_t      disp;
    uint8_t     bytes;
} z80_decoded;

int  z80_decode_one(const uint8_t *mem, uint16_t pc, z80_decoded *out);
const char *z80_op_name(z80_op_type t);

/* ============================================================================
 * Public API (will grow)
 * ========================================================================= */

/* Initialize a CPU context with 64KB of memory */
void z80_cpu_init(z80_cpu_t *cpu);

/* Reset (like power-on or RST 0) */
void z80_cpu_reset(z80_cpu_t *cpu);

/* Load a .COM file at 0x0100, set PC=0x0100, SP high, etc. */
int  z80_load_com(z80_cpu_t *cpu, const char *path);

/* One instruction step (for the interpreter and for debugging) */
int  z80_step(z80_cpu_t *cpu);

/* Run until a certain condition (used by interp and by DBT run loop) */
void z80_run(z80_cpu_t *cpu);

/* Helper: compute the real F byte from lazy state (DBT calls this when needed) */
uint8_t z80_materialize_flags(z80_cpu_t *cpu);

#endif /* Z80_H */
