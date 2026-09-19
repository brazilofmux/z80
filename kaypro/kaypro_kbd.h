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
 *                       times (default 20) with nothing to give it, or
 *                       has blocked in a read — i.e. it wants a key
 *   @sleep MS           wall-clock pause
 *   @dump FILE          write the screen (kaypro_video_dump) now; "-" is
 *                       stdout
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
#define KBD_IDLE_POLLS 64

/* Host-terminal source (the default). */
void kaypro_kbd_init(void);

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
uint64_t kaypro_kbd_reads(void);

#endif /* KAYPRO_KBD_H */
