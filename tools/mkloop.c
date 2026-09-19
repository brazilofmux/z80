/* tools/mkloop.c — tests/loop.com: countdown loops the translator folds.
 *
 *   DEC A / JP NZ back  (A=5, then A=0 -> 256 iterations)
 *   DEC B / JR NZ back  (B=3)
 *   DJNZ $              (B=0 -> 256 iterations)
 * After each loop the register, F and the loop's exit are recorded so
 * -V lockstep compares registers, flags AND instruction counts between
 * the folded translation and the interpreter running every iteration.
 * Prints "LOOPS OK" via BDOS 9 when the recorded values match.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static uint8_t code[512];
static int p;
static void b(int v) { code[p++] = (uint8_t)v; }
static void w(int v) { b(v & 0xFF); b(v >> 8); }

int main(void) {
    const int ORG = 0x100;
    const int RES = ORG + 0x180, MSG_OK = ORG + 0x1A0, MSG_FAIL = ORG + 0x1B0;
    /* Results: [0]=A after loop1, [1]=F, [2]=A after loop2 (256), [3]=F,
     * [4]=B after loop3, [5]=F, [6]=B after DJNZ, [7]=F */
    b(0x37);                          /* SCF: C=1 so the DEC loops must preserve it */
    b(0x3E); b(5);                    /* LD A,5 */
    int l1 = p; b(0x3D); b(0xC2); w(ORG + l1);        /* DEC A ; JP NZ,l1 */
    b(0x32); w(RES + 0);              /* LD (RES),A */
    b(0xF5); b(0xE1); b(0x7D); b(0x32); w(RES + 1); /* PUSH AF; POP HL; LD A,L; LD (RES+1),A */
    b(0xAF);                          /* XOR A -> C=0 */
    int l2 = p; b(0x3D); b(0xC2); w(ORG + l2);        /* DEC A ; JP NZ,l2  (256 times) */
    b(0x32); w(RES + 2);
    b(0xF5); b(0xE1); b(0x7D); b(0x32); w(RES + 3);
    b(0x06); b(3);                    /* LD B,3 */
    int l3 = p; b(0x05); b(0x20); b((uint8_t)(l3 - (p + 1)));  /* DEC B ; JR NZ,l3 */
    b(0x78); b(0x32); w(RES + 4);     /* LD A,B ; LD (RES+4),A */
    b(0xF5); b(0xE1); b(0x7D); b(0x32); w(RES + 5);
    b(0x06); b(0);                    /* LD B,0 */
    int l4 = p; b(0x10); b((uint8_t)(l4 - (p + 1)));            /* DJNZ $ (256 times) */
    b(0x78); b(0x32); w(RES + 6);
    b(0xF5); b(0xE1); b(0x7D); b(0x32); w(RES + 7);
    /* Check: A results 0,0; B results 0,0; F after loops 1/2/3 has Z and N set. */
    b(0x21); w(RES);                  /* LD HL,RES */
    b(0x7E); b(0xB7); b(0xC2); int f1 = p; w(0);      /* LD A,(HL); OR A; JP NZ,fail */
    b(0x23); b(0x7E); b(0xE6); b(0x42); b(0xFE); b(0x42); b(0xC2); int f2 = p; w(0); /* F1 & 42 == 42 */
    b(0x23); b(0x7E); b(0xB7); b(0xC2); int f3 = p; w(0);   /* A2 == 0 */
    b(0x23); b(0x7E); b(0xE6); b(0x43); b(0xFE); b(0x42); b(0xC2); int f4 = p; w(0); /* F2: Z,N set, C clear */
    b(0x23); b(0x7E); b(0xB7); b(0xC2); int f5 = p; w(0);   /* B3 == 0 */
    b(0x23); b(0x23); b(0x7E); b(0xB7); b(0xC2); int f6 = p; w(0); /* B4 == 0 */
    b(0x11); w(MSG_OK); b(0x0E); b(9); b(0xCD); w(5);       /* print OK */
    b(0xC3); w(0);
    int fail = p;
    b(0x11); w(MSG_FAIL); b(0x0E); b(9); b(0xCD); w(5);
    b(0xC3); w(0);
    int sites[] = { f1, f2, f3, f4, f5, f6 };
    for (int i = 0; i < 6; i++) { code[sites[i]] = (uint8_t)((ORG + fail) & 0xFF); code[sites[i] + 1] = (uint8_t)((ORG + fail) >> 8); }
    memcpy(code + (MSG_OK - ORG),   "LOOPS OK\r\n$",   11);
    memcpy(code + (MSG_FAIL - ORG), "LOOPS FAIL\r\n$", 13);
    FILE *f = fopen("tests/loop.com", "wb");
    if (!f) { perror("tests/loop.com"); return 1; }
    fwrite(code, 1, sizeof code, f);
    fclose(f);
    printf("Wrote tests/loop.com — countdown loops (DEC/JP NZ, DEC/JR NZ, DJNZ $)\n");
    return 0;
}
