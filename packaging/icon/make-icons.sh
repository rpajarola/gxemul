#!/usr/bin/env bash
# Makes gxemul's icons from the two drawings here (gxemul-icon.svg, and
# gxemul-icon-small.svg for 24 pixels and less, where the pins, the text
# and the details of the emu are too small).  Needs rsvg-convert (librsvg)
# and ImageMagick; the results are in git, so that building gxemul doesn't:
#
#	png/gxemul-N.png		N = 16 ... 256, for Linux's icon theme
#	gxemul.ico			for the Windows executable
#	../../src/console/display_icon.h	the window icon (256 pixels)
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"
mkdir -p png
for n in 16 24 32 48 64 128 256; do
	svg=gxemul-icon.svg
	[ "$n" -le 24 ] && svg=gxemul-icon-small.svg
	rsvg-convert -w "$n" -h "$n" "$svg" -o "png/gxemul-$n.png"
done
# An .ico of PNGs (which Windows reads since Vista), much smaller than one
# of bitmaps:
python3 - png/gxemul-16.png png/gxemul-24.png png/gxemul-32.png \
    png/gxemul-48.png png/gxemul-64.png png/gxemul-256.png > gxemul.ico <<'PY'
import struct, sys
pngs = [open(f, 'rb').read() for f in sys.argv[1:]]
out = sys.stdout.buffer
out.write(struct.pack('<HHH', 0, 1, len(pngs)))
ofs = 6 + 16 * len(pngs)
for data in pngs:
    w, h = struct.unpack('>II', data[16:24])
    out.write(struct.pack('<BBBBHHII', w % 256, h % 256, 0, 0, 1, 32,
        len(data), ofs))
    ofs += len(data)
for data in pngs:
    out.write(data)
PY

# The window icon, as runs of equal 32-bit pixels (0xAARRGGBB):
magick png/gxemul-256.png -depth 8 rgba:- | python3 -c '
import sys
d = sys.stdin.buffer.read()
runs = []
for i in range(0, len(d), 4):
    r, g, b, a = d[i:i + 4]
    p = (a << 24) | (r << 16) | (g << 8) | b
    if runs and runs[-1][1] == p and runs[-1][0] < 255:
        runs[-1][0] += 1
    else:
        runs.append([1, p])
out = sys.stdout
out.write("/*  Made by packaging/icon/make-icons.sh from gxemul-icon.svg.  */\n\n")
out.write("#define\tDISPLAY_ICON_SIZE\t256\n#define\tDISPLAY_ICON_RUNS\t%d\n\n" % len(runs))
out.write("/*  Runs of equal pixels: count, 0xAARRGGBB.  */\n")
out.write("static const struct { unsigned char n; uint32_t argb; } display_icon[] = {\n")
for i in range(0, len(runs), 4):
    out.write("\t" + " ".join("{%d,0x%08x}," % (n, p) for n, p in runs[i:i + 4]) + "\n")
out.write("};\n")
' > ../../src/console/display_icon.h
ls -l png gxemul.ico ../../src/console/display_icon.h
