#!/bin/sh
# dBASE II 2.41 headless smoke test; skips if disks/dbase/DBASE.COM is absent.
# Usage: tests/dbase.sh [jit|interp|verify]
set -e
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/.." && pwd)
DB="$ROOT/disks/dbase/DBASE.COM"
[ -f "$DB" ] || DB="$ROOT/disks/dbase/dbase.com"
[ -f "$DB" ] || { echo "dbase: skipped (no $DB)"; exit 0; }
case "${1:-jit}" in
    jit) FLAG=-j ;; interp) FLAG=-i ;; verify) FLAG=-V; export Z80_VERIFY_STRICT=1 ;;
    *) echo "mode must be jit, interp or verify" >&2; exit 2 ;;
esac
cd "$ROOT"
rm -f disks/dbase/TEST.DBF tests/scripts/dbase-display.dump
./z80-monster $FLAG --script tests/scripts/dbase-create.script "$DB" > /dev/null 2> tests/scripts/dbase.err || true
if grep -q 'divergence' tests/scripts/dbase.err; then
    echo "dbase ($1): LOCKSTEP DIVERGENCE"; grep -A3 divergence tests/scripts/dbase.err | head -8; exit 1
fi
fail=0
grep -q '00001  MONSTER        10' tests/scripts/dbase-display.dump || { echo "dbase: record 1 missing from DISPLAY ALL"; fail=1; }
grep -q '00002  KAYPRO          4' tests/scripts/dbase-display.dump || { echo "dbase: record 2 missing from DISPLAY ALL"; fail=1; }
if [ ! -f disks/dbase/TEST.DBF ]; then
    echo "dbase: TEST.DBF was not written"; fail=1
elif ! grep -q 'MONSTER' disks/dbase/TEST.DBF; then
    echo "dbase: TEST.DBF does not contain the record"; fail=1
fi
rm -f disks/dbase/TEST.DBF
[ $fail -eq 0 ] && echo "dbase ($1): OK — created, appended, displayed, host file correct"
exit $fail
