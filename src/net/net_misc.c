/*
 *  Copyright (C) 2004-2018  Anders Gavare.  All rights reserved.
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
 *  Misc. helper functions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "machine.h"
#include "misc.h"
#include "net.h"


/*
 *  net_debugaddr():
 *
 *  Print an address using debug().
 */
void net_debugaddr(void *addr, int type)
{
	int i;
	unsigned char *p = (unsigned char *) addr;

	switch (type) {

	case NET_ADDR_IPV4:
		for (i=0; i<4; i++)
			debug("%s%i", i? "." : "", p[i]);
		break;

	case NET_ADDR_IPV6:
		for (i=0; i<16; i+=2)
			debug("%s%4x", i? ":" : "", p[i] * 256 + p[i+1]);
		break;

	case NET_ADDR_ETHERNET:
		for (i=0; i<6; i++)
			debug("%s%02x", i? ":" : "", p[i]);
		break;

	default:
		fatal("net_debugaddr(): UNIMPLEMTED type %i\n", type);
		exit(1);
	}
}


/*
 *  net_generate_unique_mac():
 *
 *  Generate a MAC address for a machine, starting with DEC's 08:00:2b OUI
 *  followed by 3 random bytes.  (The first NIC's address can be given in a
 *  machine.json, "mac" under "network".)
 *
 *  The return value (6 bytes) are written to macbuf.
 */
void net_generate_unique_mac(struct machine *machine, unsigned char *macbuf)
{
	if (macbuf == NULL || machine == NULL) {
		fatal("**\n**  net_generate_unique_mac(): NULL ptr\n**\n");
		return;
	}

	/*  The first NIC's address may be given in the configuration:  */
	if (machine->nic_mac_set && machine->nr_of_nics == 0) {
		memcpy(macbuf, machine->nic_mac, 6);
		machine->nr_of_nics ++;
		return;
	}

	macbuf[0] = 0x08;
	macbuf[1] = 0x00;
	macbuf[2] = 0x2b;
	macbuf[3] = random() & 0xff;
	macbuf[4] = random() & 0xff;
	macbuf[5] = random() & 0xff;

	machine->nr_of_nics ++;
}


/*
 *  send_udp():
 *
 *  Send a simple UDP packet to a real (physical) host. Used for distributed
 *  network simulations.
 */
void send_udp(struct in_addr *addrp, int portnr, unsigned char *packet, size_t len)
{
	int s;
	struct sockaddr_in si;

	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) {
		perror("send_udp(): socket");
		return;
	}

	/*  fatal("send_udp(): sending to port %i\n", portnr);  */

	si.sin_family = AF_INET;
	si.sin_addr = *addrp;
	si.sin_port = htons(portnr);

	if (sendto(s, (const char *) packet, len, 0, (struct sockaddr *)&si,
	    sizeof(si)) != (ssize_t)len) {
		perror("send_udp(): sendto");
	}

	socket_close(s);
}

