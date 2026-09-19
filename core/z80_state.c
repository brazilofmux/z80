/* Z80 CPU state implementation */
#include "z80.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

int z80_mem_mirrored = 0;

/* Guest memory: 64 KB, ideally followed by a MIRROR of its first page so
 * that mem[0x10000 + k] is the same byte as mem[k]. That lets the JIT do
 * 16-bit loads/stores at address 0xFFFF as one host access with exact
 * Z80 wrap-around semantics (the high byte lands on address 0), instead
 * of splitting every PUSH/POP/CALL/RET into two masked byte accesses.
 *
 * The mirror is a second mapping of the same shared-memory object, so
 * every writer (JIT stores, the interpreter, the loader, LDIR) keeps it
 * coherent for free. If the platform trick fails we fall back to a
 * plain allocation with slack (so an out-of-range host access still
 * can't fault) and clear z80_mem_mirrored; backends must then emit the
 * split form. */
#define Z80_MEM_SIZE 65536u

static uint8_t *mem_alloc_mirrored(void) {
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || (Z80_MEM_SIZE % (size_t)page) != 0) return NULL;
    size_t mirror = (size_t)page;

    int fd = -1;
#if defined(__linux__)
    fd = memfd_create("z80-guest-mem", MFD_CLOEXEC);
#else
    char name[64];
    snprintf(name, sizeof name, "/z80mem-%ld-%p", (long)getpid(), (void *)&name);
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) shm_unlink(name);
#endif
    if (fd < 0) return NULL;
    if (ftruncate(fd, (off_t)Z80_MEM_SIZE) != 0) { close(fd); return NULL; }

    /* Reserve the whole range, then map the object over it twice. */
    uint8_t *base = mmap(NULL, Z80_MEM_SIZE + mirror, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { close(fd); return NULL; }
    void *a = mmap(base, Z80_MEM_SIZE, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_FIXED, fd, 0);
    void *b = mmap(base + Z80_MEM_SIZE, mirror, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_FIXED, fd, 0);
    close(fd);
    if (a != base || b != base + Z80_MEM_SIZE) {
        munmap(base, Z80_MEM_SIZE + mirror);
        return NULL;
    }
    memset(base, 0, Z80_MEM_SIZE);
    return base;
}

uint8_t *z80_mem_alloc(void) {
    uint8_t *m = mem_alloc_mirrored();
    if (m) {
        z80_mem_mirrored = 1;
        return m;
    }
    z80_mem_mirrored = 0;
    m = malloc(Z80_MEM_SIZE + 4096);
    if (m) memset(m, 0, Z80_MEM_SIZE + 4096);
    return m;
}

void z80_mem_free(uint8_t *mem) {
    if (!mem) return;
    if (z80_mem_mirrored) {
        long page = sysconf(_SC_PAGESIZE);
        munmap(mem, Z80_MEM_SIZE + (size_t)page);
    } else {
        free(mem);
    }
}

void z80_cpu_init(z80_cpu_t *cpu) {
    memset(cpu, 0, sizeof(*cpu));

    /* 64 KB flat memory for classic CP/M */
    cpu->mem = z80_mem_alloc();
    if (cpu->mem) cpu->mem_size = 65536;

    /* Reasonable power-on defaults */
    cpu->sp = 0xFFFF;
    cpu->im = 1;
    cpu->iff1 = 0;
    cpu->iff2 = 0;
}

void z80_cpu_reset(z80_cpu_t *cpu) {
    /* Preserve memory allocation */
    uint8_t *mem = cpu->mem;
    uint32_t mem_size = cpu->mem_size;

    memset(cpu, 0, sizeof(*cpu));

    cpu->mem = mem;
    cpu->mem_size = mem_size;

    cpu->sp = 0xFFFF;
    cpu->im = 1;
}

int z80_load_com(z80_cpu_t *cpu, const char *path) {
    /* Stub for now — real implementation will open the file,
       read it into cpu->mem + 0x0100, set PC = 0x0100, etc. */
    (void)path;
    return -1;  /* not implemented yet */
}

uint8_t z80_materialize_flags(z80_cpu_t *cpu) {
    /* For the lazy-flag future. Right now just return what we have. */
    return cpu->f;
}
