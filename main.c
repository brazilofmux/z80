#include "core/z80.h"
#include "cpm/cpm.h"
#include "dbt/dbt.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>

static struct termios orig_termios;
static int term_raw = 0;
int trace_block_ops = 0;

static void print_banner(void) {
    printf("z80-monster — 10 BIPS CP/M Monster\n");
    printf("Built on %s %s\n\n", __DATE__, __TIME__);
}

static void enter_raw_mode(void) {
    if (term_raw) return;
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    /* Pass keystrokes through verbatim: don't let the tty driver translate
     * CR <-> NL. CP/M uses 0x0D as line terminator, and with ICRNL on,
     * the ENTER key (which sends 0x0D) was being delivered to the guest
     * as 0x0A, so CP/M-era programs like Zork ignored it. */
    raw.c_iflag &= ~(ICRNL | INLCR | IGNCR | IXON);
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
    printf("  -i       Force interpreter (default)\n");
    printf("  -j       Use the JIT (DBT) — falls back to interp if unavailable\n");
    printf("  -s       Show stats on exit\n");
    printf("  -d       Print BDOS/BIOS/disk startup tracing\n");
    printf("  -T       Trace block ops (periodic register dumps)\n");
    printf("  -h       This help\n\n");
    printf("Example:\n");
    printf("  %s tests/hello.com\n\n", prog);
    printf("This is the very first bring-up of the monster.\n");
    printf("Only a handful of Z80 opcodes + BDOS 9 are implemented.\n");
}

int main(int argc, char **argv) {
    int show_stats = 0;
    int use_jit = 0;
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
        } else if (strcmp(argv[i], "-i") == 0) {
            use_jit = 0;
        } else if (strcmp(argv[i], "-j") == 0) {
            use_jit = 1;
        } else if (strcmp(argv[i], "-d") == 0) {
            cpm_debug = 1;
        } else if (strcmp(argv[i], "-T") == 0) {
            trace_block_ops = 1;
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

    /* Helpful for the common "just give me the directory" usage */
    if (cpm_debug && disk_root && !strstr(final_prog, "/")) {
        fprintf(stderr, "Running %s from disk image '%s'\n", final_prog, disk_root);
    }

    z80_cpu_t cpu;
    z80_cpu_init(&cpu);

    if (cpm_load_com(&cpu, final_prog) != 0) {
        fprintf(stderr, "Failed to load %s\n", final_prog);
        return 1;
    }

    enter_raw_mode();
    atexit(leave_raw_mode);

    if (use_jit && !dbt_jit_available()) {
        fprintf(stderr, "[dbt] JIT not available on this host — falling back to interp\n");
        use_jit = 0;
    }

    /* dbt is ~1 MB (block cache + code-coverage bitmap); heap-allocate so
     * we don't push main's stack frame into the macOS guard page. */
    z80_dbt_t *dbt = NULL;
    int used_jit = 0;
    if (use_jit) {
        dbt = malloc(sizeof(*dbt));
        if (!dbt || dbt_init(dbt, &cpu) != 0) {
            fprintf(stderr, "[dbt] init failed — falling back to interp\n");
            free(dbt);
            dbt = NULL;
            use_jit = 0;
        }
    }

    if (use_jit) {
        used_jit = 1;
        int rc = dbt_run(dbt);
        if (rc < 0) {
            fprintf(stderr, "[exit] DBT stopped with error at PC=%04X\n", cpu.pc);
        } else {
            fprintf(stderr, "[exit] DBT clean after %llu insns\n",
                    (unsigned long long)cpu.insn_count);
        }
    } else {
        /* Run the interpreter until it hits a terminating condition */
        for (;;) {
            /* Direct termination conditions (very common in real .COMs) */
            if (cpu.pc == 0 && cpu.insn_count > 4) {
                /* Classic CP/M termination: JP 0, RET to 0 on stack, etc. */
                fprintf(stderr, "[exit] PC=0000 after %llu insns (RET/JP 0 warmboot)\n",
                        (unsigned long long)cpu.insn_count);
                break;
            }

            int rc = z80_step(&cpu);
            if (rc < 0) {
                fprintf(stderr, "CPU stopped with error at PC=%04X\n", cpu.pc);
                break;
            }
            if (rc > 0) {
                /* Clean CP/M exit via BDOS 0 / WBOOT / BIOS WBOOT */
                fprintf(stderr, "[exit] BDOS/BIOS warmboot after %llu insns (C=%02X)\n",
                        (unsigned long long)cpu.insn_count, cpu.c);
                break;
            }
            if (trace_block_ops && (cpu.insn_count & 0x3FFFFFF) == 0 && cpu.insn_count) {
                fprintf(stderr, "[%llu] PC=%04X SP=%04X HL=%04X DE=%04X BC=%04X AF=%04X IX=%04X IY=%04X\n",
                        (unsigned long long)cpu.insn_count, cpu.pc, cpu.sp,
                        cpu.hl, cpu.de, cpu.bc, cpu.af, cpu.ix, cpu.iy);
            }
            if (cpu.insn_count > 50000000000ULL) {
                fprintf(stderr, "Safety limit (50B insns) reached — bug or wait?\n");
                break;
            }
        }
    }

    if (show_stats) {
        printf("\n--- stats ---\n");
        printf("Instructions: %llu\n", (unsigned long long)cpu.insn_count);
        if (used_jit) {
            printf("JIT:\n");
            dbt_print_stats(dbt, stdout);
        }
    }

    /* cleanup */
    if (used_jit) {
        dbt_cleanup(dbt);
        free(dbt);
    }
    free(cpu.mem);
    return 0;
}
