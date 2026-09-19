#!/bin/sh
# WordStar 3.00 headless smoke test. Needs disks/wordstar/WS.COM installed
# for the ADM-3A (see docs/machine-plan.md, Phase 3), with or without the
# tools/wskaypro.sh patch; skips if absent.
# Usage: tests/wordstar.sh [jit|interp|verify]
set -e
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/.." && pwd)
WS="$ROOT/disks/wordstar/WS.COM"
[ -f "$WS" ] || { echo "wordstar: skipped (no $WS)"; exit 0; }
case "${1:-jit}" in
    jit)    FLAG=-j ;;
    interp) FLAG=-i ;;
    verify) FLAG=-V; export Z80_VERIFY_STRICT=1 ;;
    *) echo "mode must be jit, interp or verify" >&2; exit 2 ;;
esac
cd "$ROOT"
rm -f disks/wordstar/TEST.TXT disks/wordstar/TEST.BAK 'disks/wordstar/TEST.$$$' \
      tests/scripts/ws-editor.dump tests/scripts/ws-after-save.dump
./z80-monster $FLAG --script tests/scripts/ws-save.script "$WS" > /dev/null 2> tests/scripts/ws.err || true
if grep -q 'divergence' tests/scripts/ws.err; then
    echo "wordstar ($1): LOCKSTEP DIVERGENCE"; grep -A3 divergence tests/scripts/ws.err | head -8; exit 1
fi
fail=0
grep -q 'A:TEST.TXT  PAGE 1 LINE 1' tests/scripts/ws-editor.dump || { echo "wordstar: editor status line missing"; fail=1; }
grep -q 'N O - F I L E   M E N U' tests/scripts/ws-after-save.dump || { echo "wordstar: no-file menu missing after save"; fail=1; }
if [ ! -f disks/wordstar/TEST.TXT ]; then
    echo "wordstar: TEST.TXT was not written"; fail=1
else
    want=$(printf 'Hello from the 10 BIPS future.\r\nSecond line.\r\n')
    got=$(tr -d '\032' < disks/wordstar/TEST.TXT)
    [ "$got" = "$want" ] || { echo "wordstar: TEST.TXT content differs"; fail=1; }
fi
rm -f disks/wordstar/TEST.TXT disks/wordstar/TEST.BAK
[ $fail -eq 0 ] && echo "wordstar ($1): OK — created, typed, saved, host file correct"
exit $fail
