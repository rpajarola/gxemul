/*
 *  Copyright (C) 2026  Rico Pajarola.  All rights reserved.
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
 */

/*
 *  Framing for the distributed network: raw Ethernet frames over UDP, one
 *  frame per datagram (the HECnet bridge format, which simh, QEMU's
 *  "-netdev dgram" and gobootd's bootbridge also use), or static L2TPv3
 *  over UDP (RFC 3931, carrying Ethernet as in RFC 4719) as Linux's
 *  "ip l2tp", QEMU's "-netdev l2tpv3" and bootbridge's -l2tp speak it.
 *
 *  L2TPv3 is used without its control connection: both ends are set up
 *  with the same session ID and cookie, and datagrams with any other
 *  session or cookie are dropped. Each datagram is
 *
 *	flags and version (4) | session ID (4) | cookie (0, 4 or 8) |
 *	[L2-specific sublayer (4)] | Ethernet frame
 *
 *  The cookie works as a password against anyone who cannot see the
 *  traffic. It is sent in clear.
 *
 *  Keepalives (an Ethernet loopback frame addressed to its sender) are
 *  sent to the remotes every minute, so a bridge that learns its peers
 *  keeps sending us broadcasts even while the emulated machine is quiet.
 *  Keepalives received are dropped.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "misc.h"
#include "net.h"

#define	L2TP_DATA		0x00030000	/*  T bit clear, version 3  */
#define	ETHERTYPE_LOOPBACK	0x9000
#define	KEEPALIVE_INTERVAL	60
#define	FRAME_MIN		60


static void put32(unsigned char *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}


static uint32_t get32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | p[3];
}


/*
 *  net_l2tp_init():
 *
 *  Switch the distributed network to L2TPv3. cookie is 8 or 16 hex digits
 *  (optionally with 0x), or empty for none. Returns false if the session
 *  or cookie is invalid.
 */
bool net_l2tp_init(struct net *net, uint32_t session, const char *cookie,
	bool counter)
{
	size_t i, n;

	if (session == 0) {
		fprintf(stderr, "l2tp_session must not be 0\n");
		return false;
	}
	if (cookie[0] == '0' && (cookie[1] == 'x' || cookie[1] == 'X'))
		cookie += 2;
	n = strlen(cookie);
	if (n != 0 && n != 8 && n != 16) {
		fprintf(stderr, "l2tp_cookie must be 8 or 16 hex digits\n");
		return false;
	}
	for (i = 0; i < n; i += 2) {
		unsigned int b;
		if (!isxdigit((unsigned char)cookie[i]) ||
		    !isxdigit((unsigned char)cookie[i+1]) ||
		    sscanf(cookie + i, "%2x", &b) != 1) {
			fprintf(stderr, "l2tp_cookie must be 8 or 16 hex "
			    "digits\n");
			return false;
		}
		net->l2tp_cookie[i / 2] = b;
	}

	net->l2tp = true;
	net->l2tp_session = session;
	net->l2tp_cookie_len = n / 2;
	net->l2tp_counter = counter;
	return true;
}


static size_t l2tp_header_len(struct net *net)
{
	return 8 + net->l2tp_cookie_len + (net->l2tp_counter ? 4 : 0);
}


/*
 *  net_distributed_send():
 *
 *  Send a frame to a remote, from our local port so that peers which
 *  learn from source addresses can answer.
 */
void net_distributed_send(struct net *net, struct remote_net *rnp,
	const unsigned char *packet, int len)
{
	unsigned char buf[24 + 1600];
	size_t hlen = 0;
	struct sockaddr_in si;

	if (len < 14 || len > 1514)
		return;

	if (net->l2tp) {
		put32(buf, L2TP_DATA);
		put32(buf + 4, net->l2tp_session);
		memcpy(buf + 8, net->l2tp_cookie, net->l2tp_cookie_len);
		if (net->l2tp_counter)
			put32(buf + 8 + net->l2tp_cookie_len, 0x40000000 |
			    (++net->l2tp_seq & 0xffffff));
		hlen = l2tp_header_len(net);
	}
	memcpy(buf + hlen, packet, len);
	if (len < FRAME_MIN) {
		memset(buf + hlen + len, 0, FRAME_MIN - len);
		len = FRAME_MIN;
	}

	if (net->local_port == 0) {
		/*  No local port: frames come from an ephemeral one.  */
		send_udp(&rnp->ipv4_addr, rnp->portnr, buf, hlen + len);
		return;
	}

	memset(&si, 0, sizeof(si));
	si.sin_family = AF_INET;
	si.sin_addr = rnp->ipv4_addr;
	si.sin_port = htons(rnp->portnr);
	if (sendto(net->local_port_socket, buf, hlen + len, 0,
	    (struct sockaddr *)&si, sizeof(si)) < 0)
		debugmsg(SUBSYS_NET, "distributed", VERBOSITY_DEBUG,
		    "send to %s failed", rnp->name);
}


/*
 *  net_distributed_decode():
 *
 *  Check a received datagram. Returns the Ethernet frame in it and sets
 *  *lenp to its length, or returns NULL if the datagram is to be dropped:
 *  not for our L2TPv3 session, not a frame, or a keepalive.
 */
unsigned char *net_distributed_decode(struct net *net, unsigned char *buf,
	int *lenp)
{
	unsigned char *frame = buf;
	int len = *lenp;

	if (net->l2tp) {
		size_t hlen = l2tp_header_len(net);

		if (len < (int)hlen ||
		    (get32(buf) & 0x800f0000) != L2TP_DATA ||
		    get32(buf + 4) != net->l2tp_session ||
		    memcmp(buf + 8, net->l2tp_cookie,
		    net->l2tp_cookie_len) != 0) {
			if (!net->l2tp_warned) {
				debugmsg(SUBSYS_NET, "l2tp", VERBOSITY_WARNING,
				    "dropping datagrams with the wrong "
				    "L2TPv3 session or cookie");
				net->l2tp_warned = true;
			}
			return NULL;
		}
		frame += hlen;
		len -= hlen;
	}

	if (len < 14 || len > 1514)
		return NULL;

	/*  A keepalive: a loopback frame addressed to its sender.  */
	if (((frame[12] << 8) | frame[13]) == ETHERTYPE_LOOPBACK &&
	    memcmp(frame, frame + 6, 6) == 0)
		return NULL;

	*lenp = len;
	return frame;
}


/*
 *  net_distributed_keepalive():
 *
 *  Send a keepalive to every remote if it is time to.
 */
void net_distributed_keepalive(struct net *net)
{
	unsigned char frame[FRAME_MIN];
	struct remote_net *rnp;
	time_t now;

	if (net->remote_nets == NULL || net->n_nics == 0)
		return;
	now = time(NULL);
	if (now < net->next_keepalive)
		return;
	net->next_keepalive = now + KEEPALIVE_INTERVAL;

	/*  Loopback reply from the first NIC to itself: skip count 0,
	    function 1 (reply), receipt number 0.  */
	memset(frame, 0, sizeof(frame));
	memcpy(frame, net->nic_data[0]->mac_address, 6);
	memcpy(frame + 6, net->nic_data[0]->mac_address, 6);
	frame[12] = ETHERTYPE_LOOPBACK >> 8;
	frame[13] = ETHERTYPE_LOOPBACK & 0xff;
	frame[16] = 1;

	for (rnp = net->remote_nets; rnp != NULL; rnp = rnp->next)
		net_distributed_send(net, rnp, frame, sizeof(frame));
}
