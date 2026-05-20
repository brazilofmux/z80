/* tools/mkblock.c — test LDIR + 16-bit ADD HL */
#include <stdio.h>
#include <stdint.h>

int main(void) {
    /* Memory layout after load at 0x100:
     *   0100-0112 : code (19 bytes)
     *   0113-011A : 8-byte dst buffer
     *   011B-0122 : "BLOCKOK$"
     *   0123-0136 : "LDIR test via block.com OK$"
     */

    uint8_t code[64] = {0};

    int p = 0;
    /* LD HL, 0x011B   (src) */
    code[p++] = 0x21; code[p++] = 0x1B; code[p++] = 0x01;
    /* LD DE, 0x0113   (dst) */
    code[p++] = 0x11; code[p++] = 0x13; code[p++] = 0x01;
    /* LD BC, 8 */
    code[p++] = 0x01; code[p++] = 0x08; code[p++] = 0x00;
    /* LDIR */
    code[p++] = 0xED; code[p++] = 0xB0;
    /* LD DE, 0x0123  (msg) */
    code[p++] = 0x11; code[p++] = 0x23; code[p++] = 0x01;
    /* LD C, 9 */
    code[p++] = 0x0E; code[p++] = 0x09;
    /* CALL 5 */
    code[p++] = 0xCD; code[p++] = 0x05; code[p++] = 0x00;
    /* JP 0  (guaranteed warm boot, easier than RET stack games) */
    code[p++] = 0xC3; code[p++] = 0x00; code[p++] = 0x00;
    /* p == 19 now — dst buffer occupies 19..26 */

    /* src string at 0x011B (offset 0x1B = 27 in array) */
    const char *src = "BLOCKOK$";
    for (int i = 0; src[i]; i++) code[27 + i] = (uint8_t)src[i];

    /* msg at 0x0123 (offset 0x23 = 35) */
    const char *msg = "LDIR+ADD HL test OK$";
    for (int i = 0; msg[i]; i++) code[35 + i] = (uint8_t)msg[i];

    FILE *f = fopen("tests/block.com", "wb");
    if (!f) { perror("tests/block.com"); return 1; }
    fwrite(code, 1, sizeof(code), f);
    fclose(f);
    printf("Wrote tests/block.com — exercises LDIR and 16-bit ADD HL paths\n");
    return 0;
}
