/* tools/mkconsole.c — interactive console test (CONST + CONIN + CONOUT) */
#include <stdio.h>
#include <stdint.h>

int main(void) {
    /* Simple echo loop using BDOS:
     *   Loop:
     *     CONST (func 11)
     *     if ready: CONIN (func 1), CONOUT (func 2)
     *     if char == 0x1B (ESC) or 'q', exit via BDOS 0
     */
    uint8_t code[] = {
        /* top: */
        0x0E, 0x0B,             /* LD C, 11 (CONST) */
        0xCD, 0x05, 0x00,       /* CALL 5 */
        0xB7,                   /* OR A */
        0xCA, 0x00, 0x01,       /* JP Z, top (no key) */

        0x0E, 0x01,             /* LD C, 1 (CONIN) */
        0xCD, 0x05, 0x00,       /* CALL 5 */
        0x5F,                   /* LD E, A */

        0x0E, 0x02,             /* LD C, 2 (CONOUT) */
        0xCD, 0x05, 0x00,       /* CALL 5 */

        0x7B,                   /* LD A, E */
        0xFE, 0x1B,             /* CP 0x1B (ESC) */
        0xCA, 0x20, 0x01,       /* JP Z, exit */
        0xFE, 0x71,             /* CP 'q' */
        0xCA, 0x20, 0x01,       /* JP Z, exit */

        0xC3, 0x00, 0x01,       /* JP top */

        /* exit: */
        0x0E, 0x00,             /* LD C, 0 */
        0xCD, 0x05, 0x00,       /* CALL 5 (warm boot) */
    };

    FILE *f = fopen("tests/console.com", "wb");
    if (!f) { perror("tests/console.com"); return 1; }
    fwrite(code, 1, sizeof(code), f);
    fclose(f);
    printf("Wrote tests/console.com — interactive CONST/CONIN/CONOUT echo test (ESC or q to exit)\n");
    return 0;
}
