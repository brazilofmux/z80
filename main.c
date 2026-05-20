#include "core/z80.h"
#include "cpm/cpm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

static struct termios orig_termios;
static int term_raw = 0;

static void print_banner(void) {
    printf("z80-monster — 10 BIPS CP/M Monster\n");
    printf("Built on %s %s\n\n", __DATE__, __TIME__);
}

static void enter_raw_mode(void) {
    if (term_raw) return;
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    term_raw = 1;
}

static void leave_raw_mode(void) {
    if (!term_raw) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    term_raw = 0;
}

static void usage(const char *prog) {
    printf("Usage: %s [options] <program.com>\n\n", prog);
    printf("Options:\n");
    printf("  -i       Force interpreter (default for now)\n");
    printf("  -s       Show stats on exit\n");
    printf("  -h       This help\n\n");
    printf("Example:\n");
    printf("  %s tests/hello.com\n\n", prog);
    printf("This is the very first bring-up of the monster.\n");
    printf("Only a handful of Z80 opcodes + BDOS 9 are implemented.\n");
}

int main(int argc, char **argv) {
    int show_stats = 0;
    const char *prog = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-s") == 0) {
            show_stats = 1;
        } else if (argv[i][0] != '-') {
            prog = argv[i];
        }
    }

    if (!prog) {
        print_banner();
        usage(argv[0]);
        return 0;
    }

    z80_cpu_t cpu;
    z80_cpu_init(&cpu);

    if (cpm_load_com(&cpu, prog) != 0) {
        fprintf(stderr, "Failed to load %s\n", prog);
        return 1;
    }

    enter_raw_mode();
    atexit(leave_raw_mode);

    /* Run the interpreter until it hits a terminating condition */
    for (;;) {
        /* Direct termination conditions (very common in real .COMs) */
        if (cpu.pc == 0 && cpu.insn_count > 4) {
            /* Classic CP/M termination: JP 0, RET to 0 on stack, etc. */
            break;
        }

        int rc = z80_step(&cpu);
        if (rc < 0) {
            fprintf(stderr, "CPU stopped with error at PC=%04X\n", cpu.pc);
            break;
        }
        if (rc > 0) {
            /* Clean CP/M exit via BDOS 0 / WBOOT / BIOS WBOOT */
            break;
        }
        if (cpu.insn_count > 50000000ULL) {
            fprintf(stderr, "Safety limit (50M insns) reached — possible infinite loop\n");
            break;
        }
    }

    if (show_stats) {
        printf("\n--- stats ---\n");
        printf("Instructions: %llu\n", (unsigned long long)cpu.insn_count);
    }

    /* cleanup */
    free(cpu.mem);
    return 0;
}
