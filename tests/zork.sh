#!/bin/sh
# Zork I headless smoke test; skips if disks/zork1/zork1.com is absent.
# Usage: tests/zork.sh [jit|interp|verify]
set -e
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/.." && pwd)
Z="$ROOT/disks/zork1/zork1.com"
[ -f "$Z" ] || { echo "zork: skipped (no $Z)"; exit 0; }
case "${1:-jit}" in
    jit) FLAG=-j ;; interp) FLAG=-i ;; verify) FLAG=-V; export Z80_VERIFY_STRICT=1 ;;
    *) echo "mode must be jit, interp or verify" >&2; exit 2 ;;
esac
cd "$ROOT"
rm -f tests/scripts/zork-look.dump
./z80-monster $FLAG --script tests/scripts/zork-look.script "$Z" > /dev/null 2> tests/scripts/zork.err || true
grep -q 'divergence' tests/scripts/zork.err && { echo "zork ($1): LOCKSTEP DIVERGENCE"; exit 1; }
grep -q 'There is a small mailbox here' tests/scripts/zork-look.dump && echo "zork ($1): OK" || { echo "zork: screen text missing"; exit 1; }
