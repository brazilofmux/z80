#!/bin/sh
# Fetch and build Macro Assembler AS (Alfred Arnold's "asl") into tools/asl/.
# It assembles both the DRI 8080 sources of CP/M 2.2 (cpm/cpm22/*.asm)
# and our Z80 BIOS. Only the binaries are built (the docs need TeX).
# The assembled cpm/cpm22/system.bin is checked in, so this is only
# needed to rebuild it.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
URL=http://john.ccac.rwth-aachen.de:8000/ftp/as/source/c_version/asl-current.tar.bz2
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
echo "downloading $URL"
curl -sSL -m 300 -o "$WORK/asl.tar.bz2" "$URL"
tar xjf "$WORK/asl.tar.bz2" -C "$WORK"
cd "$WORK"/asl-*
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64)  cp Makefile.def-samples/Makefile.def-arm-osx Makefile.def ;;
    Darwin-x86_64) cp Makefile.def-samples/Makefile.def-i386-osx Makefile.def ;;
    # Not the i386 sample: its -march=i586 is refused by an x86-64 gcc.
    Linux-x86_64)  if [ -f Makefile.def-samples/Makefile.def-x86_64-unknown-linux ]; then
                       cp Makefile.def-samples/Makefile.def-x86_64-unknown-linux Makefile.def
                   else cp Makefile.def.tmpl Makefile.def; fi ;;
    *)             cp Makefile.def.tmpl Makefile.def ;;
esac
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" binaries > build.log 2>&1 || { tail -20 build.log; exit 1; }
mkdir -p "$HERE/asl"
cp asl p2bin "$HERE/asl/"
echo "built $HERE/asl/asl and p2bin"
