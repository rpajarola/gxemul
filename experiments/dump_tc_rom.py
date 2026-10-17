#!/usr/bin/env python3
#
#  dump_tc_rom.py:  dumps a DECstation TURBOchannel option ROM over the
#  serial console, into an option ROM image for gxemul (a machine.json's
#  "slots": { "SLOT": "FILE" }, or Expansion in the machine editor).
#
#  It was written to dump the ROM of the built-in SCSI controller (slot 5,
#  PMAZ-AA) of a DECstation 5000/200, which is soldered to the system
#  module, but it works for any slot.
#
#  Connect a serial cable to the console port (the one marked 3, or the
#  printer/console port, 9600 8N1), power the machine on and wait for the
#  >> prompt.  Then, on a Linux or macOS machine with Python 3:
#
#	python3 dump_tc_rom.py --port /dev/ttyUSB0 --slot 5 -o slot5.bin
#
#  This takes about half an hour for a 64 KB ROM at 9600 bps.  Everything
#  that is read is also saved to slot5.bin.log; if the dump is interrupted,
#  run the same command again and it continues where it stopped.
#
#  Without a serial port that this script can open (e.g. on Windows, or
#  with a terminal server), print the commands to type, capture the
#  output with the terminal program's logging, and convert the log:
#
#	python3 dump_tc_rom.py --print-commands --slot 5
#	python3 dump_tc_rom.py --from-log console.log --slot 5 -o slot5.bin
#
#  Send back the .bin AND the .log files.
#
#  How the ROM is read:  an option ROM is byte-wide, and each of its bytes
#  is in its own 32-bit word of the slot's address space.  The ROM header
#  is at offset 0x3e0 (in words: 0x3e0, 0x3e4, ...), with the test pattern
#  55 00 aa ff at 0x3f0..0x3fc, and the version, vendor and module name
#  after it.  The standard place for an option ROM is offset 0x3c0000 in
#  the slot, but built-in devices can have theirs elsewhere, so a few
#  offsets are searched for the header.  The image that is written has
#  one ROM byte per file byte (the low byte of each word), like the
#  ni5.3_ethernet.bin and PMAG-BA images.
#

import argparse
import os
import re
import select
import sys
import time

#  The physical base addresses of the TURBOchannel slots of a
#  DECstation 5000/200 (KN02), as KSEG1 (uncached) addresses:
SLOT_BASE = {
	0: 0xbe000000, 1: 0xbe400000, 2: 0xbe800000,
	5: 0xbf400000, 6: 0xbf800000,
}

#  Where in a slot to look for an option ROM:
ROM_OFFSETS = [0x3c0000, 0x0c0000, 0x000000, 0x1c0000, 0x380000,
	0x040000, 0x080000, 0x100000, 0x140000, 0x180000, 0x200000,
	0x240000, 0x280000, 0x2c0000, 0x300000, 0x340000]

PATTERN = [0x55, 0x00, 0xaa, 0xff]
CHUNK = 0x400		#  ROM bytes per e command (4 KB of address space)
LINE_RE = re.compile(r'([0-9a-fA-F]{8}):\s*0x([0-9a-fA-F]{8})')


def parse_words(text):
	"""Returns {address: word} for the e -x lines in text."""
	words = {}
	for m in LINE_RE.finditer(text):
		words[int(m.group(1), 16)] = int(m.group(2), 16)
	return words


def rom_byte(word, addr, warnings):
	"""Returns the ROM byte in a word, which is in the low byte lane.
	Other lanes are either zero, or a copy of it; anything else is
	noted, since it might mean that the ROM is wider than a byte."""
	b = word & 0xff
	if word not in (b, b * 0x01010101) and len(warnings) < 20:
		warnings.append("%08x: 0x%08x" % (addr, word))
	return b


class Console:
	"""The PROM monitor on a serial port (or on a spawned command)."""

	def __init__(self, port=None, baud=9600, command=None, verbose=False):
		self.verbose = verbose
		self.pid = None
		if command is not None:
			import pty
			self.pid, self.fd = pty.fork()
			if self.pid == 0:
				os.execvp("/bin/sh", ["/bin/sh", "-c", command])
		else:
			import termios
			self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
			attr = termios.tcgetattr(self.fd)
			speed = getattr(termios, "B%d" % baud)
			attr[0] = termios.IGNPAR			# iflag
			attr[1] = 0					# oflag
			attr[2] = (termios.CS8 | termios.CREAD
			    | termios.CLOCAL)				# cflag
			attr[3] = 0					# lflag
			attr[4] = attr[5] = speed
			attr[6][termios.VMIN] = 0
			attr[6][termios.VTIME] = 0
			termios.tcsetattr(self.fd, termios.TCSANOW, attr)
			termios.tcflush(self.fd, termios.TCIOFLUSH)

	def read_until_prompt(self, timeout):
		"""Reads until the >> prompt; returns (text, found)."""
		text = b""
		end = time.time() + timeout
		while not text.rstrip().endswith(b">>") and time.time() < end:
			r, _, _ = select.select([self.fd], [], [], 0.5)
			if not r:
				continue
			try:
				d = os.read(self.fd, 4096)
			except OSError:
				break
			if not d:
				break
			text += d
			if self.verbose:
				sys.stderr.write(d.decode("latin1"))
				sys.stderr.flush()
			#  Answer --More-- in case more could not be set to 0:
			if text.rstrip().endswith(b"--More--"):
				os.write(self.fd, b" ")
			#  Each line that comes in extends the timeout:
			if b"\n" in d:
				end = max(end, time.time() + 20)
		return text.decode("latin1"), text.rstrip().endswith(b">>")

	def command(self, cmd, timeout=60):
		"""Types a command, slowly enough for the monitor, and returns
		its output."""
		for ch in cmd:
			os.write(self.fd, ch.encode())
			time.sleep(0.03)
		os.write(self.fd, b"\r")
		text, found = self.read_until_prompt(timeout)
		if not found:
			raise RuntimeError("no >> prompt after %r; got:\n%s"
			    % (cmd, text[-500:]))
		return text

	def wake(self):
		"""Gets a fresh >> prompt."""
		for _ in range(30):
			os.write(self.fd, b"\r")
			text, found = self.read_until_prompt(5)
			if found:
				return
		raise RuntimeError("no >> prompt; is the console on this port,"
		    " and is the machine at the >> prompt?")

	def close(self):
		if self.pid is not None:
			os.kill(self.pid, 9)
		os.close(self.fd)


def examine(con, log, start, end):
	"""Reads the words from start to end (inclusive), logging them."""
	text = con.command("e -x 0x%08x:0x%08x" % (start, end),
	    timeout=60 + (end - start) // 4 // 10)
	log.write(text)
	log.flush()
	return parse_words(text)


def find_header(read, base):
	"""Returns the offset in the slot of the option ROM, or None."""
	for off in ROM_OFFSETS:
		a = base + off + 0x3f0
		words = read(a, a + 0xc)
		got = [words.get(a + 4 * i) for i in range(4)]
		if None in got:
			continue
		if [w & 0xff for w in got] == PATTERN:
			return off
	return None


def describe_header(words, rom):
	"""Returns the header fields of an option ROM."""
	def b(i):
		return words.get(rom + 0x3e0 + 4 * i, 0) & 0xff

	def s(start, n):
		return "".join(chr(words.get(rom + start + 4 * i, 0) & 0xff)
		    for i in range(n))
	return {
		"width": b(0), "stride": b(1), "size": b(2), "slot_size": b(3),
		"version": s(0x400, 8), "vendor": s(0x420, 8),
		"module": s(0x440, 8), "type": s(0x460, 4),
	}


def build_image(words, rom, size):
	"""Returns the ROM image, and the addresses that are missing."""
	warnings = []
	image = bytearray(size)
	missing = []
	for i in range(size):
		a = rom + 4 * i
		if a in words:
			image[i] = rom_byte(words[a], a, warnings)
		else:
			missing.append(a)
	return image, missing, warnings


def default_size(hdr):
	#  The header's size field can't be trusted to stop at (the 32 KB
	#  ni5.3 PMAD-AA ROM says 8, which would be 64 KB in 8 KB units),
	#  so dump at least 64 KB, which is cheap insurance:
	return max(hdr["size"] * 8192, 65536)


def main():
	ap = argparse.ArgumentParser(description=__doc__ or
	    "Dumps a DECstation TURBOchannel option ROM over the console.")
	ap.add_argument("--port", help="serial port, e.g. /dev/ttyUSB0")
	ap.add_argument("--baud", type=int, default=9600)
	ap.add_argument("--command", help=argparse.SUPPRESS)
	ap.add_argument("--slot", type=int, default=5,
	    choices=sorted(SLOT_BASE))
	ap.add_argument("--offset", type=lambda x: int(x, 0),
	    help="offset of the ROM in the slot (default: search)")
	ap.add_argument("--size", type=lambda x: int(x, 0),
	    help="ROM size in bytes (default: 64 KB, or more if the header"
	    " says so)")
	ap.add_argument("-o", "--output", help="the ROM image to write")
	ap.add_argument("--from-log", metavar="LOG",
	    help="convert a captured console log instead of reading the ROM")
	ap.add_argument("--print-commands", action="store_true",
	    help="print the console commands for a manual dump")
	ap.add_argument("-v", "--verbose", action="store_true",
	    help="show the console output")
	args = ap.parse_args()
	base = SLOT_BASE[args.slot]

	if args.print_commands:
		off = args.offset if args.offset is not None else 0x3c0000
		size = args.size or 65536
		rom = base + off
		print("""\
At the >> prompt, type these.  First, note what printenv says "more"
is, so that it can be set back at the end, and check that slot %d
has a ROM:

    printenv
    cnfg
    cnfg %d
    setenv more 0

Then find the ROM.  The standard place is offset 0x3c0000 in the slot.
This is the right place if the low bytes of the four words are
55, 00, aa, ff:

    e -x 0x%08x:0x%08x
""" % (args.slot, args.slot, rom + 0x3f0, rom + 0x3fc))
		print("If not, try these until one shows 55, 00, aa, ff (and"
		    " then pass\n--offset with the offset after the slot's"
		    " 0x%08x):\n" % base)
		for o in ROM_OFFSETS:
			if o != off:
				a = base + o + 0x3f0
				print("    e -x 0x%08x:0x%08x    (--offset 0x%x)"
				    % (a, a + 0xc, o))
		print("""
Start capturing the console output to a file now, and dump the ROM
(%d KB; at 9600 bps this takes about %d minutes):

    e -x 0x%08x:0x%08x

Stop capturing, set more back to what printenv said (normally 24):

    setenv more 24

and convert the capture with:

    python3 dump_tc_rom.py --from-log LOG --slot %d%s -o slot%d.bin
""" % (size // 1024, size * 22 // 960 // 60 + 1, rom,
	    rom + 4 * size - 4, args.slot,
	    "" if off == 0x3c0000 else " --offset 0x%x" % off, args.slot))
		return 0

	if args.output is None:
		ap.error("-o is needed")

	if args.from_log:
		with open(args.from_log, "r", encoding="latin1") as f:
			words = parse_words(f.read())
		con = None
		log = None

		def read(start, end):
			return words
	else:
		if (args.port is None) == (args.command is None):
			ap.error("--port (or --from-log, or --print-commands)"
			    " is needed")
		#  Resume from an earlier log:
		words = {}
		logname = args.output + ".log"
		if os.path.exists(logname):
			with open(logname, "r", encoding="latin1") as f:
				words = parse_words(f.read())
			if words:
				print("continuing from %s (%d words)"
				    % (logname, len(words)))
		log = open(logname, "a", encoding="latin1")
		con = Console(args.port, args.baud, args.command, args.verbose)
		con.wake()

		def read(start, end):
			if all(a in words
			    for a in range(start, end + 4, 4)):
				return words
			words.update(examine(con, log, start, end))
			return words

	more = None
	try:
		if con is not None:
			text = con.command("printenv")
			log.write(text)
			m = re.search(r'more=(\d+)', text)
			more = m.group(1) if m else "24"
			print("printenv: more=%s" % more)
			text = con.command("cnfg")
			log.write(text)
			print(text.strip())
			con.command("setenv more 0")

		if args.offset is not None:
			off = args.offset
		else:
			off = find_header(read, base)
			if off is None:
				print("no option ROM header found in slot %d at"
				    " any of the usual offsets" % args.slot)
				return 1
		rom = base + off
		read(rom + 0x3e0, rom + 0x46c)
		hdr = describe_header(words, rom)
		print("ROM at slot %d offset 0x%x (0x%08x): %s %s %s %s,"
		    " width %d, stride %d, size field %d"
		    % (args.slot, off, rom, hdr["module"], hdr["vendor"],
		    hdr["version"], hdr["type"], hdr["width"],
		    hdr["stride"], hdr["size"]))
		size = args.size or default_size(hdr)

		if con is not None:
			t0 = time.time()
			for i in range(0, size, CHUNK):
				n = min(CHUNK, size - i)
				read(rom + 4 * i, rom + 4 * (i + n) - 4)
				done = i + n
				el = time.time() - t0
				sys.stdout.write("\r%6d / %d bytes, %d s"
				    % (done, size, el))
				sys.stdout.flush()
			print()
			#  Read the header again, as a check that
			#  nothing was garbled:
			again = examine(con, log, rom + 0x3e0, rom + 0x46c)
			for a, w in again.items():
				if words.get(a) != w:
					print("WARNING: 0x%08x read as 0x%08x"
					    " and then 0x%08x" % (a,
					    words.get(a), w))
	finally:
		if con is not None:
			if more is not None:
				con.command("setenv more %s" % more)
			con.close()
		if log is not None:
			log.close()

	image, missing, warnings = build_image(words, rom, size)
	if missing:
		print("WARNING: %d of %d bytes are missing (the first is at"
		    " 0x%08x); they are written as 0" % (len(missing), size,
		    missing[0]))
	for w in warnings:
		print("NOTE: a word that is not one byte wide: %s" % w)
	with open(args.output, "wb") as f:
		f.write(image)
	print("wrote %s (%d bytes); use it with \"slots\": { \"%d\": \"%s\" }"
	    % (args.output, size, args.slot, os.path.basename(args.output)))
	return 0 if not missing else 2


if __name__ == "__main__":
	sys.exit(main())
