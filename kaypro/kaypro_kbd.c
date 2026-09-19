/* kaypro_kbd.c — console input: host terminal or script. See the header. */
#include "kaypro_kbd.h"
#include "kaypro_video.h"
#include <errno.h>
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

static uint64_t n_polls, n_reads;
static unsigned empty_polls;             /* consecutive polls that found nothing */
static int scripted;

uint64_t kaypro_kbd_polls(void) { return n_polls; }
uint64_t kaypro_kbd_reads(void) { return n_reads; }
int      kaypro_kbd_scripted(void) { return scripted; }
void     kaypro_kbd_note_activity(void) { empty_polls = 0; }

void kaypro_kbd_init(void) {
    q_head = q_tail = 0;
    n_polls = n_reads = 0;
    empty_polls = 0;
    scripted = 0;
}

/* ---- host terminal source -------------------------------------------- */

/* Non-blocking: move one byte from stdin into the queue if there is one. */
static int host_fetch(int block) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    struct timeval tv = {0, 0};
    int ret;
    do {
        ret = select(STDIN_FILENO + 1, &rfds, NULL, NULL, block ? NULL : &tv);
    } while (ret < 0 && errno == EINTR);
    if (ret <= 0 || !FD_ISSET(STDIN_FILENO, &rfds)) return 0;
    unsigned char ch;
    ssize_t n = read(STDIN_FILENO, &ch, 1);
    if (n <= 0) return -1;               /* EOF or error */
    q_push(ch);
    return 1;
}

/* ---- script source ---------------------------------------------------- */

typedef enum { ST_BYTES, ST_WAIT_IDLE, ST_SLEEP, ST_DUMP, ST_END } step_kind;
typedef struct {
    step_kind kind;
    uint8_t  *bytes;   /* ST_BYTES */
    size_t    len;
    long      arg;     /* wait-idle polls / sleep ms */
    char     *path;    /* ST_DUMP */
} step_t;

static step_t  *steps;
static size_t   n_steps, cap_steps, cur_step;
static size_t   step_off;                /* ST_BYTES: bytes already queued */
static long     idle_target;             /* ST_WAIT_IDLE: polls still needed */
static int      idle_armed;

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
        if (*p == '~') { FLUSH_BYTES(); step_t *s = add_step(ST_WAIT_IDLE); s->arg = 20; continue; }
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
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        if (line[0] == '#') continue;
        if (line[0] == '@') {
            char cmd[32] = {0}; char arg[1024] = {0};
            sscanf(line + 1, "%31s %1023[^\n]", cmd, arg);
            if (!strcmp(cmd, "wait-idle")) { step_t *s = add_step(ST_WAIT_IDLE); s->arg = arg[0] ? atol(arg) : 20; }
            else if (!strcmp(cmd, "sleep")) { step_t *s = add_step(ST_SLEEP); s->arg = atol(arg); }
            else if (!strcmp(cmd, "dump"))  { step_t *s = add_step(ST_DUMP); s->path = strdup(arg[0] ? arg : "-"); }
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

static void do_dump(const char *path) {
    FILE *f = strcmp(path, "-") == 0 ? stdout : fopen(path, "w");
    if (!f) { perror(path); return; }
    kaypro_video_dump(f, 0);
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
            if (!idle_armed) { idle_armed = 1; idle_target = s->arg; }
            if (reading || idle_target <= 0) { idle_armed = 0; cur_step++; continue; }
            return 0;
        case ST_SLEEP:
            usleep((useconds_t)(s->arg * 1000));
            cur_step++;
            continue;
        case ST_DUMP:
            do_dump(s->path);
            cur_step++;
            continue;
        default:
            return -1;
        }
    }
}

/* ---- the three primitives ------------------------------------------- */

int kaypro_kbd_poll(void) {
    n_polls++;
    if (!q_empty()) { empty_polls = 0; return 1; }
    if (scripted) {
        int r = script_advance(0);
        if (r == 1) { empty_polls = 0; return 1; }
        /* Waiting for idle: this empty poll is what we're counting. */
        if (r == 0 && idle_armed) idle_target--;
        empty_polls++;
        /* Script over and the guest has done nothing but poll for a
         * while: it is waiting for a key that will never come. End the
         * session the same way a blocking read at end-of-script does. */
        if (r < 0 && empty_polls >= KBD_IDLE_POLLS) {
            fprintf(stderr, "[exit] script ended, guest polling for input\n");
            exit(0);
        }
        /* Let the guest's own polls pace a waiting script; a tiny sleep
         * keeps a hot poll loop from burning the host. */
        if (empty_polls >= KBD_IDLE_POLLS) usleep(1000);
        return 0;
    }
    if (host_fetch(0) == 1) { empty_polls = 0; return 1; }
    if (++empty_polls >= KBD_IDLE_POLLS) usleep(1000);
    return 0;
}

int kaypro_kbd_read(void) {
    n_reads++;
    empty_polls = 0;
    if (!q_empty()) return q_pop();
    if (scripted) {
        int r = script_advance(1);
        if (r == 1) return q_pop();
        return KBD_END;
    }
    int r = host_fetch(1);
    if (r == 1) return q_pop();
    return 0x1A;                         /* stdin EOF: CP/M's ^Z */
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
    host_fetch(1);
}
