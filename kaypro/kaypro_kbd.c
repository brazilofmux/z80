/* kaypro_kbd.c — console input: host terminal or script. See the header. */
#include "kaypro_kbd.h"
#include "kaypro_video.h"
#include "../core/z80.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>

/* ---- read-ahead queue (both sources) ---------------------------------- */

#define QSIZE 256
static uint8_t  queue[QSIZE];
static unsigned q_head, q_tail;          /* push at head, pop at tail */
static int q_empty(void) { return q_head == q_tail; }
static int q_full(void)  { return (q_head + 1) % QSIZE == q_tail; }
static void q_push(uint8_t c) {
    unsigned n = (q_head + 1) % QSIZE;
    if (n != q_tail) { queue[q_head] = c; q_head = n; }
}
static uint8_t q_pop(void) { uint8_t c = queue[q_tail]; q_tail = (q_tail + 1) % QSIZE; return c; }

static z80_cpu_t *attached_cpu;
void kaypro_kbd_attach(struct z80_cpu *cpu) { attached_cpu = cpu; }

static uint64_t n_polls, n_reads;
static unsigned empty_polls;             /* consecutive polls that found nothing */
static unsigned max_quiet_streak;        /* longest run of empty polls with no output between */
static int scripted;

uint64_t kaypro_kbd_polls(void) { return n_polls; }
unsigned kaypro_kbd_max_quiet_streak(void) { return max_quiet_streak; }
uint64_t kaypro_kbd_reads(void) { return n_reads; }
int      kaypro_kbd_scripted(void) { return scripted; }

static int hexval(int c);
static void keymap_defaults(void);

void kaypro_kbd_init(void) {
    q_head = q_tail = 0;
    n_polls = n_reads = 0;
    empty_polls = 0;
    scripted = 0;
    keymap_defaults();
    if (kaypro_kbd_keys_configure(getenv("Z80_KEYS")) != 0)
        fprintf(stderr, "Z80_KEYS: bad key spec ignored\n");
}

/* ---- key mapping ------------------------------------------------------ */

typedef struct { uint8_t bytes[8]; uint8_t n; } keydef_t;
static keydef_t keymap[KEY_COUNT];
static int keymap_ready;

static void key_set1(int key, uint8_t b) { keymap[key].bytes[0] = b; keymap[key].n = 1; }
void kaypro_kbd_key_set(int key, const uint8_t *bytes, size_t n) {
    if (key < 0 || key >= KEY_COUNT) return;
    if (n > sizeof keymap[key].bytes) n = sizeof keymap[key].bytes;
    memcpy(keymap[key].bytes, bytes, n);
    keymap[key].n = (uint8_t)n;
}

static void keymap_defaults(void) {
    memset(keymap, 0, sizeof keymap);
    key_set1(KEY_UP, 0x0B); key_set1(KEY_DOWN, 0x0A);
    key_set1(KEY_LEFT, 0x08); key_set1(KEY_RIGHT, 0x0C);
    key_set1(KEY_DEL, 0x7F); key_set1(KEY_BS, 0x08);
    keymap_ready = 1;
}

static const char *key_names[KEY_COUNT] = {
    "up", "down", "left", "right", "home", "end", "ins", "del", "pgup", "pgdn",
    "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12", "bs"
};

/* Decode script-style escapes (\r \n \e \^X \xHH \\) into bytes. */
static size_t unescape(const char *src, size_t len, uint8_t *out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < len && n < cap; i++) {
        char c = src[i];
        if (c != '\\') { out[n++] = (uint8_t)c; continue; }
        if (++i >= len) break;
        switch (src[i]) {
        case 'r': out[n++] = '\r'; break;
        case 'n': out[n++] = '\n'; break;
        case 't': out[n++] = '\t'; break;
        case 'e': out[n++] = 0x1B; break;
        case '^': if (++i < len) out[n++] = (uint8_t)(src[i] & 0x1F); break;
        case 'x': {
            int h = i + 1 < len ? hexval(src[i + 1]) : -1, l = i + 2 < len ? hexval(src[i + 2]) : -1;
            if (h >= 0 && l >= 0) { out[n++] = (uint8_t)(h * 16 + l); i += 2; }
            break;
        }
        default: out[n++] = (uint8_t)src[i]; break;
        }
    }
    return n;
}

int kaypro_kbd_keys_configure(const char *spec) {
    if (!keymap_ready) keymap_defaults();
    if (!spec || !*spec) return 0;
    if (!strcmp(spec, "wordstar")) {
        key_set1(KEY_UP, 0x05); key_set1(KEY_DOWN, 0x18);
        key_set1(KEY_LEFT, 0x13); key_set1(KEY_RIGHT, 0x04);
        key_set1(KEY_PGUP, 0x12); key_set1(KEY_PGDN, 0x03);
        key_set1(KEY_DEL, 0x07);                       /* ^G delete char right */
        key_set1(KEY_BS, 0x7F);                        /* DEL: delete char left */
        kaypro_kbd_key_set(KEY_HOME, (const uint8_t *)"\x11\x13", 2);  /* ^Q^S */
        kaypro_kbd_key_set(KEY_END,  (const uint8_t *)"\x11\x04", 2);  /* ^Q^D */
        return 0;
    }
    /* name=value,name=value,... */
    const char *p = spec;
    while (*p) {
        const char *eq = strchr(p, '=');
        if (!eq) return -1;
        const char *end = strchr(eq, ',');
        size_t vlen = end ? (size_t)(end - eq - 1) : strlen(eq + 1);
        int key = -1;
        for (int k = 0; k < KEY_COUNT; k++)
            if (strlen(key_names[k]) == (size_t)(eq - p) && !strncmp(p, key_names[k], (size_t)(eq - p))) key = k;
        if (key < 0) return -1;
        uint8_t bytes[8];
        size_t n = unescape(eq + 1, vlen, bytes, sizeof bytes);
        kaypro_kbd_key_set(key, bytes, n);
        if (!end) break;
        p = end + 1;
    }
    return 0;
}

/* Which key does a host escape sequence name? -1 for none. Covers the
 * xterm/VT100 forms every modern terminal sends: CSI A-D / H / F, SS3
 * A-D / H / F / P-S, CSI n ~ (1-8, 11-24), and modified arrows
 * CSI 1 ; m X as the plain key. */
static int key_for_sequence(const uint8_t *seq, size_t n) {
    if (n < 3 || seq[0] != 0x1B) return -1;
    uint8_t kind = seq[1];                 /* '[' CSI or 'O' SS3 */
    if (kind != '[' && kind != 'O') return -1;
    uint8_t last = seq[n - 1];
    if (n == 3 || (kind == '[' && n >= 6 && seq[2] == '1' && seq[3] == ';')) {
        switch (last) {
        case 'A': return KEY_UP;   case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT; case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME; case 'F': return KEY_END;
        case 'P': return n == 3 ? KEY_F1 : -1;   case 'Q': return n == 3 ? KEY_F2 : -1;
        case 'R': return n == 3 ? KEY_F3 : -1;   case 'S': return n == 3 ? KEY_F4 : -1;
        default:  return -1;
        }
    }
    if (kind == '[' && last == '~') {
        int num = 0;
        for (size_t i = 2; i + 1 < n && seq[i] >= '0' && seq[i] <= '9'; i++) num = num * 10 + (seq[i] - '0');
        switch (num) {
        case 1: case 7: return KEY_HOME;  case 4: case 8: return KEY_END;
        case 2: return KEY_INS;  case 3: return KEY_DEL;
        case 5: return KEY_PGUP; case 6: return KEY_PGDN;
        case 11: return KEY_F1;  case 12: return KEY_F2;  case 13: return KEY_F3;  case 14: return KEY_F4;
        case 15: return KEY_F5;  case 17: return KEY_F6;  case 18: return KEY_F7;  case 19: return KEY_F8;
        case 20: return KEY_F9;  case 21: return KEY_F10; case 23: return KEY_F11; case 24: return KEY_F12;
        default: return -1;
        }
    }
    return -1;
}

size_t kaypro_kbd_translate(const uint8_t *seq, size_t n, uint8_t *out, size_t outcap) {
    if (!keymap_ready) keymap_defaults();
    if (n == 0 || outcap == 0) return 0;
    int key;
    if (n == 1) {
        /* Both bytes a host Backspace key can send are the Kaypro's
         * BACKSPACE key; the Delete key (CSI 3 ~) is KEY_DEL. */
        if (seq[0] == 0x7F || seq[0] == 0x08) key = KEY_BS;
        else { out[0] = seq[0]; return 1; }
    } else {
        key = key_for_sequence(seq, n);
        if (key < 0) return 0;                      /* unknown sequence: swallowed whole */
    }
    size_t k = keymap[key].n;
    if (k > outcap) k = outcap;
    memcpy(out, keymap[key].bytes, k);
    return k;
}

/* ---- host terminal source -------------------------------------------- */

static int stdin_readable(long wait_us) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    struct timeval tv = { wait_us / 1000000, wait_us % 1000000 };
    int ret;
    do {
        ret = select(STDIN_FILENO + 1, &rfds, NULL, NULL, wait_us < 0 ? NULL : &tv);
    } while (ret < 0 && errno == EINTR);
    return ret > 0 && FD_ISSET(STDIN_FILENO, &rfds);
}

/* Move one host key into the queue: a byte, or an ESC-led sequence
 * gathered while more bytes follow within a few milliseconds (a lone
 * ESC pressed by hand arrives alone). Returns 1 if something was
 * queued, 0 if nothing was available or the key is unmapped, -1 on
 * EOF. */
static int host_fetch(int block) {
    if (!stdin_readable(block ? -1 : 0)) return 0;
    uint8_t seq[16];
    ssize_t n = read(STDIN_FILENO, seq, 1);
    if (n <= 0) return -1;               /* EOF or error */
    if (seq[0] == 0x1D) {
        /* ^] leaves the emulator, the way it leaves telnet: a native CP/M
         * session has no other way out (the CCP never exits), and the
         * atexit chain restores the terminal. */
        fprintf(stderr, "\n[exit] ^] pressed\n");
        exit(0);
    }
    size_t len = 1;
    if (seq[0] == 0x1B) {
        /* Terminals deliver a sequence in one burst; 20 ms is generous. */
        while (len < sizeof seq && stdin_readable(len == 1 ? 20000 : 2000)) {
            if (read(STDIN_FILENO, seq + len, 1) <= 0) break;
            len++;
            uint8_t c = seq[len - 1];
            if (len == 2 && c != '[' && c != 'O') break;            /* ESC x: not a sequence */
            if (len >= 3 && (seq[1] == 'O' || (c >= 0x40 && c <= 0x7E))) break;   /* final byte */
        }
    }
    uint8_t out[8];
    size_t k = kaypro_kbd_translate(seq, len, out, sizeof out);
    for (size_t i = 0; i < k; i++) q_push(out[i]);
    return k ? 1 : 0;
}

/* ---- script source ---------------------------------------------------- */

typedef enum { ST_BYTES, ST_WAIT_IDLE, ST_SLEEP, ST_DUMP, ST_MEM, ST_PACE, ST_END } step_kind;
typedef struct {
    step_kind kind;
    uint8_t  *bytes;   /* ST_BYTES */
    size_t    len;
    long      arg;     /* wait-idle polls / sleep ms / dump: with attrs+hex */
    char     *path;    /* ST_DUMP */
} step_t;

static step_t  *steps;
static size_t   n_steps, cap_steps, cur_step;
static char    *script_dir;              /* @dump paths resolve against the script's directory */
static size_t   step_off;                /* ST_BYTES: bytes already queued */
static long     idle_target;             /* ST_WAIT_IDLE: consecutive empty polls still needed */
static long     idle_wanted;             /* ... and the full count, to restart from on output */
static int      idle_armed;
static long     pace_polls;              /* @pace N: empty polls required between keys */
static long     polls_since_read;

/* Console output: the guest is not idle. Restarts a pending wait-idle,
 * because "idle" means no output between polls — WordStar repaints a
 * chunk at a time between CONST polls and only stops when it is done. */
void     kaypro_kbd_note_activity(void) { empty_polls = 0; if (idle_armed) idle_target = idle_wanted; }

static step_t *add_step(step_kind k) {
    if (n_steps == cap_steps) {
        cap_steps = cap_steps ? cap_steps * 2 : 32;
        steps = realloc(steps, cap_steps * sizeof *steps);
        if (!steps) { perror("script"); exit(1); }
    }
    step_t *s = &steps[n_steps++];
    memset(s, 0, sizeof *s);
    s->kind = k;
    return s;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse one text line into ST_BYTES / ST_WAIT_IDLE steps. */
static void parse_text(const char *line) {
    uint8_t buf[1024]; size_t n = 0;
    #define FLUSH_BYTES() do { if (n) { step_t *s = add_step(ST_BYTES); \
        s->bytes = malloc(n); memcpy(s->bytes, buf, n); s->len = n; n = 0; } } while (0)
    for (const char *p = line; *p; p++) {
        if (*p == '~') { FLUSH_BYTES(); step_t *s = add_step(ST_WAIT_IDLE); s->arg = KBD_WAIT_IDLE_DEFAULT; continue; }
        if (*p != '\\') { buf[n++] = (uint8_t)*p; continue; }
        p++;
        switch (*p) {
        case 'r': buf[n++] = '\r'; break;
        case 'n': buf[n++] = '\n'; break;
        case 't': buf[n++] = '\t'; break;
        case 'e': buf[n++] = 0x1B; break;
        case '\\': buf[n++] = '\\'; break;
        case '~': buf[n++] = '~'; break;
        case '^': p++; buf[n++] = (uint8_t)((*p & 0x1F)); break;      /* \^X */
        case 'x': {
            int h = hexval(p[1]), l = hexval(p[2]);
            if (h < 0 || l < 0) { fprintf(stderr, "script: bad \\x escape\n"); exit(1); }
            buf[n++] = (uint8_t)(h * 16 + l); p += 2; break;
        }
        case 0: fprintf(stderr, "script: trailing backslash\n"); exit(1);
        default: buf[n++] = (uint8_t)*p; break;
        }
        if (n >= sizeof buf - 1) FLUSH_BYTES();
    }
    FLUSH_BYTES();
    #undef FLUSH_BYTES
}

int kaypro_kbd_script_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    /* main chdir()s to the disk root before running, so relative @dump
     * paths are resolved against where the script lives, not the disk. */
    {
        char abs[PATH_MAX];
        if (realpath(path, abs)) {
            char *sl = strrchr(abs, '/');
            if (sl) { *sl = 0; script_dir = strdup(abs); }
        }
    }
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        if (line[0] == '#') continue;
        if (line[0] == '@') {
            char cmd[32] = {0}; char arg[1024] = {0};
            sscanf(line + 1, "%31s %1023[^\n]", cmd, arg);
            if (!strcmp(cmd, "wait-idle")) { step_t *s = add_step(ST_WAIT_IDLE); s->arg = arg[0] ? atol(arg) : KBD_WAIT_IDLE_DEFAULT; }
            else if (!strcmp(cmd, "sleep")) { step_t *s = add_step(ST_SLEEP); s->arg = atol(arg); }
            else if (!strcmp(cmd, "pace"))  { step_t *s = add_step(ST_PACE);  s->arg = atol(arg); }
            else if (!strcmp(cmd, "dump"))  {
                step_t *s = add_step(ST_DUMP);
                char *sp = strchr(arg, ' ');
                if (sp) { *sp = 0; s->arg = strstr(sp + 1, "attrs") != NULL; }
                s->path = strdup(arg[0] ? arg : "-");
            }
            else if (!strcmp(cmd, "mem"))   {
                step_t *s = add_step(ST_MEM);
                unsigned addr = 0, len = 16;
                sscanf(arg, "%x %x", &addr, &len);
                s->arg = (long)((addr & 0xFFFF) | ((uint32_t)len << 16));
            }
            else if (!strcmp(cmd, "end"))   { add_step(ST_END); break; }
            else { fprintf(stderr, "%s: unknown script command @%s\n", path, cmd); fclose(f); return -1; }
            continue;
        }
        parse_text(line);
    }
    fclose(f);
    add_step(ST_END);
    cur_step = 0;
    scripted = 1;
    return 0;
}

static void do_dump(const char *path, int attrs) {
    char full[PATH_MAX];
    if (path[0] != '-' && path[0] != '/' && script_dir) {
        snprintf(full, sizeof full, "%s/%s", script_dir, path);
        path = full;
    }
    FILE *f = strcmp(path, "-") == 0 ? stdout : fopen(path, "w");
    if (!f) { perror(path); return; }
    kaypro_video_dump(f, attrs);
    if (f != stdout) fclose(f); else fflush(f);
}

/* Advance the script until it has queued bytes, is waiting for idle,
 * or has ended. `reading` says the guest is blocking for a key, which
 * satisfies any pending wait-idle outright. Returns 1 if bytes are
 * available, 0 if waiting for idle, -1 at the end. */
static int script_advance(int reading) {
    for (;;) {
        if (!q_empty()) return 1;
        step_t *s = &steps[cur_step];
        switch (s->kind) {
        case ST_BYTES:
            /* The queue holds QSIZE-1 bytes; a long line is fed in as
             * many pieces as it takes, so nothing is dropped. */
            while (step_off < s->len && !q_full()) q_push(s->bytes[step_off++]);
            if (step_off == s->len) { step_off = 0; cur_step++; }
            return 1;
        case ST_WAIT_IDLE:
            if (!idle_armed) { idle_armed = 1; idle_wanted = idle_target = s->arg; }
            if (reading || idle_target <= 0) { idle_armed = 0; cur_step++; continue; }
            return 0;
        case ST_SLEEP:
            usleep((useconds_t)(s->arg * 1000));
            cur_step++;
            continue;
        case ST_PACE:
            pace_polls = s->arg;
            cur_step++;
            continue;
        case ST_DUMP:
            do_dump(s->path, (int)s->arg);
            cur_step++;
            continue;
        case ST_MEM: {
            unsigned addr = (unsigned)s->arg & 0xFFFF, len = (unsigned)s->arg >> 16;
            if (attached_cpu) {
                for (unsigned i = 0; i < len; i += 16) {
                    printf("%04X:", (addr + i) & 0xFFFF);
                    for (unsigned j = 0; j < 16 && i + j < len; j++)
                        printf(" %02X", attached_cpu->mem[(addr + i + j) & 0xFFFF]);
                    printf("  ");
                    for (unsigned j = 0; j < 16 && i + j < len; j++) {
                        uint8_t c = attached_cpu->mem[(addr + i + j) & 0xFFFF];
                        putchar(c >= 0x20 && c < 0x7F ? c : '.');
                    }
                    putchar('\n');
                }
                fflush(stdout);
            }
            cur_step++;
            continue;
        }
        default:
            return -1;
        }
    }
}

/* ---- the three primitives ------------------------------------------- */

/* Scripted pacing: a queued byte is withheld until the guest has polled
 * empty pace_polls times since it last read one. */
static int paced_out(void) {
    if (!scripted || pace_polls <= 0 || polls_since_read >= pace_polls) return 0;
    polls_since_read++;
    return 1;
}

int kaypro_kbd_poll(void) {
    n_polls++;
    if (!q_empty()) {
        if (paced_out()) return 0;
        empty_polls = 0;
        return 1;
    }
    if (scripted) {
        int r = script_advance(0);
        if (r == 1) { if (paced_out()) return 0; empty_polls = 0; return 1; }
        /* Waiting for idle: this empty poll is what we're counting. */
        if (r == 0 && idle_armed) idle_target--;
        empty_polls++;
        if (empty_polls > max_quiet_streak) max_quiet_streak = empty_polls;
        /* Script over and the guest has done nothing but poll for a
         * while: it is waiting for a key that will never come. End the
         * session the same way a blocking read at end-of-script does. */
        if (r < 0 && empty_polls >= KBD_IDLE_END_POLLS) {
            fprintf(stderr, "[exit] script ended, guest polling for input "
                            "(%llu polls, longest quiet run %u)\n",
                    (unsigned long long)n_polls, max_quiet_streak);
            exit(0);
        }
        /* No idle sleep in scripted mode: nobody is waiting at a keyboard,
         * and programs pace their message delays with CONST polls — at
         * guest speed those take milliseconds, with a sleep per poll they
         * would take minutes. */
        return 0;
    }
    if (host_fetch(0) == 1) { empty_polls = 0; return 1; }
    if (++empty_polls >= KBD_IDLE_POLLS) usleep(1000);
    if (empty_polls > max_quiet_streak) max_quiet_streak = empty_polls;
    return 0;
}

int kaypro_kbd_read(void) {
    n_reads++;
    empty_polls = 0;
    polls_since_read = 0;
    if (!q_empty()) return q_pop();
    if (scripted) {
        int r = script_advance(1);
        if (r == 1) return q_pop();
        return KBD_END;
    }
    for (;;) {
        int r = host_fetch(1);
        if (r == 1) return q_pop();
        if (r < 0) {
            /* stdin EOF: hand the guest CP/M's ^Z a few times (programs
             * reading piped input end on it), then end the session — a
             * native CCP would otherwise prompt forever. */
            static int eof_reads;
            if (++eof_reads > 8) {
                fprintf(stderr, "[exit] stdin closed, guest still reading\n");
                exit(0);
            }
            return 0x1A;
        }
        /* r == 0: an unmapped key was swallowed; keep waiting */
    }
}

void kaypro_kbd_wait(void) {
    if (!q_empty()) return;
    if (scripted) {
        if (script_advance(1) < 0) {
            /* Script over and the guest is idling in HALT: nothing will
             * ever wake it. End the session as a blocking read would. */
            fprintf(stderr, "[exit] script ended, guest halted\n");
            exit(0);
        }
        return;
    }
    while (host_fetch(1) == 0) { }       /* swallow unmapped keys until something arrives (or EOF) */
}
