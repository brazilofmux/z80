/* tools/mkrandom.c — test Random Read (33) and Random Write (34) */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    uint8_t code[1024];
    memset(code, 0, sizeof(code));
    int p = 0;

    /* Set DMA = 0x0200 */
    code[p++] = 0x11; code[p++] = 0x00; code[p++] = 0x02;
    code[p++] = 0x0E; code[p++] = 0x1A; /* 26 = Set DMA */
    code[p++] = 0xCD; code[p++] = 0x05; code[p++] = 0x00;

    /* Make TEST.DAT */
    code[p++] = 0x11; code[p++] = 0x00; code[p++] = 0x01; /* FCB at 0100 */
    code[p++] = 0x0E; code[p++] = 0x16; /* 22 = Make */
    code[p++] = 0xCD; code[p++] = 0x05; code[p++] = 0x00;
    code[p++] = 0xB7;
    code[p++] = 0xC2; code[p++] = 0x50; code[p++] = 0x01; /* fail */

    /* Write record 0 sequentially (just to have something) */
    code[p++] = 0x11; code[p++] = 0x00; code[p++] = 0x01;
    code[p++] = 0x0E; code[p++] = 0x15; /* 21 = Write seq */
    code[p++] = 0xCD; code[p++] = 0x05; code[p++] = 0x00;
    code[p++] = 0xB7;
    code[p++] = 0xC2; code[p++] = 0x50; code[p++] = 0x01;

    /* Random write to record 5 (set random record in FCB[33..35]) */
    /* First set the random record number (little endian) */
    code[p++] = 0x21; code[p++] = 0x00; code[p++] = 0x01; /* HL = FCB */
    code[p++] = 0x36; code[p++] = 0x05; code[p++] = 0x00; /* LD (HL+33), 5 ; low byte */
    code[p++] = 0x23; code[p++] = 0x23; code[p++] = 0x23; /* HL += 3 */
    code[p++] = 0x36; code[p++] = 0x00; /* middle */
    code[p++] = 0x23;
    code[p++] = 0x36; code[p++] = 0x00; /* high */

    code[p++] = 0x11; code[p++] = 0x00; code[p++] = 0x01;
    code[p++] = 0x0E; code[p++] = 0x22; /* 34 = Random Write */
    code[p++] = 0xCD; code[p++] = 0x05; code[p++] = 0x00;
    code[p++] = 0xB7;
    code[p++] = 0xC2; code[p++] = 0x50; code[p++] = 0x01;

    /* Random read record 5 back */
    code[p++] = 0x11; code[p++] = 0x00; code[p++] = 0x01;
    code[p++] = 0x0E; code[p++] = 0x21; /* 33 = Random Read */
    code[p++] = 0xCD; code[p++] = 0x05; code[p++] = 0x00;
    code[p++] = 0xB7;
    code[p++] = 0xC2; code[p++] = 0x50; code[p++] = 0x01;

    /* Success */
    code[p++] = 0x11; code[p++] = 0x80; code[p++] = 0x01;
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

    /* Pad to FCB location */
    while (p < 0x100) code[p++] = 0x00;

    /* FCB for TEST.DAT at 0x0100 */
    memset(&code[0x100], 0, 36);
    code[0x100] = 0;
    memcpy(&code[0x101], "TEST", 4);
    memcpy(&code[0x109], "DAT", 3);

    /* Buffer at 0x0200 */
    const char *msg = "RANDOM RECORD 5 DATA - THIS WAS WRITTEN WITH BDOS 34";
    strncpy((char*)&code[0x200], msg, 128);

    /* Messages */
    strcpy((char*)&code[0x380], "RANDOM I/O OK$");
    strcpy((char*)&code[0x390], "RANDOM I/O FAILED$");

    FILE *f = fopen("tests/random.com", "wb");
    if (!f) { perror("tests/random.com"); return 1; }
    fwrite(code, 1, sizeof(code), f);
    fclose(f);

    printf("Wrote tests/random.com — exercises Random Read (33) and Random Write (34)\n");
    return 0;
}
