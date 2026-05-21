/* block_cache.c — Direct-mapped translated-block cache.
 *
 * 64K entries × 16 bytes. Lookup is `(pc & MASK)` — no hashing; we lean
 * on Z80 code locality. A cache hit on a different guest_pc means a
 * conflict miss; we evict by overwrite.
 *
 * Invalidate is currently a "blow away the world" operation. A finer
 * SMC-aware invalidate-by-range will land when self-modifying code
 * support is wired up; today's hello.com and zexall don't need it. */
#include "dbt.h"
#include <string.h>

static inline uint32_t cache_index(uint16_t pc) {
    return pc & BLOCK_CACHE_MASK;
}

z80_block_entry_t *dbt_cache_lookup(z80_dbt_t *dbt, uint16_t pc) {
    z80_block_entry_t *e = &dbt->cache[cache_index(pc)];
    if (e->guest_pc == (uint32_t)pc) {
        dbt->cache_hits++;
        return e;
    }
    dbt->cache_misses++;
    return NULL;
}

void dbt_cache_insert(z80_dbt_t *dbt, uint16_t pc, uint8_t *code) {
    z80_block_entry_t *e = &dbt->cache[cache_index(pc)];
    e->guest_pc    = (uint32_t)pc;
    e->native_code = code;
}

void dbt_cache_invalidate_all(z80_dbt_t *dbt) {
    for (uint32_t i = 0; i < BLOCK_CACHE_SIZE; i++) {
        dbt->cache[i].guest_pc    = BLOCK_EMPTY_PC;
        dbt->cache[i].native_code = NULL;
    }
    memset(dbt->code_bitmap, 0, sizeof(dbt->code_bitmap));
    dbt->max_block_bytes = 0;
}

void dbt_mark_block_bytes(z80_dbt_t *dbt, uint16_t start, uint32_t end) {
    uint32_t bytes = (end - start) & 0xFFFF;
    if (bytes > dbt->max_block_bytes) dbt->max_block_bytes = bytes;
    for (uint32_t a = start; a < end; a++) {
        dbt->code_bitmap[a & 0xFFFF] = 1;
    }
}

static void dbt_invalidate_for_store(z80_dbt_t *dbt, uint16_t addr) {
    /* Invalidate cache entries whose start PC falls in the window
     * [addr - max_block_bytes + 1, addr] — any of them might cover byte
     * `addr`. The window is sized to the actual longest translated block,
     * not MAX_BLOCK_INSNS * worst-case-bytes-per-insn, so workloads with
     * short basic blocks (typical CP/M) pay far less per store than a
     * fixed 320-byte sweep would cost.
     *
     * We do NOT touch the bitmap here: clearing it would let a subsequent
     * store on the same byte silently skip the helper (false negative),
     * which is exactly the stale-translation bug zexdoc's repeated
     * test_op patching hit. False positives (bitmap still 1 for a byte
     * whose block was just invalidated; next store fires the helper
     * unnecessarily, helper sees the cache slot already empty and does
     * no extra work) are the price. */
    uint32_t window = dbt->max_block_bytes;
    for (uint32_t k = 0; k < window; k++) {
        uint16_t p = (uint16_t)(addr - k);
        dbt->cache[p & BLOCK_CACHE_MASK].guest_pc    = BLOCK_EMPTY_PC;
        dbt->cache[p & BLOCK_CACHE_MASK].native_code = NULL;
    }
    dbt->smc_invalidations++;
}

/* Public wrapper for every interp / cpm memory write. JIT-translated
 * stores have an inline equivalent in the emitted code. */
void z80_mem_w(z80_cpu_t *cpu, uint16_t addr, uint8_t val) {
    cpu->mem[addr & 0xFFFF] = val;
    if (!cpu->dbt) return;
    z80_dbt_t *dbt = (z80_dbt_t *)cpu->dbt;
    if (dbt->code_bitmap[addr & 0xFFFF]) {
        dbt_invalidate_for_store(dbt, addr);
    }
}
