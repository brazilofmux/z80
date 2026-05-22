/* cpm_disk.c — Basic CP/M 2.2 disk / FCB support
 *
 * Maps the host current working directory as CP/M drive A:
 * Supports enough BDOS calls for many real programs to load overlays
 * and data files (Open + Read Sequential is the minimum useful set).
 */

#include "cpm.h"
#include "../core/z80.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>

#define MAX_OPEN_FILES  8

typedef struct {
    FILE   *fp;
    uint8_t fcb_copy[CPM_FCB_SIZE];
    uint16_t logical_record;   /* current 128-byte record number (extent*128 + CR) */
} open_file_t;

static uint16_t current_dma = CPM_DEFAULT_DMA;
static open_file_t open_files[MAX_OPEN_FILES];
static uint16_t open_fcb_addr[MAX_OPEN_FILES];   /* guest FCB address that owns each slot (0 = free) */

/* Private bounce buffer for all disk I/O.
 * We always do the actual fread/fwrite into this buffer first, then
 * copy to/from the guest's current_dma. This prevents large amounts
 * of file data from permanently sitting in whatever memory the game
 * currently has mapped at its DMA address (a very common source of
 * "first command prompt is full of garbage" with Infocom titles).
 */
static uint8_t disk_io_buf[128];

static void make_host_path(const char *name, char *out, size_t outlen);   /* forward decl */

/* Drive A: root directory on the host.
 * If empty, we use the current working directory (original behavior).
 */
static char a_root[PATH_MAX] = {0};

/* Simple search context for BDOS 17/18 (Search First/Next).
 * CP/M only supports one active search at a time per process.
 */
static DIR *search_dir = NULL;
static char search_pattern[16];   /* 8.3 pattern from FCB */

/* ------------------------------------------------------------------ */

void cpm_disk_init(void)
{
    memset(open_files, 0, sizeof(open_files));
    memset(open_fcb_addr, 0, sizeof(open_fcb_addr));
    current_dma = CPM_DEFAULT_DMA;
}

void cpm_set_dma(uint16_t addr)
{
    current_dma = addr;
}

/* Convert a CP/M FCB name (8.3, space-padded) into a host filename.
 * Very simple translation — good enough for most .COMs.
 */
static void fcb_to_host_name(const uint8_t *fcb, char *host, size_t hostlen)
{
    char name[9], ext[4];
    int i;

    /* Filename */
    memcpy(name, fcb + 1, 8);
    name[8] = 0;
    for (i = 7; i >= 0 && name[i] == ' '; i--) name[i] = 0;

    /* Extension */
    memcpy(ext, fcb + 9, 3);
    ext[3] = 0;
    for (i = 2; i >= 0 && ext[i] == ' '; i--) ext[i] = 0;

    if (ext[0])
        snprintf(host, hostlen, "%s.%s", name, ext);
    else
        snprintf(host, hostlen, "%s", name);
}

/* Find a free slot in the open file table. Returns index or -1. */
static int alloc_file_slot(void)
{
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (open_files[i].fp == NULL)
            return i;
    }
    return -1;
}

/* Compute host file offset from logical 128-byte record number */
static long logical_record_to_offset(uint16_t rec)
{
    return (long)rec * 128;
}

/* Look up open slot by the guest FCB address (the value passed in DE to the BDOS call).
 * This is much more compatible than abusing the drive byte in the FCB.
 */
static int get_file_slot_from_fcb(uint16_t fcb_addr)
{
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (open_files[i].fp != NULL && open_fcb_addr[i] == fcb_addr)
            return i;
    }
    return -1;
}

/* Extract 24-bit random record number from FCB (offsets 33-35) */
static uint32_t get_random_record(const uint8_t *fcb)
{
    return (uint32_t)fcb[33]
         | ((uint32_t)fcb[34] << 8)
         | ((uint32_t)fcb[35] << 16);
}

/* Very simple CP/M 8.3 pattern matcher.
 * Supports '*' in name or ext as "match anything".
 * '?' is treated as single-char wildcard.
 */
static int fcb_name_matches(const uint8_t *fcb, const char *host_name)
{
    char fcb_name[9], fcb_ext[4];
    char host_base[64], host_ext[16];

    /* Extract FCB pattern */
    memcpy(fcb_name, fcb + 1, 8); fcb_name[8] = 0;
    memcpy(fcb_ext,  fcb + 9, 3); fcb_ext[3] = 0;

    /* Split host filename */
    const char *dot = strrchr(host_name, '.');
    if (dot) {
        strncpy(host_base, host_name, dot - host_name);
        host_base[dot - host_name] = 0;
        strcpy(host_ext, dot + 1);
    } else {
        strcpy(host_base, host_name);
        host_ext[0] = 0;
    }

    /* Crude but effective matching */
    int match_name = (fcb_name[0] == '*' || strncasecmp(fcb_name, host_base, 8) == 0);
    int match_ext  = (fcb_ext[0] == '*'  || strncasecmp(fcb_ext,  host_ext,  3) == 0);

    /* Handle simple '?' — for now treat as "anything" if we see it */
    if (!match_name) {
        match_name = 1;
        for (int i = 0; i < 8 && fcb_name[i]; i++) {
            if (fcb_name[i] != '?' && tolower(fcb_name[i]) != tolower(host_base[i] ? host_base[i] : ' ')) {
                match_name = 0;
                break;
            }
        }
    }
    if (!match_ext && fcb_ext[0]) {
        match_ext = 1;
        for (int i = 0; i < 3 && fcb_ext[i]; i++) {
            if (fcb_ext[i] != '?' && tolower(fcb_ext[i]) != tolower(host_ext[i] ? host_ext[i] : ' ')) {
                match_ext = 0;
                break;
            }
        }
    }

    return match_name && match_ext;
}

/* ------------------------------------------------------------------ */

int cpm_bdos_open_file(z80_cpu_t *cpu, uint16_t fcb_addr)
{
    uint8_t *fcb = &cpu->mem[fcb_addr];
    char host_name[64];
    FILE *fp;

    /* If an existing slot owns this FCB, retire it first. MS COBOL (and
     * many CP/M apps) reuse one FCB for a series of files — the FCB
     * itself holds the directory state in real CP/M, so re-opening is
     * "switch to a different file at the same FCB address," not
     * "duplicate open." Without this, get_file_slot_from_fcb keeps
     * returning the first (stale) match and reads come from the wrong
     * file. The compiler's "?Overlay Not Found" was exactly this. */
    {
        int prev = get_file_slot_from_fcb(fcb_addr);
        if (prev >= 0) {
            fclose(open_files[prev].fp);
            open_files[prev].fp = NULL;
            open_fcb_addr[prev] = 0;
        }
    }

    fcb_to_host_name(fcb, host_name, sizeof(host_name));
    {
        char full[PATH_MAX];
        make_host_path(host_name, full, sizeof(full));
        strncpy(host_name, full, sizeof(host_name));
        host_name[sizeof(host_name)-1] = 0;
    }

    /* "r+b" — CP/M's OPEN returns a handle the program can read AND
     * write through. Opening with plain "rb" worked for read-only
     * apps (games etc.) but breaks compilers that re-OPEN a file they
     * just MADE and then write into it (MS COBOL's "DISK FULL"). */
    fp = fopen(host_name, "r+b");
    if (!fp) {
        cpu->a = 0xFF;   /* CP/M open failure */
        return 1;
    }

    int slot = alloc_file_slot();
    if (slot < 0) {
        fclose(fp);
        cpu->a = 0xFF;
        return 1;
    }

    open_files[slot].fp = fp;
    memcpy(open_files[slot].fcb_copy, fcb, CPM_FCB_SIZE);
    open_fcb_addr[slot] = fcb_addr;   /* remember which guest FCB owns this slot */

    /* Compute real record count from host file size and update FCB[15] (RC).
     * Many .COM data loaders (Infocom etc.) look at this after OPEN. */
    struct stat st;
    uint8_t rc = 0;
    if (fstat(fileno(fp), &st) == 0) {
        long total_recs = (st.st_size + 127L) / 128L;
        if (total_recs > 0x80) total_recs = 0x80; /* one extent max for simple case */
        rc = (uint8_t)total_recs;
    }
    fcb[15] = rc;

    /* Initialize logical record from FCB (program may have set extent/CR) */
    uint8_t extent = fcb[12];
    uint8_t cr     = fcb[32];
    open_files[slot].logical_record = ((uint16_t)extent << 7) + cr;

    cpu->a = 0;
    /* Do NOT smash fcb[0] — leave the original drive code alone.
     * We track ownership via the separate open_fcb_addr table. */

    return 1;
}

int cpm_bdos_close_file(z80_cpu_t *cpu, uint16_t fcb_addr)
{
    int slot = get_file_slot_from_fcb(fcb_addr);

    if (slot >= 0 && open_files[slot].fp) {
        fclose(open_files[slot].fp);
        open_files[slot].fp = NULL;
        open_fcb_addr[slot] = 0;
    }

    cpu->a = 0;
    return 1;
}

int cpm_bdos_read_sequential(z80_cpu_t *cpu, uint16_t fcb_addr)
{
    uint8_t *fcb = &cpu->mem[fcb_addr];
    int slot = get_file_slot_from_fcb(fcb_addr);
    if (slot < 0) {
        cpu->a = 1;
        return 1;
    }

    FILE *fp = open_files[slot].fp;
    uint16_t rec = open_files[slot].logical_record;

    long offset = logical_record_to_offset(rec);

    if (fseek(fp, offset, SEEK_SET) != 0) {
        cpu->a = 1;
        return 1;
    }

    size_t n = fread(disk_io_buf, 1, 128, fp);
    if (n > 0) {
        memcpy(&cpu->mem[current_dma], disk_io_buf, n);
    }
    if (n == 0) {
        cpu->a = 1; /* EOF */
        return 1;
    }

    /* Advance */
    open_files[slot].logical_record = rec + 1;

    /* Update FCB for the caller */
    fcb[12] = (rec + 1) >> 7;     /* new extent */
    fcb[32] = (rec + 1) & 0x7F;   /* new CR */

    cpu->a = 0;
    return 1;
}

int cpm_bdos_write_sequential(z80_cpu_t *cpu, uint16_t fcb_addr)
{
    uint8_t *fcb = &cpu->mem[fcb_addr];
    int slot = get_file_slot_from_fcb(fcb_addr);
    if (slot < 0) {
        cpu->a = 1;
        return 1;
    }

    FILE *fp = open_files[slot].fp;
    uint16_t rec = open_files[slot].logical_record;

    long offset = logical_record_to_offset(rec);

    if (fseek(fp, offset, SEEK_SET) != 0) {
        cpu->a = 1;
        return 1;
    }

    /* Write 128 bytes from DMA. Pad with zeros if we only have partial data (rare). */
    memcpy(disk_io_buf, &cpu->mem[current_dma], 128);

    size_t n = fwrite(disk_io_buf, 1, 128, fp);
    if (n != 128) {
        cpu->a = 1;
        return 1;
    }

    /* Advance and flush */
    open_files[slot].logical_record = rec + 1;
    fflush(fp);

    /* Update FCB */
    fcb[12] = (rec + 1) >> 7;
    fcb[32] = (rec + 1) & 0x7F;

    /* Update record count in current extent (crude but helpful) */
    if (fcb[15] < 0x80) fcb[15]++;

    cpu->a = 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Random Record Access (BDOS 33 / 34)                                */
/* ------------------------------------------------------------------ */

int cpm_bdos_random_read(z80_cpu_t *cpu, uint16_t fcb_addr)
{
    uint8_t *fcb = &cpu->mem[fcb_addr];
    int slot = get_file_slot_from_fcb(fcb_addr);
    if (slot < 0) {
        cpu->a = 1; /* error */
        return 1;
    }

    FILE *fp = open_files[slot].fp;
    uint32_t rec = get_random_record(fcb);
    long offset = (long)rec * 128;

    if (fseek(fp, offset, SEEK_SET) != 0) {
        cpu->a = 1;
        return 1;
    }

    size_t n = fread(disk_io_buf, 1, 128, fp);
    if (n > 0) {
        memcpy(&cpu->mem[current_dma], disk_io_buf, n);
    }
    if (n == 0) {
        if (cpm_debug)
            fprintf(stderr, "  [random read] rec=%u returned EOF (A=1)\n", rec);
        cpu->a = 1; /* EOF / record not written */
        return 1;
    }
    if (n < 128 && cpm_debug) {
        fprintf(stderr, "  [random read] rec=%u short read (%zu bytes)\n", rec, n);
    }

    /* One-time diagnostic: show the header the game sees on the very first record */
    static int first_rec_dumped = 0;
    if (cpm_debug && !first_rec_dumped && rec == 0) {
        first_rec_dumped = 1;
        fprintf(stderr, "  [Zork header @DMA] ");
        for (int i = 0; i < 16; i++)
            fprintf(stderr, "%02X ", cpu->mem[current_dma + i]);
        fprintf(stderr, "\n");
    }

    /* Update sequential position so mixed seq/random works */
    fcb[12] = rec >> 7;      /* extent */
    fcb[32] = rec & 0x7F;    /* current record */

    open_files[slot].logical_record = rec;

    cpu->a = 0;
    return 1;
}

int cpm_bdos_random_write(z80_cpu_t *cpu, uint16_t fcb_addr)
{
    uint8_t *fcb = &cpu->mem[fcb_addr];
    int slot = get_file_slot_from_fcb(fcb_addr);
    if (slot < 0) {
        cpu->a = 1;
        return 1;
    }

    FILE *fp = open_files[slot].fp;
    uint32_t rec = get_random_record(fcb);
    long offset = (long)rec * 128;

    if (fseek(fp, offset, SEEK_SET) != 0) {
        cpu->a = 2; /* disk full / seek error */
        return 1;
    }

    memcpy(disk_io_buf, &cpu->mem[current_dma], 128);

    size_t n = fwrite(disk_io_buf, 1, 128, fp);
    if (n != 128) {
        if (cpm_debug)
            fprintf(stderr, "  [random write] rec=%u short write\n", rec);
        cpu->a = 2;
        return 1;
    }

    fflush(fp);

    /* Update sequential fields */
    fcb[12] = rec >> 7;
    fcb[32] = rec & 0x7F;

    open_files[slot].logical_record = rec;

    /* crude record count update */
    if (fcb[15] < 0x80) fcb[15]++;

    cpu->a = 0;
    return 1;
}

int cpm_bdos_set_dma(z80_cpu_t *cpu)
{
    current_dma = cpu->de;
    cpu->a = 0;
    return 1;
}

int cpm_bdos_make_file(z80_cpu_t *cpu, uint16_t fcb_addr)
{
    uint8_t *fcb = &cpu->mem[fcb_addr];
    char host_name[64];
    FILE *fp;

    /* Same FCB-reuse retirement as open_file. */
    {
        int prev = get_file_slot_from_fcb(fcb_addr);
        if (prev >= 0) {
            fclose(open_files[prev].fp);
            open_files[prev].fp = NULL;
            open_fcb_addr[prev] = 0;
        }
    }

    fcb_to_host_name(fcb, host_name, sizeof(host_name));
    {
        char full[PATH_MAX];
        make_host_path(host_name, full, sizeof(full));
        strncpy(host_name, full, sizeof(host_name));
        host_name[sizeof(host_name)-1] = 0;
    }

    /* Create/truncate; allow subsequent reads too (CP/M doesn't
     * distinguish create-write from create-readwrite). */
    fp = fopen(host_name, "w+b");
    if (!fp) {
        cpu->a = 0xFF;
        return 1;
    }

    int slot = alloc_file_slot();
    if (slot < 0) {
        fclose(fp);
        cpu->a = 0xFF;
        return 1;
    }

    open_files[slot].fp = fp;
    memcpy(open_files[slot].fcb_copy, fcb, CPM_FCB_SIZE);
    open_fcb_addr[slot] = fcb_addr;
    open_files[slot].logical_record = 0;

    /* Zero out extent/CR in the FCB copy for a fresh file */
    fcb[12] = 0;   /* extent */
    fcb[15] = 0;   /* record count */
    fcb[32] = 0;   /* current record */

    cpu->a = 0;
    /* leave original drive byte in fcb[0] */

    return 1;
}

int cpm_bdos_delete_file(z80_cpu_t *cpu, uint16_t fcb_addr)
{
    uint8_t *fcb = &cpu->mem[fcb_addr];
    char host_name[64];

    /* If the file is currently open via this FCB, close it first. */
    {
        int prev = get_file_slot_from_fcb(fcb_addr);
        if (prev >= 0) {
            fclose(open_files[prev].fp);
            open_files[prev].fp = NULL;
            open_fcb_addr[prev] = 0;
        }
    }

    fcb_to_host_name(fcb, host_name, sizeof(host_name));
    {
        char full[PATH_MAX];
        make_host_path(host_name, full, sizeof(full));
        strncpy(host_name, full, sizeof(host_name));
        host_name[sizeof(host_name)-1] = 0;
    }

    /* CP/M returns 0xFF when no file matched, 0..3 (directory code)
     * when a match was found and deleted. We don't track directory
     * positions, so return 0 on success and 0xFF on not-found. */
    if (unlink(host_name) == 0) {
        cpu->a = 0;
    } else {
        cpu->a = 0xFF;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Directory Search (BDOS 17 / 18)                                    */
/* ------------------------------------------------------------------ */

static void fill_dir_entry(uint8_t *dest, const char *host_name, uint8_t drive)
{
    memset(dest, 0, 32);
    dest[0] = drive;                    /* drive code (0 = current) */

    /* Split name/ext */
    char base[9], ext[4];
    const char *dot = strrchr(host_name, '.');
    if (dot) {
        strncpy(base, host_name, dot - host_name);
        base[dot-host_name] = 0;
        strncpy(ext, dot+1, 3);
        ext[3] = 0;
    } else {
        strncpy(base, host_name, 8);
        base[8] = 0;
        ext[0] = 0;
    }

    /* Pad to 8.3 */
    size_t blen = strlen(base);
    size_t elen = strlen(ext);
    for (int i = 0; i < 8; i++) dest[1+i] = (i < (int)blen) ? toupper(base[i]) : ' ';
    for (int i = 0; i < 3; i++) dest[9+i] = (i < (int)elen) ? toupper(ext[i])  : ' ';

    /* Fake some attributes / size so programs don't freak out */
    dest[12] = 0;   /* extent */
    dest[15] = 1;   /* record count (at least one) */
}

int cpm_bdos_search_first(z80_cpu_t *cpu, uint16_t fcb_addr)
{
    uint8_t *fcb = &cpu->mem[fcb_addr];
    char pattern[16];

    /* Close any previous search */
    if (search_dir) {
        closedir(search_dir);
        search_dir = NULL;
    }

    fcb_to_host_name(fcb, pattern, sizeof(pattern));
    strncpy(search_pattern, pattern, sizeof(search_pattern));

    {
        char dirpath[PATH_MAX];
        make_host_path(".", dirpath, sizeof(dirpath));
        search_dir = opendir(dirpath);
    }
    if (!search_dir) {
        cpu->a = 0xFF;
        return 1;
    }

    /* Find first matching entry */
    struct dirent *ent;
    while ((ent = readdir(search_dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;           /* skip . and .. */
        struct stat st;
        if (stat(ent->d_name, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        if (fcb_name_matches(fcb, ent->d_name)) {
            fill_dir_entry(&cpu->mem[current_dma], ent->d_name, fcb[0]);
            cpu->a = 0;
            return 1;
        }
    }

    closedir(search_dir);
    search_dir = NULL;
    cpu->a = 0xFF;
    return 1;
}

int cpm_bdos_search_next(z80_cpu_t *cpu)
{
    if (!search_dir) {
        cpu->a = 0xFF;
        return 1;
    }

    struct dirent *ent;
    while ((ent = readdir(search_dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        struct stat st;
        if (stat(ent->d_name, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        /* Loose but useful matching against the stored pattern */
        if (search_pattern[0] == '*' || strcasestr(ent->d_name, search_pattern)) {
            fill_dir_entry(&cpu->mem[current_dma], ent->d_name, 0);
            cpu->a = 0;
            return 1;
        }
    }

    closedir(search_dir);
    search_dir = NULL;
    cpu->a = 0xFF;
    return 1;
}

/* ------------------------------------------------------------------ */

void cpm_disk_install_defaults(z80_cpu_t *cpu)
{
    (void)cpu;
    current_dma = CPM_DEFAULT_DMA;
}

/* Set the host directory that will be used as CP/M drive A: */
void cpm_set_a_root(const char *path)
{
    if (!path || !*path) {
        a_root[0] = 0;
        return;
    }
    strncpy(a_root, path, sizeof(a_root) - 1);
    a_root[sizeof(a_root) - 1] = 0;
}

/* Helper: turn a relative host name into a path under the A: root */
static void make_host_path(const char *name, char *out, size_t outlen)
{
    if (a_root[0]) {
        snprintf(out, outlen, "%s/%s", a_root, name);
    } else {
        strncpy(out, name, outlen);
        out[outlen - 1] = 0;
    }
}