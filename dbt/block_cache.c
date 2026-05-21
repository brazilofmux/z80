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
}

void dbt_mark_block_bytes(z80_dbt_t *dbt, uint16_t start, uint32_t end) {
    for (uint32_t a = start; a < end; a++) {
        dbt->code_bitmap[a & 0xFFFF] = 1;
    }
}
