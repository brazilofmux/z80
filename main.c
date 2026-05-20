#include "core/z80.h"
#include "cpm/cpm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>

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
    raw.c_cc[VMIN] = 1;     /* block until at least one character for CONIN */
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
    const char *disk_root = NULL;
    const char *prog = NULL;

    /* New flexible argument handling for fast real-binary iteration:
     *   ./z80-monster
     *   ./z80-monster foo.com
     *   ./z80-monster disks/zork1/
     *   ./z80-monster disks/zork1/ zork1.com
     *   ./z80-monster disks/zork1/zork1.com
     */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-s") == 0) {
            show_stats = 1;
        } else if (argv[i][0] != '-') {
            if (!disk_root && !prog) {
                /* First non-option argument */
                if (strstr(argv[i], ".com") || strstr(argv[i], ".COM")) {
                    prog = argv[i];
                } else {
                    /* Treat as directory / disk root */
                    disk_root = argv[i];
                }
            } else if (disk_root && !prog) {
                prog = argv[i];
            }
        }
    }

    if (!prog && !disk_root) {
        print_banner();
        usage(argv[0]);
        return 0;
    }

    /* If user gave us a .com with a path, use its directory as the disk root */
    char effective_root[PATH_MAX] = {0};
    if (prog) {
        /* Try to extract directory from the program path */
        char *last_slash = strrchr((char*)prog, '/');
        if (last_slash) {
            size_t len = last_slash - prog;
            if (len < sizeof(effective_root)) {
                strncpy(effective_root, prog, len);
                effective_root[len] = 0;
            }
        }
    }

    const char *root_to_use = disk_root ? disk_root : (effective_root[0] ? effective_root : NULL);

    if (root_to_use) {
        cpm_set_a_root(root_to_use);
        /* chdir so the host directory is the root of A: */
        if (chdir(root_to_use) != 0) {
            fprintf(stderr, "Warning: could not chdir into '%s'\n", root_to_use);
        }
        /* Clear a_root — we rely on CWD now. Prevents double-prefixing in make_host_path. */
        cpm_set_a_root(NULL);
    }

    /* If no explicit program was given but we have a disk root, try to find one */
    char auto_prog[PATH_MAX] = {0};
    if (!prog && root_to_use) {
        /* Very simple heuristic: look for the first .com in the directory */
        DIR *d = opendir(".");
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d))) {
                size_t len = strlen(ent->d_name);
                if (len > 4 &&
                    (strcasecmp(ent->d_name + len - 4, ".com") == 0)) {
                    strncpy(auto_prog, ent->d_name, sizeof(auto_prog) - 1);
                    prog = auto_prog;
                    break;
                }
            }
            closedir(d);
        }
    }

    const char *final_prog = prog ? prog : auto_prog;

    if (!final_prog || !*final_prog) {
        fprintf(stderr, "No .com program specified or found in '%s'\n", root_to_use ? root_to_use : ".");
        return 1;
    }

    z80_cpu_t cpu;
    z80_cpu_init(&cpu);

    if (cpm_load_com(&cpu, final_prog) != 0) {
        fprintf(stderr, "Failed to load %s\n", final_prog);
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
        if (cpu.insn_count > 500000000ULL) {
            fprintf(stderr, "Safety limit (500M insns) reached — possible infinite loop\n");
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
