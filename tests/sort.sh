#!/bin/sh
# SORT.COM (cpm/sort/sort.asm) against tests/sort/sortref.py on synthetic
# records: 700 with a multi-key deck (a descending and a case-folded
# field) and 2000 by number (two runs, one merge). Needs the JIT build
# and cpm/sort/SORT.COM (make).
set -e
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/.." && pwd)
cd "$ROOT"
[ -f cpm/sort/SORT.COM ] || { echo "sort: skipped (make cpm/sort/SORT.COM first)"; exit 0; }
W=tests/scripts/sortwork; rm -rf "$W"; mkdir -p "$W"
cpmtext() { sed 's/$/\r/' "$1" > "$2"; printf '\032' >> "$2"; }
python3 tests/sort/sortref.py gen 700 "$W/t700.txt"
python3 tests/sort/sortref.py gen 2000 "$W/t2000.txt"
python3 tests/sort/sortref.py sort tests/sort/multikey.ctl "$W/t700.txt" "$W/t700.ref"
python3 tests/sort/sortref.py sort tests/sort/bynumber.ctl "$W/t2000.txt" "$W/t2000.ref"
cpmtext "$W/t700.txt" "$W/T700.DAT"; cpmtext "$W/t2000.txt" "$W/T2000.DAT"
cpmtext tests/sort/multikey.ctl "$W/MULTIKEY.CTL"; cpmtext tests/sort/bynumber.ctl "$W/BYNUMBER.CTL"
./tools/mkdsk -f hd "$W/a.dsk" cpm/sort/SORT.COM "$W/T700.DAT" "$W/T2000.DAT" "$W/MULTIKEY.CTL" "$W/BYNUMBER.CTL" > /dev/null
./tools/mkdsk -f hd "$W/b.dsk" > /dev/null; ./tools/mkdsk -f hd "$W/c.dsk" > /dev/null
cat > "$W/job.script" <<'JOB'
@wait-idle
SORT\r
@wait-idle
A:T700.DAT\r
@wait-idle
A:OUT700.DAT\r
@wait-idle
A:MULTIKEY.CTL\r
@wait-idle 200000
SORT\r
@wait-idle
A:T2000.DAT\r
@wait-idle
A:OUT2000.DAT\r
@wait-idle
A:BYNUMBER.CTL\r
@wait-idle 200000
@end
JOB
./z80-monster -j --script "$W/job.script" -A "$W/a.dsk" -B "$W/b.dsk" -C "$W/c.dsk" > /dev/null 2>&1 || true
fail=0
for t in 700 2000; do
    if ./tools/mkdsk -x "$W/a.dsk" "OUT$t.DAT" -o "$W" > /dev/null 2>&1; then
        LC_ALL=C tr -d '\r\032\000' < "$W/OUT$t.DAT" | sed 's/ *$//' | sed '/^$/d' > "$W/out$t.txt"
        cmp -s "$W/out$t.txt" "$W/t$t.ref" || { echo "sort $t: output differs from the reference"; fail=1; }
    else echo "sort $t: no output"; fail=1; fi
done
[ $fail -eq 0 ] && echo "sort: OK — 700 multi-key and 2000 two-run merge match the reference"
exit $fail
