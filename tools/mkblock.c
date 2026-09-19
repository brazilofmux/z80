/* tools/mkblock.c — test LDIR + 16-bit ADD HL */
#include <stdio.h>
#include <stdint.h>

int main(void) {
    /* Memory layout after load at 0x100:
     *   0100-0115 : code (22 bytes)
     *   0118-011F : 8-byte dst buffer
     *   0120-0127 : "BLOCKOK$"
     *   0128-013C : "LDIR+ADD HL test OK$"
     */

    uint8_t code[64] = {0};

    int p = 0;
    /* LD HL, 0x0120   (src) */
    code[p++] = 0x21; code[p++] = 0x20; code[p++] = 0x01;
    /* LD DE, 0x0118   (dst) */
    code[p++] = 0x11; code[p++] = 0x18; code[p++] = 0x01;
    /* LD BC, 8 */
    code[p++] = 0x01; code[p++] = 0x08; code[p++] = 0x00;
    /* LDIR */
    code[p++] = 0xED; code[p++] = 0xB0;
    /* LD DE, 0x0128  (msg) */
    code[p++] = 0x11; code[p++] = 0x28; code[p++] = 0x01;
    /* LD C, 9 */
    code[p++] = 0x0E; code[p++] = 0x09;
    /* CALL 5 */
    code[p++] = 0xCD; code[p++] = 0x05; code[p++] = 0x00;
    /* JP 0  (guaranteed warm boot, easier than RET stack games) */
    code[p++] = 0xC3; code[p++] = 0x00; code[p++] = 0x00;
    /* p == 22 now — dst buffer occupies 24..31 (0x118), clear of the JP 0 */

    /* src string at 0x0120 (offset 0x20 = 32 in array) */
    const char *src = "BLOCKOK$";
    for (int i = 0; src[i]; i++) code[32 + i] = (uint8_t)src[i];

    /* msg at 0x0128 (offset 0x28 = 40) */
    const char *msg = "LDIR+ADD HL test OK$";
    for (int i = 0; msg[i]; i++) code[40 + i] = (uint8_t)msg[i];

    FILE *f = fopen("tests/block.com", "wb");
    if (!f) { perror("tests/block.com"); return 1; }
    fwrite(code, 1, sizeof(code), f);
    fclose(f);
    printf("Wrote tests/block.com — exercises LDIR and 16-bit ADD HL paths\n");
    return 0;
}
