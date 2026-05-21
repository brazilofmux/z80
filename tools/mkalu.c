/* tools/mkalu.c — ALU corner-case sweep designed to flush flag bugs.
 *
 * Strategy: walk a fixed list of (a, b) pairs that hit half-carry,
 * signed overflow, zero result, sign change. For each pair, run every
 * ALU op (ADD/ADC/SUB/SBC/AND/OR/XOR/CP/INC/DEC) and store the
 * resulting AF (or A only, for CP/INC/DEC) to a fixed memory region
 * starting at 0x8000.
 *
 * Then we run under -V; any flag-computation divergence triggers the
 * verifier immediately at the offending JIT block. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static uint8_t buf[1024];
static size_t n = 0;

static void emit(uint8_t b) { buf[n++] = b; }
static void emit2(uint8_t a, uint8_t b) { emit(a); emit(b); }
static void emit3(uint8_t a, uint8_t b, uint8_t c) { emit(a); emit(b); emit(c); }

/* Save AF to (HL), advance HL by 2:  push af ; pop bc ; ld (hl),c ; inc hl ; ld (hl),b ; inc hl */
static void save_af(void) {
    emit(0xF5);             /* push af */
    emit(0xC1);             /* pop bc */
    emit(0x71);             /* ld (hl), c   ; F */
    emit(0x23);             /* inc hl */
    emit(0x70);             /* ld (hl), b   ; A */
    emit(0x23);             /* inc hl */
}

/* Drive one ALU op via A,n. op_n is the immediate-form opcode (0xC6=ADD,
 * 0xCE=ADC, 0xD6=SUB, 0xDE=SBC, 0xE6=AND, 0xEE=XOR, 0xF6=OR, 0xFE=CP). */
static void test_alu_n(uint8_t op_n, uint8_t a, uint8_t b) {
    emit2(0x3E, a);         /* ld a, a */
    emit2(op_n, b);         /* op a, b */
    save_af();
}

/* INC r / DEC r where r is encoded as part of the opcode (INC B=0x04,
 * DEC B=0x05, INC A=0x3C, DEC A=0x3D). */
static void test_incdec(uint8_t op, uint8_t a) {
    emit2(0x3E, a);         /* ld a, a */
    emit(op);
    save_af();
}

int main(void) {
    n = 0;

    /* Initialise carry flag in a known state via a deterministic op. */
    emit2(0x3E, 0x00);      /* ld a, 0 */
    emit2(0xC6, 0x01);      /* add a, 1   -> A=1, no flags interesting */

    /* Save destination pointer: HL = 0x8000. */
    emit3(0x21, 0x00, 0x80);    /* ld hl, 0x8000 */

    /* Pair list — chosen to span half-carry, overflow, zero, sign change. */
    static const struct { uint8_t a, b; } pairs[] = {
        {0x00, 0x00}, {0x01, 0x01}, {0x0F, 0x01}, {0x10, 0x01},
        {0x7F, 0x01}, {0x80, 0x01}, {0x80, 0x80}, {0xFF, 0x01},
        {0xFF, 0xFF}, {0x00, 0xFF}, {0x7F, 0xFF}, {0xC3, 0x55},
    };

    const uint8_t alu_ops[] = {0xC6, 0xCE, 0xD6, 0xDE, 0xE6, 0xEE, 0xF6, 0xFE};
    for (size_t op = 0; op < sizeof(alu_ops); op++) {
        for (size_t p = 0; p < sizeof(pairs)/sizeof(pairs[0]); p++) {
            test_alu_n(alu_ops[op], pairs[p].a, pairs[p].b);
        }
    }

    /* INC/DEC for A and B with corner-case starting values. */
    const uint8_t starts[] = {0x00, 0x0F, 0x10, 0x7F, 0x80, 0xFF};
    for (size_t i = 0; i < sizeof(starts); i++) {
        test_incdec(0x3C, starts[i]);   /* INC A */
        test_incdec(0x3D, starts[i]);   /* DEC A */
    }

    emit3(0xC3, 0x00, 0x00); /* jp 0 — exit */

    FILE *f = fopen("tests/alu.com", "wb");
    if (!f) { perror("alu.com"); return 1; }
    fwrite(buf, 1, n, f);
    fclose(f);
    printf("Wrote tests/alu.com (%zu bytes)\n", n);
    return 0;
}
