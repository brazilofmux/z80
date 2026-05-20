/* tools/mkhello.c — emit a tiny CP/M .COM that prints a $-string via BDOS 9 */
#include <stdio.h>
#include <stdint.h>

int main(void) {
    /* Hand-assembled equivalent of:
     *
     *   org 100h
     *   ld  de, msg
     *   ld  c, 9
     *   call 5
     *   ret
     * msg: db 'Hello from the 10 BIPS future!$'
     */

    uint8_t code[] = {
        0x11, 0x09, 0x01,   /* LD DE, 0109h */
        0x0E, 0x09,         /* LD C, 9 */
        0xCD, 0x05, 0x00,   /* CALL 0005h */
        0xC9,               /* RET */
        /* msg at 0109 */
        'H','e','l','l','o',' ','f','r','o','m',' ',
        't','h','e',' ','1','0',' ','B','I','P','S',' ',
        'f','u','t','u','r','e','!','$'
    };

    FILE *f = fopen("tests/hello.com", "wb");
    if (!f) { perror("tests/hello.com"); return 1; }
    fwrite(code, 1, sizeof(code), f);
    fclose(f);
    printf("Wrote tests/hello.com (%zu bytes)\n", sizeof(code));
    return 0;
}
