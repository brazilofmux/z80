/* kbd_test.c — host-side test of the key decoder in kaypro/kaypro_kbd.c.
 * `make test-kbd`. */
#include "../kaypro/kaypro_kbd.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
static void expect(const char *name, const char *seq, size_t n, const char *want, size_t wn) {
    uint8_t out[8];
    size_t k = kaypro_kbd_translate((const uint8_t *)seq, n, out, sizeof out);
    if (k != wn || memcmp(out, want, wn) != 0) {
        failures++;
        fprintf(stderr, "FAIL %s: got %zu bytes [", name, k);
        for (size_t i = 0; i < k; i++) fprintf(stderr, " %02X", out[i]);
        fprintf(stderr, " ] want %zu [", wn);
        for (size_t i = 0; i < wn; i++) fprintf(stderr, " %02X", (uint8_t)want[i]);
        fprintf(stderr, " ]\n");
    }
}
#define EXPECT(name, seq, want) expect(name, seq, sizeof(seq) - 1, want, sizeof(want) - 1)

int main(void) {
    kaypro_kbd_keys_configure("");                 /* defaults */
    EXPECT("plain", "a", "a");
    EXPECT("cr", "\r", "\r");
    EXPECT("lone esc", "\x1b", "\x1b");
    EXPECT("up CSI", "\x1b[A", "\x0b");
    EXPECT("down SS3", "\x1bOB", "\x0a");
    EXPECT("left", "\x1b[D", "\x08");
    EXPECT("right", "\x1b[C", "\x0c");
    EXPECT("modified up", "\x1b[1;5A", "\x0b");
    EXPECT("delete key", "\x1b[3~", "\x7f");
    EXPECT("host backspace (DEL byte)", "\x7f", "\x08");
    EXPECT("host BS byte", "\x08", "\x08");
    EXPECT("unmapped F1 swallowed", "\x1bOP", "");
    EXPECT("unmapped PgUp swallowed", "\x1b[5~", "");
    EXPECT("unknown sequence swallowed", "\x1b[99z", "");

    if (kaypro_kbd_keys_configure("wordstar") != 0) { failures++; fprintf(stderr, "FAIL wordstar preset\n"); }
    EXPECT("ws up", "\x1b[A", "\x05");
    EXPECT("ws down", "\x1b[B", "\x18");
    EXPECT("ws left", "\x1b[D", "\x13");
    EXPECT("ws right", "\x1b[C", "\x04");
    EXPECT("ws pgdn", "\x1b[6~", "\x03");
    EXPECT("ws home", "\x1b[H", "\x11\x13");
    EXPECT("ws backspace->DEL", "\x7f", "\x7f");
    EXPECT("ws delete->^G", "\x1b[3~", "\x07");

    if (kaypro_kbd_keys_configure("f1=\\^KD,up=\\x05,del=") != 0) { failures++; fprintf(stderr, "FAIL custom spec\n"); }
    EXPECT("custom f1", "\x1bOP", "\x0b" "D");
    EXPECT("custom f1 csi form", "\x1b[11~", "\x0b" "D");
    EXPECT("custom up", "\x1b[A", "\x05");
    EXPECT("custom del unmapped", "\x1b[3~", "");
    if (kaypro_kbd_keys_configure("bogus=1") == 0) { failures++; fprintf(stderr, "FAIL bad name accepted\n"); }

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("kbd_test: all cases pass");
    return 0;
}
