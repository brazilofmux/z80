#!/bin/sh
# bench/wsreplace.sh — the README's target workload: WordStar 3.00 opens a
# 50-page document (2600 lines, ~130 KB), replaces every "kaypro" with
# "monster" globally (^QA, GN), saves (^KD) and exits — under the real
# CP/M 2.2 CCP/BDOS on a hard-disk image. Reports instructions, host
# time and BIPS. Needs disks/wordstar/WS.COM installed for the ADM-3A.
# Usage: bench/wsreplace.sh [jit|interp]
set -e
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/.." && pwd)
cd "$ROOT"
[ -f disks/wordstar/WS.COM ] || { echo "missing disks/wordstar/WS.COM" >&2; exit 1; }
case "${1:-jit}" in jit) FLAG=-j ;; interp) FLAG=-i ;; *) echo "jit|interp" >&2; exit 2 ;; esac
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
# A deterministic pseudo-random document (LCG), CR/LF line ends.
awk 'BEGIN { srand(1); split("the quick brown fox jumps over lazy dog kaypro wordstar cpm bdos bios monster ten bips speed light virtual year page line search replace", w, " ");
  x = 12345;
  for (i = 0; i < 2600; i++) { x = (x * 1103515245 + 12345) % 2147483648; n = 6 + x % 6; line = "";
    for (j = 0; j < n; j++) { x = (x * 1103515245 + 12345) % 2147483648; line = line (j ? " " : "") w[1 + x % 23]; }
    printf "%s%s.\r\n", toupper(substr(line, 1, 1)), substr(line, 2) } }' > "$W/BIG.TXT"
./tools/mkdsk -f hd "$W/ws.dsk" disks/wordstar/WS.COM disks/wordstar/WSMSGS.OVR disks/wordstar/WSOVLY1.OVR "$W/BIG.TXT" > /dev/null
cat > "$W/replace.script" <<'SCRIPT'
@wait-idle
WS\r
@wait-idle
D
@wait-idle
BIG.TXT\r
@wait-idle
\^Q\^Akaypro\r
@wait-idle
monster\r
@wait-idle
GN\r
@wait-idle 60000
\^K\^D
@wait-idle 60000
X
@wait-idle
@end
SCRIPT
./z80-monster $FLAG -s --script "$W/replace.script" -A "$W/ws.dsk" 2>/dev/null > "$W/out" || true
insns=$(awk '/^Instructions:/ {print $2}' "$W/out"); secs=$(awk '/^Host time:/ {print $3}' "$W/out"); rate=$(awk '/^Rate:/ {print $2}' "$W/out")
fb=$(grep 'fallback insns' "$W/out" | sed 's/.*insns: *//')
k=$(grep -o kaypro "$W/BIG.TXT" | wc -l | tr -d ' ')
mkdir -p "$W/out.d"; ./tools/mkdsk -x "$W/ws.dsk" BIG.TXT -o "$W/out.d" > /dev/null
# WordStar marks soft characters with the high bit; strip it before counting.
left=$(perl -pe 's/[\x80-\xff]/chr(ord($&)&0x7f)/ge' "$W/out.d/BIG.TXT" 2>/dev/null | grep -o kaypro | wc -l | tr -d ' ')
echo "wsreplace ($1): $insns Z80 insns in $secs s = $rate BIPS; fallback $fb; 'kaypro' words before $k, after $left"
[ -n "$Z80_PROFILE" ] && sed -n '/^--- profile/,/^JIT:/p' "$W/out" | grep -v '^JIT:'
exit 0
