/* tests/disk_test.c — host-side test of the shim disk layer with an
 * explicit A: root (cpm_set_a_root) that is NOT the current directory
 * and whose path is longer than 64 bytes. main.c chdirs into the root
 * and clears it, so these paths are otherwise unexercised: directory
 * search must stat entries through the root, and OPEN/MAKE/DELETE
 * must not truncate the full host path. */
#include "../cpm/cpm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>

/* The two symbols cpm_disk.c takes from the rest of the emulator. */
int cpm_debug = 0;
void z80_mem_host_wrote(z80_cpu_t *cpu, uint16_t addr, uint32_t len) { (void)cpu; (void)addr; (void)len; }

static uint8_t mem[0x10000];
static z80_cpu_t cpu;
static int failures;

#define CHECK(cond, what) do { if (!(cond)) { printf("disk_test: FAIL %s\n", what); failures++; } } while (0)

#define FCB 0x005C
#define DMA 0x0080

static void set_fcb(const char *name11) {
    memset(&mem[FCB], 0, CPM_FCB_SIZE);
    memcpy(&mem[FCB + 1], name11, 11);
}

int main(void) {
    char base[256], root[512], file[600];
    const char *tmp = getenv("TMPDIR");
    snprintf(base, sizeof base, "%s/z80-disk-test-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(base)) { perror("mkdtemp"); return 1; }
    snprintf(root, sizeof root, "%s/a-drive-root-with-a-name-long-enough-to-overflow-sixty-four-bytes", base);
    if (mkdir(root, 0755) != 0) { perror(root); return 1; }
    CHECK(strlen(root) > 64, "root path is longer than 64 bytes");
    snprintf(file, sizeof file, "%s/TEST.DAT", root);

    cpu.mem = mem;
    cpm_disk_init();
    cpm_set_a_root(root);
    cpm_set_dma(DMA);

    /* MAKE + one sequential record + CLOSE land in the root */
    set_fcb("TEST    DAT");
    cpm_bdos_make_file(&cpu, FCB);
    CHECK(cpu.a == 0, "MAKE through a long root");
    memset(&mem[DMA], 0x5A, 128);
    cpm_bdos_write_sequential(&cpu, FCB);
    CHECK(cpu.a == 0, "WRITE");
    cpm_bdos_close_file(&cpu, FCB);
    struct stat st;
    CHECK(stat(file, &st) == 0 && st.st_size == 128, "file is in the root, 128 bytes");

    /* OPEN + READ */
    set_fcb("TEST    DAT");
    memset(&mem[DMA], 0, 128);
    cpm_bdos_open_file(&cpu, FCB);
    CHECK(cpu.a == 0, "OPEN through a long root");
    cpm_bdos_read_sequential(&cpu, FCB);
    CHECK(cpu.a == 0 && mem[DMA] == 0x5A && mem[DMA + 127] == 0x5A, "READ returns the record");
    cpm_bdos_close_file(&cpu, FCB);

    /* SEARCH FIRST finds it although the cwd is elsewhere; NEXT ends */
    set_fcb("????????DAT");
    cpm_bdos_search_first(&cpu, FCB);
    CHECK(cpu.a == 0 && memcmp(&mem[DMA + 1], "TEST    DAT", 11) == 0, "SEARCH FIRST through the root");
    CHECK(!(mem[DMA + 9] & 0x80), "writable file has t1' clear");
    cpm_bdos_search_next(&cpu);
    CHECK(cpu.a == 0xFF, "SEARCH NEXT ends");

    /* a read-only host file shows t1' */
    chmod(file, 0444);
    set_fcb("TEST    DAT");
    cpm_bdos_search_first(&cpu, FCB);
    CHECK(cpu.a == 0 && (mem[DMA + 9] & 0x80), "read-only file has t1' set");
    chmod(file, 0644);

    /* the allocation vector counts the file: directory 8 blocks + 1 */
    cpm_bdos_alloc_vector(&cpu);
    CHECK(mem[CPM_SHIM_ALV] == 0xFF && mem[CPM_SHIM_ALV + 1] == 0x80, "ALV: 8 directory blocks + 1 file block");

    /* DELETE removes it from the root */
    set_fcb("TEST    DAT");
    cpm_bdos_delete_file(&cpu, FCB);
    CHECK(cpu.a == 0 && stat(file, &st) != 0, "DELETE through a long root");

    rmdir(root);
    rmdir(base);
    if (failures) { printf("disk_test: %d failure(s)\n", failures); return 1; }
    printf("disk_test: all cases pass\n");
    return 0;
}
