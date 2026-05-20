/* tools/mkcb.c — simple test for CB rotate + BIT */
#include <stdio.h>
#include <stdint.h>

int main(void) {
    /* Hand-assembled at 0x100:
     *   LD A, 0x01
     *   RLC A          ; CB 07 -> A=0x02, C=1
     *   BIT 1, A       ; CB 50 -> should set Z=0 (bit 1 is 1 in 0x02)
     *   JP 0
     */

    uint8_t code[] = {
        0x3E, 0x01,         /* LD A, 1 */
        0xCB, 0x07,         /* RLC A */
        0xCB, 0x50,         /* BIT 1, A */
        0xC3, 0x00, 0x00    /* JP 0 */
    };

    FILE *f = fopen("tests/cb.com", "wb");
    if (!f) { perror("tests/cb.com"); return 1; }
    fwrite(code, 1, sizeof(code), f);
    fclose(f);
    printf("Wrote tests/cb.com — exercises CB rotate + BIT\n");
    return 0;
}
