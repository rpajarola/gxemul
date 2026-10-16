#!/usr/bin/env python3
#
#  patch_rom_tests.py:  makes a copy of a DECstation ROM image in which some
#  self-tests always pass, for tests that fail in gxemul only because it
#  does not emulate what they check:
#
#	KN02-BA V5.7j (5000/1xx system ROM):
#	    3/cache/isol, 3/cache/seg	need a real R3000 data cache
#	    3/rtc/time			needs real-time I/O timing
#
#	PMAG-BA V5.3a (option ROM of the color frame buffer):
#	    0/curs, 0/ptrn		read the Bt459's signature registers,
#					which gxemul does not compute
#
#  Usage:
#
#	python3 patch_rom_tests.py decstation5000_133.bin kn02ba-notests.bin
#	python3 patch_rom_tests.py PMAG-BA_V5.3a.bin pmag-ba-notests.bin
#
#  and use the copies instead of the originals, e.g. in a config file:
#
#	prom_emulation(no)
#	load("0xbfc00000:kn02ba-notests.bin")
#	tc_rom("0:pmag-ba-notests.bin")
#
#  The ROM is recognized by its name and version strings.  By default all of its
#  tests listed above are patched; --test NAME (repeatable) patches only
#  those, and --list shows the known tests.  The original instructions are
#  checked before patching, so other ROM versions are refused, not damaged.
#
#  How a test is patched:  the first two instructions of its function
#  become "jr ra; move v0,zero", so it returns 0 (passed) at once, both at
#  power-on and when run with "t".  Nothing else changes.
#
#  How the functions were found:  the ROMs copy their test code to RAM and
#  run it there, linked at 0xa0020000.  The KN02-BA PROM's test table has
#  entries of a 16-byte name, the function's address, an argument parser
#  and an argument count (e.g. "cache/isol" at ROM offset 0x26ccc, function
#  0xa0021650), and the code linked at 0xa0020000 starts at ROM offset
#  0x194a4.  The PMAG-BA ROM's table has entries of the function, the name
#  and the usage string (e.g. "curs" at 0xa00204c0), and its code linked at
#  0xa0020000 starts at ROM offset 0x1594.  All of these functions return 0
#  when the test passes.
#

import argparse
import struct
import sys

JR_RA_MOVE_V0_ZERO = struct.pack('<II', 0x03e00008, 0x00001021)

ROMS = [
    {
        'name': 'KN02-BA V5.7j',
        'id': (b'KN02-BA', b'V5.7j'),
        'base': 0x194a4,		# ROM offset of the code at 0xa0020000
        'tests': [
            # (name, function address, its first instruction)
            ('3/cache/seg', 0xa00212a0, 0x27bdffb8),
            ('3/cache/isol', 0xa0021650, 0x27bdffd8),
            ('3/rtc/time', 0xa0021ef0, 0x27bdffc8),
        ],
    },
    {
        'name': 'PMAG-BA V5.3a',
        'id': (b'PMAG-BA', b'V5.3a'),
        'base': 0x1594,
        'tests': [
            ('0/curs', 0xa00204c0, 0x3c0ea002),
            ('0/ptrn', 0xa0021b80, 0x3c02a002),
        ],
    },
]


def identify(image):
    for rom in ROMS:
        if all(s in image for s in rom['id']):
            # The version must also match: check the first instructions.
            for name, addr, first in rom['tests']:
                off = rom['base'] + addr - 0xa0020000
                if off + 8 > len(image) or \
                    struct.unpack('<I', image[off:off + 4])[0] != first:
                    break
            else:
                return rom
    return None


def main():
    p = argparse.ArgumentParser(description='Make a copy of a DECstation '
        'ROM image in which self-tests that gxemul cannot pass always pass.')
    p.add_argument('input', nargs='?', help='original ROM image')
    p.add_argument('output', nargs='?', help='patched copy to write')
    p.add_argument('--test', action='append', metavar='NAME',
        help='patch only this test (repeatable), e.g. 3/rtc/time')
    p.add_argument('--list', action='store_true',
        help='list the known ROMs and tests')
    args = p.parse_args()

    if args.list:
        for rom in ROMS:
            print('%s:  %s' % (rom['name'],
                '  '.join(t[0] for t in rom['tests'])))
        return
    if not args.input or not args.output:
        p.error('give the input and output image')

    image = bytearray(open(args.input, 'rb').read())
    rom = identify(image)
    if rom is None:
        sys.exit('%s: not a known ROM version (see --list), or already '
            'patched' % args.input)

    known = [t[0] for t in rom['tests']]
    for name in args.test or []:
        if name not in known:
            sys.exit('%s has no test %s (it has: %s)' % (rom['name'], name,
                ', '.join(known)))

    for name, addr, first in rom['tests']:
        if args.test and name not in args.test:
            continue
        off = rom['base'] + addr - 0xa0020000
        image[off:off + 8] = JR_RA_MOVE_V0_ZERO
        print('%s: %s (0x%08x, ROM offset 0x%x) now passes' %
            (rom['name'], name, addr, off))

    with open(args.output, 'wb') as f:
        f.write(image)


if __name__ == '__main__':
    main()
