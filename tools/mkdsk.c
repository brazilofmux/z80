/* mkdsk.c — make, fill and list z80-monster CP/M 2.2 disk images.
 *
 *   mkdsk [-f fd|hd] IMAGE            create a blank image (default: hd)
 *   mkdsk IMAGE FILE...               add host files (names -> 8.3 upper)
 *   mkdsk -l IMAGE                    list the directory
 *   mkdsk -x IMAGE [NAME...] [-o DIR] extract files (all if no names)
 *
 * The formats are the two in tools/diskdefs (cpmtools-compatible): raw,
 * track-major, 128-byte records, no skew, two system tracks. The format
 * of an existing image is told from its size. This is a straightforward
 * writer of the CP/M 2.2 directory — 32-byte entries, EXM=1 so each
 * entry spans two logical extents (32K), 8- or 16-bit block pointers as
 * DSM demands — good enough to build boot disks and test material; for
 * anything fancier there is cpmtools.
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    const char *name;
    long     size;          /* image bytes */
    unsigned spt, tracks, off;
    unsigned bsize;         /* block size */
    unsigned dsm;           /* last block number */
    unsigned drm;           /* last directory entry */
    unsigned exm;
} fmt_t;

static const fmt_t FMT_FD = { "fd", 409600L,  40,   80, 2, 2048,  194,   63, 1 };
static const fmt_t FMT_HD = { "hd", 8388608L, 64, 1024, 2, 4096, 2043, 1023, 1 };

static const fmt_t *fmt_for_size(long size) {
    if (size == FMT_FD.size) return &FMT_FD;
    if (size == FMT_HD.size) return &FMT_HD;
    return NULL;
}

typedef struct {
    const fmt_t *f;
    uint8_t *data;
    unsigned dir_blocks;    /* blocks holding the directory */
    unsigned ptrs;          /* block pointers per entry: 16 (8-bit) or 8 (16-bit) */
    uint8_t *alloc;         /* one byte per block: 1 = used */
} img_t;

static long data_base(const fmt_t *f) { return (long)f->off * f->spt * 128; }
static uint8_t *block_ptr(img_t *im, unsigned b) { return im->data + data_base(im->f) + (long)b * im->f->bsize; }
static uint8_t *entry_ptr(img_t *im, unsigned e) { return block_ptr(im, 0) + e * 32; }

static void die(const char *msg, const char *arg) {
    fprintf(stderr, "mkdsk: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
    exit(1);
}

static img_t *img_load(const char *path, const fmt_t *create_as) {
    img_t *im = calloc(1, sizeof *im);
    FILE *fp = fopen(path, "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        im->f = fmt_for_size(size);
        if (!im->f) die("not a 400K or 8 MB image", path);
        im->data = malloc(size);
        fseek(fp, 0, SEEK_SET);
        if (fread(im->data, 1, size, fp) != (size_t)size) die("short read", path);
        fclose(fp);
    } else {
        if (!create_as) die("cannot open", path);
        im->f = create_as;
        im->data = malloc(im->f->size);
        memset(im->data, 0xE5, im->f->size);
    }
    im->dir_blocks = ((im->f->drm + 1) * 32 + im->f->bsize - 1) / im->f->bsize;
    im->ptrs = im->f->dsm < 256 ? 16 : 8;
    im->alloc = calloc(im->f->dsm + 1, 1);
    for (unsigned b = 0; b < im->dir_blocks; b++) im->alloc[b] = 1;
    /* Mark blocks owned by existing files. */
    for (unsigned e = 0; e <= im->f->drm; e++) {
        uint8_t *d = entry_ptr(im, e);
        if (d[0] == 0xE5 || d[0] > 15) continue;
        for (unsigned i = 0; i < im->ptrs; i++) {
            unsigned b = im->ptrs == 16 ? d[16 + i] : (unsigned)(d[16 + 2 * i] | (d[17 + 2 * i] << 8));
            if (b && b <= im->f->dsm) im->alloc[b] = 1;
        }
    }
    return im;
}

static void img_save(img_t *im, const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp) die("cannot write", path);
    if (fwrite(im->data, 1, im->f->size, fp) != (size_t)im->f->size) die("short write", path);
    fclose(fp);
}

static unsigned alloc_block(img_t *im) {
    for (unsigned b = im->dir_blocks; b <= im->f->dsm; b++)
        if (!im->alloc[b]) { im->alloc[b] = 1; return b; }
    die("image full", NULL);
    return 0;
}

static int free_entry(img_t *im) {
    for (unsigned e = 0; e <= im->f->drm; e++)
        if (entry_ptr(im, e)[0] == 0xE5) return (int)e;
    die("directory full", NULL);
    return -1;
}

/* Host name -> 11-byte FNAME.EXT, uppercase, space padded. */
static void to_cpm_name(const char *host, uint8_t out[11]) {
    const char *base = strrchr(host, '/');
    base = base ? base + 1 : host;
    memset(out, ' ', 11);
    int i = 0;
    const char *p = base;
    for (; *p && *p != '.' && i < 8; p++) if (*p != ' ') out[i++] = (uint8_t)toupper((unsigned char)*p);
    while (*p && *p != '.') p++;
    if (*p == '.') {
        p++;
        i = 8;
        for (; *p && i < 11; p++) if (*p != '.' && *p != ' ') out[i++] = (uint8_t)toupper((unsigned char)*p);
    }
}

static int name_eq(const uint8_t *entry, const uint8_t name[11]) {
    for (int i = 0; i < 11; i++) if ((entry[1 + i] & 0x7F) != name[i]) return 0;
    return 1;
}

static void delete_file(img_t *im, const uint8_t name[11]) {
    for (unsigned e = 0; e <= im->f->drm; e++) {
        uint8_t *d = entry_ptr(im, e);
        if (d[0] != 0 || !name_eq(d, name)) continue;
        for (unsigned i = 0; i < im->ptrs; i++) {
            unsigned b = im->ptrs == 16 ? d[16 + i] : (unsigned)(d[16 + 2 * i] | (d[17 + 2 * i] << 8));
            if (b && b <= im->f->dsm) im->alloc[b] = 0;
        }
        d[0] = 0xE5;
    }
}

static void add_file(img_t *im, const char *host) {
    FILE *fp = fopen(host, "rb");
    if (!fp) die("cannot read", host);
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = malloc(size + 128);
    if (fread(buf, 1, size, fp) != (size_t)size) die("short read", host);
    fclose(fp);
    unsigned records = (unsigned)((size + 127) / 128);
    memset(buf + size, 0x1A, (records * 128) - size);     /* CP/M pads text with ^Z */

    uint8_t name[11];
    to_cpm_name(host, name);
    delete_file(im, name);

    unsigned per_entry = 256;                 /* EXM=1: two 128-record extents per entry */
    unsigned rec = 0, k = 0;
    do {
        unsigned n = records - rec;
        if (n > per_entry) n = per_entry;
        int e = free_entry(im);
        uint8_t *d = entry_ptr(im, (unsigned)e);
        memset(d, 0, 32);
        d[0] = 0;
        memcpy(d + 1, name, 11);
        unsigned ext = 2 * k + (n > 128 ? 1 : 0);   /* last logical extent in this entry */
        d[12] = (uint8_t)(ext & 0x1F);
        d[14] = (uint8_t)(ext >> 5);
        unsigned rc = n > 128 ? n - 128 : n;
        d[15] = (uint8_t)(rc == 0 ? 0 : rc);
        unsigned blocks = (n * 128 + im->f->bsize - 1) / im->f->bsize;
        for (unsigned i = 0; i < blocks; i++) {
            unsigned b = alloc_block(im);
            if (im->ptrs == 16) d[16 + i] = (uint8_t)b;
            else { d[16 + 2 * i] = (uint8_t)b; d[17 + 2 * i] = (uint8_t)(b >> 8); }
            unsigned first = rec + i * (im->f->bsize / 128);
            unsigned cnt = (im->f->bsize / 128);
            if (first + cnt > rec + n) cnt = rec + n - first;
            memset(block_ptr(im, b), 0x1A, im->f->bsize);
            memcpy(block_ptr(im, b), buf + (long)first * 128, cnt * 128);
        }
        rec += n;
        k++;
    } while (rec < records);
    free(buf);
    printf("  %c%c%c%c%c%c%c%c.%c%c%c  %6ld bytes, %u records\n",
           name[0], name[1], name[2], name[3], name[4], name[5], name[6], name[7],
           name[8], name[9], name[10], size, records);
}

static void list_dir(img_t *im) {
    unsigned files = 0, used_blocks = 0;
    for (unsigned e = 0; e <= im->f->drm; e++) {
        uint8_t *d = entry_ptr(im, e);
        if (d[0] == 0xE5 || d[0] > 15) continue;
        unsigned ext = (d[12] & 0x1F) | (d[14] << 5);
        unsigned recs = (ext & im->f->exm) * 128 + d[15];
        unsigned blocks = 0;
        for (unsigned i = 0; i < im->ptrs; i++) {
            unsigned b = im->ptrs == 16 ? d[16 + i] : (unsigned)(d[16 + 2 * i] | (d[17 + 2 * i] << 8));
            if (b) blocks++;
        }
        used_blocks += blocks;
        if ((ext & ~im->f->exm) == 0) files++;   /* first entry of the file */
        printf("%2u  %.8s.%.3s  user %u  extent %2u  %3u recs  %u blocks\n",
               e, d + 1, d + 9, d[0], ext, recs, blocks);
    }
    unsigned total = im->f->dsm + 1 - im->dir_blocks;
    printf("%s image: %u files, %u/%u data blocks of %uK used\n",
           im->f->name, files, used_blocks, total, im->f->bsize / 1024);
}

static void extract(img_t *im, const uint8_t *want, const char *outdir) {
    for (unsigned e = 0; e <= im->f->drm; e++) {
        uint8_t *d = entry_ptr(im, e);
        if (d[0] != 0) continue;
        unsigned ext = (d[12] & 0x1F) | (d[14] << 5);
        /* The first entry of a file has logical extent 0 or, when it is
         * full, EXM (its LAST extent is recorded); anything beyond the
         * EXM bits belongs to a later entry. */
        if (ext & ~im->f->exm) continue;         /* not the first entry */
        /* first entry of some file: is it wanted? */
        uint8_t name[11]; memcpy(name, d + 1, 11);
        for (int i = 0; i < 11; i++) name[i] &= 0x7F;
        if (want && !name_eq(d, want)) continue;
        char host[64];
        int n = 0;
        for (int i = 0; i < 8 && name[i] != ' '; i++) host[n++] = (char)name[i];
        if (name[8] != ' ') { host[n++] = '.'; for (int i = 8; i < 11 && name[i] != ' '; i++) host[n++] = (char)name[i]; }
        host[n] = 0;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", outdir, host);
        FILE *fp = fopen(path, "wb");
        if (!fp) die("cannot write", path);
        /* Walk every entry of this file in extent order. */
        for (unsigned x = 0; ; x += 2) {
            uint8_t *m = NULL;
            for (unsigned e2 = 0; e2 <= im->f->drm; e2++) {
                uint8_t *c = entry_ptr(im, e2);
                if (c[0] != 0 || !name_eq(c, name)) continue;
                unsigned ce = (c[12] & 0x1F) | (c[14] << 5);
                if ((ce & ~im->f->exm) == x) { m = c; break; }
            }
            if (!m) break;
            unsigned ce = (m[12] & 0x1F) | (m[14] << 5);
            unsigned recs = (ce & im->f->exm) * 128 + m[15];
            for (unsigned r = 0; r < recs; r++) {
                unsigned bi = r / (im->f->bsize / 128);
                unsigned b = im->ptrs == 16 ? m[16 + bi] : (unsigned)(m[16 + 2 * bi] | (m[17 + 2 * bi] << 8));
                fwrite(block_ptr(im, b) + (r % (im->f->bsize / 128)) * 128, 1, 128, fp);
            }
            if (recs < 256) break;
        }
        fclose(fp);
        printf("  %s\n", host);
    }
}

int main(int argc, char **argv) {
    const fmt_t *create = &FMT_HD;
    int mode = 'a';                 /* a: add/create, l: list, x: extract */
    const char *outdir = ".";
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            i++;
            if (!strcmp(argv[i], "fd")) create = &FMT_FD;
            else if (!strcmp(argv[i], "hd")) create = &FMT_HD;
            else die("format must be fd or hd", argv[i]);
        } else if (!strcmp(argv[i], "-l")) mode = 'l';
        else if (!strcmp(argv[i], "-x")) mode = 'x';
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outdir = argv[++i];
        else die("unknown option", argv[i]);
    }
    if (i >= argc) die("usage: mkdsk [-f fd|hd] IMAGE [FILE...] | -l IMAGE | -x IMAGE [NAME...] [-o DIR]", NULL);
    const char *image = argv[i++];

    if (mode == 'l') { img_t *im = img_load(image, NULL); list_dir(im); return 0; }
    if (mode == 'x') {
        img_t *im = img_load(image, NULL);
        /* -o may come anywhere after the image; find it before extracting. */
        for (int j = i; j < argc; j++)
            if (!strcmp(argv[j], "-o") && j + 1 < argc) outdir = argv[j + 1];
        int named = 0;
        for (int j = i; j < argc; j++) {
            if (!strcmp(argv[j], "-o")) { j++; continue; }
            uint8_t name[11]; to_cpm_name(argv[j], name); extract(im, name, outdir); named = 1;
        }
        if (!named) extract(im, NULL, outdir);
        return 0;
    }
    img_t *im = img_load(image, create);
    for (; i < argc; i++) add_file(im, argv[i]);
    img_save(im, image);
    return 0;
}
