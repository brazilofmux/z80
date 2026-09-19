/* video_test.c — host-side unit test for kaypro/kaypro_video.c.
 *
 * Feeds byte sequences to the interpreter and checks the cell buffer.
 * Runs on the host (no Z80 involved): `make test-video`. Each case is a
 * sequence and the rows it should produce; unlisted rows must be blank.
 */
#include "../kaypro/kaypro_video.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;

static void feed(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) kaypro_video_putc((uint8_t)s[i]);
}
#define FEED(lit) feed(lit, sizeof(lit) - 1)

/* Compare row r against `want` padded with spaces to 80 columns. */
static void expect_row(const char *name, int r, const char *want) {
    char got[KV_COLS + 1];
    for (int c = 0; c < KV_COLS; c++) got[c] = (char)kaypro_video.cells[r][c].ch;
    got[KV_COLS] = 0;
    char exp[KV_COLS + 1];
    memset(exp, ' ', KV_COLS); exp[KV_COLS] = 0;
    memcpy(exp, want, strlen(want));
    if (memcmp(got, exp, KV_COLS) != 0) {
        failures++;
        fprintf(stderr, "FAIL %s row %d\n  want |%s|\n  got  |%s|\n", name, r, exp, got);
    }
}
static void expect_cursor(const char *name, int r, int c) {
    if (kaypro_video.cur_row != r || kaypro_video.cur_col != c) {
        failures++;
        fprintf(stderr, "FAIL %s cursor want (%d,%d) got (%d,%d)\n",
                name, r, c, kaypro_video.cur_row, kaypro_video.cur_col);
    }
}
static void expect_attr(const char *name, int r, int c, uint8_t attr) {
    if (kaypro_video.cells[r][c].attr != attr) {
        failures++;
        fprintf(stderr, "FAIL %s attr at (%d,%d) want %02X got %02X\n",
                name, r, c, attr, kaypro_video.cells[r][c].attr);
    }
}
static void expect_blank_except(const char *name, uint32_t rows_mask) {
    for (int r = 0; r < KV_ROWS; r++) {
        if (rows_mask & (1u << r)) continue;
        for (int c = 0; c < KV_COLS; c++)
            if (kaypro_video.cells[r][c].ch != ' ' || kaypro_video.cells[r][c].attr) {
                failures++;
                fprintf(stderr, "FAIL %s row %d should be blank (col %d = %02X/%02X)\n",
                        name, r, c, kaypro_video.cells[r][c].ch, kaypro_video.cells[r][c].attr);
                return;
            }
    }
}

int main(void) {
    /* Plain text, CR/LF. */
    kaypro_video_init(KAYPRO_MODEL_84);
    FEED("Hello\r\nWorld");
    expect_row("text", 0, "Hello");
    expect_row("text", 1, "World");
    expect_cursor("text", 1, 5);
    expect_blank_except("text", 0x3);

    /* Auto-wrap at column 80 and LF scrolling at the bottom. */
    kaypro_video_init(KAYPRO_MODEL_84);
    char eighty[81]; memset(eighty, 'x', 80); eighty[80] = 0;
    feed(eighty, 80);
    expect_cursor("wrap", 1, 0);
    FEED("y");
    expect_row("wrap", 0, eighty);
    expect_row("wrap", 1, "y");
    kaypro_video_init(KAYPRO_MODEL_84);
    for (int i = 0; i < 24; i++) { char b[8]; snprintf(b, sizeof b, "L%02d\r\n", i); feed(b, strlen(b)); }
    /* 24 lines + LF after the 24th = one scroll of the 24-row text area:
     * L00 gone, L23 on row 22, cursor row 23; the status line (row 24)
     * is untouched. */
    expect_row("scroll", 0, "L01");
    expect_row("scroll", 22, "L23");
    expect_row("scroll", 23, "");
    expect_row("scroll", 24, "");
    expect_cursor("scroll", 23, 0);

    /* ESC = cursor addressing, biased by 0x20. */
    kaypro_video_init(KAYPRO_MODEL_84);
    FEED("\x1b=" "\x25\x2a" "at 5,10");      /* row 5, col 10 */
    expect_row("esc=", 5, "          at 5,10");
    FEED("\x1b=\x20\x20" "home");
    expect_row("esc=", 0, "home");
    FEED("\x1b=\x7f\x7f" "Z");                 /* out of range clamps to 24,79 (status line) */
    /* The status line doesn't scroll: writing its last cell wraps the
     * cursor to column 0 of the same row. */
    expect_row("esc= clamp", 24, "                                                                               Z");
    expect_cursor("esc= clamp", 24, 0);
    FEED("\x1b=\x37\x7f" "Q");                 /* row 23 col 79: bottom-right of the text area */
    /* Immediate wrap + scroll (ADM-3A has no pending-wrap state): Q ends
     * on row 22, row 23 is blank, the status line keeps its Z. */
    expect_row("bottom-right", 22, "                                                                               Q");
    expect_row("bottom-right", 23, "");
    expect_row("bottom-right", 24, "                                                                               Z");
    expect_cursor("bottom-right", 23, 0);

    /* Cursor motion controls: ^H ^J ^K ^L ^M ^^ */
    kaypro_video_init(KAYPRO_MODEL_84);
    FEED("\x1b=\x25\x25" "\x0b" "U");         /* up from (5,5) -> writes at (4,5) */
    expect_row("motion", 4, "     U");
    FEED("\x0a\x0a" "D");                      /* down twice from (4,6) -> (6,6) */
    expect_row("motion", 6, "      D");
    FEED("\x08\x08" "b");                      /* back twice from (6,7) -> (6,5) */
    expect_row("motion", 6, "     bD");
    FEED("\x0c\x0c" "r");                      /* right twice from (6,6) -> (6,8) */
    expect_row("motion", 6, "     bD r");
    FEED("\x0d" "c");                          /* CR -> col 0 */
    expect_row("motion", 6, "c    bD r");
    FEED("\x1e" "H");                          /* home */
    expect_row("motion", 0, "H");
    kaypro_video_init(KAYPRO_MODEL_84);
    FEED("\x08" "X");                          /* BS at column 0 stays */
    expect_row("bs@0", 0, "X");
    kaypro_video_init(KAYPRO_MODEL_84);
    FEED("\x0b" "X");                          /* up at row 0 stays */
    expect_row("up@0", 0, "X");

    /* Erase: ^X to end of line, ^W to end of screen, ^Z clear. */
    kaypro_video_init(KAYPRO_MODEL_84);
    FEED("abcdefgh\r\nsecond\r\nthird");
    FEED("\x1b=\x20\x23" "\x18");             /* row 0 col 3: erase to EOL */
    expect_row("^X", 0, "abc");
    expect_row("^X", 1, "second");
    FEED("\x1b=\x21\x22" "\x17");             /* row 1 col 2: erase to EOS */
    expect_row("^W", 0, "abc");
    expect_row("^W", 1, "se");
    expect_row("^W", 2, "");
    FEED("\x1a" "Z");
    expect_row("^Z", 0, "Z");
    expect_cursor("^Z", 0, 1);
    expect_blank_except("^Z", 0x1);

    /* '84 attributes: ESC B n on, ESC C n off. */
    kaypro_video_init(KAYPRO_MODEL_84);
    FEED("\x1b" "B0" "R" "\x1b" "B3" "U" "\x1b" "C0" "u" "\x1b" "C3" "p");
    expect_row("attr", 0, "RUup");
    expect_attr("attr", 0, 0, KV_ATTR_REVERSE);
    expect_attr("attr", 0, 1, KV_ATTR_REVERSE | KV_ATTR_UNDERLINE);
    expect_attr("attr", 0, 2, KV_ATTR_UNDERLINE);
    expect_attr("attr", 0, 3, 0);
    FEED("\x1b" "B4");
    if (!kaypro_video.cursor_visible) { failures++; fprintf(stderr, "FAIL cursor on\n"); }
    FEED("\x1b" "C4");
    if (kaypro_video.cursor_visible)  { failures++; fprintf(stderr, "FAIL cursor off\n"); }

    /* ESC B 6 remembers the cursor, ESC C 6 returns to it. */
    kaypro_video_init(KAYPRO_MODEL_84);
    FEED("\x1b=\x2a\x2a" "\x1b" "B6" "\x1b=\x20\x20" "far" "\x1b" "C6" "back");
    expect_row("save/restore", 0, "far");
    expect_row("save/restore", 10, "          back");

    /* Status line: ESC B 7 (default on) keeps row 24 out of scroll and
     * clear; ESC C 7 lets the whole 25 rows scroll. */
    kaypro_video_init(KAYPRO_MODEL_84);
    FEED("\x1b=\x38\x20" "STATUS" "\x1b=\x20\x20" "top" "\x1a");
    expect_row("status ^Z", 0, "");
    expect_row("status ^Z", 24, "STATUS");
    FEED("\x1b" "C7" "\x1a");
    expect_row("status off ^Z", 24, "");
    FEED("\x1b=\x38\x20" "S25" "\n");        /* LF on row 24 with preservation off scrolls all 25 */
    expect_row("status off LF", 23, "S25");
    expect_row("status off LF", 24, "");

    /* NUL displays as an accent grave (guide p. 87); ESC A is a no-op. */
    kaypro_video_init(KAYPRO_MODEL_84);
    FEED("a\x00" "b" "\x1b" "Ac");
    expect_row("nul/escA", 0, "a`bc");

    /* '84 insert / delete line. */
    kaypro_video_init(KAYPRO_MODEL_84);
    FEED("one\r\ntwo\r\nthree");
    FEED("\x1b=\x21\x20" "\x1bR");            /* delete row 1 */
    expect_row("ESC R", 0, "one");
    expect_row("ESC R", 1, "three");
    expect_row("ESC R", 2, "");
    FEED("\x1b=\x20\x20" "\x1b" "E");            /* insert at row 0 */
    expect_row("ESC E", 0, "");
    expect_row("ESC E", 1, "one");
    expect_row("ESC E", 2, "three");

    /* Graphics sequences consume their arguments and draw nothing. */
    kaypro_video_init(KAYPRO_MODEL_84);
    FEED("\x1b*\x30\x30" "\x1b \x30\x30" "\x1bL\x30\x30\x40\x40" "\x1b" "D\x30\x30\x40\x40" "ok");
    expect_row("graphics", 0, "ok");
    expect_blank_except("graphics", 0x1);

    /* Model II: extensions are not recognised — ESC + byte swallowed,
     * arguments printed as text (that is what a real II did). */
    kaypro_video_init(KAYPRO_MODEL_II);
    FEED("\x1b" "B0" "x");
    expect_row("model II", 0, "0x");
    if (kaypro_video.unknown_esc != 1) { failures++; fprintf(stderr, "FAIL unknown_esc=%u\n", kaypro_video.unknown_esc); }
    FEED("\x1b=\x21\x21" "y");                /* ESC = works on the II */
    expect_row("model II esc=", 1, " y");
    FEED("\x1b=\x38\x20" "w");                /* row 24 doesn't exist on a II: clamps to 23 */
    expect_row("model II rows", 23, "w");

    /* Unknown control bytes are swallowed and counted; DEL is ignored. */
    kaypro_video_init(KAYPRO_MODEL_84);
    FEED("a\x01\x7f" "b");
    expect_row("unknown ctrl", 0, "ab");
    if (kaypro_video.unknown_ctrl != 1) { failures++; fprintf(stderr, "FAIL unknown_ctrl=%u\n", kaypro_video.unknown_ctrl); }

    /* Dirty tracking: one row written marks one row. */
    kaypro_video_init(KAYPRO_MODEL_84);
    kaypro_video_clear_dirty();
    FEED("\x1b=\x27\x20" "dirty");
    if (kaypro_video.dirty_rows != (1u << 7)) { failures++; fprintf(stderr, "FAIL dirty_rows=%08X\n", kaypro_video.dirty_rows); }

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("video_test: all cases pass");
    return 0;
}
