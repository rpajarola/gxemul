/*
 *  Copyright (C) 2026  Antigravity
 *
 *  Motorola MVME393 Intelligent VMEbus Graphics Controller
 *
 *  Provides VME A16 Short I/O CSR, A32 Dual-Port RAM (512 KB),
 *  circular packet queue processing, TMS34010/68k firmware handshake,
 *  VMEbus interrupt delivery, and 1024x768 8bpp framebuffer scanout.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "cpu.h"
#include "device.h"
#include "devices.h"
#include "emul.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"

#if defined(WITH_SDL) || defined(WITH_SDL2)
#include <SDL.h>
#include <SDL_keycode.h>
#endif

#define MVME393_DPR_SIZE        0x80000         /*  512 KB Dual-Port RAM  */
#define MVME393_A16_BASE        0xa800          /*  Short I/O base        */
#define MVME393_A16_SIZE        0x10            /*  16 bytes per card     */
#define MVME393_DPR_BASE        0xfd800000ULL   /*  A32 base address      */

#define READ_DPR16(d, a)        (((uint16_t)(d)->dpr[(a)] << 8) | (d)->dpr[(a) + 1])
#define WRITE_DPR16(d, a, v)    do { \
	(d)->dpr[(a)] = ((v) >> 8) & 0xff; \
	(d)->dpr[(a) + 1] = (v) & 0xff; \
} while (0)

#define READ_DPR32(d, a)        (((uint32_t)READ_DPR16(d, (a)) << 16) | READ_DPR16(d, (a) + 2))
#define WRITE_DPR32(d, a, v)    do { \
	WRITE_DPR16(d, (a), ((v) >> 16) & 0xffff); \
	WRITE_DPR16(d, (a) + 2, (v) & 0xffff); \
} while (0)

struct mvme393_data {
	struct machine      *machine;
	struct mvme187_data *mvme187;
	struct vfb_data     *fb;

	uint8_t             dpr[MVME393_DPR_SIZE];
	uint8_t             vram[1024 * 768];
	uint16_t            tms_ram[512 * 1024];

	uint16_t            csr;
	uint8_t             ivec;
	uint8_t             ilev;
	int                 host_irq;

	uint32_t            window_addr;
	uint16_t            host_write_ptr;
	uint32_t            queue_ptr;

	uint32_t            tms_dest;
	uint32_t            m_68k_dest;
	uint32_t            vram_dest;

	int                 vram_dirty;
};

static struct mvme393_data *s_mvme393 = NULL;

#define MVME393_KBD_QUEUE_SIZE 128

struct mvme393_kbd_event {
	uint32_t key;
	uint32_t down;
};

static struct mvme393_kbd_event s_kbd_queue[MVME393_KBD_QUEUE_SIZE];
static int s_kbd_head = 0, s_kbd_tail = 0;

static void dev_mvme393_enqueue_key(uint32_t key, uint32_t down)
{
	int next = (s_kbd_head + 1) % MVME393_KBD_QUEUE_SIZE;
	if (next != s_kbd_tail) {
		s_kbd_queue[s_kbd_head].key = key;
		s_kbd_queue[s_kbd_head].down = down;
		s_kbd_head = next;
	}
}

static void dev_mvme393_flush_kbd_queue(struct mvme393_data *d)
{
	if (d != NULL && d->fb != NULL && d->fb->framebuffer != NULL) {
		uint8_t *in = d->fb->framebuffer + 0xfe000;
		if (s_kbd_head != s_kbd_tail) {
			uint32_t kbd_pending = ((uint32_t)in[0x18]<<24) | ((uint32_t)in[0x19]<<16) |
			                       ((uint32_t)in[0x1a]<<8)  | in[0x1b];
			if (kbd_pending == 0) {
				struct mvme393_kbd_event ev = s_kbd_queue[s_kbd_tail];
				s_kbd_tail = (s_kbd_tail + 1) % MVME393_KBD_QUEUE_SIZE;

				in[0x10] = (ev.key >> 24) & 0xff;
				in[0x11] = (ev.key >> 16) & 0xff;
				in[0x12] = (ev.key >> 8) & 0xff;
				in[0x13] = ev.key & 0xff;

				in[0x14] = (ev.down >> 24) & 0xff;
				in[0x15] = (ev.down >> 16) & 0xff;
				in[0x16] = (ev.down >> 8) & 0xff;
				in[0x17] = ev.down & 0xff;

				in[0x18] = 0; in[0x19] = 0; in[0x1a] = 0; in[0x1b] = 1;
			}
		}
	}
}

static int dev_mvme393_sdl_to_keycode(int sym)
{
#if defined(WITH_SDL) || defined(WITH_SDL2)
	switch (sym) {
	/* Modifiers */
	case SDLK_LSHIFT:	return 16;
	case SDLK_RSHIFT:	return 21;
	case SDLK_LCTRL:	return 17;
	case SDLK_RCTRL:	return 22;
	case SDLK_LALT:		return 18;
	case SDLK_RALT:		return 19;
	case SDLK_CAPSLOCK:	return 20;
	case SDLK_LGUI:		return 23;
	case SDLK_RGUI:		return 140;

	/* Editing & Navigation */
	case SDLK_RETURN:
	case SDLK_KP_ENTER:	return 13;
	case SDLK_BACKSPACE:	return 8;
	case SDLK_TAB:		return 9;
	case SDLK_ESCAPE:	return 27;
	case SDLK_DELETE:	return 127;
	case SDLK_INSERT:	return 15;
	case SDLK_HOME:		return 29;
	case SDLK_END:		return 30;
	case SDLK_PAGEUP:	return 31;
	case SDLK_PAGEDOWN:	return 14;
	case SDLK_LEFT:		return 24;
	case SDLK_UP:		return 25;
	case SDLK_RIGHT:	return 26;
	case SDLK_DOWN:		return 28;

	/* Keypad */
	case SDLK_KP_0:		return (SDL_GetModState() & KMOD_NUM) ? '0' : 15;  /* XK_Insert */
	case SDLK_KP_1:		return (SDL_GetModState() & KMOD_NUM) ? '1' : 30;  /* XK_End */
	case SDLK_KP_2:		return (SDL_GetModState() & KMOD_NUM) ? '2' : 28;  /* XK_Down */
	case SDLK_KP_3:		return (SDL_GetModState() & KMOD_NUM) ? '3' : 14;  /* XK_Next */
	case SDLK_KP_4:		return (SDL_GetModState() & KMOD_NUM) ? '4' : 24;  /* XK_Left */
	case SDLK_KP_5:		return (SDL_GetModState() & KMOD_NUM) ? '5' : -1;
	case SDLK_KP_6:		return (SDL_GetModState() & KMOD_NUM) ? '6' : 26;  /* XK_Right */
	case SDLK_KP_7:		return (SDL_GetModState() & KMOD_NUM) ? '7' : 29;  /* XK_Home */
	case SDLK_KP_8:		return (SDL_GetModState() & KMOD_NUM) ? '8' : 25;  /* XK_Up */
	case SDLK_KP_9:		return (SDL_GetModState() & KMOD_NUM) ? '9' : 31;  /* XK_Prior */
	case SDLK_KP_PERIOD:	return (SDL_GetModState() & KMOD_NUM) ? '.' : 127; /* XK_Delete */
	case SDLK_KP_DIVIDE:	return '/';
	case SDLK_KP_MULTIPLY:	return '*';
	case SDLK_KP_MINUS:	return '-';
	case SDLK_KP_PLUS:	return '+';

	/* Function keys */
	case SDLK_F1:		return 128;
	case SDLK_F2:		return 129;
	case SDLK_F3:		return 130;
	case SDLK_F4:		return 131;
	case SDLK_F5:		return 132;
	case SDLK_F6:		return 133;
	case SDLK_F7:		return 134;
	case SDLK_F8:		return 135;
	case SDLK_F9:		return 136;
	case SDLK_F10:		return 137;
	case SDLK_F11:		return 138;
	case SDLK_F12:		return 139;

	default:
		if (sym >= 'A' && sym <= 'Z')
			return sym + ('a' - 'A');
		if (sym >= 32 && sym <= 126)
			return sym;
		return -1;
	}
#else
	if (sym >= 'A' && sym <= 'Z')
		return sym + ('a' - 'A');
	if (sym >= 32 && sym <= 126)
		return sym;
	return -1;
#endif
}

static void dev_mvme393_key_event(struct machine *m, int sym, int down)
{
	(void)m;

	if (down < 0) {
		/* Focus lost: release all modifiers */
		dev_mvme393_enqueue_key(16, 0); /* Shift_L */
		dev_mvme393_enqueue_key(21, 0); /* Shift_R */
		dev_mvme393_enqueue_key(17, 0); /* Control_L */
		dev_mvme393_enqueue_key(22, 0); /* Control_R */
		dev_mvme393_enqueue_key(18, 0); /* Alt_L */
		dev_mvme393_enqueue_key(19, 0); /* Alt_R */
		dev_mvme393_enqueue_key(23, 0); /* Meta_L */
		dev_mvme393_enqueue_key(140, 0); /* Meta_R */
		if (s_mvme393 != NULL)
			dev_mvme393_flush_kbd_queue(s_mvme393);
		return;
	}

	int keycode = dev_mvme393_sdl_to_keycode(sym);
	if (keycode > 0) {
		dev_mvme393_enqueue_key((uint32_t)keycode, (uint32_t)down);
		if (s_mvme393 != NULL)
			dev_mvme393_flush_kbd_queue(s_mvme393);
	}
}

static void dev_mvme393_display_key(struct machine *m, int ch)
{
	(void)m;

	if (ch >= 1 && ch <= 26 && ch != '\t' && ch != '\n' && ch != '\r') {
		/* Control characters (Ctrl+A .. Ctrl+Z) */
		dev_mvme393_enqueue_key(17, 1);            /* XK_Control_L press */
		dev_mvme393_enqueue_key('a' + ch - 1, 1);  /* key press */
		dev_mvme393_enqueue_key('a' + ch - 1, 0);  /* key release */
		dev_mvme393_enqueue_key(17, 0);            /* XK_Control_L release */
	} else if (ch == '\r' || ch == '\n') {
		dev_mvme393_enqueue_key(13, 1);            /* XK_Return press */
		dev_mvme393_enqueue_key(13, 0);            /* XK_Return release */
	} else if (ch == '\b') {
		dev_mvme393_enqueue_key(8, 1);             /* XK_BackSpace press */
		dev_mvme393_enqueue_key(8, 0);             /* XK_BackSpace release */
	} else if (ch == '\t') {
		dev_mvme393_enqueue_key(9, 1);             /* XK_Tab press */
		dev_mvme393_enqueue_key(9, 0);             /* XK_Tab release */
	} else if (ch == 27) {
		dev_mvme393_enqueue_key(27, 1);            /* XK_Escape press */
		dev_mvme393_enqueue_key(27, 0);            /* XK_Escape release */
	} else if (ch == 127) {
		dev_mvme393_enqueue_key(127, 1);           /* XK_Delete press */
		dev_mvme393_enqueue_key(127, 0);           /* XK_Delete release */
	} else if (ch >= 32 && ch < 127) {
		dev_mvme393_enqueue_key((uint32_t)ch, 1);  /* ASCII key press */
		dev_mvme393_enqueue_key((uint32_t)ch, 0);  /* ASCII key release */
	}
}

static void send_response(struct mvme393_data *d, uint16_t op, uint16_t status)
{
	WRITE_DPR16(d, 0x40008, 0x0005); /* Response opcode = 5 */
	WRITE_DPR16(d, 0x4000a, 10);
	WRITE_DPR16(d, 0x4000c, 0);
	WRITE_DPR16(d, 0x4000e, op);
	WRITE_DPR16(d, 0x40010, status);
	WRITE_DPR16(d, 0x40000, 0x0002);
}

static void dev_mvme393_reset(struct mvme393_data *d)
{
	int i;

	d->csr = 0x0000;
	d->ivec = 0x98;
	d->ilev = 4;
	d->host_irq = 0;
	d->window_addr = 0;
	d->host_write_ptr = 0;
	d->queue_ptr = 0;
	d->tms_dest = 0;
	d->m_68k_dest = 0;
	d->vram_dest = 0;
	d->vram_dirty = 1;
	s_kbd_head = s_kbd_tail = 0;

	memset(d->dpr, 0, sizeof(d->dpr));
	memset(d->vram, 0, sizeof(d->vram));
	memset(d->tms_ram, 0, sizeof(d->tms_ram));

	/* Initialize dual-port RAM user queue window at 0x40000 */
	WRITE_DPR16(d, 0x40000, 0x0001); /* status = 1 (queue ready) */
	WRITE_DPR16(d, 0x40002, 0x0000);
	WRITE_DPR16(d, 0x40004, 0x1fff); /* read_ptr = 8191 (queue empty) */
	WRITE_DPR16(d, 0x40006, 0x0000); /* write_ptr = 0 */
	WRITE_DPR16(d, 0x40000 + 8200, 0x0000);
	WRITE_DPR16(d, 0x40000 + 8202, 0x0000);
	WRITE_DPR16(d, 0x40000 + 8204, 0x0000);
	WRITE_DPR16(d, 0x40000 + 8206, 0x0fff); /* 4095 */
	WRITE_DPR16(d, 0x40000 + 8208, 0x0000);

	/* Board ready and model ID */
	WRITE_DPR16(d, 0x4f000, 0x0001); /* Firmware/board active */
	WRITE_DPR16(d, 0x4d034, 0x0001); /* MVME792 transition module detected */
	WRITE_DPR16(d, 0x4f0e0, 0x00ef); /* MVME393 model identifier */
	WRITE_DPR16(d, 0x4f0e4, 0x0000);
	WRITE_DPR16(d, 0x4f0e6, 0x00ef); /* MVME393 model identifier */

	/* Default interrupt vector table in dual-port RAM */
	for (i = 0; i < 7; i++)
		WRITE_DPR16(d, 0x4fff2 + i * 2, d->ivec);
}

static void handle_doorbell(struct mvme393_data *d)
{
	int handled = 0;
	uint16_t mbox_status, mbox_op;
	uint32_t arg, p, e;

	if (READ_DPR16(d, 0x4f04e) == 4) {
		WRITE_DPR16(d, 0x4f000, 1);
		WRITE_DPR16(d, 0x4f0e6, READ_DPR16(d, 0x4f0e6) | 0x000c);
		WRITE_DPR16(d, 0x40000, 1);
		WRITE_DPR16(d, 0x40004, 0x1fff);
		WRITE_DPR16(d, 0x40006, 0);
		d->host_write_ptr = 0;
	}

	mbox_status = READ_DPR16(d, 0x4d00e);
	mbox_op = (READ_DPR16(d, 0x4d004) >> 8) & 0xff;
	arg = READ_DPR32(d, 0x4d000);

	if (mbox_status & 0x1000) {
		WRITE_DPR16(d, 0x4d00e, mbox_status & ~0x3000);
	}

	/* 1. Check mailbox at 0x4d000 */
	if (mbox_op != 0 || (mbox_status & 0x8000)) {
		if (mbox_op == 0x0d) { /* Init queue */
			d->queue_ptr = arg & 0x7ffff;
			WRITE_DPR16(d, 0x4d00e, (READ_DPR16(d, 0x4d00e) & ~0x8000) | 0x2000);
			WRITE_DPR16(d, 0x4d008, 0);
			WRITE_DPR16(d, 0x4d004, READ_DPR16(d, 0x4d004) & 0x00ff);
			handled = 1;
		} else if (mbox_op == 0x11 || mbox_op == 0x17) { /* Firmware start */
			WRITE_DPR32(d, 0x2984c, 0x00000000);
			WRITE_DPR32(d, 0x29850, 0x00060000);
			WRITE_DPR32(d, 0x29854, 0x00060002);
			WRITE_DPR32(d, 0x29858, 0x00060004);
			WRITE_DPR32(d, 0x2985c, 0x00060006);

			WRITE_DPR16(d, 0x4f000, 0x0001);
			WRITE_DPR16(d, 0x4f0e0, 0x00ef);
			WRITE_DPR16(d, 0x4f0e6, READ_DPR16(d, 0x4f0e6) | 0x0004);

			WRITE_DPR16(d, 0x4d00e, (READ_DPR16(d, 0x4d00e) & ~0x8000) | 0x2000);
			WRITE_DPR16(d, 0x4d008, 0x0000);
			WRITE_DPR16(d, 0x4d004, READ_DPR16(d, 0x4d004) & 0x00ff);
			handled = 1;
		} else {
			WRITE_DPR16(d, 0x4d00e, (READ_DPR16(d, 0x4d00e) & ~0x8000) | 0x2000);
			WRITE_DPR16(d, 0x4d008, 0x0000);
			WRITE_DPR16(d, 0x4d004, READ_DPR16(d, 0x4d004) & 0x00ff);
			handled = 1;
		}
	}

	/* 2. Check packet queue in dual-port RAM */
	for (p = 0x4f9ac; p < 0x4ffe0; p += 0x40) {
		uint8_t status = d->dpr[p + 0x11];
		if (status == 1) {
			uint16_t op = READ_DPR16(d, p + 0x0a);
			uint32_t target = READ_DPR32(d, p + 0x12);
			uint32_t buf_off = READ_DPR32(d, p + 0x16);
			uint32_t count = READ_DPR32(d, p + 0x1a);
			uint32_t matching_elem = 0;
			uint32_t elem_vme;

			(void)target;
			(void)count;

			if (op == 0x4018) { /* HALT */
				d->csr |= 0x0100;
			} else if (op == 0x4020) { /* RESUME / RUN */
				d->csr &= ~0x0100;
			} else if (op == 0x2003) { /* GET BOARD INFO */
				uint32_t dpr_off = buf_off & 0x7ffff;
				WRITE_DPR16(d, 0x4d034, 0x0001);
				if (dpr_off != 0 && dpr_off < 0x7ff00)
					WRITE_DPR16(d, dpr_off + 0x24, 0x0001);
			} else if (op == 0x4014) { /* READ MEMORY */
				uint32_t dpr_off = buf_off & 0x7ffff;
				if ((target == 0x200 || target == 0) && count >= 2 && dpr_off < 0x7ff00)
					WRITE_DPR16(d, dpr_off, 0xdead);
				else if ((target == 0xfd100030 || (target & 0xffff) == 0x0030) && dpr_off < 0x7ff00)
					WRITE_DPR16(d, dpr_off, 0xd0d0);
				else if (target == 0xfd100000 && dpr_off < 0x7ff00) {
					uint32_t i;
					for (i = 0; i < count && (dpr_off + i) < 0x7ff00; i += 2)
						WRITE_DPR16(d, dpr_off + i, 0x0000);
				}
			}

			/* Find matching queue element */
			for (e = 0x4f9b0; e <= 0x4f9e0; e += 0x0c) {
				uint32_t pkt_ptr = READ_DPR32(d, e + 4);
				if ((pkt_ptr & 0x7ffff) == p) {
					matching_elem = e;
					break;
				}
			}

			elem_vme = 0xfd840000 | (matching_elem ? matching_elem : 0x4f9b0);
			WRITE_DPR32(d, p + 6, elem_vme);

			if (matching_elem) {
				d->dpr[matching_elem + 8] = 0;
				if (d->queue_ptr != 0) {
					uint32_t cur_head = READ_DPR32(d, d->queue_ptr);
					if ((cur_head & 0x7ffff) == matching_elem) {
						uint32_t next_vme = READ_DPR32(d, matching_elem);
						WRITE_DPR32(d, d->queue_ptr, next_vme);
					}
				}
			}

			/* Ensure free list at 0x4f9ac */
			if (READ_DPR32(d, 0x4f9ac) == 0)
				WRITE_DPR32(d, 0x4f9ac, elem_vme);

			/* Clear status byte to indicate completion */
			d->dpr[p + 0x11] = 0;
			handled = 1;
			break;
		}
	}

	if (handled) {
		if (READ_DPR16(d, 0x4fff2) != 0)
			d->ivec = READ_DPR16(d, 0x4fff2) & 0xff;

		WRITE_DPR16(d, 0x4f0e0, 0x0001); /* Command complete event */
		d->csr |= 0x0400; /* Interrupt Pending */
		d->host_irq = 1;
		if (d->mvme187 != NULL)
			dev_mvme187_vme_irq(d->mvme187, d->ilev, 1);
	}
}

/*
 *  VME Short I/O A16 Access (0xa800..0xa80f)
 */
static int dev_mvme393_a16_access(struct cpu *cpu, struct memory *mem,
	uint64_t relative_addr, unsigned char *data, size_t len,
	int writeflag, void *extra)
{
	struct mvme393_data *d = (struct mvme393_data *) extra;
	uint32_t a = relative_addr & 0xffff;
	uint32_t offset;

	(void)cpu;
	(void)mem;

	if (a < MVME393_A16_BASE || a >= MVME393_A16_BASE + MVME393_A16_SIZE)
		return 0;

	offset = a - MVME393_A16_BASE;

	if (offset == 0) { /* CSR */
		if (writeflag == MEM_READ) {
			uint16_t val = d->csr;
			if (d->host_irq)
				val |= 0x0400;

			if (len == 2) {
				data[0] = (val >> 8) & 0xff;
				data[1] = val & 0xff;
			} else if (len == 1) {
				data[0] = (val >> 8) & 0xff;
			}
			return 1;
		} else {
			uint16_t wdata;
			if (len == 2)
				wdata = ((uint16_t)data[0] << 8) | data[1];
			else
				wdata = (uint16_t)data[0] << 8;

			if (wdata & 0x0007)
				d->ilev = wdata & 0x07;

			if (wdata & 0x8000) {
				dev_mvme393_reset(d);
				return 1;
			}

			if (wdata & 0x0100)
				d->csr |= 0x0100;
			else
				d->csr &= ~0x0100;

			/* Host cleared interrupt */
			if (!(wdata & 0x0400) && !(wdata & 0x0004)) {
				if (d->host_irq) {
					d->host_irq = 0;
					d->csr &= ~0x0400;
					if (d->mvme187 != NULL)
						dev_mvme187_vme_irq(d->mvme187, d->ilev, 0);
				}
			}

			/* Doorbell interrupt */
			if ((wdata & 0x2000) || (wdata & 0x0020))
				handle_doorbell(d);

			d->csr = (d->csr & ~0x0007) | d->ilev;
			return 1;
		}
	} else if (offset == 2) { /* IVR */
		if (writeflag == MEM_READ) {
			data[0] = d->ivec;
			if (len > 1)
				data[1] = 0;
		} else {
			d->ivec = data[0];
		}
		return 1;
	} else if (offset == 4) { /* ICR */
		if (writeflag == MEM_READ) {
			data[0] = d->ilev;
			if (len > 1)
				data[1] = 0;
		} else {
			d->ilev = data[0] & 0x07;
		}
		return 1;
	}

	return 1;
}

/*
 *  VME A32 Dual-Port RAM Access (0xfd800000..0xfd87ffff, 512 KB)
 */
DEVICE_ACCESS(mvme393_ram)
{
	struct mvme393_data *d = (struct mvme393_data *) extra;
	uint32_t addr = relative_addr & 0x7ffff;
	size_t i;

	(void)cpu;
	(void)mem;

	if (addr + len > MVME393_DPR_SIZE)
		return 0;

	/* Window data access at 0x60000..0x60fff */
	if (writeflag == MEM_READ && addr >= 0x60000 && addr < 0x61000) {
		if (d->window_addr == 0x200 || d->window_addr == 0) {
			if (len >= 4) {
				data[0] = 0xde; data[1] = 0xad; data[2] = 0x00; data[3] = 0x00;
			} else if (len >= 2) {
				data[0] = 0xde; data[1] = 0xad;
			}
			return 1;
		}
		if (d->window_addr == 0xfd100030 || (d->window_addr & 0xffff) == 0x0030) {
			if (len >= 4) {
				data[0] = 0xd0; data[1] = 0xd0; data[2] = 0x00; data[3] = 0x00;
			} else if (len >= 2) {
				data[0] = 0xd0; data[1] = 0xd0;
			}
			return 1;
		}
	}

	if (writeflag == MEM_READ) {
		memcpy(data, &d->dpr[addr], len);
		return 1;
	}

	/* Write handling */
	if (addr == 0x50000) {
		if (len >= 2)
			d->window_addr = (d->window_addr & 0xffff0000) | ((data[0] << 8) | data[1]);
	} else if (addr == 0x52000) {
		if (len >= 2)
			d->window_addr = (d->window_addr & 0x0000ffff) | (((uint32_t)((data[0] << 8) | data[1])) << 16);
	}

	if (addr == 0x4f000 && READ_DPR16(d, 0x4f04e) == 4) {
		WRITE_DPR16(d, 0x4f000, 1);
		WRITE_DPR16(d, 0x4f0e6, READ_DPR16(d, 0x4f0e6) | 0x000c);
		WRITE_DPR16(d, 0x40000, 1);
		WRITE_DPR16(d, 0x40004, 0x1fff);
		WRITE_DPR16(d, 0x40006, 0);
		d->host_write_ptr = 0;
		return 1;
	}

	/* Write to memory */
	for (i = 0; i < len; i++)
		d->dpr[addr + i] = data[i];

	/* Host queue write pointer updated */
	if (addr < 0x40008 && addr + len > 0x40006) {
		uint16_t new_write_ptr = READ_DPR16(d, 0x40006);
		uint16_t cur_ptr = d->host_write_ptr;
		d->host_write_ptr = new_write_ptr;

		while (cur_ptr != new_write_ptr) {
			uint32_t pkt_addr = 0x40008 + cur_ptr;
			uint16_t op, pkt_len, count, chunk_flag;
			uint32_t target;

			if (pkt_addr + 14 > 0x40000 + 8192)
				break;

			op = READ_DPR16(d, pkt_addr);
			pkt_len = READ_DPR16(d, pkt_addr + 2);
			target = READ_DPR32(d, pkt_addr + 6);
			count = READ_DPR16(d, pkt_addr + 10);
			chunk_flag = READ_DPR16(d, pkt_addr + 12);

			if (op == 6) { /* TMS34010 code / data download */
				uint16_t idx;
				if (target >= 0xff800000) {
					uint32_t word_base = (target - 0xff800000) >> 4;
					if (d->tms_dest < word_base || d->tms_dest >= word_base + 0x80000)
						d->tms_dest = word_base;
					for (idx = 0; idx < (count + 1) / 2; idx++) {
						if (d->tms_dest < 0x80000)
							d->tms_ram[d->tms_dest++] = READ_DPR16(d, pkt_addr + 14 + idx * 2);
					}
					if (chunk_flag == 0)
						d->tms_dest = 0;
				} else {
					uint32_t byte_base = target >> 3;
					if (d->vram_dest < byte_base || d->vram_dest >= byte_base + sizeof(d->vram))
						d->vram_dest = byte_base;
					for (idx = 0; idx < count; idx++) {
						if (d->vram_dest < sizeof(d->vram)) {
							d->vram[d->vram_dest] = d->dpr[pkt_addr + 14 + idx];
							if (d->fb != NULL && d->fb->framebuffer != NULL && d->vram_dest < d->fb->framebuffer_size)
								d->fb->framebuffer[d->vram_dest] = d->dpr[pkt_addr + 14 + idx];
							d->vram_dest++;
						}
					}
					d->vram_dirty = 1;
					if (chunk_flag == 0)
						d->vram_dest = 0;
				}
			} else if (op == 5) { /* 68010 code download */
				uint16_t idx;
				if (d->m_68k_dest < target || d->m_68k_dest >= target + 0x40000)
					d->m_68k_dest = target;
				for (idx = 0; idx < count; idx++) {
					if (d->m_68k_dest < sizeof(d->dpr))
						d->dpr[d->m_68k_dest++] = d->dpr[pkt_addr + 14 + idx];
				}
				if (chunk_flag == 0)
					d->m_68k_dest = 0;
			} else if (op == 4) { /* Start TMS34010 */
				/* TMS34010 started */
			} else if (op == 3) { /* Start 68010 */
				WRITE_DPR32(d, 0x2084c, 0x00400000);
				WRITE_DPR32(d, 0x20850, 0x00060000);
				WRITE_DPR32(d, 0x20854, 0x00060002);
				WRITE_DPR32(d, 0x20858, 0x00060004);
				WRITE_DPR32(d, 0x2085c, 0x00060006);

				WRITE_DPR32(d, 0x2984c, 0x00400000);
				WRITE_DPR32(d, 0x29850, 0x00060000);
				WRITE_DPR32(d, 0x29854, 0x00060002);
				WRITE_DPR32(d, 0x29858, 0x00060004);
				WRITE_DPR32(d, 0x2985c, 0x00060006);

				WRITE_DPR16(d, 0x4f000, 0x0001);
				WRITE_DPR16(d, 0x4f0e6, READ_DPR16(d, 0x4f0e6) | 0x0004);
				WRITE_DPR16(d, 0x40000, 0x0002);
			} else if (op == 25 || op == 9 || op == 12 || op == 23 || op == 40 || op == 2) {
				send_response(d, op, 0);
			} else if (op == 37) { /* Set screen timing */
				WRITE_DPR16(d, 0x40000, 0x0002);
			} else if (op == 55) { /* Probe command */
				WRITE_DPR16(d, 0x40000, 0x0002);
				WRITE_DPR16(d, 0x40008, 0x0015);
			}

			if (pkt_len == 0)
				break;

			cur_ptr += pkt_len;
			if (cur_ptr >= 8192)
				cur_ptr = 0;
		}

		/* Advance read_ptr to indicate packets have been consumed */
		WRITE_DPR16(d, 0x40004, (new_write_ptr - 1) & 0x1fff);
	}

	return 1;
}

/*
 *  VME Interrupt Acknowledge
 */
static int dev_mvme393_iack(void *extra, int level)
{
	struct mvme393_data *d = (struct mvme393_data *) extra;

	if (level == d->ilev && d->host_irq) {
		d->host_irq = 0;
		d->csr &= ~0x0400;
		if (d->mvme187 != NULL)
			dev_mvme187_vme_irq(d->mvme187, d->ilev, 0);
		return d->ivec;
	}

	return -1;
}

/*
 *  Framebuffer refresh tick
 */
static void dev_mvme393_tick(struct cpu *cpu, void *extra)
{
	struct mvme393_data *d = (struct mvme393_data *) extra;
	int dx, dy, buttons, fb_nr;

	(void)cpu;

	if (d->fb != NULL && d->fb->framebuffer != NULL) {
		uint8_t *in = d->fb->framebuffer + 0xfe000;
		uint8_t *pal = d->fb->framebuffer + 0xff000;
		int pal_dirty = 0;
		int i;

		for (i = 0; i < 256; i++) {
			uint8_t r = pal[i * 4 + 1];
			uint8_t g = pal[i * 4 + 2];
			uint8_t b = pal[i * 4 + 3];
			if (d->fb->rgb_palette[i * 3 + 0] != r ||
			    d->fb->rgb_palette[i * 3 + 1] != g ||
			    d->fb->rgb_palette[i * 3 + 2] != b) {
				d->fb->rgb_palette[i * 3 + 0] = r;
				d->fb->rgb_palette[i * 3 + 1] = g;
				d->fb->rgb_palette[i * 3 + 2] = b;
				pal_dirty = 1;
			}
		}
		if (pal_dirty) {
			d->vram_dirty = 1;
		}

		console_getmouse(&dx, &dy, &buttons, &fb_nr);
		if (dx != 0 || dy != 0 || buttons != 0) {
			int32_t cur_dx = ((int32_t)in[0]<<24) | ((int32_t)in[1]<<16) | ((int32_t)in[2]<<8) | in[3];
			int32_t cur_dy = ((int32_t)in[4]<<24) | ((int32_t)in[5]<<16) | ((int32_t)in[6]<<8) | in[7];
			cur_dx += dx;
			cur_dy += dy;
			in[0] = (cur_dx >> 24) & 0xff; in[1] = (cur_dx >> 16) & 0xff; in[2] = (cur_dx >> 8) & 0xff; in[3] = cur_dx & 0xff;
			in[4] = (cur_dy >> 24) & 0xff; in[5] = (cur_dy >> 16) & 0xff; in[6] = (cur_dy >> 8) & 0xff; in[7] = cur_dy & 0xff;
			in[8] = (buttons >> 24) & 0xff; in[9] = (buttons >> 16) & 0xff; in[10] = (buttons >> 8) & 0xff; in[11] = buttons & 0xff;
			in[12] = 0; in[13] = 0; in[14] = 0; in[15] = 1;
		}

		/* Deliver queued keyboard events if previous event was consumed */
		dev_mvme393_flush_kbd_queue(d);

		if (d->vram_dirty) {
			d->fb->update_x1 = 0;
			d->fb->update_y1 = 0;
			d->fb->update_x2 = 1023;
			d->fb->update_y2 = 767;
			d->vram_dirty = 0;
		}
	}
}

/*
 *  Initialization
 */
void dev_mvme393_init(struct machine *machine, struct mvme187_data *mvme187)
{
	struct mvme393_data *d;

	if (s_mvme393 != NULL)
		return;

	CHECK_ALLOCATION(d = (struct mvme393_data *) malloc(sizeof(struct mvme393_data)));
	memset(d, 0, sizeof(struct mvme393_data));

	d->machine = machine;
	d->mvme187 = mvme187;
	s_mvme393 = d;

	dev_mvme393_reset(d);

	/* Register 512 KB Dual-Port RAM in VME A32 space at 0xfd800000 */
	memory_device_register(machine->memory, "mvme393_ram",
		MVME393_DPR_BASE, MVME393_DPR_SIZE,
		dev_mvme393_ram_access, d, DM_DEFAULT, NULL);

	/* Hook into MVME187 VME A16 Short I/O dispatcher and IACK */
	if (mvme187 != NULL) {
		dev_mvme187_set_vme_a16(mvme187, dev_mvme393_a16_access, d);
		dev_mvme187_set_vme_iack(mvme187, dev_mvme393_iack, d);
	}

	/* Initialize 1024x768 (1 MB buffer) 8bpp color framebuffer at 0xfd000000 */
	d->fb = dev_fb_init(machine, machine->memory, 0xfd000000ULL,
		VFB_GENERIC, 1024, 768, 1024, 1024, 8, "MVME393 1024x768");

	if (d->fb != NULL) {
		int i;
		for (i = 0; i < 256; i++) {
			int r = ((i >> 5) & 7) * 255 / 7;
			int g = ((i >> 2) & 7) * 255 / 7;
			int b = (i & 3) * 255 / 3;
			d->fb->rgb_palette[i * 3 + 0] = r;
			d->fb->rgb_palette[i * 3 + 1] = g;
			d->fb->rgb_palette[i * 3 + 2] = b;
		}
		/* X11 CFB default colormap: pixel 0 is white, pixel 1 is black */
		d->fb->rgb_palette[0] = 255; d->fb->rgb_palette[1] = 255; d->fb->rgb_palette[2] = 255;
		d->fb->rgb_palette[3] = 0;   d->fb->rgb_palette[4] = 0;   d->fb->rgb_palette[5] = 0;
		d->fb->rgb_palette[255 * 3 + 0] = 255;
		d->fb->rgb_palette[255 * 3 + 1] = 255;
		d->fb->rgb_palette[255 * 3 + 2] = 255;

		if (d->fb->framebuffer != NULL) {
			uint8_t *pal = d->fb->framebuffer + 0xff000;
			for (i = 0; i < 256; i++) {
				pal[i * 4 + 0] = 0;
				pal[i * 4 + 1] = d->fb->rgb_palette[i * 3 + 0];
				pal[i * 4 + 2] = d->fb->rgb_palette[i * 3 + 1];
				pal[i * 4 + 3] = d->fb->rgb_palette[i * 3 + 2];
			}
			/* Initial event pending to display mouse cursor immediately */
			uint8_t *in = d->fb->framebuffer + 0xfe000;
			in[15] = 1;
		}
	}

	machine->display_key_handler = dev_mvme393_display_key;
	machine->display_key_event = dev_mvme393_key_event;

	machine_add_tickfunction(machine, dev_mvme393_tick, d, 14);
}

DEVINIT(mvme393)
{
	dev_mvme393_init(devinit->machine, devinit->machine->md.mvme187);
	return 1;
}
