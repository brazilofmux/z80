/* tools/mkfileio.c — minimal Make + Write Sequential test */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    uint8_t code[1024];
    memset(code, 0, sizeof(code));
    int p = 0;

    /* Set DMA = 0x0200 */
    code[p++] = 0x11; code[p++] = 0x00; code[p++] = 0x02;   /* LD DE,0200 */
    code[p++] = 0x0E; code[p++] = 0x1A;                   /* LD C,26 */
    code[p++] = 0xCD; code[p++] = 0x05; code[p++] = 0x00; /* CALL 5 */

    /* MAKE file (FCB at 0x0100) */
    code[p++] = 0x11; code[p++] = 0x00; code[p++] = 0x01;
    code[p++] = 0x0E; code[p++] = 0x16;                   /* LD C,22 (MAKE) */
    code[p++] = 0xCD; code[p++] = 0x05; code[p++] = 0x00;
    code[p++] = 0xB7;                                     /* OR A */
    code[p++] = 0xC2; code[p++] = 0x30; code[p++] = 0x01; /* JP NZ, fail */

    /* Write one record */
    code[p++] = 0x11; code[p++] = 0x00; code[p++] = 0x01;
    code[p++] = 0x0E; code[p++] = 0x15;                   /* LD C,21 (WRITE) */
    code[p++] = 0xCD; code[p++] = 0x05; code[p++] = 0x00;
    code[p++] = 0xB7;
    code[p++] = 0xC2; code[p++] = 0x30; code[p++] = 0x01;

    /* Close */
    code[p++] = 0x11; code[p++] = 0x00; code[p++] = 0x01;
    code[p++] = 0x0E; code[p++] = 0x10;
    code[p++] = 0xCD; code[p++] = 0x05; code[p++] = 0x00;

    /* Success */
    code[p++] = 0x11; code[p++] = 0x80; code[p++] = 0x01; /* msg */
    code[p++] = 0x0E; code[p++] = 0x09;
    code[p++] = 0xCD; code[p++] = 0x05; code[p++] = 0x00;
    code[p++] = 0x0E; code[p++] = 0x00;
    code[p++] = 0xCD; code[p++] = 0x05; code[p++] = 0x00;

    /* Fail */
    code[p++] = 0x11; code[p++] = 0x90; code[p++] = 0x01;
    code[p++] = 0x0E; code[p++] = 0x09;
    code[p++] = 0xCD; code[p++] = 0x05; code[p++] = 0x00;
    code[p++] = 0x0E; code[p++] = 0x00;
    code[p++] = 0xCD; code[p++] = 0x05; code[p++] = 0x00;

    /* Pad to 0x0100 for FCB */
    while (p < 0x100) code[p++] = 0x00;

    /* FCB at 0x0100: TEST.DAT */
    code[0x100] = 0;                    /* drive A */
    memcpy(&code[0x101], "TEST    ", 8);
    memcpy(&code[0x109], "DAT", 3);

    /* Buffer at 0x0200 */
    const char *msg = "Hello from Write Sequential! This is record zero.";
    strncpy((char*)&code[0x200], msg, 128);

    /* Messages */
    strcpy((char*)&code[0x380], "FILE WRITE OK$");
    strcpy((char*)&code[0x390], "FILE WRITE FAILED$");

    FILE *f = fopen("tests/fileio.com", "wb");
    if (!f) { perror("tests/fileio.com"); return 1; }
    fwrite(code, 1, sizeof(code), f);
    fclose(f);

    printf("Wrote tests/fileio.com — exercises Make + Write Sequential\n");
    return 0;
}
