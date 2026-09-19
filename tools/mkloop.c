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

static uint8_t code[2048];
static int p;
static void b(int v) { code[p++] = (uint8_t)v; }
static void w(int v) { b(v & 0xFF); b(v >> 8); }

int main(void) {
    const int ORG = 0x100;
    /* Layout: code below 0x200; SRC 0x200; DST 0x300..0x408, fill area DST+0x180;
     * results 0x500; messages 0x520/0x530; S2 0x540; D2 0x580. */
    const int RES = ORG + 0x500, MSG_OK = ORG + 0x520, MSG_FAIL = ORG + 0x530;
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
    /* Copy loop: 5 bytes SRC -> DST counted in C; then 256 bytes counted in B. */
    const int SRC = ORG + 0x200, DST = ORG + 0x300;
    b(0x21); w(SRC); b(0x11); w(DST); b(0x0E); b(5);              /* LD HL,SRC; LD DE,DST; LD C,5 */
    int l5 = p; b(0x7E); b(0x12); b(0x23); b(0x13); b(0x0D); b(0xC2); w(ORG + l5);
    b(0x32); w(RES + 8);                                           /* A = last byte copied */
    b(0x21); w(DST + 8); b(0x11); w(SRC); b(0x06); b(0);           /* (DE)=SRC -> (HL)=DST+8, 256 bytes, B=0 */
    int l6 = p; b(0x1A); b(0x77); b(0x13); b(0x23); b(0x05); b(0x20); b((uint8_t)(l6 - (p + 1)));  /* LD A,(DE); LD (HL),A; INC DE; INC HL; DEC B; JR NZ */
    /* HL should be DST+8+256: store L and H */
    b(0x7D); b(0x32); w(RES + 9); b(0x7C); b(0x32); w(RES + 10);
    /* Fill loop: 3 bytes of 0x55 at DST+0x180 counted in E. */
    b(0x21); w(DST + 0x180); b(0x3E); b(0x55); b(0x1E); b(3);
    int l7 = p; b(0x77); b(0x23); b(0x1D); b(0xC2); w(ORG + l7);
    b(0x7D); b(0x32); w(RES + 11);
    /* Filtered copy (WordStar's shape): three runs, one per exit. */
    const int S2 = ORG + 0x540, D2 = ORG + 0x580;
    #define FILTER_LOOP(exitlbl) do { int h = p; \
        b(0x1A); b(0xE6); b(0x7F); b(0xFE); b(0x20); b(0xDA); int x1 = p; w(0); \
        b(0xFE); b(0x7F); b(0xCA); int x2 = p; w(0); \
        b(0x77); b(0x23); b(0x13); b(0x04); b(0x0D); b(0xC2); w(ORG + h); \
        code[x1] = (uint8_t)((ORG + p) & 0xFF); code[x1 + 1] = (uint8_t)((ORG + p) >> 8); \
        code[x2] = code[x1]; code[x2 + 1] = code[x1 + 1]; } while (0)
    /* run 1: "ABCDE\x01..." with C=10 -> exits on the control char after 5 */
    b(0x11); w(S2); b(0x21); w(D2); b(0x06); b(0); b(0x0E); b(10);
    FILTER_LOOP(1);
    b(0x32); w(RES + 14);                                 /* A = 1 (the control char, masked) */
    b(0x78); b(0x32); w(RES + 12);                       /* B = 5 */
    b(0x79); b(0x32); w(RES + 13);                       /* C = 5 */
    /* run 2: C=3 over printable text -> exits when C reaches 0 */
    b(0x11); w(S2); b(0x21); w(D2 + 0x10); b(0x06); b(0); b(0x0E); b(3);
    FILTER_LOOP(2);
    b(0x32); w(RES + 16);                                 /* A = 'C' */
    b(0x78); b(0x32); w(RES + 15);                       /* B = 3 */
    /* run 3: sentinel 0x7F (stored as 0xFF, masked to 7F) at S2+8+2 */
    b(0x11); w(S2 + 8); b(0x21); w(D2 + 0x20); b(0x06); b(0); b(0x0E); b(10);
    FILTER_LOOP(3);
    b(0x32); w(RES + 18);                                 /* A = 7F */
    b(0x78); b(0x32); w(RES + 17);                       /* B = 2 */
    /* Check: A results 0,0; B results 0,0; F after loops 1/2/3 has Z and N set. */
    b(0x21); w(RES);                  /* LD HL,RES */
    b(0x7E); b(0xB7); b(0xC2); int f1 = p; w(0);      /* LD A,(HL); OR A; JP NZ,fail */
    b(0x23); b(0x7E); b(0xE6); b(0x42); b(0xFE); b(0x42); b(0xC2); int f2 = p; w(0); /* F1 & 42 == 42 */
    b(0x23); b(0x7E); b(0xB7); b(0xC2); int f3 = p; w(0);   /* A2 == 0 */
    b(0x23); b(0x7E); b(0xE6); b(0x43); b(0xFE); b(0x42); b(0xC2); int f4 = p; w(0); /* F2: Z,N set, C clear */
    b(0x23); b(0x7E); b(0xB7); b(0xC2); int f5 = p; w(0);   /* B3 == 0 */
    b(0x23); b(0x23); b(0x7E); b(0xB7); b(0xC2); int f6 = p; w(0); /* B4 == 0 */
    b(0x21); w(RES + 8); b(0x7E); b(0xFE); b(0x14); b(0xC2); int f7 = p; w(0);   /* last byte copied = SRC[4] = 0x14 */
    b(0x21); w(DST); b(0x7E); b(0xFE); b(0x10); b(0xC2); int f8 = p; w(0);       /* DST[0] = 0x10 */
    b(0x21); w(DST + 4); b(0x7E); b(0xFE); b(0x14); b(0xC2); int f9 = p; w(0);   /* DST[4] = 0x14 */
    b(0x21); w(DST + 8 + 255); b(0x7E); b(0xFE); b(0x0F); b(0xC2); int f10 = p; w(0); /* 256th copied byte = SRC[255] = 0x10+255 */
    b(0x3A); w(RES + 9); b(0xFE); b((DST + 8 + 256) & 0xFF); b(0xC2); int f11 = p; w(0);  /* L after copy */
    b(0x3A); w(RES + 10); b(0xFE); b((DST + 8 + 256) >> 8); b(0xC2); int f12 = p; w(0);   /* H after copy */
    b(0x21); w(DST + 0x180 + 2); b(0x7E); b(0xFE); b(0x55); b(0xC2); int f13 = p; w(0);   /* fill byte */
    b(0x21); w(DST + 0x180 + 3); b(0x7E); b(0xFE); b(0x55); b(0xCA); int f14 = p; w(0);   /* one past: NOT filled */
    b(0x3A); w(RES + 11); b(0xFE); b((DST + 0x180 + 3) & 0xFF); b(0xC2); int f15 = p; w(0);
    b(0x3A); w(RES + 12); b(0xFE); b(5); b(0xC2); int f16 = p; w(0);
    b(0x3A); w(RES + 13); b(0xFE); b(5); b(0xC2); int f17 = p; w(0);
    b(0x3A); w(RES + 14); b(0xFE); b(1); b(0xC2); int f18 = p; w(0);
    b(0x21); w(D2 + 4); b(0x7E); b(0xFE); b('E'); b(0xC2); int f19 = p; w(0);
    b(0x21); w(D2 + 5); b(0x7E); b(0xB7); b(0xC2); int f20 = p; w(0);          /* not written */
    b(0x3A); w(RES + 15); b(0xFE); b(3); b(0xC2); int f21 = p; w(0);
    b(0x3A); w(RES + 16); b(0xFE); b('C'); b(0xC2); int f22 = p; w(0);
    b(0x3A); w(RES + 17); b(0xFE); b(2); b(0xC2); int f23 = p; w(0);
    b(0x3A); w(RES + 18); b(0xFE); b(0x7F); b(0xC2); int f24 = p; w(0);
    b(0x21); w(D2 + 0x21); b(0x7E); b(0xFE); b('Y'); b(0xC2); int f25 = p; w(0);  /* 'y'|0x80 masked to 'y'? no: S2+9 = 'Y' */
    b(0x11); w(MSG_OK); b(0x0E); b(9); b(0xCD); w(5);       /* print OK */
    b(0xC3); w(0);
    int fail = p;                       /* E = check letter: print it, then the message */
    b(0x0E); b(2); b(0xCD); w(5);
    b(0x11); w(MSG_FAIL); b(0x0E); b(9); b(0xCD); w(5);
    b(0xC3); w(0);
    int sites[] = { f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
                    f16, f17, f18, f19, f20, f21, f22, f23, f24, f25 };
    /* Per-check stubs live at 0x600: LD E,'A'+i ; JP fail */
    for (int i = 0; i < 25; i++) {
        int stub = 0x600 + 5 * i;
        code[stub] = 0x1E; code[stub + 1] = (uint8_t)('A' + i);
        code[stub + 2] = 0xC3; code[stub + 3] = (uint8_t)((ORG + fail) & 0xFF); code[stub + 4] = (uint8_t)((ORG + fail) >> 8);
        code[sites[i]] = (uint8_t)((ORG + stub) & 0xFF); code[sites[i] + 1] = (uint8_t)((ORG + stub) >> 8);
    }
    memcpy(code + (S2 - ORG), "ABCDE\x01GH" "XY\xFFQ", 12);   /* S2+8..: 'X','Y',0xFF(->7F),'Q' */
    for (int i = 0; i < 256; i++) code[(SRC - ORG) + i] = (uint8_t)(0x10 + i);   /* SRC[i] = 0x10 + i */
    memcpy(code + (MSG_OK - ORG),   "LOOPS OK\r\n$",   11);
    memcpy(code + (MSG_FAIL - ORG), ": LOOPS FAIL\r\n$", 15);
    if (p > 0x200) { fprintf(stderr, "mkloop: code is %d bytes, overlaps the data at 0x200\n", p); return 1; }
    FILE *f = fopen("tests/loop.com", "wb");
    if (!f) { perror("tests/loop.com"); return 1; }
    fwrite(code, 1, sizeof code, f);
    fclose(f);
    printf("Wrote tests/loop.com — countdown loops (DEC/JP NZ, DEC/JR NZ, DJNZ $)\n");
    return 0;
}
