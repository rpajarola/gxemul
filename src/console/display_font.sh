#! /bin/sh

# src/console/display_font.sh: makes display_font.h, the fonts for the
# display's menus and status line, from the Spleen fonts
# (https://github.com/fcambus/spleen):
#
#   sh display_font.sh spleen-8x16.bdf spleen-16x32.bdf > display_font.h
#
# the fonts have the Latin-1 characters, 0x20 through 0xff.  each
# character is a bitmap of rows from top to bottom, each row in
# whole bytes with the leftmost pixel in the most significant bit.

if test $# -ne 2; then
    echo "usage: $0 SPLEEN-8x16.bdf SPLEEN-16x32.bdf" 1>&2
    exit 1
fi

cat <<'EOF2'
/* src/console/display_font.h: the font for the display's menus */

/* this file was made by display_font.sh from the Spleen fonts.  do not
   edit it.  the fonts are:

   Copyright (c) 2018-2026, Frederic Cambus
   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are met:

     * Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.

     * Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
   AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
   ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
   BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
   CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
   SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
   INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
   CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
   ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
   POSSIBILITY OF SUCH DAMAGE.
*/

/* the first and last characters in the fonts: */
#define DISPLAY_FONT_FIRST	(0x20)
#define DISPLAY_FONT_LAST	(0xff)
EOF2

for bdf in "$@"; do
    awk '
/^FONTBOUNDINGBOX/ { width = $2; height = $3 }
/^ENCODING/ { encoding = $2 }
/^BBX/ {
  if ($2 != width || $3 != height) {
    print "display_font.sh: character " encoding " is not " width "x" height > "/dev/stderr"
    exit 1
  }
}
/^BITMAP/ { row = 0; inbitmap = (encoding >= 32 && encoding <= 255); next }
/^ENDCHAR/ { inbitmap = 0; next }
inbitmap { bitmap[encoding, row++] = $1 }
END {
  bytes = int((width + 7) / 8)
  printf "\n/* the %dx%d font: */\n", width, height
  printf "#define DISPLAY_FONT_%dX%d_WIDTH\t(%d)\n", width, height, width
  printf "#define DISPLAY_FONT_%dX%d_HEIGHT\t(%d)\n", width, height, height
  printf "static const unsigned char display_font_%dx%d[%d][%d] = {\n", width, height, 224, height * bytes
  for (c = 32; c <= 255; c++) {
    line = "  {"
    for (r = 0; r < height; r++) {
      hex = ((c, r) in bitmap ? bitmap[c, r] : "")
      while (length(hex) < bytes * 2) {
        hex = hex "0"
      }
      for (b = 0; b < bytes; b++) {
        line = line " 0x" tolower(substr(hex, b * 2 + 1, 2)) ","
      }
    }
    printf "%s },\t/* 0x%02x */\n", line, c
  }
  printf "};\n"
}' "$bdf" || exit 1
done
