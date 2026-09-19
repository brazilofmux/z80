/* kaypro_render_tty.h — paint the Kaypro cell buffer on the host terminal.
 *
 * A diffing renderer: it keeps a copy of what it last painted and, on
 * each flush, emits only cursor moves + SGR changes + the characters
 * that differ, coalescing runs. A 4 BIPS guest can rewrite the screen
 * thousands of times between two host frames; the diff makes the
 * terminal see one.
 *
 * Output is ANSI/VT100 (every host terminal we care about): alternate
 * screen on init, cursor positioning, SGR 7/2/5/4 for the Kaypro's
 * inverse/dim/blink/underline. Kaypro graphics characters (bytes with
 * bit 7 set, a 2x4 pixel block each) are drawn as Unicode braille,
 * which is also 2x4 — see graphic_to_braille in the .c.
 */
#ifndef KAYPRO_RENDER_TTY_H
#define KAYPRO_RENDER_TTY_H

#include <stdio.h>

/* Where the escape stream goes; stdout unless a test redirects it. */
void kaypro_render_tty_set_output(FILE *f);

/* Switch the terminal to the alternate screen, clear it, and forget
 * any previous paint state so the next flush paints everything. */
void kaypro_render_tty_init(void);

/* Leave the alternate screen and reset attributes. Safe to call twice;
 * registered with atexit by main.c so every exit path restores. */
void kaypro_render_tty_shutdown(void);

/* Paint the differences since the last flush (all cells if `force`).
 * Cheap when nothing changed: a couple of comparisons, no output. */
void kaypro_render_tty_flush(int force);

/* Throttled flush for output bursts: paints if at least ~16 ms have
 * passed since the last paint. Called from the console-output shim. */
void kaypro_render_tty_flush_if_due(void);

/* Full repaint on the next flush (SIGWINCH, or anything else that may
 * have disturbed the host screen). */
void kaypro_render_tty_invalidate(void);

/* Nonzero once init has run and shutdown has not. */
int kaypro_render_tty_active(void);

#endif /* KAYPRO_RENDER_TTY_H */
