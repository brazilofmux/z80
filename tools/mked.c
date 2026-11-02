/* tools/mked.c — self-checking test for the ED interrupt/system column:
 * IM 0/1/2 (all eight encodings), RETN/RETI, LD I,A / LD A,I,
 * LD R,A / LD A,R, the undocumented NEG copies, and ED no-ops.
 *
 * Prints "ED OK" on success, "ED FAIL n" (n = check number) otherwise.
 * Under -V the IM/IFF/I/R state is also compared in lockstep. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static uint8_t code[1024];
static int pc;                 /* offset from 0100h */
#define ORG 0x100

static void b(int x) { code[pc++] = (uint8_t)x; }
static void w(int x) { b(x & 0xFF); b(x >> 8); }

/* Forward references to the "fail" stub: each check gets its own stub
 * that loads the check number and jumps to the common failure path. */
static int fail_fix[64], fail_num[64], nfail;
static void jp_cc_fail(int opcode, int num) {
    b(opcode); fail_fix[nfail] = pc; fail_num[nfail++] = num; w(0);
}
#define JP_NZ 0xC2
#define JP_Z  0xCA
#define JP_PO 0xE2
#define JP_PE 0xEA

int main(void) {
    /* 1: LD I,A / LD A,I round trip */
    b(0x3E); b(0x5A);            /* LD A,5Ah   */
    b(0xED); b(0x47);            /* LD I,A     */
    b(0xAF);                     /* XOR A      */
    b(0xED); b(0x57);            /* LD A,I     */
    b(0xFE); b(0x5A);            /* CP 5Ah     */
    jp_cc_fail(JP_NZ, 1);

    /* 2: LD A,I with IFF2 = 0 -> P/V clear */
    b(0xF3);                     /* DI         */
    b(0xED); b(0x57);            /* LD A,I     */
    jp_cc_fail(JP_PE, 2);

    /* 3: LD A,I with IFF2 = 1 -> P/V set */
    b(0xFB);                     /* EI         */
    b(0xED); b(0x57);            /* LD A,I     */
    jp_cc_fail(JP_PO, 3);
    b(0xF3);                     /* DI         */

    /* 4: LD A,I with I = 0 -> Z set */
    b(0xAF);                     /* XOR A      */
    b(0xED); b(0x47);            /* LD I,A     */
    b(0x3C);                     /* INC A (A=1, Z clear) */
    b(0xED); b(0x57);            /* LD A,I     */
    jp_cc_fail(JP_NZ, 4);

    /* IM in every encoding; the last one leaves IM 2 */
    static const uint8_t ims[] = { 0x46, 0x4E, 0x56, 0x66, 0x6E, 0x76, 0x7E, 0x5E };
    for (unsigned i = 0; i < sizeof ims; i++) { b(0xED); b(ims[i]); }

    /* 5: LD R,A keeps bit 7 of R across instructions */
    b(0x3E); b(0x80);            /* LD A,80h   */
    b(0xED); b(0x4F);            /* LD R,A     */
    b(0x00); b(0x00); b(0x00);   /* NOP x3     */
    b(0xED); b(0x5F);            /* LD A,R     */
    b(0xE6); b(0x80);            /* AND 80h    */
    jp_cc_fail(JP_Z, 5);

    /* 6: LD A,R advances (R was 80h, several insns ran) */
    b(0x3E); b(0x80);            /* LD A,80h   */
    b(0xED); b(0x4F);            /* LD R,A     */
    b(0xED); b(0x5F);            /* LD A,R     */
    b(0xFE); b(0x80);            /* CP 80h     */
    jp_cc_fail(JP_Z, 6);

    /* 7..14: NEG and its seven copies: 1 -> FFh */
    static const uint8_t negs[] = { 0x44, 0x4C, 0x54, 0x5C, 0x64, 0x6C, 0x74, 0x7C };
    for (unsigned i = 0; i < sizeof negs; i++) {
        b(0x3E); b(0x01);        /* LD A,1     */
        b(0xED); b(negs[i]);     /* NEG        */
        b(0xFE); b(0xFF);        /* CP FFh     */
        jp_cc_fail(JP_NZ, 7 + (int)i);
    }

    /* ED no-ops: must not stop the CPU or disturb A */
    static const uint8_t nops[] = { 0x00, 0x3F, 0x77, 0x7F, 0x80, 0xA4, 0xBF, 0xC0, 0xFF };
    b(0x3E); b(0x33);            /* LD A,33h   */
    for (unsigned i = 0; i < sizeof nops; i++) { b(0xED); b(nops[i]); }
    b(0xFE); b(0x33);            /* CP 33h     */
    jp_cc_fail(JP_NZ, 15);

    /* 16..23: RETN/RETI in every encoding return to the pushed address
     * and copy IFF2 into IFF1 (here both stay 0: DI is in effect). */
    static const uint8_t rets[] = { 0x45, 0x4D, 0x55, 0x5D, 0x65, 0x6D, 0x75, 0x7D };
    for (unsigned i = 0; i < sizeof rets; i++) {
        int target = ORG + pc + 3 + 1 + 2 + 3;   /* after LD HL / PUSH / RETN / JP */
        b(0x21); w(target);      /* LD HL,target */
        b(0xE5);                 /* PUSH HL      */
        b(0xED); b(rets[i]);     /* RETN / RETI  */
        b(0xC3); fail_fix[nfail] = pc; fail_num[nfail++] = 16 + (int)i; w(0);  /* JP fail */
    }

    /* success */
    int msg_ok_fix = pc + 1;
    b(0x11); w(0);               /* LD DE,ok   */
    b(0x0E); b(0x09);            /* LD C,9     */
    b(0xCD); w(0x0005);          /* CALL 5     */
    b(0x0E); b(0x00);            /* LD C,0     */
    b(0xCD); w(0x0005);          /* CALL 5     */

    /* failure stubs: LD A,n ; JP common */
    int common_fix[64];
    for (int i = 0; i < nfail; i++) {
        int at = ORG + pc;
        code[fail_fix[i]] = at & 0xFF; code[fail_fix[i] + 1] = at >> 8;
        b(0x3E); b(fail_num[i]); /* LD A,n     */
        b(0xC3); common_fix[i] = pc; w(0);
    }
    /* common: store the number as two decimal digits into the message */
    int common = ORG + pc;
    for (int i = 0; i < nfail; i++) {
        code[common_fix[i]] = common & 0xFF; code[common_fix[i] + 1] = common >> 8;
    }
    int digits_fix;
    b(0x06); b(0x2F);            /* LD B,'0'-1 */
    int loop = ORG + pc;
    b(0x04);                     /* INC B      */
    b(0xD6); b(10);              /* SUB 10     */
    b(0xD2); w(loop);            /* JP NC,loop */
    b(0xC6); b(10 + '0');        /* ADD A,10+'0' */
    b(0x21); digits_fix = pc; w(0);  /* LD HL,digits */
    b(0x70);                     /* LD (HL),B  */
    b(0x23);                     /* INC HL     */
    b(0x77);                     /* LD (HL),A  */
    int msg_fail_fix = pc + 1;
    b(0x11); w(0);               /* LD DE,fail */
    b(0x0E); b(0x09);            /* LD C,9     */
    b(0xCD); w(0x0005);          /* CALL 5     */
    b(0x0E); b(0x00);            /* LD C,0     */
    b(0xCD); w(0x0005);          /* CALL 5     */

    int ok = ORG + pc;
    memcpy(code + pc, "ED OK\r\n$", 8); pc += 8;
    int fail = ORG + pc;
    memcpy(code + pc, "ED FAIL ", 8); pc += 8;
    int digits = ORG + pc;
    memcpy(code + pc, "00\r\n$", 5); pc += 5;

    code[msg_ok_fix] = ok & 0xFF;     code[msg_ok_fix + 1] = ok >> 8;
    code[msg_fail_fix] = fail & 0xFF; code[msg_fail_fix + 1] = fail >> 8;
    code[digits_fix] = digits & 0xFF; code[digits_fix + 1] = digits >> 8;

    FILE *f = fopen("tests/ed.com", "wb");
    if (!f) { perror("tests/ed.com"); return 1; }
    fwrite(code, 1, (size_t)pc, f);
    fclose(f);
    printf("Wrote tests/ed.com — ED interrupt/system column (IM, RETN/RETI, I, R, NEG copies, ED no-ops)\n");
    return 0;
}
