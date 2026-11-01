/*
 *  Copyright (C) 2003-2021  Anders Gavare.  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright  
 *     notice, this list of conditions and the following disclaimer in the 
 *     documentation and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 *  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE   
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 *  OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 *  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 *  OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 *  SUCH DAMAGE.
 *
 *
 *  COMMENT: LK201 keyboard and mouse, used by the dc7085 and scc controllers
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "console.h"
#include "devices.h"
#include "machine.h"
#include "misc.h"

#include "thirdparty/dc7085.h"	/*  for port names  */
#include "thirdparty/lk201.h"


// #define debug fatal


/*  LK201 key codes which aren't in thirdparty/lk201.h:  */
#define	LK_KEY_FIND		0x8a
#define	LK_KEY_INSERT		0x8b
#define	LK_KEY_REMOVE		0x8c
#define	LK_KEY_SELECT		0x8d
#define	LK_KEY_PREV		0x8e
#define	LK_KEY_NEXT		0x8f
#define	LK_KEY_LEFT		0xa7
#define	LK_KEY_RIGHT		0xa8
#define	LK_KEY_DOWN		0xa9
#define	LK_KEY_UP		0xaa
#define	LK_KEY_COMPOSE		0xb1
#define	LK_KEY_TAB		0xbe

/*  The modifiers held with a key, as an escape sequence's parameter m has
    them (m - 1, as display_key() and xterm send them): Shift, Alt (the
    LK201's Compose key) and Ctrl:  */
#define	LK201_MOD_SHIFT		1
#define	LK201_MOD_ALT		2
#define	LK201_MOD_CTRL		4

/*  The keyboard sends at 4800 bps: about 2 ms per byte, or 4 ticks of the
    serial controllers (every 16384 cycles, at about 33 MHz):  */
#define	LK201_TICKS_PER_BYTE	4

/*  How long the rest of an escape sequence may take to arrive (stdin and
    telnet input can split a key's bytes); after that, what did arrive is
    typed, so a lone ESC is Esc:  */
#define	LK201_ESCAPE_USEC	100000

/*  The farthest one mouse update moves:  */
#define	LK201_MOUSE_MAX		50


static int lk201_char_key(int ch, int *mods);


/*
 *  lk201_escape_key():
 *
 *  The key of a terminal's escape sequence (what display_key() sends for
 *  function and editing keys, and an xterm's), given what follows the
 *  escape: "[A" for Up, "OP" for F1, "[15~" for F5, "[Z" for Shift+Tab,
 *  "[1;5A" for Ctrl+Up, "[1;3S" for Alt+F4, and "[27;3;102~" for Alt+f
 *  (display_char(), as xterm's modifyOtherKeys sends a character with
 *  modifiers).  The parameter m after the first ';' is 1 + the modifiers
 *  held with the key, which go into *mods (LK201_MOD_*).  Returns -1 if
 *  the sequence isn't complete yet, and 0 if it isn't a key's.
 */
static int lk201_escape_key(const unsigned char *seq, int len, int *mods)
{
	/*  The keys of a VT220's "[n~" sequences: F1 to F20 (F15 is Help,
	    F16 is Do) and the editing keys:  */
	static const struct { int n, key; } tilde[] = {
		{ 1, LK_KEY_FIND }, { 2, LK_KEY_INSERT }, { 3, LK_KEY_REMOVE },
		{ 4, LK_KEY_SELECT }, { 5, LK_KEY_PREV }, { 6, LK_KEY_NEXT },
		{ 11, 0x56 }, { 12, 0x57 }, { 13, 0x58 }, { 14, 0x59 },
		{ 15, 0x5a }, { 17, 0x64 }, { 18, 0x65 }, { 19, 0x66 },
		{ 20, 0x67 }, { 21, 0x68 }, { 23, 0x71 }, { 24, 0x72 },
		{ 25, 0x73 }, { 26, 0x74 }, { 28, 0x7c }, { 29, 0x7d },
		{ 31, 0x80 }, { 32, 0x81 }, { 33, 0x82 }, { 34, 0x83 },
		{ 0, 0 } };
	int final = seq[len - 1], param[3] = { 0, 0, 0 }, nparam = 0, i, key;

	*mods = 0;
	if (len < 2)
		return -1;

	if (seq[0] == 'O') {
		/*  F1 to F4:  */
		if (seq[1] >= 'P' && seq[1] <= 'S')
			return 0x56 + seq[1] - 'P';
		return 0;
	}
	if (seq[0] != '[')
		return 0;

	/*  The sequence ends with a byte that isn't a digit or ';':  */
	if ((final >= '0' && final <= '9') || final == ';')
		return -1;

	/*  Its parameters, n;m;c:  */
	for (i = 1; i < len - 1; i++) {
		if (seq[i] == ';' && nparam < 2)
			nparam ++;
		else if (seq[i] >= '0' && seq[i] <= '9' && param[nparam] < 1000)
			param[nparam] = param[nparam] * 10 + seq[i] - '0';
		else
			return 0;
	}
	if (param[1] > 1)
		*mods = (param[1] - 1) &
		    (LK201_MOD_SHIFT | LK201_MOD_ALT | LK201_MOD_CTRL);

	switch (final) {
	case 'A':	return LK_KEY_UP;
	case 'B':	return LK_KEY_DOWN;
	case 'C':	return LK_KEY_RIGHT;
	case 'D':	return LK_KEY_LEFT;
	case 'H':	return LK_KEY_FIND;		/*  Home  */
	case 'F':	return LK_KEY_SELECT;		/*  End  */
	case 'P': case 'Q': case 'R': case 'S':
			/*  F1 to F4 with modifiers:  */
			return 0x56 + final - 'P';
	case 'Z':	*mods |= LK201_MOD_SHIFT;	/*  Shift+Tab  */
			return LK_KEY_TAB;
	case '~':
		if (param[0] == 27 && nparam == 2) {
			/*  A character, with modifiers:  */
			key = lk201_char_key(param[2], mods);
			return key > 0? key : 0;
		}
		for (i = 0; tilde[i].n != 0; i++)
			if (tilde[i].n == param[0])
				return tilde[i].key;
	}
	return 0;
}


/*
 *  lk201_send_key():
 *
 *  Sends a key's press and release, with the modifier keys of mods
 *  (LK201_MOD_*) held, and returns how many bytes that is.
 */
static int lk201_send_key(struct lk201_data *d, int key, int mods)
{
	int n = 0;

	d->add_to_rx_queue(d->add_data, KEY_UP, DCKBD_PORT), n++;
	if (mods & LK201_MOD_SHIFT)
		d->add_to_rx_queue(d->add_data, KEY_SHIFT, DCKBD_PORT), n++;
	if (mods & LK201_MOD_CTRL)
		d->add_to_rx_queue(d->add_data, KEY_CONTROL, DCKBD_PORT), n++;
	if (mods & LK201_MOD_ALT)
		d->add_to_rx_queue(d->add_data, LK_KEY_COMPOSE, DCKBD_PORT), n++;

	/*  The key, and its release:  */
	d->add_to_rx_queue(d->add_data, key, DCKBD_PORT), n++;
	d->add_to_rx_queue(d->add_data, KEY_UP, DCKBD_PORT), n++;
	return n;
}


/*
 *  lk201_ascii_key():
 *
 *  Returns the LK201 key that types ch (*shifted tells if it needs Shift),
 *  or -1 if there is none.
 */
static int lk201_ascii_key(int ch, int *shifted)
{
	int i;

	*shifted = 0;
	for (i=0; i<256; i++) {
		/*  Skip numeric digits, so that the normal
			digits are used instead.  */
		if (i >= 0x92 && i<=0xa0)
			continue;

		if (unshiftedAscii[i] == ch)
			return i;
	}

	/*  unshift ch:  */
	if (ch >= 'A' && ch <= 'Z')
		ch = ch + ('a' - 'A');
	for (i=0; i<256; i++)
		if (shiftedAscii[i] == ch) {
			*shifted = 1;
			return i;
		}

	return -1;
}


/*
 *  lk201_char_key():
 *
 *  Returns the LK201 key that types the character ch, or -1 if there is
 *  none, and adds the modifiers it needs to *mods: Shift, and Ctrl for a
 *  control character (Ctrl-A is Ctrl+a, Ctrl-] is Ctrl+], and ESC is
 *  Ctrl+[, see lk201_send_escape()).
 */
static int lk201_char_key(int ch, int *mods)
{
	int key, shifted;

	switch (ch) {
	case '\b':
		return 0xbc;
	case '\n':
	case '\r':
		return 0xbd;
	case '\t':
		return LK_KEY_TAB;
	}

	if (ch < 32) {
		ch = ch >= 1 && ch <= 26 ? 'a' + ch - 1 : ch + '@';
		*mods |= LK201_MOD_CTRL;
	}
	key = lk201_ascii_key(ch, &shifted);
	if (shifted)
		*mods |= LK201_MOD_SHIFT;
	return key;
}


/*
 *  lk201_send_escape():
 *
 *  The LK201 has no Esc key: Ctrl+[ types ESC, in X as on the console.
 *  Returns how many bytes were added.
 */
static int lk201_send_escape(struct lk201_data *d)
{
	int shifted;
	int key = lk201_ascii_key('[', &shifted);

	return lk201_send_key(d, key, LK201_MOD_CTRL |
	    (shifted ? LK201_MOD_SHIFT : 0));
}


/*
 *  lk201_escape_continues():
 *
 *  Tells if ch can be the next byte of a key's escape sequence, given the
 *  len bytes after its ESC so far.  No key's sequence has a control
 *  character or a byte that isn't ASCII in it, and ESC O is only F1 to
 *  F4: in vi, Esc and then O (open a line above) and a letter is ESC O
 *  and that letter.
 */
static bool lk201_escape_continues(const unsigned char *seq, int len,
	unsigned char ch)
{
	if (ch < 32 || ch >= 127)
		return false;
	if (len == 0)
		return ch == '[' || ch == 'O';
	if (seq[0] == 'O')
		return ch >= 'P' && ch <= 'S';
	return true;
}


/*
 *  lk201_type_escape():
 *
 *  Types the start of an escape sequence in keyb_buf that isn't a key's
 *  as the keys it came from: the ESC as Ctrl+[, and the characters after
 *  it as themselves, while the receive queue has room.  Returns how many
 *  bytes were added.
 */
static int lk201_type_escape(struct lk201_data *d)
{
	int i, key, mods, len = d->keyb_buf_pos;
	int n = lk201_send_escape(d);

	d->keyb_buf_pos = 0;
	for (i = 1; i < len &&
	    d->space_available_in_queue(d->add_data, DCKBD_PORT); i++) {
		mods = 0;
		key = lk201_char_key(d->keyb_buf[i], &mods);
		if (key >= 0)
			n += lk201_send_key(d, key, mods);
	}
	return n;
}


/*
 *  lk201_convert_ascii_to_keybcode():
 *
 *  Converts ascii console input to LK201 keyboard scan codes, and adds
 *  those scancodes using the add_to_rx_queue() function. Escape sequences
 *  for function and editing keys become those keys, and other complete
 *  ESC [ sequences type nothing. What starts with ESC but isn't a key's
 *  sequence (a lone ESC, or Esc, O and a letter as typed in vi) is typed
 *  as Ctrl+[ and the characters after it (by lk201_tick(), if nothing
 *  more arrives). Returns how many bytes were added.
 */
int lk201_convert_ascii_to_keybcode(struct lk201_data *d, unsigned char ch)
{
	int key, mods = 0;

	if (d->keyb_buf_pos > 0 && !lk201_escape_continues(d->keyb_buf + 1,
	    d->keyb_buf_pos - 1, ch)) {
		/*  What came before ch isn't a key's escape sequence:  */
		int n = lk201_type_escape(d);
		return n + lk201_convert_ascii_to_keybcode(d, ch);
	}

	if (d->keyb_buf_pos > 0) {
		/*  Escape sequence:  */
		d->keyb_buf[d->keyb_buf_pos++] = ch;

		key = lk201_escape_key(d->keyb_buf + 1, d->keyb_buf_pos - 1,
		    &mods);
		if (key < 0 && d->keyb_buf_pos < (int)sizeof(d->keyb_buf))
			return 0;

		d->keyb_buf_pos = 0;
		return key > 0? lk201_send_key(d, key, mods) : 0;
	}

	if (ch == 27) {
		/*  esc  */
		d->keyb_buf[0] = 27;
		d->keyb_buf_pos = 1;
		return 0;
	}

	key = lk201_char_key(ch, &mods);

	/*  A character that no key types:  */
	if (key < 0)
		return 0;

	return lk201_send_key(d, key, mods);
}


/*
 *  lk201_send_mouse_update_sequence():
 */
static int lk201_send_mouse_update_sequence(struct lk201_data *d, int xdelta,
	int ydelta, int mouse_buttons, int mouse_fb_nr)
{
	int xsign, ysign, m;

	/*  If no change, then don't send any update!  */
	if (xdelta == 0 && ydelta == 0 && d->mouse_buttons == mouse_buttons)
		return 0;

	m = LK201_MOUSE_MAX;

	if (xdelta > m)
		xdelta = m;
	if (xdelta < -m)
		xdelta = -m;
	if (ydelta > m)
		ydelta = m;
	if (ydelta < -m)
		ydelta = -m;

	d->mouse_buttons = mouse_buttons;

	/*
	 *  TODO: Update d->mouse_framebuffer_nr some way!
	 */

	xsign = xdelta < 0? 1 : 0;
	ysign = ydelta < 0? 1 : 0;

	switch (d->mouse_mode) {

	case 0:
		/*  Do nothing (before the mouse is initialized)  */
		return 0;

	case MOUSE_INCREMENTAL:
		if (xdelta < 0)
			xdelta = -xdelta;
		if (ydelta < 0)
			ydelta = -ydelta;

		/*  Reverse sign of x:  (this is needed for some reason)  */
		xsign ^= 1;

		d->add_to_rx_queue(d->add_data, MOUSE_START_FRAME +
		    MOUSE_X_SIGN*xsign + MOUSE_Y_SIGN*ysign +
		    (mouse_buttons & 7), DCMOUSE_PORT);
		d->add_to_rx_queue(d->add_data, xdelta, DCMOUSE_PORT);
		d->add_to_rx_queue(d->add_data, ydelta, DCMOUSE_PORT);
		break;

	default:
		/*  TODO:  prompt mode and perhaps more stuff  */
		fatal("[ lk201: mouse mode 0x%02x unknown: TODO ]\n",
		    d->mouse_mode);
		exit(1);
	}

	return 1;
}


/*
 *  lk201_usec():
 *
 *  The time, in microseconds.
 */
static uint64_t lk201_usec(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}


/*
 *  lk201_tick():
 *
 *  This function should be called "every now and then".
 *  If keys are available from the keyboard, add them to the rx queue.
 *  If other bits are set, an interrupt might need to be caused.
 */
void lk201_tick(struct machine *machine, struct lk201_data *d)
{
	/*
	 *  Different machines seem to use different ports for their
	 *  serial console:
	 *
	 *  DEC MIPSMATE 5100 uses the keyboard port.
	 *  DECstation 3100 (PMAX) and 5000/2000 (3MAX) use the printer port.
	 *  Others seem to use the comm port.
	 */
	int port = DCCOMM_PORT;

	if (machine->machine_type == MACHINE_PMAX) {
		switch (machine->machine_subtype) {
		case MACHINE_DEC_MIPSMATE_5100:
			port = DCKBD_PORT;
			break;
		case MACHINE_DEC_PMAX_3100:
		case MACHINE_DEC_3MAX_5000:
			port = DCPRINTER_PORT;
			break;
		default:
			port = DCCOMM_PORT;
		}
	}

	if (d->use_fb)
		port = DCKBD_PORT;

	/*  Keys are sent one at a time, as fast as the keyboard's serial
	    line would send them; serial console input as fast as it fits:  */
	if (d->use_fb && d->keyb_delay > 0)
		d->keyb_delay --;
	while (d->space_available_in_queue(d->add_data, port) &&
	    console_charavail(d->console_handle) &&
	    (!d->use_fb || d->keyb_delay == 0)) {
		unsigned char ch = console_readchar(d->console_handle);
		if (d->use_fb) {
			d->keyb_delay = LK201_TICKS_PER_BYTE *
			    lk201_convert_ascii_to_keybcode(d, ch);
			if (d->keyb_buf_pos > 0)
				d->keyb_esc_usec = lk201_usec();
		} else
			d->add_to_rx_queue(d->add_data, ch, port);
	}

	/*
	 *  An escape sequence that nothing follows for a while was typed: a
	 *  lone ESC is Esc (Ctrl+[), and Esc and O in vi are Ctrl+[ and O:
	 */
	if (d->use_fb && d->keyb_buf_pos > 0 && d->keyb_delay == 0 &&
	    !console_charavail(d->console_handle) &&
	    d->space_available_in_queue(d->add_data, port) &&
	    lk201_usec() - d->keyb_esc_usec >= LK201_ESCAPE_USEC)
		d->keyb_delay = LK201_TICKS_PER_BYTE * lk201_type_escape(d);

	/*  Don't do mouse updates if we're running in serial console mode:  */
	if (!d->use_fb)
		return;

	/*  The mouse also sends at 4800 bps, 3 bytes per update:  */
	if (d->mouse_delay > 0) {
		d->mouse_delay --;
		return;
	}

	int dx, dy, mouse_buttons, mouse_fb_nr;
	console_getmouse(&dx, &dy, &mouse_buttons, &mouse_fb_nr);

	/*  An update moves at most this far; the rest comes next:  */
	int cx = dx < -LK201_MOUSE_MAX? -LK201_MOUSE_MAX :
	    dx > LK201_MOUSE_MAX? LK201_MOUSE_MAX : dx;
	int cy = dy < -LK201_MOUSE_MAX? -LK201_MOUSE_MAX :
	    dy > LK201_MOUSE_MAX? LK201_MOUSE_MAX : dy;
	if (cx != dx || cy != dy)
		console_mouse_coordinate_update(dx - cx, dy - cy, mouse_fb_nr);

	if (lk201_send_mouse_update_sequence(d, cx, cy, mouse_buttons,
	    mouse_fb_nr))
		d->mouse_delay = 3 * LK201_TICKS_PER_BYTE;
}


void lk201_tx_data(struct lk201_data *d, int port, int idata)
{
	switch (port) {
	case DCKBD_PORT:		/*  port 0  */
		if (!d->use_fb) {
			/*  Simply print the character to stdout:  */
			console_putchar(d->console_handle, idata);
		} else {
			debug("[ lk201: writing data to KBD: 0x%02x ]\n", idata);
			switch (idata) {
			case LK_LED_DISABLE:	/*  0x11  */
				break;
			case LK_LED_ENABLE:	/*  0x13  */
				break;
			case LK_BELL_ENABLE:	/*  0x23  */
				break;
			case 0x41:
				d->add_to_rx_queue(d->add_data, 0xa, DCKBD_PORT);
				d->add_to_rx_queue(d->add_data, 0xb, DCKBD_PORT);
				d->add_to_rx_queue(d->add_data, 0xc, DCKBD_PORT);
				d->add_to_rx_queue(d->add_data, 0xd, DCKBD_PORT);
				break;
			case LED_1:
			case LED_2:
				break;
			case LED_3:
				break;
			case LED_4:
				break;
			case LK_KBD_ENABLE:	/*  0x8b  */
				break;
			case LED_ALL:		/*  0x8f  */
				break;
			case LK_RING_BELL:	/*  0xa7  */
				break;
			case 0xab:	/*  Get Keyboard ID:  */
				/*
				 *  First byte:
				 *	1 = LK201
				 *	2 = LK401
				 *	3 = LK443
				 *	4 = LK421
				 *
				 *  TODO: What about the second byte? 0x22 or
				 *  0x00?
				 */
				d->add_to_rx_queue(d->add_data, 0x01, DCKBD_PORT);
				d->add_to_rx_queue(d->add_data, 0x00, DCKBD_PORT);
				break;
			case LK_DEFAULTS:	/*  0xd3  */
				/*  TODO?  */
				break;
			case 0xfd:	/*  Keyboard self-test:  */
				/*  Suitable return values according to
				    Mach/PMAX source code:  */
				d->add_to_rx_queue(d->add_data,
				    0x01, DCKBD_PORT);
				d->add_to_rx_queue(d->add_data,
				    0x00, DCKBD_PORT);
				d->add_to_rx_queue(d->add_data,
				    0x00, DCKBD_PORT);
				d->add_to_rx_queue(d->add_data,
				    0x00, DCKBD_PORT);
				break;
			default:
				debug("[ lk201: unimplemented keyboard control: 0x%x ]\n",
				    idata);
			}
		}
		break;
	case DCMOUSE_PORT:		/*  port 1  */
		debug("[ lk201: writing data to MOUSE: 0x%x ", idata);
		switch (idata) {
		case MOUSE_INCREMENTAL:
			d->mouse_mode = MOUSE_INCREMENTAL;
			break;
		case MOUSE_SELF_TEST:
			/*
			 *  Mouse self-test:
			 *
			 *  TODO: Find out if this is correct. The lowest
			 *        four bits of the second byte should be
			 *        0x2, according to NetBSD/pmax. But the
			 *        other bits and bytes?
			 */
			debug("(mouse self-test request)");
			d->add_to_rx_queue(d->add_data,
			    0xa0 | d->mouse_revision, DCMOUSE_PORT);
			d->add_to_rx_queue(d->add_data, 0x02, DCMOUSE_PORT);
			d->add_to_rx_queue(d->add_data, 0x00, DCMOUSE_PORT);
			d->add_to_rx_queue(d->add_data, 0x00, DCMOUSE_PORT);
			break;
		default:
			debug("UNKNOWN byte; TODO");
		}
		debug(" ]\n");
		break;
	case DCCOMM_PORT:		/*  port 2  */
	case DCPRINTER_PORT:		/*  port 3  */
		/*  Simply print the character to stdout:  */
		console_putchar(d->console_handle, idata);
	}
}


/*
 *  lk201_init():
 *
 *  Initialize lk201 keyboard/mouse settings.
 */
void lk201_init(struct lk201_data *d, int use_fb,
	int (*space_available_in_queue)(void *,int),
	void (*add_to_rx_queue)(void *,int,int),
	int console_handle, void *add_data)
{
	memset(d, 0, sizeof(struct lk201_data));

	d->space_available_in_queue = space_available_in_queue;
	d->add_to_rx_queue = add_to_rx_queue;
	d->add_data = add_data;

	d->use_fb = use_fb;
	d->mouse_mode = 0;
	d->mouse_revision = 0;	/*  0..15  */
	d->console_handle = console_handle;

	/*  The keys typed into the framebuffer window come with the
	    modifiers held, Shift, Ctrl and Alt (see lk201_escape_key()):  */
	if (use_fb)
		console_set_keys_with_modifiers(console_handle);

	/*
	 *  Power up self-test result, as per
	 *  https://www.netbsd.org/docs/Hardware/Machines/DEC/lk201.html#power_up
	 */
	if (d->use_fb) {
		d->add_to_rx_queue(d->add_data, 0x01, DCKBD_PORT);
		d->add_to_rx_queue(d->add_data, 0x00, DCKBD_PORT);
		d->add_to_rx_queue(d->add_data, 0x00, DCKBD_PORT);
		d->add_to_rx_queue(d->add_data, 0x00, DCKBD_PORT);
	}
}


