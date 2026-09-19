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
    if (e->guest_pc == (uint32_t)pc ||
        e->guest_pc == ((uint32_t)pc | BLOCK_REFUSED_BIT)) {
        dbt->cache_hits++;
        return e;
    }
    dbt->cache_misses++;
    return NULL;
}

void dbt_cache_insert(z80_dbt_t *dbt, uint16_t pc, uint8_t *code) {
    z80_block_entry_t *e = &dbt->cache[cache_index(pc)];
    e->guest_pc    = code ? (uint32_t)pc : ((uint32_t)pc | BLOCK_REFUSED_BIT);
    /* Refusal is decided from the bytes at pc, but we don't track how
     * many the decoder looked at — treat the sentinel as covering any
     * store that lands in the window so it's always retried. */
    e->span        = code ? dbt->last_block_bytes : 0xFFFFFFFFu;
    e->native_code = code;
    /* Point every block that direct-links to this PC at the fresh
     * translation (or back to the probe, for a refused sentinel). */
    dbt_links_repatch(dbt, pc, code);
}

/* NOTE: every caller of this full wipe also rewinds the code buffer
 * (dbt_init from scratch; the buffer-full path in dbt_translate_block).
 * The link registry leans on that: resetting the pool below silently
 * abandons all patch sites, which is only sound because the code
 * containing them is being discarded wholesale. A future caller that
 * wipes the cache but KEEPS the code buffer must unpatch instead. */
void dbt_cache_invalidate_all(z80_dbt_t *dbt) {
    for (uint32_t i = 0; i < BLOCK_CACHE_SIZE; i++) {
        dbt->cache[i].guest_pc    = BLOCK_EMPTY_PC;
        dbt->cache[i].native_code = NULL;
        dbt->link_head[i]         = LINK_NONE;
    }
    dbt->link_used = 0;
    dbt->link_free = LINK_NONE;
    memset(dbt->code_bitmap, 0, sizeof(dbt->code_bitmap));
    dbt->max_block_bytes = 0;
}

int dbt_link_record(z80_dbt_t *dbt, uint16_t pc, uint32_t site_off) {
    uint32_t i;
    if (dbt->link_free != LINK_NONE) {
        i = dbt->link_free;
        dbt->link_free = dbt->link_pool[i].next;
    } else if (dbt->link_used < LINK_POOL_SIZE) {
        i = dbt->link_used++;
    } else {
        return 0;
    }
    dbt->link_pool[i].site_off = site_off;
    dbt->link_pool[i].next     = dbt->link_head[pc];
    dbt->link_head[pc] = i;
    dbt->links_created++;
    return 1;
}

void dbt_links_repatch(z80_dbt_t *dbt, uint16_t pc, uint8_t *code) {
    uint32_t i = dbt->link_head[pc];
    if (i == LINK_NONE) return;
    dbt_jit_writable_begin();
    if (code) {
        for (; i != LINK_NONE; i = dbt->link_pool[i].next) {
            dbt_arch_patch_link(dbt, dbt->link_pool[i].site_off, code);
            dbt->links_patched++;
        }
    } else {
        /* Unlink AND free the list: an invalidated target's sites almost
         * always belong to blocks dying in the same SMC wave, and their
         * re-translations will re-record fresh sites. Keeping the stale
         * records would grow the lists (and the invalidation walks)
         * without bound under retranslation-heavy workloads. The rare
         * survivor site simply keeps using its probe fallback. */
        while (i != LINK_NONE) {
            uint32_t next = dbt->link_pool[i].next;
            dbt_arch_patch_link(dbt, dbt->link_pool[i].site_off, NULL);
            dbt->link_pool[i].next = dbt->link_free;
            dbt->link_free = i;
            dbt->links_unpatched++;
            i = next;
        }
        dbt->link_head[pc] = LINK_NONE;
    }
    dbt_jit_writable_end();
}

void dbt_mark_block_bytes(z80_dbt_t *dbt, uint16_t start, uint32_t end) {
    uint32_t bytes = (end - start) & 0xFFFF;
    if (bytes > dbt->max_block_bytes) dbt->max_block_bytes = bytes;
    dbt->last_block_bytes = bytes;
    for (uint32_t a = start; a < end; a++) {
        dbt->code_bitmap[a & 0xFFFF] = 1;
    }
}

uint32_t *dbt_smc_hist;   /* Z80_CODEMAP: stores into code bytes, by address */

static void dbt_invalidate_for_store(z80_dbt_t *dbt, uint16_t addr) {
    if (dbt_smc_hist) dbt_smc_hist[addr]++;
    /* Invalidate cache entries whose start PC falls in the window
     * [addr - max_block_bytes + 1, addr] — any of them might cover byte
     * `addr`. The window is sized to the actual longest translated block,
     * not MAX_BLOCK_INSNS * worst-case-bytes-per-insn, so workloads with
     * short basic blocks (typical CP/M) pay far less per store than a
     * fixed 320-byte sweep would cost.
     *
     * The bitmap byte is cleared at the end, once every covering block
     * has been invalidated (see there); clearing more than that byte,
     * or clearing it before the sweep, is the stale-translation bug
     * zexdoc's repeated test_op patching once hit. */
    uint32_t window = dbt->max_block_bytes;
    for (uint32_t k = 0; k < window; k++) {
        uint16_t p = (uint16_t)(addr - k);
        z80_block_entry_t *e = &dbt->cache[p & BLOCK_CACHE_MASK];
        /* The cache is direct-mapped 1:1 (64K entries, 16-bit PC), so
         * this slot can only hold the block starting at p. Its span
         * tells us exactly whether that block covers byte `addr`
         * (addr = p + k, covered iff k < span) — skip the clear and the
         * unlink storm for the blocks that merely start nearby. */
        if (e->guest_pc == BLOCK_EMPTY_PC || k >= e->span) continue;
        e->guest_pc    = BLOCK_EMPTY_PC;
        e->native_code = NULL;
        /* Any block direct-linked to p would bypass the cleared cache
         * entry and run the stale translation — unlink synchronously.
         * (This can run mid-JIT via z80_jit_post_store; we're in C code
         * here, so the W^X toggle inside repatch is safe.) */
        dbt_links_repatch(dbt, p, NULL);
    }
    dbt->smc_invalidations++;
    /* Every block that covered this byte is gone now, and translating
     * one again re-marks it (dbt_mark_block_bytes), so the byte can
     * drop out of the bitmap: the next store to it skips the sweep.
     * (The cache is 1:1, so no covering block can be hiding evicted.)
     * MS-COBOL's generated code patches a few operand bytes millions
     * of times a job; without this each store paid the window sweep. */
    dbt->code_bitmap[addr & 0xFFFF] = 0;
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

void z80_mem_host_wrote(z80_cpu_t *cpu, uint16_t addr, uint32_t len) {
    if (!cpu->dbt) return;                 /* interpreter only: nothing cached */
    for (uint32_t i = 0; i < len; i++) {
        uint16_t a = (uint16_t)(addr + i);
        z80_mem_w(cpu, a, cpu->mem[a]);    /* value already there; this is the invalidation */
    }
}
