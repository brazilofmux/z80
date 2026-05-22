#!/bin/sh
# bench/squaro.sh — Newton-Raphson square-root benchmark
#
# Drives MS COBOL's SQUARO.COM (compiled and linked by MS COBOL 4.65 +
# Link-80, both running on the JIT — see disks/mscobol/) through N
# inputs and reports throughput. Good check on a real-world CP/M
# workload: integer/decimal arithmetic, PIC-formatted I/O, and the
# COBOL runtime's call/return discipline.
#
# Usage: bench/squaro.sh [N=2000] [jit|interp]

set -e
N=${1:-2000}
MODE=${2:-jit}

case "$MODE" in
    jit)    FLAG="-j" ;;
    interp) FLAG="-i" ;;
    *) echo "mode must be 'jit' or 'interp'" >&2; exit 1 ;;
esac

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
BIN="$ROOT/z80-monster"
PROG="$ROOT/disks/mscobol/SQUARO.COM"

[ -x "$BIN" ]  || { echo "build first: make" >&2; exit 1; }
[ -f "$PROG" ] || { echo "missing $PROG — see disks/mscobol/" >&2; exit 1; }

LOG=$(mktemp)
TIMELOG=$(mktemp)
trap 'rm -f "$LOG" "$TIMELOG"' EXIT

# Generate inputs (1..N then 0 to terminate). Stats go to stdout, time
# to stderr; split them so we don't have to disentangle one stream.
{ seq 1 "$N"; echo 0; } |
    /usr/bin/time -p "$BIN" $FLAG -s "$PROG" > "$LOG" 2> "$TIMELOG"

INSNS=$( awk '/^Instructions:/    {print $2}' "$LOG")
FALL=$(  awk '/interp fallback/   {print $4}' "$LOG")
BLOCKS=$(awk '/blocks translated/ {print $3}' "$LOG")
USER=$(  awk '/^user /            {print $2}' "$TIMELOG")
REAL=$(  awk '/^real /            {print $2}' "$TIMELOG")

# Avoid divide-by-zero if user time rounded to 0.00.
MIPS=$(awk -v i="$INSNS" -v t="$USER" \
       'BEGIN { if (t > 0) printf "%.1f", i/t/1e6; else print "n/a" }')
PCT=$( awk -v f="$FALL"  -v i="$INSNS" \
       'BEGIN { if (i > 0) printf "%.2f", f*100/i; else print "n/a" }')

printf 'squaro N=%s mode=%s : %ss user / %ss real  %s Z80-insns  %s MIPS  %s%% interp-fallback  %s blocks\n' \
       "$N" "$MODE" "$USER" "$REAL" "$INSNS" "$MIPS" "$PCT" "$BLOCKS"
