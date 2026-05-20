/* tools/mkix.c — minimal test for IX+d and half-register */
#include <stdio.h>
#include <stdint.h>

int main(void) {
    /* LD IX, 0x0120
     * LD (IX+3), 0x42     ; 'B'
     * LD A, (IX+3)
     * JP 0
     */
    uint8_t code[] = {
        0xDD, 0x21, 0x20, 0x01,   /* LD IX, 0120h */
        0xDD, 0x36, 0x03, 0x42,   /* LD (IX+3), 0x42 */
        0xDD, 0x7E, 0x03,         /* LD A, (IX+3) */
        0xC3, 0x00, 0x00          /* JP 0 */
    };

    FILE *f = fopen("tests/ix.com", "wb");
    if (!f) { perror("tests/ix.com"); return 1; }
    fwrite(code, 1, sizeof(code), f);
    fclose(f);
    printf("Wrote tests/ix.com — exercises LD IX, LD (IX+d), LD A,(IX+d)\n");
    return 0;
}
