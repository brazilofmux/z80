#!/bin/sh
# Give a WordStar 3.0 WS.COM the Kaypro '84 terminal strings INSTALL cannot:
# INSTALL (1981) predates the Kaypro, so the nearest choice is the ADM-3A,
# which leaves WordStar without highlighting, erase-to-end-of-line and
# line insert/delete — the whole screen is repainted with spaces and the
# menus are plain text. The Kaypro 2X/4'84/10 terminal (and kaypro_video
# in this repo) has all four, so patch the user area (WordStar 3.0
# Installation Manual, Appendix E, USER1 listing):
#   ERAEOL 026D  ^X            erase to end of line
#   LINDEL 0274  ESC R         delete line
#   LININS 027B  ESC E         insert line
#   IVON   0284  ESC B 0       inverse video on
#   IVOFF  028B  ESC C 0       inverse video off
# Each item is a length byte followed by the bytes. Run INSTALL first
# (terminal A, ADM-3A), then this on the result. In place; keeps a .adm3a
# copy. Usage: tools/wskaypro.sh disks/wordstar/WS.COM
set -e
ws=${1:?usage: $0 WS.COM}
[ -f "$ws" ] || { echo "$ws: not found" >&2; exit 1; }
grep -q 'release 3.00' "$ws" || { echo "$ws: not a WordStar 3.00 WS.COM" >&2; exit 1; }
if [ "$(dd if="$ws" bs=1 skip=$((0x16D)) count=1 2>/dev/null | od -An -tx1 | tr -d ' ')" != "00" ]; then
    echo "$ws: ERAEOL already patched; nothing to do"; exit 0
fi
cp -p "$ws" "$ws.adm3a"
poke() { printf "$2" | dd of="$ws" bs=1 seek=$(($1 - 0x100)) conv=notrunc 2>/dev/null; }
poke 0x26D '\001\030'            # ERAEOL: ^X
poke 0x274 '\002\033R'           # LINDEL: ESC R
poke 0x27B '\002\033E'           # LININS: ESC E
poke 0x284 '\003\033B0'          # IVON:   ESC B 0
poke 0x28B '\003\033C0'          # IVOFF:  ESC C 0
echo "$ws: patched for the Kaypro '84 terminal (original kept as $ws.adm3a)"
