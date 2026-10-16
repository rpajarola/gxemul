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
 *  COMMENT: Serial controller used in some DECsystem and SGI machines
 *
 *  Hm... Same as Z8530? Most of the code in here is written for DECsystem
 *  emulation, though.
 *
 *  NOTE:
 *	Each scc device is responsible for two lines; the first scc device
 *	controls mouse (0) and keyboard (1), and the second device controls
 *	serial ports (2 and 3).
 *
 *  TODO:
 *	Mouse support!!!  (scc0 and scc1 need to cooperate, in order to
 *		emulate the same lk201 behaviour as when using the dc device)
 *	DMA
 *	More correct interrupt support.
 *
 ******************************************************************************
 *	 _____ ___  ____   ___  _ 
 *	|_   _/ _ \|  _ \ / _ \| |
 *	  | || | | | | | | | | | |
 *	  | || |_| | |_| | |_| |_|
 *	  |_| \___/|____/ \___/(_)
 *
 *	Since this is actually a Z8530, it should be merged with dev_z8530.c!
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "cpu.h"
#include "devices.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"

#include "thirdparty/dc7085.h"
#include "thirdparty/sccreg.h"


#define	SCC_TICK_SHIFT		14

#define	N_SCC_PORTS		2
#define	N_SCC_REGS		16
#define	MAX_QUEUE_LEN		1024

/*  #define SCC_DEBUG  */


struct scc_data {
	struct interrupt	irq;

	int		use_fb;
	int		console_handle;

	int		scc_nr;
	int		addrmul;

	int		register_select_in_progress[N_SCC_PORTS];
	int		register_selected[N_SCC_PORTS];

	unsigned char	scc_register_r[N_SCC_PORTS * N_SCC_REGS];
	unsigned char	scc_register_w[N_SCC_PORTS * N_SCC_REGS];

	unsigned char	rx_queue_char[N_SCC_PORTS * MAX_QUEUE_LEN];
	int		cur_rx_queue_pos_write[N_SCC_PORTS];
	int		cur_rx_queue_pos_read[N_SCC_PORTS];

	struct lk201_data lk201;
};


/*
 *  The DECstation 5000/1xx's serial lines, numbered like the DC7085's (and
 *  as in Ultrix), and the SCC channels they are on.  The keyboard and the
 *  mouse share the LK201 emulation of the SCC with the keyboard.
 */
static const struct {
	int	scc_nr, port;
} scc_line[4] = {
	{ 1, SCC_CHANNEL_A },	/*  0: keyboard  */
	{ 0, SCC_CHANNEL_A },	/*  1: mouse  */
	{ 0, SCC_CHANNEL_B },	/*  2: communication port  */
	{ 1, SCC_CHANNEL_B }	/*  3: printer port (the serial console)  */
};

static struct scc_data *scc_by_nr[2];


static int scc_to_line(int scc_nr, int port)
{
	int line;

	for (line = 0; line < 4; line++)
		if (scc_line[line].scc_nr == scc_nr &&
		    scc_line[line].port == port)
			break;

	return line;
}


/*
 *  Check if space is available in the receive queue.
 */
int dev_scc_space_available_in_queue(void *e, int line)
{
	struct scc_data *d = scc_by_nr[scc_line[line].scc_nr];
	int portnr = scc_line[line].port;
	int entries_in_use;

	if (d == NULL)
		return 0;

	entries_in_use = d->cur_rx_queue_pos_write[portnr] -
	    d->cur_rx_queue_pos_read[portnr];

	while (entries_in_use < 0)
		entries_in_use += MAX_QUEUE_LEN;

	if (entries_in_use < MAX_QUEUE_LEN - 20)
		return 1;

	return 0;
}


/*
 *  rx_enqueue():
 *
 *  Add a character to the receive queue of one of this SCC's channels.
 */
static void rx_enqueue(struct scc_data *d, int portnr, int ch)
{
        d->rx_queue_char[portnr * MAX_QUEUE_LEN +
	    d->cur_rx_queue_pos_write[portnr]] = ch; 
        d->cur_rx_queue_pos_write[portnr] ++;
        if (d->cur_rx_queue_pos_write[portnr] == MAX_QUEUE_LEN)
                d->cur_rx_queue_pos_write[portnr] = 0;

        if (d->cur_rx_queue_pos_write[portnr] ==
	    d->cur_rx_queue_pos_read[portnr])
                fatal("warning: add_to_rx_queue(): rx_queue overrun!\n");
}


/*
 *  dev_scc_add_to_rx_queue():
 *
 *  Add a character to the receive queue of a serial line (see scc_line[]),
 *  which may be on the other SCC.
 */
void dev_scc_add_to_rx_queue(void *e, int ch, int line)
{
	struct scc_data *d = scc_by_nr[scc_line[line].scc_nr];

	if (d != NULL)
		rx_enqueue(d, scc_line[line].port, ch);
}


static int rx_avail(struct scc_data *d, int portnr)
{
	return d->cur_rx_queue_pos_write[portnr] !=
	    d->cur_rx_queue_pos_read[portnr];
}


static unsigned char rx_nextchar(struct scc_data *d, int portnr)
{
	unsigned char ch;
	ch = d->rx_queue_char[portnr * MAX_QUEUE_LEN +
	    d->cur_rx_queue_pos_read[portnr]];
	d->cur_rx_queue_pos_read[portnr]++;
	if (d->cur_rx_queue_pos_read[portnr] == MAX_QUEUE_LEN)
		d->cur_rx_queue_pos_read[portnr] = 0;
	return ch;
}


/*
 *  scc_clear_ip():
 *
 *  Clears interrupt pending bits in RR3, and the interrupt when none are
 *  left.
 */
static void scc_clear_ip(struct scc_data *d, int bits)
{
	d->scc_register_r[N_SCC_REGS + SCC_RR3] &= ~bits;
	if (d->scc_register_r[N_SCC_REGS + SCC_RR3] == 0)
		INTERRUPT_DEASSERT(d->irq);
}


/*
 *  scc_channel_reset():
 *
 *  Resets a channel (from a WR9 channel or hardware reset): interrupts,
 *  the receiver, transmitter, and baud rate generator are disabled, and
 *  no interrupts are pending.
 */
static void scc_channel_reset(struct scc_data *d, int port)
{
	unsigned char *w = &d->scc_register_w[port * N_SCC_REGS];

	w[SCC_WR1] = w[SCC_WR3] = w[SCC_WR5] = w[SCC_WR14] = 0;
	w[SCC_WR15] = 0xf8;

	d->scc_register_r[port * N_SCC_REGS + SCC_RR0] =
	    SCC_RR0_TX_EMPTY | SCC_RR0_TX_UNDERRUN;
	scc_clear_ip(d, port == SCC_CHANNEL_A?
	    SCC_RR3_EXT_IP_A | SCC_RR3_TX_IP_A | SCC_RR3_RX_IP_A :
	    SCC_RR3_EXT_IP_B | SCC_RR3_TX_IP_B | SCC_RR3_RX_IP_B);
}


/*
 *  scc_wr0_command():
 *
 *  The commands in a write to WR0.  (The 5000/1xx PROM's self-test uses
 *  Send Abort, which ends the message, to get a Tx Underrun/EOM
 *  external/status interrupt.)
 */
static void scc_wr0_command(struct scc_data *d, int port, int wr0)
{
	int ext_ip = port == SCC_CHANNEL_A? SCC_RR3_EXT_IP_A : SCC_RR3_EXT_IP_B;

	switch ((wr0 >> 3) & 7) {
	case 2:		/*  Reset external/status interrupts  */
		scc_clear_ip(d, ext_ip);
		break;
	case 3:		/*  Send abort  */
		d->scc_register_r[port * N_SCC_REGS + SCC_RR0] |=
		    SCC_RR0_TX_UNDERRUN;
		if (d->scc_register_w[port * N_SCC_REGS + SCC_WR15] &
		    SCC_WR15_TX_UNDERRUN_IE &&
		    d->scc_register_w[port * N_SCC_REGS + SCC_WR1] &
		    SCC_WR1_EXT_IE) {
			d->scc_register_r[N_SCC_REGS + SCC_RR3] |= ext_ip;
			if (d->scc_register_w[N_SCC_REGS + SCC_WR9] &
			    SCC_WR9_MASTER_IE)
				INTERRUPT_ASSERT(d->irq);
		}
		break;
	case 5:		/*  Reset Tx interrupt pending  */
		scc_clear_ip(d, port == SCC_CHANNEL_A? SCC_RR3_TX_IP_A :
		    SCC_RR3_TX_IP_B);
		break;
	}

	/*  Reset Tx underrun/EOM latch (until the next character has been
	    sent; see the data register write):  */
	if ((wr0 & 0xc0) == 0xc0)
		d->scc_register_r[port * N_SCC_REGS + SCC_RR0] &=
		    ~SCC_RR0_TX_UNDERRUN;
}


DEVICE_TICK(scc)
{
	struct scc_data *d = (struct scc_data *) extra;
	int i, rxi;

	/*  Add keystrokes to the rx queue:  */
	if (d->scc_nr == 1) {
		if (d->use_fb == 0) {
			if (console_charavail(d->console_handle))
				dev_scc_add_to_rx_queue(extra, console_readchar(
				    d->console_handle), DCPRINTER_PORT);
		} else if (d->use_fb == 1)
			lk201_tick(cpu->machine, &d->lk201);
	}

	for (i=0; i<N_SCC_PORTS; i++) {
		d->scc_register_r[i * N_SCC_REGS + SCC_RR0] |= SCC_RR0_TX_EMPTY;
		d->scc_register_r[i * N_SCC_REGS + SCC_RR1] = 0;
		    /*  No receive errors  */

		d->scc_register_r[i * N_SCC_REGS + SCC_RR0] &=
		    ~SCC_RR0_RX_AVAIL;
		if (rx_avail(d, i))
			d->scc_register_r[i * N_SCC_REGS + SCC_RR0] |=
			    SCC_RR0_RX_AVAIL;

		/*
		 *  A running baud rate generator counts down to zero
		 *  many times between ticks, which gives an external/
		 *  status interrupt if Zero Count interrupts are enabled.
		 *  (The 5000/1xx PROM's self-test checks for that.)
		 */
		if (d->scc_register_w[i * N_SCC_REGS + SCC_WR14] &
		    SCC_WR14_BAUDR_ENABLE &&
		    d->scc_register_w[i * N_SCC_REGS + SCC_WR15] &
		    SCC_WR15_ZERO_COUNT_IE &&
		    d->scc_register_w[i * N_SCC_REGS + SCC_WR1] &
		    SCC_WR1_EXT_IE) {
			d->scc_register_r[N_SCC_REGS + SCC_RR3] |=
			    i == SCC_CHANNEL_A? SCC_RR3_EXT_IP_A :
			    SCC_RR3_EXT_IP_B;
			if (d->scc_register_w[N_SCC_REGS + SCC_WR9] &
			    SCC_WR9_MASTER_IE)
				INTERRUPT_ASSERT(d->irq);
		}

		/*
		 *  Interrupts:
		 *  (NOTE: Interrupt enables are always at channel A)
		 */
		if (d->scc_register_w[N_SCC_REGS + SCC_WR9] &
		    SCC_WR9_MASTER_IE) {
			/*  TX interrupts?  */
			if (d->scc_register_w[i * N_SCC_REGS + SCC_WR1] &
			    SCC_WR1_TX_IE) {
				if (d->scc_register_r[i * N_SCC_REGS + SCC_RR3]
				    & SCC_RR3_TX_IP_A ||
				    d->scc_register_r[i * N_SCC_REGS + SCC_RR3]
				    & SCC_RR3_TX_IP_B) {
					INTERRUPT_ASSERT(d->irq);
				}
			}

			/*  RX interrupts? (Not in "special condition only"
			    mode, which DMA receivers use.)  */
			rxi = d->scc_register_w[i * N_SCC_REGS + SCC_WR1] &
			    SCC_WR1_RXI_SPECIAL_O;
			if (rxi == SCC_WR1_RXI_FIRST_CHAR ||
			    rxi == SCC_WR1_RXI_ALL_CHAR) {
				if (d->scc_register_r[i * N_SCC_REGS + SCC_RR0]
				    & SCC_RR0_RX_AVAIL) {
					if (i == SCC_CHANNEL_A)
						d->scc_register_r[N_SCC_REGS +
						    SCC_RR3] |= SCC_RR3_RX_IP_A;
					else
						d->scc_register_r[N_SCC_REGS +
						    SCC_RR3] |= SCC_RR3_RX_IP_B;
				}

				if (d->scc_register_r[N_SCC_REGS + SCC_RR3] &
				    (i == SCC_CHANNEL_A? SCC_RR3_RX_IP_A :
				    SCC_RR3_RX_IP_B))
					INTERRUPT_ASSERT(d->irq);
			}
		}
	}
}


/*
 *  scc_tx():
 *
 *  Sends a character, which is sent at once, so the transmitter then
 *  underruns (see scc_wr0_command()).
 */
static void scc_tx(struct scc_data *d, int port, int ch)
{
	int line = scc_to_line(d->scc_nr, port);
	struct scc_data *k = d;

	/*  The keyboard and mouse use SCC 1's LK201 emulation:  */
	if ((line == DCKBD_PORT || line == DCMOUSE_PORT) &&
	    scc_by_nr[1] != NULL)
		k = scc_by_nr[1];

	lk201_tx_data(&k->lk201, line, ch);

	d->scc_register_r[port * N_SCC_REGS + SCC_RR0] |= SCC_RR0_TX_UNDERRUN;

	/*  Loopback:  */
	if (d->scc_register_w[port * N_SCC_REGS + SCC_WR14] &
	    SCC_WR14_LOCAL_LOOPB)
		rx_enqueue(d, port, ch);
}


/*
 *  dev_scc_dma_tx(), dev_scc_dma_rx():
 *
 *  Transmit a character, or take the next received character (-1 if there
 *  is none), for an IOASIC DMA channel. The IOASIC's SCC DMA channels are
 *  connected to channel B of each SCC (the communication port of SCC 0,
 *  and the printer/console port of SCC 1 on the 5000/1xx).
 */
void dev_scc_dma_tx(void *e, int ch)
{
	scc_tx((struct scc_data *) e, SCC_CHANNEL_B, ch);
}

int dev_scc_dma_rx(void *e)
{
	struct scc_data *d = (struct scc_data *) e;
	int ch;

	if (!rx_avail(d, SCC_CHANNEL_B))
		return -1;

	ch = rx_nextchar(d, SCC_CHANNEL_B);
	if (!rx_avail(d, SCC_CHANNEL_B))
		scc_clear_ip(d, SCC_RR3_RX_IP_B);

	return ch;
}


DEVICE_ACCESS(scc)
{
	struct scc_data *d = (struct scc_data *) extra;
	uint64_t idata = 0, odata = 0;
	int port;
	int ultrix_mode = 0;

	if (writeflag == MEM_WRITE)
		idata = memory_readmax64(cpu, data, len);

	/*  relative_addr /= d->addrmul;  */
		/*  See SGI comment below instead.  */
	/*
	 *  SGI writes command to 0x0f, and data to 0x1f.
	 *  (TODO: This works for port nr 0, how about port nr 1?)
	 */
	if ((relative_addr & 0x0f) == 0xf) {
		if (relative_addr == 0x0f)
			relative_addr = 1;
		else
			relative_addr = 5;
	}

	port = relative_addr / 8;
	relative_addr &= 7;

	dev_scc_tick(cpu, extra);

	/*
	 *  Ultrix writes words such as 0x1200 to relative address 0,
	 *  instead of writing the byte 0x12 directly to address 1.
	 */
	if ((relative_addr == 0 || relative_addr == 4) && (idata & 0xff) == 0) {
		ultrix_mode = 1;
		relative_addr ++;
		idata >>= 8;
	}

	switch (relative_addr) {
	case 1:		/*  command  */
		if (writeflag==MEM_READ) {
			odata = d->scc_register_r[port * N_SCC_REGS +
			    d->register_selected[port]];

			/*  Reading RR3 doesn't clear the interrupts; they
			    are cleared by reading the received character,
			    and by the WR0 commands (scc_wr0_command()).  */
			if (d->register_selected[port] == SCC_RR3 &&
			    port == SCC_CHANNEL_B)
				fatal("WARNING! scc channel B has no RR3\n");

#ifdef SCC_DEBUG
			fatal("[ scc: port %i, register %i, read value "
			    "0x%02x ]\n", port, d->register_selected[port],
			    (int)odata);
#endif
			d->register_select_in_progress[port] = 0;
			d->register_selected[port] = 0;
			/*  debug("[ scc: (port %i) read from 0x%08lx ]\n",
			    port, (long)relative_addr);  */
		} else {
			/*
			 *  If no register is selected, then this is a write
			 *  to WR0, whose low 3 bits select the register for
			 *  the next access (plus 8 with the "point high"
			 *  command), and whose other bits are commands.  WR0
			 *  commands with register 0 select nothing; e.g.
			 *  0x10 (reset ext/status) or 0xc0 (reset TX
			 *  underrun) are followed by another WR0 write.
			 *  Otherwise, write to the selected register.
			 */
			if (d->register_select_in_progress[port] == 0) {
				int reg = idata & 7;

				if ((idata & 0x38) == 0x08)
					reg += 8;
				d->scc_register_w[port * N_SCC_REGS + SCC_WR0] =
				    idata;
				d->register_selected[port] = reg;
				d->register_select_in_progress[port] = reg != 0;
				scc_wr0_command(d, port, idata);
			} else {
				d->scc_register_w[port * N_SCC_REGS +
				    d->register_selected[port]] = idata;

				/*  WR9 resets (the 5000/1xx PROM's self-test
				    expects them to leave no interrupts):  */
				if (d->register_selected[port] == SCC_WR9) {
					if (idata & 0x80)
						scc_channel_reset(d,
						    SCC_CHANNEL_A);
					if (idata & 0x40)
						scc_channel_reset(d,
						    SCC_CHANNEL_B);
				}

				/*  Disabling an interrupt source in WR1 also
				    clears its interrupt pending bit:  */
				if (d->register_selected[port] == SCC_WR1) {
					int a = port == SCC_CHANNEL_A, ip = 0;
					if (!(idata & SCC_WR1_EXT_IE))
						ip |= a? SCC_RR3_EXT_IP_A :
						    SCC_RR3_EXT_IP_B;
					if (!(idata & SCC_WR1_TX_IE))
						ip |= a? SCC_RR3_TX_IP_A :
						    SCC_RR3_TX_IP_B;
					if (!(idata & (SCC_WR1_RXI_FIRST_CHAR |
					    SCC_WR1_RXI_ALL_CHAR)))
						ip |= a? SCC_RR3_RX_IP_A :
						    SCC_RR3_RX_IP_B;
					scc_clear_ip(d, ip);
				}
#ifdef SCC_DEBUG
				fatal("[ scc: port %i, register %i, write "
				    "value 0x%02x ]\n", port,
				    d->register_selected[port], idata);
#endif

				d->scc_register_r[port * N_SCC_REGS +
				    SCC_RR12] = d->scc_register_w[port *
				    N_SCC_REGS + SCC_WR12];
				d->scc_register_r[port * N_SCC_REGS +
				    SCC_RR13] = d->scc_register_w[port *
				    N_SCC_REGS + SCC_WR13];

				d->register_select_in_progress[port] = 0;
				d->register_selected[port] = 0;
			}
		}
		break;
	case 5:		/*  data  */
		if (writeflag==MEM_READ) {
			if (rx_avail(d, port))
				odata = rx_nextchar(d, port);

			/*  The receive interrupt is pending until all the
			    received characters have been read:  */
			if (!rx_avail(d, port))
				scc_clear_ip(d, port == SCC_CHANNEL_A?
				    SCC_RR3_RX_IP_A : SCC_RR3_RX_IP_B);

//			debug("[ scc: (port %i) read from 0x%08lx: 0x%02x ]\n",
//			    port, (long)relative_addr, (int)odata);
		} else {
			/*  debug("[ scc: (port %i) write to  0x%08lx: "
			    "0x%08x ]\n", port, (long)relative_addr,
			    (int)idata);  */

			scc_tx(d, port, idata);

			/*  TX interrupt:  */
			if (d->scc_register_w[port * N_SCC_REGS + SCC_WR9] &
			    SCC_WR9_MASTER_IE &&
			    d->scc_register_w[port * N_SCC_REGS + SCC_WR1] &
			    SCC_WR1_TX_IE) {
				if (port == SCC_CHANNEL_A)
					d->scc_register_r[N_SCC_REGS + SCC_RR3]
					    |= SCC_RR3_TX_IP_A;
				else
					d->scc_register_r[N_SCC_REGS + SCC_RR3]
					    |= SCC_RR3_TX_IP_B;
			}

			dev_scc_tick(cpu, extra);
		}
		break;
	default:
		if (writeflag==MEM_READ) {
			debug("[ scc: (port %i) read from 0x%08lx ]\n",
			    port, (long)relative_addr);
		} else {
			debug("[ scc: (port %i) write to  0x%08lx: 0x%08x ]\n",
			    port, (long)relative_addr, (int)idata);
		}
	}

	if (ultrix_mode && writeflag == MEM_READ) {
		odata <<= 8;
	}

	if (writeflag == MEM_READ)
		memory_writemax64(cpu, data, len, odata);

	return 1;
}


/*
 *  dev_scc_init():
 *
 *	use_fb = non-zero when using graphical console + keyboard
 *	scc_nr = 0 or 1
 *	addmul = 1 in most cases, 8 on SGI?
 */
void *dev_scc_init(struct machine *machine, struct memory *mem,
	uint64_t baseaddr, char* irq_path, int use_fb, int scc_nr, int addrmul)
{
	struct scc_data *d;

	CHECK_ALLOCATION(d = (struct scc_data *) malloc(sizeof(struct scc_data)));
	memset(d, 0, sizeof(struct scc_data));

	INTERRUPT_CONNECT(irq_path, d->irq);
	d->scc_nr  = scc_nr;
	d->use_fb  = use_fb;
	d->addrmul = addrmul;
	/*  Only SCC 1 reads keystrokes from its console (see the tick
	    function), so SCC 0's console is output only, and does not
	    need an xterm of its own:  */
	d->console_handle = console_start_slave(machine, "SCC",
	    scc_nr == 1? 1 : CONSOLE_OUTPUT_ONLY);

	/*  ... and it is the main console, which gets the keystrokes
	    typed into the framebuffer window:  */
	if (scc_nr == 1)
		machine->main_console_handle = d->console_handle;

	d->scc_register_r[SCC_RR0] |= SCC_RR0_TX_UNDERRUN;

	lk201_init(&d->lk201, use_fb,
	    dev_scc_space_available_in_queue,
	    dev_scc_add_to_rx_queue,
	    d->console_handle, d);

	memory_device_register(mem, "scc", baseaddr, DEV_SCC_LENGTH,
	    dev_scc_access, d, DM_DEFAULT, NULL);
	machine_add_tickfunction(machine, dev_scc_tick, d, SCC_TICK_SHIFT);

	if (scc_nr >= 0 && scc_nr < 2)
		scc_by_nr[scc_nr] = d;

	return (void *) d;
}

