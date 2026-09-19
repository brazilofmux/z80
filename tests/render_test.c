/* render_test.c — host-side test of kaypro/kaypro_render_tty.c.
 *
 * Captures the renderer's escape stream in a memory buffer and checks
 * that a flush after a small change is small and targeted, that
 * attributes come out as the right SGRs, and that graphics characters
 * become braille. `make test-render`.
 */
#include "../kaypro/kaypro_video.h"
#include "../kaypro/kaypro_render_tty.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void feed(const char *s) { while (*s) kaypro_video_putc((uint8_t)*s++); }

/* Run one flush into a fresh memstream and return the bytes. */
static char *capture(int force, size_t *len) {
    char *buf = NULL; size_t n = 0;
    FILE *m = open_memstream(&buf, &n);
    kaypro_render_tty_set_output(m);
    kaypro_render_tty_flush(force);
    fclose(m);
    kaypro_render_tty_set_output(NULL);
    *len = n;
    return buf;
}

static void expect_contains(const char *name, const char *hay, const char *needle) {
    if (!strstr(hay, needle)) {
        failures++;
        fprintf(stderr, "FAIL %s: output lacks %s\n  got: ", name, needle);
        for (const char *p = hay; *p; p++) fputc(*p == 0x1b ? '^' : *p, stderr);
        fputc('\n', stderr);
    }
}
static void expect_absent(const char *name, const char *hay, const char *needle) {
    if (strstr(hay, needle)) {
        failures++;
        fprintf(stderr, "FAIL %s: output should not contain %s\n", name, needle);
    }
}

int main(void) {
    /* Init goes to a scratch stream so the alternate-screen switch is
     * not written to the real terminal. */
    char *ibuf = NULL; size_t ilen = 0;
    FILE *init_out = open_memstream(&ibuf, &ilen);
    kaypro_render_tty_set_output(init_out);
    kaypro_video_init(KAYPRO_MODEL_84);
    kaypro_render_tty_init();
    fclose(init_out);
    expect_contains("init", ibuf, "\033[?1049h");
    free(ibuf);

    /* First flush: full paint of two lines of text, cursor after them. */
    feed("Hello\r\nWorld");
    size_t n; char *o = capture(0, &n);
    expect_contains("first", o, "\033[1;1HHello");
    expect_contains("first", o, "\033[2;1HWorld");
    expect_contains("first cursor", o, "\033[2;6H");
    expect_contains("first cursor on", o, "\033[?25h");
    free(o);

    /* Nothing changed: no output at all. */
    o = capture(0, &n);
    if (n != 0) { failures++; fprintf(stderr, "FAIL idle flush wrote %zu bytes\n", n); }
    free(o);

    /* One character changed in the middle of a line: exactly one goto
     * and that character, nothing about the other row. */
    feed("\033=\x20\x22" "L");         /* row 0 col 2: 'l' -> 'L' */
    o = capture(0, &n);
    expect_contains("one cell", o, "\033[1;3HL");
    expect_absent("one cell", o, "World");
    expect_absent("one cell", o, "Hello");
    if (n > 40) { failures++; fprintf(stderr, "FAIL one-cell flush is %zu bytes\n", n); }
    free(o);

    /* Attributes: inverse and underline become SGR 7 / 4, reset after. */
    feed("\033=\x23\x20" "\033" "B0" "inv" "\033" "C0" "\033" "B3" "und" "\033" "C3" "plain");
    o = capture(0, &n);
    expect_contains("sgr inverse", o, "\033[0;7minv");
    expect_contains("sgr underline", o, "\033[0;4mund");
    expect_contains("sgr reset", o, "\033[0mplain");
    free(o);

    /* Scroll marks every row dirty; the diff still only rewrites cells
     * that actually differ from the shadow. Fill 24 rows, scroll once. */
    kaypro_video_init(KAYPRO_MODEL_84);
    kaypro_render_tty_invalidate();
    for (int i = 0; i < 24; i++) { char b[16]; snprintf(b, sizeof b, "row%02d\r\n", i); feed(b); }
    o = capture(0, &n); free(o);           /* paint the 23 remaining rows */
    feed("last");                          /* on row 23 after the scroll */
    o = capture(0, &n);
    expect_contains("scroll", o, "\033[24;1Hlast");
    free(o);

    /* Video-mode graphics block: first byte LSB=1 (pixel #7 -> dot 7),
     * second byte 0x81 (pixel #0 -> dot 4): dots 4+7 = U+2848 = E2 A1 88. */
    kaypro_video_init(KAYPRO_MODEL_84);
    kaypro_render_tty_invalidate();
    feed("\033" "B5"); kaypro_video_putc(0x81); kaypro_video_putc(0x81); feed("\033" "C5");
    o = capture(0, &n);
    expect_contains("braille", o, "\xE2\xA1\x88");
    expect_absent("braille no sgr for pixel7", o, "\033[0;");
    free(o);

    /* High-bit text outside video mode is inverse. */
    kaypro_video_putc(0xC1);   /* 'A' | 0x80 */
    o = capture(0, &n);
    expect_contains("hibit inverse", o, "\033[0;7mA");
    free(o);

    /* Cursor hidden by ESC C 4: no show sequence, hide is present. */
    feed("\033" "C4");
    o = capture(0, &n);
    expect_contains("cursor hide", o, "\033[?25l");
    expect_absent("cursor hide", o, "\033[?25h");
    free(o);

    /* HUD: painted on host line 26 when the terminal has one, dim, and
     * only when its text changes; a 24-row terminal never sees it. */
    kaypro_render_tty_set_size(24, 80);
    kaypro_render_tty_set_hud("4.31 BIPS");
    o = capture(0, &n);
    expect_absent("hud on short terminal", o, "\033[26;1H");
    free(o);
    kaypro_render_tty_set_size(30, 80);
    kaypro_render_tty_set_hud("4.32 BIPS");
    o = capture(0, &n);
    expect_contains("hud", o, "\033[26;1H\033[0;2m4.32 BIPS");
    free(o);
    o = capture(0, &n);
    if (n != 0) { failures++; fprintf(stderr, "FAIL unchanged hud repainted (%zu bytes)\n", n); }
    free(o);
    kaypro_render_tty_set_hud(NULL);
    o = capture(0, &n);
    expect_contains("hud cleared", o, "\033[26;1H\033[K");
    free(o);

    /* Shutdown leaves the alternate screen. */
    char *sbuf = NULL; size_t slen = 0;
    FILE *sd = open_memstream(&sbuf, &slen);
    kaypro_render_tty_set_output(sd);
    kaypro_render_tty_shutdown();
    fclose(sd);
    expect_contains("shutdown", sbuf, "\033[?1049l");
    free(sbuf);

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("render_test: all cases pass");
    return 0;
}
