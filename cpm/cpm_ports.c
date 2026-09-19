/* cpm_ports.c — default port device: a latch per port, plus a debug port.
 *
 * Enough to exercise the interpreter's ED I/O family end to end (OUT
 * then IN reads back the byte) and to give Phase 2's BIOS a place to
 * grow. Nothing here is Kaypro hardware; the real port map lives in the
 * machine plan (docs/machine-plan.md).
 */
#include "cpm.h"
#include <stdio.h>
#include <string.h>

#define PORT_DEBUG 0xF0

static uint8_t latch[256];

static uint8_t ports_in(z80_cpu_t *cpu, uint8_t port, uint8_t high) {
    (void)cpu; (void)high;
    return latch[port];
}

static void ports_out(z80_cpu_t *cpu, uint8_t port, uint8_t high, uint8_t val) {
    (void)cpu; (void)high;
    latch[port] = val;
    if (port == PORT_DEBUG && cpm_debug)
        fprintf(stderr, "[port F0] %02X\n", val);
}

void cpm_install_ports(z80_cpu_t *cpu) {
    memset(latch, 0xFF, sizeof latch);
    cpu->port_in  = ports_in;
    cpu->port_out = ports_out;
}
