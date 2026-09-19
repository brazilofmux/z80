/* kaypro_kbd.h — the console input side: host keyboard or a script.
 *
 * Everything the guest can ask about input funnels through here:
 *   poll  — is a byte available? (BIOS CONST, BDOS 6/11)
 *   read  — take one, blocking if none (BIOS CONIN, BDOS 1/6/10)
 *   wait  — block until poll would succeed (HALT)
 * Two sources implement those: the host terminal (raw stdin, with a
 * one-byte read-ahead so poll-then-read never races), and a script
 * file for headless runs. A script is a list of keystrokes with
 * synchronisation points, so a test can type into WordStar at guest
 * speed and know when the program has caught up:
 *
 *   # comment
 *   look\r              text line: bytes with C-style escapes (\r \n \t
 *                       \e \\ \xHH), and \^X for control-X; the line's
 *                       own newline is NOT sent
 *   ~                   inline: wait until the guest is idle (see below)
 *   @wait-idle [N]      wait until the guest has polled for input N
 *                       times in a row (default 20000) with nothing to
 *                       give it and no console output in between, or
 *                       has blocked in a read — i.e. it is done drawing
 *                       and wants a key. The default outlasts message
 *                       delays: WordStar paces "NEW FILE" with ~16500
 *                       silent CONST polls, and a quiet run that long
 *                       costs a few milliseconds at guest speed
 *   @sleep MS           wall-clock pause
 *   @dump FILE [attrs]  write the screen (kaypro_video_dump) now; "-" is
 *                       stdout; a relative FILE is placed next to the
 *                       script; "attrs" adds the attribute and hex planes
 *   @mem ADDR LEN       hex-dump LEN bytes of guest memory at ADDR (hex)
 *                       to stdout — a debugging aid for scripted runs
 *   @end                stop here (implicit at end of file)
 *
 * When the script runs out and the guest asks for another key, the run
 * is over: kaypro_kbd_read returns KBD_END and the console shim exits
 * the emulator cleanly (--screen-dump happens on the way out).
 *
 * Idle detection also protects the host: a guest spinning on CONST
 * with nothing else going on (no output between polls) costs a full
 * core at 4 BIPS, so after KBD_IDLE_POLLS consecutive empty polls each
 * further poll sleeps a millisecond. Any console output resets the
 * count, so a program that polls between units of real work is not
 * slowed down.
 */
#ifndef KAYPRO_KBD_H
#define KAYPRO_KBD_H

#include <stdint.h>

#define KBD_END (-1)
#define KBD_IDLE_POLLS 64          /* host terminal: sleep after this many quiet polls */
#define KBD_IDLE_END_POLLS 40000   /* script exhausted: end after this many quiet polls */
#define KBD_WAIT_IDLE_DEFAULT 20000 /* @wait-idle / ~ with no count */

/* Host-terminal source (the default). */
void kaypro_kbd_init(void);

/* The cpu whose memory @mem dumps (main sets it once). */
struct z80_cpu;
void kaypro_kbd_attach(struct z80_cpu *cpu);

/* Load a script and make it the source. Returns 0 on success. */
int  kaypro_kbd_script_load(const char *path);
int  kaypro_kbd_scripted(void);

int  kaypro_kbd_poll(void);        /* 1 if read would not block */
int  kaypro_kbd_read(void);        /* 0..255, or KBD_END */
void kaypro_kbd_wait(void);        /* block until poll() or end of input */

/* Console output happened: the guest is not idle. */
void kaypro_kbd_note_activity(void);

/* Bookkeeping the tests and the HUD look at. */
uint64_t kaypro_kbd_polls(void);
unsigned kaypro_kbd_max_quiet_streak(void);   /* longest quiet poll run (tuning @wait-idle) */
uint64_t kaypro_kbd_reads(void);

#endif /* KAYPRO_KBD_H */
