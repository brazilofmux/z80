/* tools/mkix.c — test for IX+d memory ops + half registers + clean exit */
#include <stdio.h>
#include <stdint.h>

int main(void) {
    /* Exercises:
     *   LD IX, nn
     *   LD (IX+d), n
     *   LD A, (IX+d)
     *   LD IXH, n
     *   LD A, IXH
     *   INC (IX+d)
     *   ADD A, (IX+d)
     * Then BDOS 0 for clean termination.
     */
    uint8_t code[] = {
        0xDD, 0x21, 0x00, 0x01,       /* LD IX, 0100h */
        0x3E, 0x41,                   /* LD A, 'A' */
        0xDD, 0x77, 0x10,             /* LD (IX+0x10), A   (this form has execution) */
        0xDD, 0x7E, 0x10,             /* LD A, (IX+0x10) */
        0xDD, 0x26, 0x42,             /* LD IXH, 'B' */
        0xDD, 0x7C,                   /* LD A, IXH */
        0xDD, 0x34, 0x10,             /* INC (IX+0x10) */
        0xDD, 0x86, 0x10,             /* ADD A, (IX+0x10) */
        0x0E, 0x00,                   /* LD C, 0 */
        0xCD, 0x05, 0x00,             /* CALL 5 (BDOS 0 - clean exit) */
    };

    FILE *f = fopen("tests/ix.com", "wb");
    if (!f) { perror("tests/ix.com"); return 1; }
    fwrite(code, 1, sizeof(code), f);
    fclose(f);
    printf("Wrote tests/ix.com — exercises multiple IX+d and IXH forms + clean BDOS exit\n");
    return 0;
}

