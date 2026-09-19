/* kaypro_render_tty.c — diffing ANSI renderer for the Kaypro cell buffer.
 *
 * See kaypro_render_tty.h. The painter walks the rows the video model
 * flagged dirty (the model marks whole rows; a scroll marks them all),
 * compares each cell with the shadow copy of the last paint, and emits
 * the minimum: one cursor move per run of changed cells, one SGR per
 * attribute change, the characters. The Kaypro cursor is placed last.
 */
#include "kaypro_render_tty.h"
#include "kaypro_video.h"
#include <string.h>
#include <time.h>

static FILE *out;                       /* NULL until set; defaults to stdout */
static int   active;
static kaypro_cell_t shadow[KV_ROWS][KV_COLS];
static int   shadow_valid;              /* 0 => next flush paints everything */
static int   shadow_cur_row = -1, shadow_cur_col = -1, shadow_cursor_visible = -1;
static struct timespec last_paint;

#define FRAME_NS 16000000L              /* ~60 Hz cap on repaints during bursts */

static FILE *stream(void) { return out ? out : stdout; }

void kaypro_render_tty_set_output(FILE *f) { out = f; }
int  kaypro_render_tty_active(void) { return active; }
void kaypro_render_tty_invalidate(void) { shadow_valid = 0; }

void kaypro_render_tty_init(void) {
    FILE *f = stream();
    /* Alternate screen, reset attributes, clear, home, hide the cursor
     * until the first flush places it. */
    fputs("\033[?1049h\033[0m\033[2J\033[H\033[?25l", f);
    fflush(f);
    shadow_valid = 0;
    shadow_cur_row = shadow_cur_col = shadow_cursor_visible = -1;
    clock_gettime(CLOCK_MONOTONIC, &last_paint);
    active = 1;
}

void kaypro_render_tty_shutdown(void) {
    if (!active) return;
    FILE *f = stream();
    fputs("\033[0m\033[?25h\033[?1049l", f);
    fflush(f);
    active = 0;
}

/* Kaypro graphics character (bit 7 set; bits 0..6 = pixels #0..#6 of a
 * 2-wide x 4-tall block, guide p. 67) -> Unicode braille, whose eight
 * dots have the same 2x4 shape. Kaypro pixel layout (left, right per
 * row): (#1,#0) (#3,#2) (#5,#4) (#7,#6); braille dots 1-3 run down the
 * left column, 4-6 down the right, 7 and 8 are the bottom row. Pixel #7
 * is only reachable through "video mode" (Addendum p. 18), which the
 * model does not track yet, so it stays off. */
static void put_graphic(FILE *f, uint8_t g) {
    unsigned dots = 0;
    if (g & 0x02) dots |= 0x01;   /* #1 -> dot 1 (left, row 1) */
    if (g & 0x08) dots |= 0x02;   /* #3 -> dot 2 (left, row 2) */
    if (g & 0x20) dots |= 0x04;   /* #5 -> dot 3 (left, row 3) */
    if (g & 0x01) dots |= 0x08;   /* #0 -> dot 4 (right, row 1) */
    if (g & 0x04) dots |= 0x10;   /* #2 -> dot 5 (right, row 2) */
    if (g & 0x10) dots |= 0x20;   /* #4 -> dot 6 (right, row 3) */
    if (g & 0x40) dots |= 0x80;   /* #6 -> dot 8 (right, row 4) */
    unsigned cp = 0x2800 + dots;  /* U+2800..U+28FF: 3-byte UTF-8 */
    fputc((int)(0xE0 | (cp >> 12)), f);
    fputc((int)(0x80 | ((cp >> 6) & 0x3F)), f);
    fputc((int)(0x80 | (cp & 0x3F)), f);
}

static void put_cell_char(FILE *f, uint8_t ch) {
    if (ch & 0x80)                    put_graphic(f, ch);
    else if (ch >= 0x20 && ch < 0x7F) fputc(ch, f);
    else                              fputc('?', f);   /* model never stores these */
}

/* Emit the SGR for a Kaypro attribute byte, from a clean state. */
static void emit_attr(FILE *f, uint8_t attr) {
    if (!attr) { fputs("\033[0m", f); return; }
    fputs("\033[0", f);
    if (attr & KV_ATTR_DIM)       fputs(";2", f);
    if (attr & KV_ATTR_UNDERLINE) fputs(";4", f);
    if (attr & KV_ATTR_BLINK)     fputs(";5", f);
    if (attr & KV_ATTR_REVERSE)   fputs(";7", f);
    fputc('m', f);
}

static void emit_goto(FILE *f, int r, int c) {
    fprintf(f, "\033[%d;%dH", r + 1, c + 1);
}

void kaypro_render_tty_flush(int force) {
    if (!active) return;
    kaypro_video_t *v = &kaypro_video;
    if (!force && shadow_valid && !v->dirty_rows && !v->cursor_moved) return;

    FILE *f = stream();
    int full = force || !shadow_valid;
    uint32_t rows = full ? (1u << KV_ROWS) - 1 : v->dirty_rows;
    /* Every flush leaves the terminal at attribute 0 (see the end), and
     * nothing else writes to it while the renderer is active, so a
     * partial paint can assume it starts clean. */
    int cur_attr = 0;

    if (full) fputs("\033[?25l\033[0m\033[2J", f);
    else      fputs("\033[?25l", f);

    for (int r = 0; r < KV_ROWS; r++) {
        if (!(rows & (1u << r))) continue;
        /* Per-cell "needs painting". On a full paint the screen was just
         * cleared, so blank default cells are already right. */
        uint8_t diff[KV_COLS];
        int last = -1;
        for (int c = 0; c < KV_COLS; c++) {
            const kaypro_cell_t *cell = &v->cells[r][c];
            const kaypro_cell_t *old  = &shadow[r][c];
            diff[c] = full ? !(cell->ch == ' ' && cell->attr == 0)
                           : !(cell->ch == old->ch && cell->attr == old->attr);
            if (diff[c]) last = c;
        }
        int c = 0;
        while (c <= last) {
            if (!diff[c]) { c++; continue; }
            emit_goto(f, r, c);
            /* Paint from here through the last differing cell of this
             * run, rewriting unchanged cells inside short gaps: a cursor
             * move costs 6-9 bytes, so a gap of up to GAP_MAX cells is
             * cheaper to write through than to skip. */
            enum { GAP_MAX = 6 };
            while (c <= last) {
                if (!diff[c]) {
                    int k = 0;
                    while (c + k <= last && !diff[c + k]) k++;
                    if (k > GAP_MAX) break;   /* skip it with a goto */
                }
                const kaypro_cell_t *cell = &v->cells[r][c];
                if (cell->attr != cur_attr) { emit_attr(f, cell->attr); cur_attr = cell->attr; }
                put_cell_char(f, cell->ch);
                c++;
            }
        }
        memcpy(shadow[r], v->cells[r], sizeof shadow[r]);
    }
    if (cur_attr != 0) fputs("\033[0m", f);

    /* Park the terminal cursor where the Kaypro's is, and match its
     * visibility. */
    emit_goto(f, v->cur_row, v->cur_col);
    if (v->cursor_visible) fputs("\033[?25h", f);
    shadow_cur_row = v->cur_row; shadow_cur_col = v->cur_col;
    shadow_cursor_visible = v->cursor_visible;

    fflush(f);
    shadow_valid = 1;
    kaypro_video_clear_dirty();
    clock_gettime(CLOCK_MONOTONIC, &last_paint);
}

void kaypro_render_tty_flush_if_due(void) {
    if (!active) return;
    kaypro_video_t *v = &kaypro_video;
    if (shadow_valid && !v->dirty_rows && !v->cursor_moved) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ns = (now.tv_sec - last_paint.tv_sec) * 1000000000L + (now.tv_nsec - last_paint.tv_nsec);
    if (ns >= FRAME_NS) kaypro_render_tty_flush(0);
}
