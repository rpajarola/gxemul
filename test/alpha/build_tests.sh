#!/bin/sh
#
#  Builds the Alpha instruction tests:  build_tests.sh outdir
#
#  Needs an Alpha cross assembler and linker. Set ALPHA_AS and ALPHA_LD,
#  or have alpha-linux-gnu-as/-ld in PATH.
#
#  Produces outdir/NAME.gx (for gxemul's testalpha machine),
#  outdir/NAME.linux (Linux user mode, for reference output) and
#  outdir/NAME.list (one description per output line).

set -e
OUT="$1"
[ -n "$OUT" ] || { echo "usage: $0 outdir"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
AS=${ALPHA_AS:-alpha-linux-gnu-as}
LD=${ALPHA_LD:-alpha-linux-gnu-ld}
LDFLAGS="-static -N --no-warn-rwx-segments"

mkdir -p "$OUT"
python3 "$HERE/gen_tests.py" "$OUT" > "$OUT/tests.txt"
for f in rt_gxemul rt_linux common; do
	$AS -o "$OUT/$f.o" "$HERE/$f.S"
done
for t in $(cat "$OUT/tests.txt"); do
	$AS -o "$OUT/$t.o" "$OUT/$t.S"
	$LD $LDFLAGS -Ttext=0xfffffc0000100000 -o "$OUT/$t.gx" \
	    "$OUT/rt_gxemul.o" "$OUT/common.o" "$OUT/$t.o"
	$LD $LDFLAGS -o "$OUT/$t.linux" \
	    "$OUT/rt_linux.o" "$OUT/common.o" "$OUT/$t.o"
done

#  Tests of the 21064 privileged architecture (PALmode etc.), written by
#  hand. They are their own PALcode and run on the "testalphapal" machine.
CPP=${CPP:-cpp}
for t in pal_ev4; do
	$CPP -P -undef -nostdinc "$HERE/$t.S" > "$OUT/$t.s"
	$AS -o "$OUT/$t.o" "$OUT/$t.s"
	$LD $LDFLAGS -Ttext=0xfffffc0000000000 -o "$OUT/$t.gx" \
	    "$OUT/$t.o" "$OUT/common.o"
	cp "$HERE/$t.list" "$OUT/$t.list"
	echo $t >> "$OUT/tests.txt"
done
