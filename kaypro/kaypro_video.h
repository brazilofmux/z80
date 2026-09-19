/* kaypro_video.h — the Kaypro's 80x24 character screen as a cell buffer.
 *
 * The guest's console output stream (BDOS 2/6/9, BIOS CONOUT, later the
 * real BIOS through its port) is fed byte by byte into kaypro_video_putc,
 * which interprets the Kaypro terminal's control codes and escape
 * sequences and updates a 24x80 array of (character, attribute) cells.
 * Nothing here touches the host terminal: renderers (kaypro_render_*.c)
 * read the cells and the dirty-row mask and paint however they like —
 * a diffing ANSI painter for a tty, a text dump for the headless tests,
 * a pixel window later.
 *
 * Escape set: the Kaypro '84 models (2X, 4'84, 10) implement the
 * Lear-Siegler ADM-3A control set plus Kaypro extensions (attributes,
 * insert/delete line, 160x100 bit graphics). The original Kaypro II/4 is
 * the ADM-3A subset only; kaypro_video_set_model(KAYPRO_MODEL_II) turns
 * the extensions off so software that probes for them sees a II.
 *
 * Verified against the Kaypro 10 User's Guide (1984): "Video graphics
 * and attributes command set" pp. 64-69 of the guide (PDF pp. 76-81) and
 * "Video command protocol for KAYPRO 10" pp. 77-78 / 86-87 (PDF pp. 99-100,
 * 114-115). The screen is 25 rows: 24 text rows plus a 25th status line
 * that scrolling and clearing leave alone while "status line
 * preservation" (ESC B 7, the default) is on. The guide lists insert/
 * delete line both ways round (ESC E/ESC R on p. 87, ESC R/ESC E on
 * p. 77); we follow p. 87, which the September 1984 Addendum (p. 28,
 * "takes precedence over material in that user's guide") and the termcap
 * entry for the Kaypro (al=\EE, dl=\ER) both confirm.
 */
#ifndef KAYPRO_VIDEO_H
#define KAYPRO_VIDEO_H

#include <stdint.h>
#include <stdio.h>

#define KV_ROWS 25          /* 24 text rows + status line (row 24) */
#define KV_TEXT_ROWS 24
#define KV_COLS 80

/* Cell attribute bits. */
#define KV_ATTR_REVERSE   0x01
#define KV_ATTR_DIM       0x02
#define KV_ATTR_BLINK     0x04
#define KV_ATTR_UNDERLINE 0x08
#define KV_ATTR_PIXEL7    0x10   /* graphics cell only: the 8th pixel (video-mode 2-byte form) */

typedef struct {
    uint8_t ch;      /* the byte the guest wrote; 0x20 for erased cells */
    uint8_t attr;    /* KV_ATTR_* bits in force when it was written */
} kaypro_cell_t;

enum { KAYPRO_MODEL_II = 0, KAYPRO_MODEL_84 = 1 };

typedef struct {
    kaypro_cell_t cells[KV_ROWS][KV_COLS];
    int      cur_row, cur_col;
    uint8_t  attr;                /* current attribute for new characters */
    int      cursor_visible;
    int      model;               /* KAYPRO_MODEL_* */
    int      status_preserve;     /* '84: row 24 excluded from scroll/clear (ESC B/C 7) */
    int      saved_row, saved_col;/* '84: ESC B 6 / ESC C 6 */
    /* '84 "video mode" (ESC B 5 / ESC C 5, Addendum p. 18). Off (the
     * default): a byte with bit 7 set is the character in its low seven
     * bits shown highlighted (inverse) — what Turbo Pascal's "Kaypro
     * with hilite" definition relies on. On: bit-7 bytes are 2x4 pixel
     * graphics blocks, two bytes each — the first byte's LSB is pixel #7,
     * the second byte's low seven bits are pixels #0-#6. */
    int      video_mode;
    int      gfx_pending;         /* video mode: first byte of a pair seen */
    uint8_t  gfx_first;

    /* Escape-sequence state machine. */
    int      esc_state;           /* 0 = none, 1 = got ESC, 2 = collecting args */
    uint8_t  esc_lead;            /* the byte after ESC */
    int      esc_need;            /* args still to collect */
    uint8_t  esc_args[4];
    int      esc_nargs;

    /* Renderer support: bit r set => row r changed since last clear. */
    uint32_t dirty_rows;
    int      cursor_moved;

    /* Bookkeeping. */
    uint64_t bytes_in;
    uint32_t bells;
    uint32_t unknown_ctrl;        /* control bytes we swallowed */
    uint32_t unknown_esc;         /* ESC sequences we swallowed */
} kaypro_video_t;

/* The single screen. Everything in this module works on it. */
extern kaypro_video_t kaypro_video;

/* When nonzero, the console layer routes CONOUT here instead of stdout.
 * Set by main.c (-K). */
extern int kaypro_video_enabled;

void kaypro_video_init(int model);
void kaypro_video_putc(uint8_t ch);

/* Renderer helpers. */
static inline int  kaypro_video_row_dirty(int r) { return (kaypro_video.dirty_rows >> r) & 1; }
static inline void kaypro_video_clear_dirty(void) { kaypro_video.dirty_rows = 0; kaypro_video.cursor_moved = 0; }

/* Text dump for tests and --screen-dump: 25 lines of 80 characters (the
 * last is the status line), graphics blocks shown as '#', other
 * non-printables as '.', trailing spaces
 * kept (fixed width). With with_attrs, a second 25-line block follows
 * using one hex digit per cell for the attribute bits. */
void kaypro_video_dump(FILE *f, int with_attrs);

#endif /* KAYPRO_VIDEO_H */
