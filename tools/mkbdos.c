/* tools/mkbdos.c — self-checking test of the shim BDOS functions beyond
 * console and basic file I/O: 3/4/5 (RDR/PUN/LST), 7/8 (IOBYTE), 24
 * (login vector), 27 (allocation vector), 28/29/37 (R/O vector), 30
 * (attributes), 31 (DPB), 32 (user code), 35 (file size), 36 (set
 * random record), 40 (write random with zero fill), plus OPEN with an
 * attribute bit set in the name.
 *
 * Works on a scratch file BDOSTEST.TMP in the program's directory and
 * deletes it at the end. Prints "BDOS OK" on success, "BDOS FAIL nn"
 * otherwise; sends "LST" to the list device (check with --list). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define ORG   0x100
#define TMPL  0x0800            /* FCB template (in the image) */
#define FCB   0x0880            /* working FCB */
#define DMA   0x0080

static uint8_t code[0x800];
static int pc;

static void b(int x) { code[pc++] = (uint8_t)x; }
static void w(int x) { b(x & 0xFF); b((x >> 8) & 0xFF); }

static int fail_fix[128], fail_num[128], nfail;
static void jp_fail(int opcode, int num) { b(opcode); fail_fix[nfail] = pc; fail_num[nfail++] = num; w(0); }
#define JP_NZ 0xC2
#define JP_Z  0xCA

static void bdos(int c)       { b(0x0E); b(c); b(0xCD); w(0x0005); }
static void ld_de(int v)      { b(0x11); w(v); }
static void ld_e(int v)       { b(0x1E); b(v); }
static void ld_a(int v)       { b(0x3E); b(v); }
static void ld_a_mem(int a)   { b(0x3A); w(a); }
static void ld_mem_a(int a)   { b(0x32); w(a); }
static void cp(int v)         { b(0xFE); b(v); }
static void or_a(void)        { b(0xB7); }
/* A == v, else fail k */
static void expect_a(int v, int k) { cp(v); jp_fail(JP_NZ, k); }
/* (addr) == v, else fail k */
static void expect_mem(int addr, int v, int k) { ld_a_mem(addr); expect_a(v, k); }
/* HL == v, else fail k */
static void expect_hl(int v, int k) {
    b(0x7D); expect_a(v & 0xFF, k);     /* LD A,L */
    b(0x7C); expect_a(v >> 8, k);       /* LD A,H */
}
/* fresh working FCB from the template */
static void fcb_init(void) { b(0x21); w(TMPL); ld_de(FCB); b(0x01); w(36); b(0xED); b(0xB0); }
static void fcb_call(int c) { ld_de(FCB); bdos(c); }

int main(void) {
    /* 1: IOBYTE set/get, and it is the byte at 0003 */
    ld_e(0x95); bdos(8);
    bdos(7); expect_a(0x95, 1);
    expect_mem(0x0003, 0x95, 1);
    ld_e(0x00); bdos(8);

    /* 2: login vector = A: */
    bdos(24); expect_hl(0x0001, 2);

    /* 3: DPB address, DSM = 07FF */
    bdos(31); expect_hl(0xE500, 3);
    expect_mem(0xE505, 0xFF, 3);
    expect_mem(0xE506, 0x07, 3);

    /* 4: allocation vector: directory blocks 0-7 in use */
    bdos(27); expect_hl(0xE510, 4);
    expect_mem(0xE510, 0xFF, 4);

    /* 5: write-protect, R/O vector, reset drive */
    bdos(28); bdos(29); expect_hl(0x0001, 5);
    ld_de(0x0001); bdos(37);
    bdos(29); expect_hl(0x0000, 5);

    /* 6: user code set/get */
    ld_e(5); bdos(32);
    ld_e(0xFF); bdos(32); expect_a(5, 6);
    ld_e(0); bdos(32);

    /* 7: reader with nothing attached reads ^Z */
    bdos(3); expect_a(0x1A, 7);

    /* 8: make the scratch file, three sequential records, close */
    fcb_init(); fcb_call(19);                       /* leftover from a failed run */
    fcb_init(); fcb_call(22); cp(0xFF); jp_fail(JP_Z, 8);
    for (int i = 0; i < 3; i++) { fcb_call(21); or_a(); jp_fail(JP_NZ, 8); }
    fcb_call(16);

    /* 9: compute file size: 3 records */
    fcb_init(); fcb_call(35); or_a(); jp_fail(JP_NZ, 9);
    expect_mem(FCB + 33, 3, 9);
    expect_mem(FCB + 34, 0, 9);
    expect_mem(FCB + 35, 0, 9);

    /* 10: open, read two records, set random record -> 2 */
    fcb_init(); fcb_call(15); cp(0xFF); jp_fail(JP_Z, 10);
    for (int i = 0; i < 2; i++) { fcb_call(20); or_a(); jp_fail(JP_NZ, 10); }
    fcb_call(36); expect_mem(FCB + 33, 2, 10);
    expect_mem(FCB + 34, 0, 10);

    /* 11: write random with zero fill at record 10 -> size 11 */
    ld_a(10); ld_mem_a(FCB + 33);
    b(0xAF); ld_mem_a(FCB + 34); ld_mem_a(FCB + 35);  /* XOR A */
    fcb_call(40); or_a(); jp_fail(JP_NZ, 11);
    fcb_call(16);
    fcb_init(); fcb_call(35); expect_mem(FCB + 33, 11, 11);

    /* 12: set R/O (t1'), and search reports it */
    fcb_init(); ld_a_mem(FCB + 9); b(0xF6); b(0x80); ld_mem_a(FCB + 9);  /* OR 80h */
    fcb_call(30); or_a(); jp_fail(JP_NZ, 12);
    fcb_init(); fcb_call(17); cp(0xFF); jp_fail(JP_Z, 12);
    ld_a_mem(DMA + 9); b(0xE6); b(0x80); jp_fail(JP_Z, 12);             /* AND 80h */

    /* 13: an R/O file still opens (read-only) and reads */
    fcb_init(); fcb_call(15); cp(0xFF); jp_fail(JP_Z, 13);
    fcb_call(20); or_a(); jp_fail(JP_NZ, 13);
    fcb_call(16);

    /* 14: clear R/O, search no longer reports it */
    fcb_init(); fcb_call(30); or_a(); jp_fail(JP_NZ, 14);
    fcb_init(); fcb_call(17); cp(0xFF); jp_fail(JP_Z, 14);
    ld_a_mem(DMA + 9); b(0xE6); b(0x80); jp_fail(JP_NZ, 14);

    /* 15: OPEN ignores an attribute bit (f1') in the name */
    fcb_init(); ld_a_mem(FCB + 1); b(0xF6); b(0x80); ld_mem_a(FCB + 1);
    fcb_call(15); cp(0xFF); jp_fail(JP_Z, 15);
    fcb_call(16);

    /* 16: delete; size of a missing file is 0 */
    fcb_init(); fcb_call(19); cp(0xFF); jp_fail(JP_Z, 16);
    fcb_init(); fcb_call(35); expect_mem(FCB + 33, 0, 16);

    /* LST: and PUN: (punch output is discarded) */
    ld_e('L'); bdos(5); ld_e('S'); bdos(5); ld_e('T'); bdos(5);
    ld_e('x'); bdos(4);

    /* success */
    int ok_fix = pc + 1;
    ld_de(0); bdos(9); bdos(0);

    /* failure stubs: LD A,n ; JP common */
    int common_fix[128];
    for (int i = 0; i < nfail; i++) {
        int at = ORG + pc;
        code[fail_fix[i]] = at & 0xFF; code[fail_fix[i] + 1] = at >> 8;
        ld_a(fail_num[i]);
        b(0xC3); common_fix[i] = pc; w(0);
    }
    int common = ORG + pc;
    for (int i = 0; i < nfail; i++) { code[common_fix[i]] = common & 0xFF; code[common_fix[i] + 1] = common >> 8; }
    b(0x06); b('0' - 1);                     /* LD B,'0'-1 */
    int loop = ORG + pc;
    b(0x04); b(0xD6); b(10); b(0xD2); w(loop);   /* INC B; SUB 10; JP NC,loop */
    b(0xC6); b(10 + '0');                    /* ADD A,10+'0' */
    int digits_fix = pc + 1; b(0x21); w(0);  /* LD HL,digits */
    b(0x70); b(0x23); b(0x77);               /* LD (HL),B; INC HL; LD (HL),A */
    int fail_msg_fix = pc + 1;
    ld_de(0); bdos(9); bdos(0);

    int okm = ORG + pc;   memcpy(code + pc, "BDOS OK\r\n$", 10); pc += 10;
    int failm = ORG + pc; memcpy(code + pc, "BDOS FAIL ", 10); pc += 10;
    int digits = ORG + pc; memcpy(code + pc, "00\r\n$", 5); pc += 5;
    code[ok_fix] = okm & 0xFF;          code[ok_fix + 1] = okm >> 8;
    code[fail_msg_fix] = failm & 0xFF;  code[fail_msg_fix + 1] = failm >> 8;
    code[digits_fix] = digits & 0xFF;   code[digits_fix + 1] = digits >> 8;

    if (ORG + pc > TMPL) { fprintf(stderr, "mkbdos: code overran the FCB template\n"); return 1; }
    pc = TMPL - ORG;
    b(0); memcpy(code + pc, "BDOSTESTTMP", 11); pc += 11;
    pc += 24;                                 /* EX S1 S2 RC, map, CR, r0-r2: zero */

    FILE *f = fopen("tests/bdos.com", "wb");
    if (!f) { perror("tests/bdos.com"); return 1; }
    fwrite(code, 1, (size_t)pc, f);
    fclose(f);
    printf("Wrote tests/bdos.com — shim BDOS 3-5, 7/8, 24, 27-32, 35-37, 40\n");
    return 0;
}
