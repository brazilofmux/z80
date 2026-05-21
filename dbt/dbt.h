/* dbt.h — Z80 Dynamic Binary Translator public interface.
 *
 * Modeled on ~/riscv/dbt/dbt.h. The translator runs alongside the
 * interpreter: blocks the backend can translate run as native code,
 * everything else (CALL, OUT, IN, LDIR, IM, EX (SP),HL, EI/DI, anything
 * that can trap) ends the block early and the run loop steps the
 * interpreter for that single instruction before retrying the JIT.
 *
 * Block ABI: a translated block is entered with
 *   X19 = z80_cpu_t *cpu        (callee-saved)
 *   X20 = cpu->mem (uint8_t *)  (callee-saved)
 *   X21 = z80_block_entry_t *   cache base (callee-saved)
 * and exits via RET after updating cpu->pc.
 */
#ifndef DBT_H
#define DBT_H

#include "../core/z80.h"
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

/* ----------------------------------------------------------------------
 * Apple Silicon W^X bracketing.
 *
 * On macOS/aarch64 a JIT page can be writable XOR executable per-thread;
 * every byte written into dbt->code_buf MUST be inside a paired
 * dbt_jit_writable_begin()/end(). On Linux and Intel macOS the helpers
 * are no-ops.
 * ---------------------------------------------------------------------- */
#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>
#define DBT_JIT_MMAP_FLAGS (MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT)
static inline void dbt_jit_writable_begin(void) { pthread_jit_write_protect_np(0); }
static inline void dbt_jit_writable_end(void)   { pthread_jit_write_protect_np(1); }
#else
#define DBT_JIT_MMAP_FLAGS (MAP_PRIVATE | MAP_ANONYMOUS)
static inline void dbt_jit_writable_begin(void) { }
static inline void dbt_jit_writable_end(void)   { }
#endif

/* Block cache: direct-mapped, 64K entries × 16 bytes.
 * empty slot encoded by guest_pc == 0xFFFFFFFFu (no legal Z80 PC matches). */
typedef struct {
    uint32_t guest_pc;
    uint32_t _pad;
    uint8_t *native_code;
} z80_block_entry_t;

_Static_assert(sizeof(z80_block_entry_t) == 16, "z80_block_entry_t must be 16 bytes");

#define BLOCK_CACHE_SIZE   (64 * 1024)
#define BLOCK_CACHE_MASK   (BLOCK_CACHE_SIZE - 1)
#define BLOCK_EMPTY_PC     0xFFFFFFFFu

/* Upper bound on guest instructions per translated block. The backend
 * stops emitting earlier when it hits any control-flow or untranslatable
 * op. 64 is plenty for typical CP/M basic blocks. */
#define MAX_BLOCK_INSNS    64

#define CODE_BUF_SIZE      (64 * 1024 * 1024)   /* 64 MB JIT buffer */

typedef struct {
    z80_cpu_t *cpu;

    z80_block_entry_t cache[BLOCK_CACHE_SIZE];

    /* Per-guest-byte tag: nonzero iff some currently-cached block covers
     * this byte. JIT-emitted stores call into z80_jit_post_store(), which
     * checks the byte the store landed on; if marked, we invalidate the
     * whole cache (with bitmap reset) so the next block lookup forces a
     * re-translation of whatever the patched bytes now decode to.
     *
     * Coarse-but-cheap design: a store to a non-code byte costs a single
     * branch; SMC pays the full cache wipe but is rare. zexdoc's hot
     * "patch the test op, run it, repeat" loop is exactly this pattern. */
    uint8_t  code_bitmap[65536];

    uint8_t *code_buf;
    uint32_t code_used;

    /* Stats */
    uint64_t blocks_translated;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t interp_fallback_insns;
    uint64_t jit_block_entries;
    uint64_t smc_invalidations;

    int trace;
} z80_dbt_t;

/* ----------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */
int  dbt_init(z80_dbt_t *dbt, z80_cpu_t *cpu);
int  dbt_run(z80_dbt_t *dbt);
void dbt_cleanup(z80_dbt_t *dbt);
int  dbt_jit_available(void);
void dbt_print_stats(z80_dbt_t *dbt, FILE *out);

/* Cache management (block_cache.c) */
z80_block_entry_t *dbt_cache_lookup(z80_dbt_t *dbt, uint16_t pc);
void               dbt_cache_insert(z80_dbt_t *dbt, uint16_t pc, uint8_t *code);
void               dbt_cache_invalidate_all(z80_dbt_t *dbt);

/* Mark guest bytes [start, end) as "covered by a cached block" so a
 * subsequent JIT store to any byte in that range triggers SMC
 * invalidation. Idempotent. */
void dbt_mark_block_bytes(z80_dbt_t *dbt, uint16_t start, uint32_t end);

/* ----------------------------------------------------------------------
 * Backend hooks — implemented by dbt_a64.c or dbt_x64.c.
 *
 * dbt_translate_block: emit code for the block starting at guest_pc;
 *   return entry pointer into dbt->code_buf, or NULL if the very first
 *   instruction is not translatable (so the run loop steps interp once).
 *
 * dbt_emit_trampoline: emit a one-shot host-arch shim at the start of
 *   dbt->code_buf, called from C as
 *     void (*)(z80_cpu_t *cpu, uint8_t *mem, void *block, void *cache_base);
 *   The trampoline saves callee-saved regs, binds X19/X20/X21 (or the x64
 *   equivalents), BLRs into `block`, and returns.
 * ---------------------------------------------------------------------- */
uint8_t *dbt_translate_block(z80_dbt_t *dbt, uint16_t guest_pc);
void     dbt_emit_trampoline(z80_dbt_t *dbt);

#endif /* DBT_H */
