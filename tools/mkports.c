/* tools/mkports.c — exercise the ED port I/O family against the default
 * latch port device (cpm/cpm_ports.c): OUT (n),A, OUT (C),r, OTIR, IN A,(n),
 * IN r,(C), INI. Every port op traps to the interpreter, so the same
 * program checks -i, -j and -V. Prints PORT I/O OK or PORT I/O FAIL. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static uint8_t code[256];
static int p;
static int fail_sites[16], n_fail;

static void b(int v) { code[p++] = (uint8_t)v; }
static void w(int v) { b(v & 0xFF); b(v >> 8); }
static void jr_nz_fail(void) { b(0x20); fail_sites[n_fail++] = p; b(0); }

int main(void) {
    const int ORG = 0x100;
    const int DATA = ORG + 0xC0, BUF = ORG + 0xD0, MSG_OK = ORG + 0xE0, MSG_FAIL = ORG + 0xF0;

    /* OUT (0x30),A with A=0x99, read back with IN A,(0x30) */
    b(0x3E); b(0x99);                 /* LD A,99h */
    b(0xD3); b(0x30);                 /* OUT (30h),A */
    b(0x3E); b(0x00);                 /* LD A,0 */
    b(0xDB); b(0x30);                 /* IN A,(30h) */
    b(0xFE); b(0x99);                 /* CP 99h */
    jr_nz_fail();

    /* OTIR: 4 bytes from DATA to port 10h; ends with B=0 (Z set) */
    b(0x21); w(DATA);                 /* LD HL,DATA */
    b(0x01); w(0x0410);               /* LD BC,0410h  (B=4, C=10h) */
    b(0xED); b(0xB3);                 /* OTIR */
    jr_nz_fail();                     /* Z must be set */
    b(0xDB); b(0x10);                 /* IN A,(10h) -> last byte written, 44h */
    b(0xFE); b(0x44);                 /* CP 44h */
    jr_nz_fail();

    /* OUT (C),r with r=E, then IN D,(C) */
    b(0x0E); b(0x20);                 /* LD C,20h */
    b(0x1E); b(0x5A);                 /* LD E,5Ah */
    b(0xED); b(0x59);                 /* OUT (C),E */
    b(0xED); b(0x50);                 /* IN D,(C) */
    b(0x7A);                          /* LD A,D */
    b(0xFE); b(0x5A);                 /* CP 5Ah */
    jr_nz_fail();

    /* INI: one byte from port 20h into BUF; B=1 -> 0 (Z set) */
    b(0x21); w(BUF);                  /* LD HL,BUF */
    b(0x01); w(0x0120);               /* LD BC,0120h */
    b(0xED); b(0xA2);                 /* INI */
    jr_nz_fail();
    b(0x3A); w(BUF);                  /* LD A,(BUF) */
    b(0xFE); b(0x5A);                 /* CP 5Ah */
    jr_nz_fail();

    /* IN (C) flags-only form: port 10h holds 44h (even parity, nonzero)
     * -> PV set, Z clear. JP PO,fail / JR Z,fail. */
    b(0x0E); b(0x10);                 /* LD C,10h */
    b(0xED); b(0x70);                 /* IN (C) */
    b(0x28); fail_sites[n_fail++] = p; b(0);   /* JR Z,fail */
    b(0xE2); int jp_po = p; w(0);     /* JP PO,fail (patched) */

    /* success */
    b(0x11); w(MSG_OK);               /* LD DE,MSG_OK */
    b(0x0E); b(0x09);                 /* LD C,9 */
    b(0xCD); w(0x0005);               /* CALL 5 */
    b(0xC3); w(0x0000);               /* JP 0 */

    int fail = p;
    b(0x11); w(MSG_FAIL);
    b(0x0E); b(0x09);
    b(0xCD); w(0x0005);
    b(0xC3); w(0x0000);

    for (int i = 0; i < n_fail; i++)
        code[fail_sites[i]] = (uint8_t)(fail - (fail_sites[i] + 1));
    code[jp_po] = (uint8_t)((ORG + fail) & 0xFF);
    code[jp_po + 1] = (uint8_t)((ORG + fail) >> 8);

    const uint8_t data[4] = { 0x11, 0x22, 0x33, 0x44 };
    memcpy(code + (DATA - ORG), data, 4);
    /* CP/M strings end at '$'; no NUL — MSG_FAIL's would land one byte
     * past the 256-byte array (macOS's fortified strcpy aborts on it). */
    memcpy(code + (MSG_OK - ORG),   "PORT I/O OK\r\n$",   14);
    memcpy(code + (MSG_FAIL - ORG), "PORT I/O FAIL\r\n$", 16);

    FILE *f = fopen("tests/ports.com", "wb");
    if (!f) { perror("tests/ports.com"); return 1; }
    fwrite(code, 1, sizeof code, f);
    fclose(f);
    printf("Wrote tests/ports.com — exercises OUT (n),A / OUT (C),r / OTIR / IN A,(n) / IN r,(C) / INI\n");
    return 0;
}
