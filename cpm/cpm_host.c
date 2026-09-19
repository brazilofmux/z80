/* cpm_host.c — the host half of the CP/M 2.2 BIOS (cpm/cpm22/bios.asm).
 *
 * When the emulator boots a real system image, the guest runs Digital
 * Research's CCP and BDOS and our BIOS as ordinary Z80 code. The only
 * way that code reaches the host is through the ports below: console
 * bytes in and out, 128-byte disk records in and out of the mounted
 * images, a "put the system back" for warm boot, and an exit. Every
 * port instruction traps to the interpreter, which calls these hooks.
 *
 *   E0 IN   CONST: 0 / FF
 *   E1 IN   CONIN (blocks until a key)        OUT  CONOUT
 *   E2                                        OUT  LIST (host file or nothing)
 *   E3 IN   READER (always ^Z)                OUT  PUNCH (dropped)
 *   E5                                        OUT  0: reload CCP+BDOS  1: exit emulator
 *   E7                                        OUT  debug byte (-d shows it)
 *   E8 IN   type of the selected drive         OUT  select drive 0-15
 *          (0 none, 1 floppy, 2 hard disk)
 *   E9/EA                                     OUT  track low / high
 *   EB/EC                                     OUT  record (sector) low / high, 0-based
 *   ED/EE                                     OUT  DMA low / high
 *   EF IN   result of the last command        OUT  0 read, 1 write
 *
 * Images are raw, track-major, 128-byte records, no skew — the two
 * formats in tools/diskdefs (cpmtools-compatible), told apart by size.
 * Host writes into guest memory go through z80_mem_host_wrote() so the
 * JIT drops any translation of code they overwrite.
 */
#include "cpm.h"
#include "../kaypro/kaypro_kbd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define HOST_DRIVES 16

typedef struct {
    FILE    *fp;
    char    *path;
    int      type;          /* 0 none, 1 fd, 2 hd */
    uint32_t spt;           /* records per track */
    uint32_t tracks;
    int      readonly;
} host_drive_t;

static host_drive_t drives[HOST_DRIVES];
static int      cur_drive = -1;
static uint16_t cur_track, cur_sector, cur_dma;
static uint8_t  last_result;
static uint8_t  latch[256];
static FILE    *list_fp;

/* The system image (CCP+BDOS+BIOS) as loaded, for warm-boot reloads. */
static uint8_t  *sys_image;
static uint32_t  sys_len;
static uint16_t  sys_base;

int cpm_traps_enabled = 1;

/* ---- images ---------------------------------------------------------- */

static void cpm_host_flush(void) {
    for (int i = 0; i < HOST_DRIVES; i++)
        if (drives[i].fp) fflush(drives[i].fp);
}

int cpm_host_mount(int drive, const char *path) {
    static int registered;
    if (!registered) { atexit(cpm_host_flush); registered = 1; }
    if (drive < 0 || drive >= HOST_DRIVES) return -1;
    struct stat st;
    if (stat(path, &st) != 0) { perror(path); return -1; }
    host_drive_t *d = &drives[drive];
    if (st.st_size == 409600L)        { d->type = 1; d->spt = 40; d->tracks = 80; }
    else if (st.st_size == 8388608L)  { d->type = 2; d->spt = 64; d->tracks = 1024; }
    else {
        fprintf(stderr, "%s: %lld bytes is neither a 400K floppy (409600) nor an 8 MB hard disk (8388608) image\n",
                path, (long long)st.st_size);
        return -1;
    }
    d->fp = fopen(path, "r+b");
    d->readonly = 0;
    if (!d->fp) { d->fp = fopen(path, "rb"); d->readonly = 1; }
    if (!d->fp) { perror(path); d->type = 0; return -1; }
    d->path = strdup(path);
    if (cpm_debug)
        fprintf(stderr, "[host] %c: %s (%s%s)\n", 'A' + drive, path,
                d->type == 1 ? "floppy" : "hard disk", d->readonly ? ", read-only" : "");
    return 0;
}

int cpm_host_drive_type(int drive) {
    return (drive >= 0 && drive < HOST_DRIVES) ? drives[drive].type : 0;
}

void cpm_host_set_system(const uint8_t *image, uint32_t len, uint16_t base) {
    free(sys_image);
    sys_image = malloc(len);
    memcpy(sys_image, image, len);
    sys_len = len;
    sys_base = base;
}

static void reload_system(z80_cpu_t *cpu) {
    if (!sys_image) return;
    /* Only the CCP and BDOS: the BIOS is running this code. */
    uint32_t n = CPM_BIOS_BASE - sys_base;
    if (n > sys_len) n = sys_len;
    memcpy(&cpu->mem[sys_base], sys_image, n);
    z80_mem_host_wrote(cpu, sys_base, n);
}

static int disk_io(z80_cpu_t *cpu, int write) {
    if (cur_drive < 0 || cur_drive >= HOST_DRIVES || !drives[cur_drive].fp) return 1;
    host_drive_t *d = &drives[cur_drive];
    if (cur_track >= d->tracks || cur_sector >= d->spt) {
        if (cpm_debug)
            fprintf(stderr, "[host] %c: %s beyond image: track %u sector %u\n",
                    'A' + cur_drive, write ? "write" : "read", cur_track, cur_sector);
        return 1;
    }
    if (write && d->readonly) return 1;
    long off = ((long)cur_track * d->spt + cur_sector) * 128L;
    if (fseek(d->fp, off, SEEK_SET) != 0) return 1;
    uint8_t buf[128];
    if (write) {
        memcpy(buf, &cpu->mem[cur_dma], 128);
        if (fwrite(buf, 1, 128, d->fp) != 128) return 1;
        /* No flush per record: a sort writing 55K records four times
         * over was spending most of its wall clock in write(2). The
         * images are flushed at exit (cpm_host_flush) and by the OS. */
    } else {
        size_t n = fread(buf, 1, 128, d->fp);
        if (n < 128) memset(buf + n, 0xE5, 128 - n);   /* past EOF of a sparse image: blank */
        /* The DMA buffer may straddle 0xFFFF only in theory; keep it simple. */
        for (int i = 0; i < 128; i++) cpu->mem[(uint16_t)(cur_dma + i)] = buf[i];
        z80_mem_host_wrote(cpu, cur_dma, 128);
    }
    return 0;
}

/* ---- the ports ------------------------------------------------------- */

/* Host events (HUD tick, window resize, ^C) are flagged from a signal
 * handler and normally serviced by the run loops between blocks. A
 * native CP/M program idling at the keyboard never gets there: its
 * CONST loop is a chain of translated blocks with the port access
 * inlined, so the console ports service the flag themselves. */
static inline void host_events(z80_cpu_t *cpu) {
    if (cpu->host_event) {
        cpu->host_event = 0;
        if (cpu->on_host_event) cpu->on_host_event(cpu);
    }
}

static uint8_t host_in(z80_cpu_t *cpu, uint8_t port, uint8_t high) {
    (void)high;
    switch (port) {
    case 0xE0: host_events(cpu); return cpm_constat();
    case 0xE1: host_events(cpu); return cpm_conin();
    case 0xE3: return 0x1A;                                   /* reader: end of tape */
    case 0xE8: return (uint8_t)cpm_host_drive_type(cur_drive);
    case 0xEF: return last_result;
    default:   return latch[port];                            /* anything else: the latch */
    }
}

static void host_out(z80_cpu_t *cpu, uint8_t port, uint8_t high, uint8_t val) {
    (void)high;
    latch[port] = val;
    switch (port) {
    case 0xE1: cpm_conout(val); break;
    case 0xE2: if (list_fp) { fputc(val, list_fp); fflush(list_fp); } break;
    case 0xE3: break;                                         /* punch: dropped */
    case 0xE5:
        if (val == 0) reload_system(cpu);
        else if (val == 1) {
            fflush(stdout);
            fprintf(stderr, "[exit] guest requested exit after %llu insns\n",
                    (unsigned long long)cpu->insn_count);
            exit(0);
        }
        break;
    case 0xE7: if (cpm_debug) fprintf(stderr, "[port E7] %02X\n", val); break;
    case 0xE8: cur_drive = val < HOST_DRIVES ? val : -1; break;
    case 0xE9: cur_track  = (uint16_t)((cur_track & 0xFF00) | val); break;
    case 0xEA: cur_track  = (uint16_t)((cur_track & 0x00FF) | (val << 8)); break;
    case 0xEB: cur_sector = (uint16_t)((cur_sector & 0xFF00) | val); break;
    case 0xEC: cur_sector = (uint16_t)((cur_sector & 0x00FF) | (val << 8)); break;
    case 0xED: cur_dma    = (uint16_t)((cur_dma & 0xFF00) | val); break;
    case 0xEE: cur_dma    = (uint16_t)((cur_dma & 0x00FF) | (val << 8)); break;
    case 0xEF: last_result = (uint8_t)disk_io(cpu, val == 1); break;
    default: break;
    }
}

void cpm_host_install(z80_cpu_t *cpu, const char *list_path) {
    memset(latch, 0xFF, sizeof latch);
    if (list_path && *list_path) {
        list_fp = fopen(list_path, "ab");
        if (!list_fp) perror(list_path);
    }
    cpu->port_in  = host_in;
    cpu->port_out = host_out;
    cpm_traps_enabled = 0;      /* the guest has a real BDOS and BIOS now */
}

/* Boot: put the system image in place and start the BIOS cold boot. */
int cpm_host_boot(z80_cpu_t *cpu, const char *system_path) {
    FILE *f = fopen(system_path, "rb");
    if (!f) { perror(system_path); return -1; }
    uint8_t image[0x10000];
    size_t n = fread(image, 1, sizeof image, f);
    fclose(f);
    if (n < 0x1600 + 51) {
        fprintf(stderr, "%s: too small for CCP+BDOS+BIOS (%zu bytes)\n", system_path, n);
        return -1;
    }
    const uint16_t base = CPM_CCP_BASE;
    if (base + n > 0x10000) n = 0x10000 - base;
    memcpy(&cpu->mem[base], image, n);
    cpm_host_set_system(image, (uint32_t)n, base);
    cpu->pc = CPM_BIOS_BASE;    /* cold boot */
    cpu->sp = 0x80;
    return 0;
}
