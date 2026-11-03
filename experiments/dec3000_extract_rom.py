#!/usr/bin/env python3
"""
Extract the DEC 3000 system and I/O ROM images from a firmware update
file (Mx00_Vy_z.EXE/.SYS/.BTP, as found on the Alpha firmware CDs).

Each update file carries its ROM images as plain byte copies, each starting
with the DEC 3000 ROM header (System Programmer's Manual, section 13.3):
width, stride, size/8K, slot size, the 55/00/AA/FF pattern, then revision,
vendor, module name and firmware type (8 bytes each). The image runs up to
the next header (or the end of the data); the rest of the 256 KB flash is
padded with 0xff (erased).

usage: dec3000_extract_rom.py update-file [output-prefix]

Writes <prefix>-<n>-<module>.bin for each image found.
"""
import re
import struct
import sys

ROM_SIZE = 256 * 1024


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    data = open(sys.argv[1], 'rb').read()
    prefix = sys.argv[2] if len(sys.argv) > 2 else 'dec3000rom'

    starts = []
    for m in re.finditer(rb'\x55\x00\xaa\xff', data):
        o = m.start() - 4
        if o < 0:
            continue
        width, stride = data[o], data[o + 1]
        vendor = data[o + 0x10:o + 0x18]
        if stride == 4 and width in (1, 4) and vendor == b'DEC     ':
            starts.append(o)

    if not starts:
        sys.exit('no DEC 3000 ROM images found')

    for n, o in enumerate(starts):
        end = starts[n + 1] if n + 1 < len(starts) else len(data)
        img = data[o:end].rstrip(b'\0')
        rev = data[o + 0x08:o + 0x10].decode('ascii').strip()
        mod = data[o + 0x18:o + 0x20].decode('ascii').strip()
        ftype = data[o + 0x30:o + 0x3c].decode('ascii', 'replace').strip()
        length = struct.unpack('<I', data[o + 0x2c:o + 0x30])[0]
        flags = data[o + 0x28:o + 0x2c].hex()
        if len(img) > ROM_SIZE:
            sys.exit('image at 0x%x is larger than 256 KB' % o)
        img = img + b'\xff' * (ROM_SIZE - len(img))
        out = '%s-%d-%s.bin' % (prefix, n, mod.lower())
        open(out, 'wb').write(img)
        print('0x%06x  %-8s %-12s %s  width %d  flags %s  length field '
              '0x%x  -> %s' % (o, mod, ftype, rev, data[o], flags, length,
                               out))


if __name__ == '__main__':
    main()
