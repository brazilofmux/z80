#!/bin/sh
# Native CP/M 2.2 smoke test: build a floppy image from the generated
# test programs, boot DRI's CCP/BDOS on our BIOS, DIR, run two programs.
# Needs only what the build makes. Usage: tests/cpm22.sh [jit|interp|verify]
set -e
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/.." && pwd)
case "${1:-jit}" in
    jit) FLAG=-j ;; interp) FLAG=-i ;; verify) FLAG=-V; export Z80_VERIFY_STRICT=1 ;;
    *) echo "mode must be jit, interp or verify" >&2; exit 2 ;;
esac
cd "$ROOT"
IMG=tests/scripts/cpm22-boot.dsk
rm -f "$IMG" tests/scripts/cpm22-boot.dump
./tools/mkdsk -f fd "$IMG" tests/hello.com tests/block.com > /dev/null
./z80-monster $FLAG --script tests/scripts/cpm22-boot.script -A "$IMG" > /dev/null 2> tests/scripts/cpm22.err || true
grep -q 'divergence' tests/scripts/cpm22.err && { echo "cpm22 ($1): LOCKSTEP DIVERGENCE"; grep -A3 divergence tests/scripts/cpm22.err | head -6; exit 1; }
D=tests/scripts/cpm22-boot.dump
fail=0
grep -q '62K CP/M 2.2' "$D"            || { echo "cpm22: no sign-on"; fail=1; }
grep -q 'A>DIR' "$D"                   || { echo "cpm22: no prompt/echo"; fail=1; }
grep -q 'HELLO    COM : BLOCK    COM' "$D" || { echo "cpm22: DIR listing wrong"; fail=1; }
grep -q 'Hello from the 10 BIPS future' "$D" || { echo "cpm22: HELLO did not run"; fail=1; }
grep -q 'LDIR+ADD HL test OK' "$D"     || { echo "cpm22: BLOCK did not run"; fail=1; }
rm -f "$IMG"
[ $fail -eq 0 ] && echo "cpm22 ($1): OK — booted, DIR, ran HELLO and BLOCK"
exit $fail
