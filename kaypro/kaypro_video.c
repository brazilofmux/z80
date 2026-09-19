/* kaypro_video.c — 80x25 cell buffer + Kaypro/ADM-3A control interpreter.
 *
 * See kaypro_video.h for the model. Two tables drive the interpreter:
 * ctrl_handlers[] for bytes 0x00..0x1F and esc_table[] for the byte
 * after ESC (with the argument count each sequence takes). Adding or
 * correcting a sequence is a table entry, not a code path. Every entry
 * is checked against the Kaypro 10 User's Guide and the September 1984
 * Addendum (whose p. 28 "Video Command Protocol" supersedes the guide);
 * page references below are to the guide's own numbering (its p. 64 is
 * PDF p. 76).
 */
#include "kaypro_video.h"
#include <string.h>

kaypro_video_t kaypro_video;
int kaypro_video_enabled = 0;

#define KV (&kaypro_video)

/* ---- primitive screen operations ------------------------------------ */

static inline void mark_dirty(int r) { KV->dirty_rows |= 1u << r; }
static inline void mark_all_dirty(void) { KV->dirty_rows = (1u << KV_ROWS) - 1; }

/* Rows that scroll/clear touch: the text area, or the whole screen when
 * status-line preservation is off (or on a II, which has no status line
 * semantics to preserve — it simply has 24 rows). */
static inline int scroll_rows(void) {
    return (KV->model >= KAYPRO_MODEL_84 && !KV->status_preserve) ? KV_ROWS : KV_TEXT_ROWS;
}

static void blank_cells(kaypro_cell_t *c, int n) {
    for (int i = 0; i < n; i++) { c[i].ch = ' '; c[i].attr = 0; }
}

static void scroll_up(void) {
    int n = scroll_rows();
    memmove(&KV->cells[0][0], &KV->cells[1][0],
            sizeof(kaypro_cell_t) * KV_COLS * (n - 1));
    blank_cells(KV->cells[n - 1], KV_COLS);
    mark_all_dirty();
}

/* Highest row the cursor may occupy: the II has 24 rows; the '84 has 25
 * and ESC = can address the status line whatever the preservation flag
 * says (guide p. 66: vertical coordinate 32..56). */
static inline int max_row(void) {
    return (KV->model >= KAYPRO_MODEL_84 ? KV_ROWS : KV_TEXT_ROWS) - 1;
}

static void set_cursor(int r, int c) {
    if (r < 0) r = 0; else if (r > max_row()) r = max_row();
    if (c < 0) c = 0; else if (c >= KV_COLS) c = KV_COLS - 1;
    KV->cur_row = r; KV->cur_col = c;
    KV->cursor_moved = 1;
}

/* Cursor down one; at the bottom of the scrolling region the region
 * scrolls (ADM-3A LF). On the status line, with preservation on, down
 * stays put — nothing to scroll into. */
static void cursor_down(void) {
    int bottom = scroll_rows() - 1;
    if (KV->cur_row < bottom) KV->cur_row++;
    else if (KV->cur_row == bottom) scroll_up();
    KV->cursor_moved = 1;
}

static void put_char(uint8_t ch) {
    kaypro_cell_t *cell = &KV->cells[KV->cur_row][KV->cur_col];
    cell->ch = ch; cell->attr = KV->attr;
    mark_dirty(KV->cur_row);
    /* ADM-3A auto-wrap: the cursor moves immediately, no pending-wrap
     * state. Writing the last column of the last row scrolls. */
    if (KV->cur_col == KV_COLS - 1) {
        KV->cur_col = 0;
        cursor_down();
    } else {
        KV->cur_col++;
    }
    KV->cursor_moved = 1;
}

static void erase_to_eol(void) {
    blank_cells(&KV->cells[KV->cur_row][KV->cur_col], KV_COLS - KV->cur_col);
    mark_dirty(KV->cur_row);
}

static void erase_to_eos(void) {
    int n = scroll_rows();
    erase_to_eol();
    for (int r = KV->cur_row + 1; r < n; r++) blank_cells(KV->cells[r], KV_COLS);
    mark_all_dirty();
}

static void clear_screen(void) {
    int n = scroll_rows();
    for (int r = 0; r < n; r++) blank_cells(KV->cells[r], KV_COLS);
    set_cursor(0, 0);
    mark_all_dirty();
}

/* '84 ESC R: delete the cursor line, lines below move up, blank line at
 * the bottom of the scrolling region. */
static void delete_line(void) {
    int r = KV->cur_row, n = scroll_rows();
    if (r >= n) return;
    if (r < n - 1)
        memmove(&KV->cells[r][0], &KV->cells[r + 1][0],
                sizeof(kaypro_cell_t) * KV_COLS * (n - 1 - r));
    blank_cells(KV->cells[n - 1], KV_COLS);
    mark_all_dirty();
}

/* '84 ESC E: insert a blank line at the cursor, lines below move down,
 * the bottom line of the region is lost. */
static void insert_line(void) {
    int r = KV->cur_row, n = scroll_rows();
    if (r >= n) return;
    if (r < n - 1)
        memmove(&KV->cells[r + 1][0], &KV->cells[r][0],
                sizeof(kaypro_cell_t) * KV_COLS * (n - 1 - r));
    blank_cells(KV->cells[r], KV_COLS);
    mark_all_dirty();
}

/* ---- control characters (0x00..0x1F) -------------------------------- */

typedef void (*ctrl_fn)(void);

static void c_bell(void)  { KV->bells++; }
static void c_bs(void)    { if (KV->cur_col > 0) { KV->cur_col--; KV->cursor_moved = 1; } }
static void c_lf(void)    { cursor_down(); }
static void c_up(void)    { if (KV->cur_row > 0) { KV->cur_row--; KV->cursor_moved = 1; } }
static void c_right(void) {
    /* ADM-3A forward space: wraps to the next line, scrolling at the bottom. */
    if (KV->cur_col == KV_COLS - 1) { KV->cur_col = 0; cursor_down(); }
    else KV->cur_col++;
    KV->cursor_moved = 1;
}
static void c_cr(void)    { KV->cur_col = 0; KV->cursor_moved = 1; }
static void c_eos(void)   { erase_to_eos(); }
static void c_eol(void)   { erase_to_eol(); }
static void c_clear(void) { clear_screen(); }
static void c_esc(void)   { KV->esc_state = 1; }
static void c_home(void)  { set_cursor(0, 0); }
static void c_nul(void)   { put_char('`'); }   /* guide p. 87: NUL shows as an accent grave */

static const ctrl_fn ctrl_handlers[32] = {
    [0x00] = c_nul,
    [0x07] = c_bell,      /* ^G */
    [0x08] = c_bs,        /* ^H cursor left */
    [0x0A] = c_lf,        /* ^J cursor down / scroll */
    [0x0B] = c_up,        /* ^K cursor up */
    [0x0C] = c_right,     /* ^L cursor right */
    [0x0D] = c_cr,        /* ^M carriage return */
    [0x17] = c_eos,       /* ^W erase to end of screen */
    [0x18] = c_eol,       /* ^X erase to end of line */
    [0x1A] = c_clear,     /* ^Z clear screen + home */
    [0x1B] = c_esc,       /* ESC */
    [0x1E] = c_home,      /* ^^ home */
};

/* ---- escape sequences ----------------------------------------------- */

typedef void (*esc_fn)(const uint8_t *args);

/* ESC = row col : cursor address, both biased by 0x20 (ADM-3A). */
static void e_cursor(const uint8_t *a) { set_cursor(a[0] - 0x20, a[1] - 0x20); }

/* ESC B n / ESC C n : '84 attribute n on / off (guide pp. 68, 77-78):
 *   0 inverse video, 1 reduced intensity, 2 blinking, 3 underline,
 *   4 cursor on/off, 5 video mode on/off (see kaypro_video_t.video_mode),
 *   6 B = remember cursor position, C = return to it,
 *   7 status-line preservation on/off. */
static void attr_set(uint8_t n, int on) {
    uint8_t bit = 0;
    switch (n) {
    case '0': bit = KV_ATTR_REVERSE;   break;
    case '1': bit = KV_ATTR_DIM;       break;
    case '2': bit = KV_ATTR_BLINK;     break;
    case '3': bit = KV_ATTR_UNDERLINE; break;
    case '4': KV->cursor_visible = on; KV->cursor_moved = 1; return;
    case '5': KV->video_mode = on; KV->gfx_pending = 0; return;
    case '6':
        if (on) { KV->saved_row = KV->cur_row; KV->saved_col = KV->cur_col; }
        else    set_cursor(KV->saved_row, KV->saved_col);
        return;
    case '7': KV->status_preserve = on; return;
    default:  KV->unknown_esc++; return;
    }
    if (on) KV->attr |= bit; else KV->attr &= (uint8_t)~bit;
}
static void e_attr_on(const uint8_t *a)  { attr_set(a[0], 1); }
static void e_attr_off(const uint8_t *a) { attr_set(a[0], 0); }
static void e_del_line(const uint8_t *a) { (void)a; delete_line(); }
static void e_ins_line(const uint8_t *a) { (void)a; insert_line(); }
/* ESC * V H / ESC <sp> V H : pixel on/off; ESC L / ESC D V1 H1 V2 H2 :
 * line draw/erase on the 160x100 grid (guide pp. 64-65). Arguments are
 * consumed; nothing is drawn until there is a pixel plane to draw on. */
static void e_graphics(const uint8_t *a) { (void)a; }
/* ESC A : "display lower case alphabet" (guide p. 87) — a factory test
 * pattern by the look of it; recognised so it can't be mistaken for text. */
static void e_nop(const uint8_t *a) { (void)a; }

typedef struct {
    uint8_t lead;
    int     nargs;
    int     model;      /* minimum model that has this sequence */
    esc_fn  fn;
} esc_entry_t;

static const esc_entry_t esc_table[] = {
    { '=', 2, KAYPRO_MODEL_II, e_cursor   },
    { 'B', 1, KAYPRO_MODEL_84, e_attr_on  },
    { 'C', 1, KAYPRO_MODEL_84, e_attr_off },
    { 'R', 0, KAYPRO_MODEL_84, e_del_line },   /* p. 87 + Addendum p. 28 (p. 77 has R/E swapped) */
    { 'E', 0, KAYPRO_MODEL_84, e_ins_line },
    { 'A', 0, KAYPRO_MODEL_84, e_nop      },
    { '*', 2, KAYPRO_MODEL_84, e_graphics },
    { ' ', 2, KAYPRO_MODEL_84, e_graphics },
    { 'L', 4, KAYPRO_MODEL_84, e_graphics },
    { 'D', 4, KAYPRO_MODEL_84, e_graphics },
};

static const esc_entry_t *esc_lookup(uint8_t lead) {
    for (size_t i = 0; i < sizeof esc_table / sizeof esc_table[0]; i++)
        if (esc_table[i].lead == lead && KV->model >= esc_table[i].model)
            return &esc_table[i];
    return NULL;
}

/* ---- public API ----------------------------------------------------- */

void kaypro_video_init(int model) {
    memset(KV, 0, sizeof *KV);
    KV->model = model;
    KV->cursor_visible = 1;
    KV->status_preserve = 1;      /* guide: the default; BIOS messages live there */
    for (int r = 0; r < KV_ROWS; r++) blank_cells(KV->cells[r], KV_COLS);
    set_cursor(0, 0);
    mark_all_dirty();
}

void kaypro_video_putc(uint8_t ch) {
    KV->bytes_in++;

    switch (KV->esc_state) {
    case 1: {   /* byte after ESC */
        const esc_entry_t *e = esc_lookup(ch);
        if (!e) {
            /* Unknown: swallow ESC + this byte, as the real terminal did. */
            KV->unknown_esc++;
            KV->esc_state = 0;
            return;
        }
        KV->esc_lead = ch;
        KV->esc_nargs = 0;
        KV->esc_need = e->nargs;
        if (KV->esc_need == 0) {
            KV->esc_state = 0;
            e->fn(KV->esc_args);
        } else {
            KV->esc_state = 2;
        }
        return;
    }
    case 2:     /* collecting arguments */
        KV->esc_args[KV->esc_nargs++] = ch;
        if (--KV->esc_need == 0) {
            KV->esc_state = 0;
            esc_lookup(KV->esc_lead)->fn(KV->esc_args);
        }
        return;
    default:
        break;
    }

    if (ch & 0x80) {
        if (!KV->video_mode) {
            /* Highlighted text: the low seven bits, shown inverse. A
             * control code under the high bit has no defined look; show
             * it as a block so it isn't silently lost. */
            uint8_t low = ch & 0x7F;
            if (low >= 0x20 && low != 0x7F) {
                uint8_t saved = KV->attr;
                KV->attr |= KV_ATTR_REVERSE;
                put_char(low);
                KV->attr = saved;
            } else {
                put_char(ch);
            }
            return;
        }
        /* Video mode: two bytes per graphics block. */
        if (!KV->gfx_pending) { KV->gfx_pending = 1; KV->gfx_first = ch; return; }
        KV->gfx_pending = 0;
        uint8_t saved = KV->attr;
        KV->attr = (uint8_t)((saved & ~KV_ATTR_PIXEL7) | ((KV->gfx_first & 1) ? KV_ATTR_PIXEL7 : 0));
        put_char((uint8_t)(0x80 | (ch & 0x7F)));
        KV->attr = saved;
        return;
    }
    if (ch < 0x20) {
        ctrl_fn f = ctrl_handlers[ch];
        if (f) f(); else KV->unknown_ctrl++;
        return;
    }
    if (ch == 0x7F) return;     /* DEL: no-op on the terminal */
    put_char(ch);
}

void kaypro_video_dump(FILE *f, int with_attrs) {
    for (int r = 0; r < KV_ROWS; r++) {
        char line[KV_COLS + 1];
        for (int c = 0; c < KV_COLS; c++) {
            uint8_t ch = KV->cells[r][c].ch;
            /* '#' = Kaypro graphics block (bit 7), '.' = anything else
             * unprintable (the model shouldn't store any). */
            line[c] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : (ch & 0x80) ? '#' : '.';
        }
        line[KV_COLS] = 0;
        fprintf(f, "%s\n", line);
    }
    if (with_attrs) {
        fputc('\n', f);
        for (int r = 0; r < KV_ROWS; r++) {
            for (int c = 0; c < KV_COLS; c++)
                fputc("0123456789ABCDEF"[KV->cells[r][c].attr & 0xF], f);
            fputc('\n', f);
        }
    }
}
