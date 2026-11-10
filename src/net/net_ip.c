/*
 *  Copyright (C) 2004-2021  Anders Gavare.  All rights reserved.
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
 *  Internet Protocol related networking stuff.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <errno.h>

#include "misc.h"
#include "net.h"


/*  #define debug fatal  */
static int net_ip_debug = 0;	// replace by debugmsg(....);



/*
 *  net_ip_checksum():
 *
 *  Fill in an IP header checksum. (This works for ICMP too.)
 *  chksumoffset should be 10 for IP headers, and len = 20.
 *  For ICMP packets, chksumoffset = 2 and len = length of the ICMP packet.
 */
void net_ip_checksum(unsigned char *ip_header, int chksumoffset, int len)
{
	int i;
	uint32_t sum = 0;

	for (i=0; i<len; i+=2)
		if (i != chksumoffset) {
			uint16_t w = (ip_header[i] << 8) + ip_header[i+1];
			sum += w;
			while (sum > 65535) {
				int to_add = sum >> 16;
				sum = (sum & 0xffff) + to_add;
			}
		}

	sum ^= 0xffff;
	ip_header[chksumoffset + 0] = sum >> 8;
	ip_header[chksumoffset + 1] = sum & 0xff;
}


/*
 *  net_ip_tcp_checksum():
 *
 *  Fill in a TCP header checksum. This differs slightly from the IP
 *  checksum. The checksum is calculated on a pseudo header, the actual
 *  TCP header, and the data.  This is what the pseudo header looks like:
 *
 *	uint32_t srcaddr;
 *	uint32_t dstaddr;
 *	uint16_t protocol; (= 6 for tcp)
 *	uint16_t tcp_len;
 *
 *  tcp_len is length of header PLUS data.  The psedo header is created
 *  internally here, and does not need to be supplied by the caller.
 */
void net_ip_tcp_checksum(unsigned char *tcp_header, int chksumoffset,
	int tcp_len, unsigned char *srcaddr, unsigned char *dstaddr,
	int udpflag)
{
	int i, pad = 0;
	unsigned char pseudoh[12];
	uint32_t sum = 0;

	memcpy(pseudoh + 0, srcaddr, 4);
	memcpy(pseudoh + 4, dstaddr, 4);
	pseudoh[8] = 0x00;
	pseudoh[9] = udpflag? 17 : 6;
	pseudoh[10] = tcp_len >> 8;
	pseudoh[11] = tcp_len & 255;

	for (i=0; i<12; i+=2) {
		uint16_t w = (pseudoh[i] << 8) + pseudoh[i+1];
		sum += w;
		while (sum > 65535) {
			int to_add = sum >> 16;
			sum = (sum & 0xffff) + to_add;
		}
	}

	if (tcp_len & 1) {
		tcp_len ++;
		pad = 1;
	}

	for (i=0; i<tcp_len; i+=2)
		if (i != chksumoffset) {
			uint16_t w;
			if (!pad || i < tcp_len-2)
				w = (tcp_header[i] << 8) + tcp_header[i+1];
			else
				w = (tcp_header[i] << 8) + 0x00;
			sum += w;
			while (sum > 65535) {
				int to_add = sum >> 16;
				sum = (sum & 0xffff) + to_add;
			}
		}

	sum ^= 0xffff;
	tcp_header[chksumoffset + 0] = sum >> 8;
	tcp_header[chksumoffset + 1] = sum & 0xff;
}


/*
 *  net_ip_icmp():
 *
 *  Handle an ICMP packet.
 *
 *  The IP header (at offset 14) could look something like
 *
 *	ver=45 tos=00 len=0054 id=001a ofs=0000 ttl=ff p=01 sum=a87e
 *	src=0a000005 dst=03050607
 *
 *  and the ICMP specific data (beginning at offset 34):
 *
 *	type=08 code=00 chksum=b8bf
 *	000c0008d5cee94089190c0008090a0b
 *	0c0d0e0f101112131415161718191a1b
 *	1c1d1e1f202122232425262728292a2b
 *	2c2d2e2f3031323334353637
 */
static void net_ip_icmp(struct net *net, struct nic_data *nic,
	unsigned char *packet, int len)
{
	int type;
	struct ethernet_packet_link *lp;

	type = packet[34];

	switch (type) {
	case 8:	/*  ECHO request  */
		debugmsg(SUBSYS_NET, "ICMP", VERBOSITY_DEBUG, "ECHO request");

		lp = net_allocate_ethernet_packet_link(net, nic, len);

		/*  Copy the old packet first:  */
		memcpy(lp->data + 12, packet + 12, len - 12);

		/*  Switch to and from ethernet addresses:  */
		memcpy(lp->data + 0, packet + 6, 6);
		memcpy(lp->data + 6, packet + 0, 6);

		/*  Switch to and from IP addresses:  */
		memcpy(lp->data + 26, packet + 30, 4);
		memcpy(lp->data + 30, packet + 26, 4);

		/*  Change from echo REQUEST to echo REPLY:  */
		lp->data[34] = 0x00;

		/*  Decrease the TTL to a low value:  */
		lp->data[22] = 2;

		/*  Recalculate ICMP checksum:  */
		net_ip_checksum(lp->data + 34, 2, len - 34);

		/*  Recalculate IP header checksum:  */
		net_ip_checksum(lp->data + 14, 10, 20);

		break;
	default:
		debugmsg(SUBSYS_NET, "ICMP", VERBOSITY_WARNING, "type %i not yet implemented", type);
	}
}


/*  The host's time in milliseconds, for TCP retransmissions:  */
static int64_t tcp_now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (int64_t) tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/*  How long the guest has to acknowledge data before it is sent again:  */
#define	TCP_RETRANSMIT_MS	300

/*  At most this many segments go to the guest per connection and call:  */
#define	TCP_SEGMENTS_PER_ROUND	16

/*  The size of the data in the segments we send to the guest:  */
#define	TCP_SEGMENT_SIZE	1400


/*
 *  tcp_closeconnection():
 *
 *  Helper function which closes down a TCP connection completely.
 */
static void tcp_closeconnection(struct net *net, int con_id)
{
	socket_close(net->tcp_connections[con_id].socket);
	net->tcp_connections[con_id].state = TCP_OUTSIDE_DISCONNECTED;
	net->tcp_connections[con_id].in_use = 0;
	net->tcp_connections[con_id].incoming_buf_len = 0;
	free(net->tcp_connections[con_id].incoming_buf);
	net->tcp_connections[con_id].incoming_buf = NULL;
}


/*
 *  net_ip_tcp_connectionreply():
 *
 *  When changing from state _TRYINGTOCONNECT to _CONNECTED, then this
 *  function should be called with connecting set to 1.
 *
 *  To send a generic ack reply, set connecting to 0.
 *
 *  To send data (PSH), set data to non-NULL and datalen to the length.
 *
 *  This creates an ethernet packet for the guest OS with an ACK to the
 *  initial SYN packet.
 */
void net_ip_tcp_connectionreply(struct net *net, struct nic_data *nic,
	int con_id, int connecting, unsigned char *data, int datalen, int rst)
{
	struct ethernet_packet_link *lp;
	int tcp_length, ip_len, option_len = 20;

	if (connecting)
		net->tcp_connections[con_id].outside_acknr =
		    net->tcp_connections[con_id].inside_seqnr + 1;

	net->tcp_connections[con_id].tcp_id ++;
	tcp_length = 20 + option_len + datalen;
	ip_len = 20 + tcp_length;
	lp = net_allocate_ethernet_packet_link(net, nic, 14 + ip_len);

	/*  Ethernet header:  */
	memcpy(lp->data + 0, net->tcp_connections[con_id].ethernet_address, 6);
	memcpy(lp->data + 6, net->gateway_ethernet_addr, 6);
	lp->data[12] = 0x08;	/*  IP = 0x0800  */
	lp->data[13] = 0x00;

	/*  IP header:  */
	lp->data[14] = 0x45;	/*  ver  */
	lp->data[15] = 0x10;	/*  tos  */
	lp->data[16] = ip_len >> 8;
	lp->data[17] = ip_len & 0xff;
	lp->data[18] = net->tcp_connections[con_id].tcp_id >> 8;
	lp->data[19] = net->tcp_connections[con_id].tcp_id & 0xff;
	lp->data[20] = 0x40;	/*  don't fragment  */
	lp->data[21] = 0x00;
	lp->data[22] = 0x40;	/*  ttl  */
	lp->data[23] = 6;	/*  p = TCP  */
	memcpy(lp->data + 26, net->tcp_connections[con_id].
	    outside_ip_address, 4);
	memcpy(lp->data + 30, net->tcp_connections[con_id].
	    inside_ip_address, 4);
	net_ip_checksum(lp->data + 14, 10, 20);

	/*  TCP header and options at offset 34:  */
	lp->data[34] = net->tcp_connections[con_id].outside_tcp_port >> 8;
	lp->data[35] = net->tcp_connections[con_id].outside_tcp_port & 0xff;
	lp->data[36] = net->tcp_connections[con_id].inside_tcp_port >> 8;
	lp->data[37] = net->tcp_connections[con_id].inside_tcp_port & 0xff;
	lp->data[38] = (net->tcp_connections[con_id].
	    outside_seqnr >> 24) & 0xff;
	lp->data[39] = (net->tcp_connections[con_id].
	    outside_seqnr >> 16) & 0xff;
	lp->data[40] = (net->tcp_connections[con_id].
	    outside_seqnr >>  8) & 0xff;
	lp->data[41] = net->tcp_connections[con_id].
	    outside_seqnr & 0xff;
	lp->data[42] = (net->tcp_connections[con_id].
	    outside_acknr >> 24) & 0xff;
	lp->data[43] = (net->tcp_connections[con_id].
	    outside_acknr >> 16) & 0xff;
	lp->data[44] = (net->tcp_connections[con_id].
	    outside_acknr >>  8) & 0xff;
	lp->data[45] = net->tcp_connections[con_id].outside_acknr & 0xff;

	/*  Control  */
	lp->data[46] = (option_len + 20) / 4 * 0x10;
	lp->data[47] = 0x10;	/*  ACK  */
	if (connecting)
		lp->data[47] |= 0x02;	/*  SYN  */
	if (net->tcp_connections[con_id].state == TCP_OUTSIDE_CONNECTED)
		lp->data[47] |= 0x08;	/*  PSH  */
	if (rst)
		lp->data[47] |= 0x04;	/*  RST  */
	if (net->tcp_connections[con_id].state >= TCP_OUTSIDE_DISCONNECTED)
		lp->data[47] |= 0x01;	/*  FIN  */

	/*  Window  */
	lp->data[48] = 0x10;
	lp->data[49] = 0x00;

	/*  no urgent ptr  */

	/*  options  */
	/*  TODO:  HAHA, this is ugly  */
	lp->data[54] = 0x02;
	lp->data[55] = 0x04;
	lp->data[56] = 0x05;
	lp->data[57] = 0xb4;
	lp->data[58] = 0x01;
	lp->data[59] = 0x03;
	lp->data[60] = 0x03;
	lp->data[61] = 0x00;
	lp->data[62] = 0x01;
	lp->data[63] = 0x01;
	lp->data[64] = 0x08;
	lp->data[65] = 0x0a;
	lp->data[66] = (net->timestamp >> 24) & 0xff;
	lp->data[67] = (net->timestamp >> 16) & 0xff;
	lp->data[68] = (net->timestamp >> 8) & 0xff;
	lp->data[69] = net->timestamp & 0xff;
	lp->data[70] = (net->tcp_connections[con_id].
	    inside_timestamp >> 24) & 0xff;
	lp->data[71] = (net->tcp_connections[con_id].
	    inside_timestamp >> 16) & 0xff;
	lp->data[72] = (net->tcp_connections[con_id].
	    inside_timestamp >> 8) & 0xff;
	lp->data[73] = net->tcp_connections[con_id].
	    inside_timestamp & 0xff;

	/*  data:  */
	if (data != NULL) {
		memcpy(lp->data + 74, data, datalen);
		net->tcp_connections[con_id].outside_seqnr += datalen;
	}

	/*  Checksum:  */
	net_ip_tcp_checksum(lp->data + 34, 16, tcp_length,
	    lp->data + 26, lp->data + 30, 0);

	if (net_ip_debug) {
		debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ net_ip_tcp_connectionreply(%i): ", connecting);
		for (int i=0; i<ip_len+14; i++)
			fatal_error(SUBSYS_NET, "net_ip", "%02x", lp->data[i]);
		fatal_error(SUBSYS_NET, "net_ip", " ]\n");
	}

	if (connecting)
		net->tcp_connections[con_id].outside_seqnr ++;
}


/*
 *  net_ip_tcp():
 *
 *  Handle a TCP packet comming from the emulated OS.
 *
 *  The IP header (at offset 14) could look something like
 *
 *	ver=45 tos=00 len=003c id=0006 ofs=0000 ttl=40 p=11 sum=b798
 *	src=0a000001 dst=c1abcdef
 *
 *  TCP header, at offset 34:
 *
 *	srcport=fffe dstport=0015 seqnr=af419a1d acknr=00000000
 *	control=a002 window=4000 checksum=fe58 urgent=0000
 *	and then "options and padding" and then data.
 *	(020405b4010303000101080a0000000000000000)
 *
 *  See the following URLs for good descriptions of TCP:
 *
 *	http://www.networksorcery.com/enp/protocol/tcp.htm
 *	http://www.tcpipguide.com/free/t_TCPIPTransmissionControlProtocolTCP.htm
 */
static void net_ip_tcp(struct net *net, struct nic_data *nic,
	unsigned char *packet, int len)
{
	int con_id, free_con_id, i, res;
	int srcport, dstport, data_offset, window, checksum, urgptr;
	int syn, ack, psh, rst, urg, fin;
	uint32_t seqnr, acknr;
	struct sockaddr_in remote_ip;
	fd_set rfds;
	struct timeval tv;
	int send_ofs;

	if (net_ip_debug) {
		debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ net: TCP: ");
		for (i=0; i<26; i++)
			fatal_error(SUBSYS_NET, "net_ip", "%02x", packet[i]);
		fatal_error(SUBSYS_NET, "net_ip", " ");
	}

	srcport = (packet[34] << 8) + packet[35];
	dstport = (packet[36] << 8) + packet[37];

	seqnr   = (packet[38] << 24) + (packet[39] << 16)
		+ (packet[40] << 8) + packet[41];
	acknr   = (packet[42] << 24) + (packet[43] << 16)
		+ (packet[44] << 8) + packet[45];

	if (net_ip_debug)
		fatal_error(SUBSYS_NET, "net_ip", "%i.%i.%i.%i:%i -> %i.%i.%i.%i:%i, seqnr=%lli acknr=%lli ",
		    packet[26], packet[27], packet[28], packet[29], srcport,
		    packet[30], packet[31], packet[32], packet[33], dstport,
		    (long long)seqnr, (long long)acknr);

	data_offset = (packet[46] >> 4) * 4 + 34;
	/*  data_offset is now data offset within packet :-)  */

	urg = packet[47] & 32;
	ack = packet[47] & 16;
	psh = packet[47] &  8;
	rst = packet[47] &  4;
	syn = packet[47] &  2;
	fin = packet[47] &  1;
	window   = (packet[48] << 8) + packet[49];
	checksum = (packet[50] << 8) + packet[51];
	urgptr   = (packet[52] << 8) + packet[53];

	if (net_ip_debug) {
		fatal_error(SUBSYS_NET, "net_ip", urg? "URG " : "");
		fatal_error(SUBSYS_NET, "net_ip", ack? "ACK " : "");
		fatal_error(SUBSYS_NET, "net_ip", psh? "PSH " : "");
		fatal_error(SUBSYS_NET, "net_ip", rst? "RST " : "");
		fatal_error(SUBSYS_NET, "net_ip", syn? "SYN " : "");
		fatal_error(SUBSYS_NET, "net_ip", fin? "FIN " : "");

		fatal_error(SUBSYS_NET, "net_ip", "window=0x%04x checksum=0x%04x urgptr=0x%04x ", window, checksum, urgptr);

		fatal_error(SUBSYS_NET, "net_ip", "options=");
		for (i=34+20; i<data_offset; i++)
			fatal_error(SUBSYS_NET, "net_ip", "%02x", packet[i]);

		fatal_error(SUBSYS_NET, "net_ip", " data=");
		for (i=data_offset; i<len; i++)
			fatal_error(SUBSYS_NET, "net_ip", "%02x", packet[i]);

		fatal_error(SUBSYS_NET, "net_ip", " ]\n");
	}

	net_ip_tcp_checksum(packet + 34, 16, len - 34,
		packet + 26, packet + 30, 0);
	if (packet[50] * 256 + packet[51] != checksum) {
		debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "TCP: dropping packet because of checksum mismatch "
		    "(0x%04x != 0x%04x)\n", packet[50] * 256 + packet[51],
		    checksum);

		return;
	}

	/*  Does this packet belong to a current connection?  */
	con_id = free_con_id = -1;
	for (i=0; i<MAX_TCP_CONNECTIONS; i++) {
		if (!net->tcp_connections[i].in_use)
			free_con_id = i;
		if (net->tcp_connections[i].in_use &&
		    net->tcp_connections[i].inside_tcp_port == srcport &&
		    net->tcp_connections[i].outside_tcp_port == dstport &&
		    memcmp(net->tcp_connections[i].inside_ip_address,
			packet + 26, 4) == 0 &&
		    memcmp(net->tcp_connections[i].outside_ip_address,
			packet + 30, 4) == 0) {
			con_id = i;
			break;
		}
	}

	/*
	 *  Unknown connection, and not SYN? Then drop the packet.
	 *  TODO:  Send back RST?
	 */
	if (con_id < 0 && !syn) {
		debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ net: TCP: dropping packet from unknown connection,"
		    " %i.%i.%i.%i:%i -> %i.%i.%i.%i:%i %s%s%s%s%s]\n",
		    packet[26], packet[27], packet[28], packet[29], srcport,
		    packet[30], packet[31], packet[32], packet[33], dstport,
		    fin? "FIN ": "", syn? "SYN ": "", ack? "ACK ": "",
		    psh? "PSH ": "", rst? "RST ": "");
		return;
	}

	/*  Known connection, and SYN? Then ignore the packet.  */
	if (con_id >= 0 && syn) {
		debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ net: TCP: ignoring redundant SYN packet from known"
		    " connection, %i.%i.%i.%i:%i -> %i.%i.%i.%i:%i ]\n",
		    packet[26], packet[27], packet[28], packet[29], srcport,
		    packet[30], packet[31], packet[32], packet[33], dstport);
		return;
	}

	/*
	 *  A new outgoing connection?
	 */
	if (con_id < 0 && syn) {
		debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ net: TCP: new outgoing connection, %i.%i.%i.%i:%i"
		    " -> %i.%i.%i.%i:%i ]\n",
		    packet[26], packet[27], packet[28], packet[29], srcport,
		    packet[30], packet[31], packet[32], packet[33], dstport);

		/*  Find a free connection id to use:  */
		if (free_con_id < 0) {
#if 1
			/*
			 *  TODO:  Reuse the oldest one currently in use, or
			 *  just drop the new connection attempt? Drop for now.
			 */
			debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ TOO MANY TCP CONNECTIONS IN USE! "
			    "Increase MAX_TCP_CONNECTIONS! ]\n");
			return;
#else
			int i;
			int64_t oldest = net->
			    tcp_connections[0].last_used_timestamp;
			free_con_id = 0;

			debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ NO FREE TCP SLOTS, REUSING OLDEST ONE ]\n");
			for (i=0; i<MAX_TCP_CONNECTIONS; i++)
				if (net->tcp_connections[i].
				    last_used_timestamp < oldest) {
					oldest = net->tcp_connections[i].
					    last_used_timestamp;
					free_con_id = i;
				}
			tcp_closeconnection(net, free_con_id);
#endif
		}

		con_id = free_con_id;
		memset(&net->tcp_connections[con_id], 0,
		    sizeof(struct tcp_connection));

		memcpy(net->tcp_connections[con_id].ethernet_address,
		    packet + 6, 6);
		memcpy(net->tcp_connections[con_id].inside_ip_address,
		    packet + 26, 4);
		net->tcp_connections[con_id].inside_tcp_port = srcport;
		memcpy(net->tcp_connections[con_id].outside_ip_address,
		    packet + 30, 4);
		net->tcp_connections[con_id].outside_tcp_port = dstport;
		net->tcp_connections[con_id].inside_wscale = -1;

		/*  The guest's window scale, if its SYN has one (we always
		    offer ours, shift 0):  */
		for (i = 34 + 20; i < data_offset && i < len; ) {
			int kind = packet[i];
			if (kind == 0)
				break;
			if (kind == 1) {
				i ++;
				continue;
			}
			if (i + 1 >= len || packet[i + 1] < 2)
				break;
			if (kind == 3 && packet[i + 1] == 3 && i + 2 < len)
				net->tcp_connections[con_id].inside_wscale =
				    packet[i + 2] > 14? 14 : packet[i + 2];
			i += packet[i + 1];
		}
		net->tcp_connections[con_id].inside_window = window;

		net->tcp_connections[con_id].socket =
		    socket(AF_INET, SOCK_STREAM, 0);
		if (net->tcp_connections[con_id].socket < 0) {
			debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ net: TCP: socket() returned %i ]",
			    net->tcp_connections[con_id].socket);
			return;
		}

		debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ new tcp outgoing socket=%i ]\n",
		    net->tcp_connections[con_id].socket);

		net->tcp_connections[con_id].in_use = 1;

		/*  Set the socket to non-blocking:  */
		socket_set_nonblocking(net->tcp_connections[con_id].socket);
		socket_set_cloexec(net->tcp_connections[con_id].socket);

		remote_ip.sin_family = AF_INET;
		memcpy((unsigned char *)&remote_ip.sin_addr,
		    net->tcp_connections[con_id].outside_ip_address, 4);
		remote_ip.sin_port = htons(
		    net->tcp_connections[con_id].outside_tcp_port);

		res = connect(net->tcp_connections[con_id].socket,
		    (struct sockaddr *)&remote_ip, sizeof(remote_ip));

		/*  connect() usually returns -1 with EINPROGRESS (or
		    WSAEWOULDBLOCK), as it hasn't connected right away.  If it
		    failed at once, net_tcp_rx_avail() refuses the connection:  */
		if (res < 0 && !socket_connect_pending(socket_error()))
			net->tcp_connections[con_id].connect_error =
			    socket_error();

		net->tcp_connections[con_id].state =
		    TCP_OUTSIDE_TRYINGTOCONNECT;

		net->tcp_connections[con_id].outside_acknr = 0;
		net->tcp_connections[con_id].outside_seqnr =
		    ((random() & 0xffff) << 16) + (random() & 0xffff);
	}

	if (rst) {
		debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ 'rst': disconnecting TCP connection %i ]\n", con_id);
		net_ip_tcp_connectionreply(net, nic, con_id, 0, NULL, 0, 1);
		tcp_closeconnection(net, con_id);
		return;
	}

	if (ack && net->tcp_connections[con_id].state
	    == TCP_OUTSIDE_DISCONNECTED2) {
		debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ 'ack': guestOS's final termination of TCP "
		    "connection %i ]\n", con_id);

		/*  Send an RST?  (TODO, this is wrong...)  */
		net_ip_tcp_connectionreply(net, nic, con_id, 0, NULL, 0, 1);

		/*  ... and forget about this connection:  */
		tcp_closeconnection(net, con_id);
		return;
	}

	if (fin && net->tcp_connections[con_id].state
	    == TCP_OUTSIDE_DISCONNECTED) {
		debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ 'fin': response to outside's disconnection of "
		    "TCP connection %i ]\n", con_id);

		/*  Send an ACK:  */
		net->tcp_connections[con_id].state = TCP_OUTSIDE_CONNECTED;
		net_ip_tcp_connectionreply(net, nic, con_id, 0, NULL, 0, 0);
		net->tcp_connections[con_id].state = TCP_OUTSIDE_DISCONNECTED2;
		return;
	}

	if (fin) {
		struct tcp_connection *c = &net->tcp_connections[con_id];
		int32_t ofs = (int32_t) (c->outside_acknr - seqnr);

		debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ 'fin': guestOS disconnecting TCP connection %i ]\n",
		    con_id);

		/*  The FIN can come with the last of the guest's data (or
		    again, if we didn't take all of it the first time):  */
		if ((c->state == TCP_OUTSIDE_CONNECTED ||
		    c->state == TCP_OUTSIDE_DISCONNECTED2) && ofs >= 0 &&
		    data_offset + ofs < len) {
			res = send(c->socket, (const char *) packet +
			    data_offset + ofs, len - data_offset - ofs, 0);
			if (res > 0)
				c->outside_acknr += res;
			if (data_offset + ofs + res < len) {
				/*  Not all of it: ack what we took; the
				    guest sends the rest, and the FIN, again.  */
				net_ip_tcp_connectionreply(net, nic, con_id,
				    0, NULL, 0, 0);
				return;
			}
		}

		/*  Send ACK:  */
		net_ip_tcp_connectionreply(net, nic, con_id, 0, NULL, 0, 0);
		net->tcp_connections[con_id].state = TCP_OUTSIDE_DISCONNECTED2;

		/*  Return and send FIN:  */
		goto ret;
	}

	if (ack) {
		struct tcp_connection *c = &net->tcp_connections[con_id];
		int32_t adv = (int32_t) (acknr - c->incoming_buf_seqnr);

		c->inside_window = c->inside_wscale > 0?
		    (uint32_t) window << c->inside_wscale : (uint32_t) window;

		if (adv > 0 && adv <= c->incoming_buf_len) {
			/*  New data acknowledged: drop it from the buffer.  */
			memmove(c->incoming_buf, c->incoming_buf + adv,
			    c->incoming_buf_len - adv);
			c->incoming_buf_len -= adv;
			c->incoming_buf_seqnr = acknr;
			c->last_progress_ms = tcp_now_ms();
			c->dupacks = 0;
			if ((int32_t) (c->outside_seqnr - acknr) < 0)
				c->outside_seqnr = acknr;
		} else if (adv == 0 && data_offset >= len && !syn && !fin &&
		    c->incoming_buf_len != 0 &&
		    c->outside_seqnr != c->incoming_buf_seqnr) {
			/*  A duplicate: after three, a segment was lost; go
			    back to it (net_tcp_rx_avail() sends again):  */
			if (++ c->dupacks == 3)
				c->outside_seqnr = c->incoming_buf_seqnr;
		}
		c->inside_acknr = acknr;
	}

	net->tcp_connections[con_id].inside_seqnr = seqnr;

	/*  TODO: This is hardcoded for a specific NetBSD packet:  */
	if (packet[34 + 30] == 0x08 && packet[34 + 31] == 0x0a)
		net->tcp_connections[con_id].inside_timestamp =
		    (packet[34 + 32 + 0] << 24) +
		    (packet[34 + 32 + 1] << 16) +
		    (packet[34 + 32 + 2] <<  8) +
		    (packet[34 + 32 + 3] <<  0);


	net->timestamp ++;
	net->tcp_connections[con_id].last_used_timestamp = net->timestamp;


	if (net->tcp_connections[con_id].state != TCP_OUTSIDE_CONNECTED) {
		debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ not connected to outside ]\n");
		return;
	}


	if (data_offset >= len)
		return;


	/*
	 *  We are here if this is a known connection, and data is to be
	 *  transmitted to the outside world.
	 */

	send_ofs = data_offset;
	send_ofs += ((int32_t)net->tcp_connections[con_id].outside_acknr
	    - (int32_t)seqnr);
#if 1
	debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ %i bytes of tcp data to be sent, beginning at seqnr %u, ",
	    len - data_offset, seqnr);
	debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "outside is at acknr %u ==> %i actual bytes to be sent ]\n",
	    net->tcp_connections[con_id].outside_acknr, len - send_ofs);
#endif

	/*  Drop outgoing packet if the guest OS' seqnr is not
	    the same as we have acked. (We have missed something, perhaps.)  */
	if (seqnr != net->tcp_connections[con_id].outside_acknr) {
		debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "!! outgoing TCP packet dropped (seqnr = %u, "
		    "outside_acknr = %u)\n", seqnr,
		    net->tcp_connections[con_id].outside_acknr);
		goto ret;
	}

	if (len - send_ofs > 0) {
		/*  Is the socket available for output?  */
		FD_ZERO(&rfds);		/*  write  */
		FD_SET(net->tcp_connections[con_id].socket, &rfds);
		tv.tv_sec = tv.tv_usec = 0;
		socket_clear_error();
		res = select(net->tcp_connections[con_id].socket+1,
		    NULL, &rfds, NULL, &tv);
		if (res < 1) {
			net->tcp_connections[con_id].state =
			    TCP_OUTSIDE_DISCONNECTED;
			debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ TCP: disconnect on select for writing ]\n");
			goto ret;
		}

		res = send(net->tcp_connections[con_id].socket,
		    (const char *) packet + send_ofs, len - send_ofs, 0);

		if (res > 0) {
			net->tcp_connections[con_id].outside_acknr += res;
		} else if (socket_would_block(socket_error())) {
			/*  Just ignore this attempt.  */
			return;
		} else {
			debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ error writing %i bytes to TCP connection %i:"
			    " error %i ]\n", len - send_ofs, con_id,
			    socket_error());
			net->tcp_connections[con_id].state =
			    TCP_OUTSIDE_DISCONNECTED;
			debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ TCP: disconnect on write() ]\n");
			goto ret;
		}
	}

ret:
	/*  Send an ACK (or FIN) to the guest OS:  */
	net_ip_tcp_connectionreply(net, nic, con_id, 0, NULL, 0, 0);
}


/*
 *  udp_reassemble():
 *
 *  Collects an IP fragment of a UDP datagram from the guest. When it was the
 *  last missing piece, returns the whole datagram as one packet (in a buffer
 *  that is used again later), with its length in *lenp; else NULL.
 */
static void net_ip_broadcast_dhcp(struct net *net, struct nic_data *nic,
	unsigned char *packet, int len);


static unsigned char *udp_reassemble(struct net *net,
	const unsigned char *packet, int len, int *lenp)
{
	struct udp_reassembly *r = NULL, *oldest = NULL;
	int ip_id = (packet[18] << 8) + packet[19];
	int ofs = (((packet[20] & 0x1f) << 8) + packet[21]) * 8;
	int more = packet[20] & 0x20;
	int data_len = len - 34, i;
	int64_t now = time(NULL);

	if (data_len <= 0 || ofs + data_len > 65535 || (more && (data_len & 7)))
		return NULL;

	for (i = 0; i < N_UDP_REASSEMBLIES; i++) {
		struct udp_reassembly *t = &net->udp_reassemblies[i];

		/*  Lost fragments; a later datagram with the same IP ID must
		    not complete this one:  */
		if (t->in_use && (now < t->first_time ||
		    now - t->first_time > UDP_REASSEMBLY_SECONDS))
			t->in_use = 0;

		if (t->in_use && t->ip_id == ip_id &&
		    memcmp(t->src_ip_address, packet + 26, 4) == 0 &&
		    memcmp(t->dst_ip_address, packet + 30, 4) == 0) {
			r = t;
			break;
		}
		if (oldest == NULL || !t->in_use || (oldest->in_use &&
		    t->last_used_timestamp < oldest->last_used_timestamp))
			oldest = t;
	}

	if (r == NULL) {
		/*  A new datagram, instead of the oldest unfinished one
		    (whose missing fragments are probably lost):  */
		r = oldest;
		if (r->packet == NULL)
			CHECK_ALLOCATION(r->packet = (unsigned char *)
			    malloc(34 + 65535));
		r->in_use = 1;
		r->first_time = now;
		memcpy(r->src_ip_address, packet + 26, 4);
		memcpy(r->dst_ip_address, packet + 30, 4);
		r->ip_id = ip_id;
		r->data_len = -1;
		memset(r->have, 0, sizeof(r->have));
	}

	net->timestamp ++;
	r->last_used_timestamp = net->timestamp;

	/*  The first fragment has the UDP header, after the IP header:  */
	if (ofs == 0)
		memcpy(r->packet, packet, 34);
	if (r->data_len >= 0 &&
	    (ofs + data_len > r->data_len ||
	     (!more && ofs + data_len != r->data_len))) {
		r->in_use = 0;
		return NULL;
	}
	memcpy(r->packet + 34 + ofs, packet + 34, data_len);
	for (i = ofs / 8; i < (ofs + data_len + 7) / 8; i++)
		r->have[i / 8] |= 1 << (i & 7);
	if (!more)
		r->data_len = ofs + data_len;

	if (r->data_len < 0)
		return NULL;
	for (i = 0; i < (r->data_len + 7) / 8; i++)
		if (!(r->have[i / 8] & (1 << (i & 7))))
			return NULL;

	/*  All there: one packet, with the first fragment's headers.  */
	r->in_use = 0;
	r->packet[16] = (20 + r->data_len) >> 8;
	r->packet[17] = (20 + r->data_len) & 0xff;
	r->packet[20] = r->packet[21] = 0;
	*lenp = 34 + r->data_len;
	return r->packet;
}


/*
 *  udp_bind_reserved():
 *
 *  Binds a UDP socket to a reserved port of the host (below 1024), port if
 *  it is free, as NFS servers and others trust only clients that send from
 *  one.  Where only root may bind those (Linux), the socket is left as it
 *  was.
 */
static void udp_bind_reserved(int s, int port)
{
	struct sockaddr_in sin;
	int i;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	for (i = 0; i < 512; i++) {
		sin.sin_port = htons(i == 0 ? port : 1024 - i);
		if (bind(s, (struct sockaddr *) &sin, sizeof(sin)) == 0)
			return;
#ifndef _WIN32
		if (errno != EADDRINUSE)
			return;
#endif
	}
}


/*
 *  net_ip_udp():
 *
 *  Handle a UDP packet.
 *
 *  (See http://www.networksorcery.com/enp/protocol/udp.htm.)
 *
 *  The IP header (at offset 14) could look something like
 *
 *	ver=45 tos=00 len=003c id=0006 ofs=0000 ttl=40 p=11 sum=b798
 *	src=0a000001 dst=c1abcdef
 *
 *  and the UDP data (beginning at offset 34):
 *
 *	srcport=fffc dstport=0035 length=0028 chksum=76b6
 *	43e20100000100000000000003667470066e6574627364036f726700001c0001
 *
 *  The data is sent from a socket of the host, which is kept for replies
 *  as a "connection". A datagram that came in IP fragments is put together
 *  first (udp_reassemble()), and one sent from a reserved port (as NFS's
 *  and mount's are) is sent from one of the host too (udp_bind_reserved()).
 */
/*
 *  net_dns_synth_ptr():
 *
 *  Old resolvers (SYSTEM V/88's libsocket gethostbyname, for one) do a
 *  reverse lookup of every address they get, and give up when there is no
 *  PTR record, which is common today. In a reply relayed from the host's
 *  nameserver, an empty or NXDOMAIN answer to "d.c.b.a.in-addr.arpa PTR"
 *  becomes "ip-a-b-c-d.nat".
 *
 *  Returns the (possibly new) length of the DNS message in buf.
 */
static int dns_label_is(const unsigned char *p, int l, const char *s)
{
	int i;

	if (l != (int) strlen(s))
		return 0;
	for (i = 0; i < l; i++) {
		int c = p[i];
		if (c >= 'A' && c <= 'Z')
			c += 'a' - 'A';
		if (c != s[i])
			return 0;
	}
	return 1;
}

static int net_dns_synth_ptr(unsigned char *buf, int len, int bufsize)
{
	int pos = 12, qend, i, nlab = 0, rcode, n;
	int octet[4];
	char name[32];
	unsigned char *p;

	/*  A response, standard query, one question, no answers.  */
	if (len < 12 || !(buf[2] & 0x80) || (buf[2] & 0x78) != 0)
		return len;
	rcode = buf[3] & 0x0f;
	if ((buf[4] << 8 | buf[5]) != 1 || (buf[6] << 8 | buf[7]) != 0 ||
	    (rcode != 0 && rcode != 3))
		return len;

	/*  Question name: d.c.b.a.in-addr.arpa  */
	while (pos < len && buf[pos] != 0) {
		int l = buf[pos];
		if (l > 63 || pos + 1 + l > len)
			return len;
		if (nlab < 4) {
			int v = 0;
			if (l < 1 || l > 3)
				return len;
			for (i = 0; i < l; i++) {
				int c = buf[pos + 1 + i];
				if (c < '0' || c > '9')
					return len;
				v = v * 10 + c - '0';
			}
			if (v > 255)
				return len;
			octet[3 - nlab] = v;
		} else if (nlab == 4) {
			if (!dns_label_is(buf + pos + 1, l, "in-addr"))
				return len;
		} else if (nlab == 5) {
			if (!dns_label_is(buf + pos + 1, l, "arpa"))
				return len;
		} else
			return len;
		nlab ++;
		pos += 1 + l;
	}
	if (nlab != 6 || pos + 5 > len)
		return len;
	qend = pos + 1 + 4;
	if ((buf[pos + 1] << 8 | buf[pos + 2]) != 12)		/*  PTR  */
		return len;
	if ((buf[pos + 3] << 8 | buf[pos + 4]) != 1)		/*  IN  */
		return len;

	snprintf(name, sizeof(name), "ip-%d-%d-%d-%d",
	    octet[0], octet[1], octet[2], octet[3]);
	n = strlen(name);
	if (qend + 12 + 1 + n + 5 > bufsize)
		return len;

	/*  Answer: c00c, PTR, IN, ttl 60, rdata = ip-a-b-c-d.nat.  */
	p = buf + qend;
	*p++ = 0xc0; *p++ = 0x0c;
	*p++ = 0; *p++ = 12;
	*p++ = 0; *p++ = 1;
	*p++ = 0; *p++ = 0; *p++ = 0; *p++ = 60;
	*p++ = 0; *p++ = 1 + n + 5;
	*p++ = n;
	memcpy(p, name, n);
	p += n;
	*p++ = 3;
	memcpy(p, "nat", 3);
	p += 3;
	*p++ = 0;

	buf[3] &= 0xf0;				/*  rcode = NOERROR  */
	buf[6] = 0; buf[7] = 1;			/*  ancount = 1  */
	buf[8] = 0; buf[9] = 0;			/*  nscount = 0  */
	buf[10] = 0; buf[11] = 0;		/*  arcount = 0  */
	return p - buf;
}


static void net_ip_udp(struct net *net, struct nic_data *nic,
	unsigned char *packet, int len)
{
	int con_id, free_con_id, i, srcport, dstport, udp_len;
	ssize_t res;
	struct sockaddr_in remote_ip;

	/*  More fragments, or a fragment offset:  */
	if ((packet[20] & 0x3f) != 0 || packet[21] != 0) {
		packet = udp_reassemble(net, packet, len, &len);
		if (packet == NULL)
			return;
	}

	srcport = (packet[34] << 8) + packet[35];
	dstport = (packet[36] << 8) + packet[37];
	udp_len = (packet[38] << 8) + packet[39];
	/*  chksum at offset 40 and 41  */

	/*
	 *  DHCP to the gateway: a client that has its address sends the
	 *  messages of its lease (renewing it, for example) to its server,
	 *  which is the gateway.
	 */
	if (srcport == 68 && dstport == 67 &&
	    memcmp(packet + 30, &net->gateway_ipv4_addr[0], 4) == 0) {
		net_ip_broadcast_dhcp(net, nic, packet, len);
		return;
	}

	debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ net: UDP: ");
	debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "srcport=%i dstport=%i len=%i ", srcport, dstport, udp_len);
	for (i=42; i<len; i++) {
		if (packet[i] >= ' ' && packet[i] < 127)
			debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "%c", packet[i]);
		else
			debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[%02x]", packet[i]);
	}
	debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, " ]\n");

	/*  Is this "connection" new, or a currently ongoing one?  */
	con_id = free_con_id = -1;
	for (i=0; i<MAX_UDP_CONNECTIONS; i++) {
		if (!net->udp_connections[i].in_use)
			free_con_id = i;
		if (net->udp_connections[i].in_use &&
		    net->udp_connections[i].inside_udp_port == srcport &&
		    net->udp_connections[i].outside_udp_port == dstport &&
		    memcmp(net->udp_connections[i].inside_ip_address,
			packet + 26, 4) == 0 &&
		    memcmp(net->udp_connections[i].outside_ip_address,
			packet + 30, 4) == 0) {
			con_id = i;
			break;
		}
	}

	debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "&& UDP connection is ");
	if (con_id >= 0)
		debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "ONGOING");
	else {
		debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "NEW");
		if (free_con_id < 0) {
			int64_t oldest = net->
			    udp_connections[0].last_used_timestamp;
			free_con_id = 0;

			debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, ", NO FREE SLOTS, REUSING OLDEST ONE");
			for (int j=0; j<MAX_UDP_CONNECTIONS; j++)
				if (net->udp_connections[j].last_used_timestamp < oldest) {
					oldest = net->udp_connections[j].last_used_timestamp;
					free_con_id = j;
				}

			socket_close(net->udp_connections[free_con_id].socket);
		}
		con_id = free_con_id;
		memset(&net->udp_connections[con_id], 0,
		    sizeof(struct udp_connection));

		memcpy(net->udp_connections[con_id].ethernet_address,
		    packet + 6, 6);
		memcpy(net->udp_connections[con_id].inside_ip_address,
		    packet + 26, 4);
		net->udp_connections[con_id].inside_udp_port = srcport;
		memcpy(net->udp_connections[con_id].outside_ip_address,
		    packet + 30, 4);
		net->udp_connections[con_id].outside_udp_port = dstport;

		net->udp_connections[con_id].socket = socket(AF_INET,
		    SOCK_DGRAM, 0);
		if (net->udp_connections[con_id].socket < 0) {
			debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ net: UDP: socket() returned %i ]",
			    net->udp_connections[con_id].socket);
			return;
		}

		debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, " {socket=%i}", net->udp_connections[con_id].socket);

		if (srcport < 1024)
			udp_bind_reserved(net->udp_connections[con_id].socket,
			    srcport);

		/*  Room for the largest datagram (macOS sends at most 9216
		    bytes by default):  */
		i = 65535;
		setsockopt(net->udp_connections[con_id].socket, SOL_SOCKET,
		    SO_SNDBUF, (const char *) &i, sizeof(i));

		net->udp_connections[con_id].in_use = 1;

		/*  Set the socket to non-blocking:  */
		socket_set_nonblocking(net->udp_connections[con_id].socket);
		socket_set_cloexec(net->udp_connections[con_id].socket);
	}

	debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, ", connection id %i\n", con_id);

	net->timestamp ++;
	net->udp_connections[con_id].last_used_timestamp = net->timestamp;

	remote_ip.sin_family = AF_INET;
	memcpy((unsigned char *)&remote_ip.sin_addr,
	    net->udp_connections[con_id].outside_ip_address, 4);

	/*
	 *  Special case for the nameserver:  If a UDP packet is sent to
	 *  the gateway, it will be forwarded to the nameserver, if it is
	 *  known.
	 */
	if (net->nameserver_known &&
	    memcmp(net->udp_connections[con_id].outside_ip_address,
	    &net->gateway_ipv4_addr[0], 4) == 0) {
		memcpy((unsigned char *)&remote_ip.sin_addr,
		    &net->nameserver_ipv4, 4);
		net->udp_connections[con_id].fake_ns = 1;
	}

	remote_ip.sin_port = htons(
	    net->udp_connections[con_id].outside_udp_port);

	res = sendto(net->udp_connections[con_id].socket,
	    (const char *) packet + 42,
	    len - 42, 0, (const struct sockaddr *)&remote_ip,
	    sizeof(remote_ip));

	if (res != len-42)
		debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ net: UDP: unable to send %i bytes ]\n", len-42);
	else
		debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ net: UDP: OK!!! ]\n");
}


/*
 *  net_ip():
 *
 *  Handle an IP packet, coming from the emulated NIC.
 */
void net_ip(struct net *net, struct nic_data *nic, unsigned char *packet,
	int len)
{
	int i;

	if (ENOUGH_VERBOSITY(SUBSYS_NET, VERBOSITY_DEBUG)) {
		char s[2000];
		
		snprintf(s, sizeof(s), "ver=%02x ", packet[14]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "tos=%02x ", packet[15]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "len=%02x%02x ", packet[16], packet[17]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "id=%02x%02x ",  packet[18], packet[19]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "ofs=%02x%02x ", packet[20], packet[21]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "ttl=%02x ", packet[22]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "p=%02x ", packet[23]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "sum=%02x%02x ", packet[24], packet[25]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "src=%02x%02x%02x%02x ",
		    packet[26], packet[27], packet[28], packet[29]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "dst=%02x%02x%02x%02x ",
		    packet[30], packet[31], packet[32], packet[33]);
		for (i=34; i<len; i++) {
			if (strlen(s) > sizeof(s) - 5)
				break;
			snprintf(s+strlen(s), sizeof(s)-strlen(s), "%02x", packet[i]);
		}

		debugmsg(SUBSYS_NET, "IP", VERBOSITY_DEBUG, "%s", s);
	}

	/*  Cut off overflowing tail data:  */
	if (len > 14 + packet[16]*256 + packet[17])
		len = 14 + packet[16]*256 + packet[17];

	if (packet[14] == 0x45) {
		/*  IPv4:  */
		switch (packet[23]) {
		case 1:	/*  ICMP  */
			net_ip_icmp(net, nic, packet, len);
			break;
		case 6:	/*  TCP  */
			net_ip_tcp(net, nic, packet, len);
			break;
		case 17:/*  UDP  */
			net_ip_udp(net, nic, packet, len);
			break;
		default:debugmsg(SUBSYS_NET, "IP", VERBOSITY_WARNING,
			    "UNIMPLEMENTED protocol %i",
			    packet[23]);
		}
	} else {
		debugmsg(SUBSYS_NET, "IP", VERBOSITY_WARNING,
		    "UNIMPLEMENTED ip version, first byte = 0x%02x",
		    packet[14]);
	}
}


/*
 *  net_ip_dhcp_option():
 *
 *  Find option opt among the options of a BOOTP or DHCP message, which
 *  follow the magic cookie at offset 278 of the Ethernet packet. Returns a
 *  pointer to the length byte of the option, or NULL if there is none.
 */
static unsigned char *net_ip_dhcp_option(unsigned char *packet, int len,
	int opt)
{
	int i = 282;

	if (len < 282 || packet[278] != 99 || packet[279] != 130 ||
	    packet[280] != 83 || packet[281] != 99)
		return NULL;

	while (i < len && packet[i] != 255) {
		if (packet[i] == 0) {
			i ++;
			continue;
		}
		if (i + 2 > len || i + 2 + packet[i+1] > len)
			break;
		if (packet[i] == opt)
			return packet + i + 1;
		i += 2 + packet[i+1];
	}

	return NULL;
}


/*
 *  net_ip_broadcast_dhcp():
 *
 *  Handle an IPv4 DHCP broadcast packet, coming from the emulated NIC.
 *  (Or one sent to the gateway, see net_ip_udp().)
 *
 *  Read http://tools.ietf.org/html/rfc2131 for details on DHCP.
 *  (And http://users.telenet.be/mydotcom/library/network/dhcp.htm.)
 */
static void net_ip_broadcast_dhcp(struct net *net, struct nic_data *nic,
	unsigned char *packet, int len)
{
	/*  The address of the client, and its subnet mask:  */
	unsigned char client_ipv4[4], client_mask[4];
	static const unsigned char zero_ipv4[4] = { 0, 0, 0, 0 };
	struct ethernet_packet_link *lp;
	unsigned char *opt;
        int i, reply_len, dhcp_type = 0, inform = 0;

	if (ENOUGH_VERBOSITY(SUBSYS_NET, VERBOSITY_DEBUG)) {
		char s[4000];
		
		snprintf(s, sizeof(s), "ver=%02x ", packet[14]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "tos=%02x ", packet[15]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "len=%02x%02x ", packet[16], packet[17]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "id=%02x%02x ",  packet[18], packet[19]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "ofs=%02x%02x ", packet[20], packet[21]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "ttl=%02x ", packet[22]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "p=%02x ", packet[23]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "sum=%02x%02x ", packet[24], packet[25]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "src=%02x%02x%02x%02x ",
		    packet[26], packet[27], packet[28], packet[29]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "dst=%02x%02x%02x%02x ",
		    packet[30], packet[31], packet[32], packet[33]);
		for (i=34; i<len; i++) {
			if (strlen(s) > sizeof(s) - 5)
				break;
			snprintf(s+strlen(s), sizeof(s)-strlen(s), "%02x", packet[i]);
		}

		debugmsg(SUBSYS_NET, "IPv4 DHCP", VERBOSITY_DEBUG, "%s", s);
	}

	if (len < 34 + 8 + 236) {
		debugmsg(SUBSYS_NET, "IPv4 DHCP", VERBOSITY_ERROR, "packet too short? len=%i", len);
		return;
	}

	/*
	 *  UDP data (at offset 34):
	 *
	 *  srcport=0044 dstport=0043 length=0134 chksum=a973
	 *  data = 01010600d116d276000000000000000000000000000000
	 *         0000000000102030405060...0000...638253633501...000
	 */

#if 0
	fatal_error(SUBSYS_NET, "net_ip", "op=%02x ", packet[42]);
	fatal_error(SUBSYS_NET, "net_ip", "htype=%02x ", packet[43]);
	fatal_error(SUBSYS_NET, "net_ip", "hlen=%02x ", packet[44]);
	fatal_error(SUBSYS_NET, "net_ip", "hops=%02x ", packet[45]);
	fatal_error(SUBSYS_NET, "net_ip", "xid=%02x%02x%02x%02x ", packet[46], packet[47],
	    packet[48], packet[49]);
	fatal_error(SUBSYS_NET, "net_ip", "secs=%02x%02x ", packet[50], packet[51]);
	fatal_error(SUBSYS_NET, "net_ip", "flags=%02x%02x ", packet[52], packet[53]);
	fatal_error(SUBSYS_NET, "net_ip", "ciaddr=%02x%02x%02x%02x ", packet[54], packet[55],
	    packet[56], packet[57]);
	fatal_error(SUBSYS_NET, "net_ip", "yiaddr=%02x%02x%02x%02x ", packet[58], packet[59],
	    packet[60], packet[61]);
	fatal_error(SUBSYS_NET, "net_ip", "siaddr=%02x%02x%02x%02x ", packet[62], packet[63],
	    packet[64], packet[65]);
	fatal_error(SUBSYS_NET, "net_ip", "giaddr=%02x%02x%02x%02x ", packet[66], packet[67],
	    packet[68], packet[69]);
	fatal_error(SUBSYS_NET, "net_ip", "chaddr=");
	for (i=70; i<70+16; i++)
		fatal_error(SUBSYS_NET, "net_ip", "%02x", packet[i]);
	/*
	   |                          sname   (64)                         |
	   |                          file    (128)                        |
	 */
	fatal_error(SUBSYS_NET, "net_ip", " ]\n");
#endif

	/*
	 *  A DHCP client tells the type of its message in option 53, and
	 *  expects the type of the answer there: an offer for a discover, an
	 *  ack for a request, or a nak when it asks for an address that is not
	 *  its address here (one it leased on another network), an ack
	 *  without an address for an inform. A decline or a release gets no
	 *  answer. A BOOTP client sends no option 53, and gets a BOOTP reply.
	 */
	net_client_ipv4(net, nic, client_ipv4, client_mask);
	opt = net_ip_dhcp_option(packet, len, 53);
	if (opt != NULL && opt[0] == 1) {
		switch (opt[1]) {
		case 1:	/*  DHCPDISCOVER  */
			dhcp_type = 2;			/*  DHCPOFFER  */
			break;
		case 3:	/*  DHCPREQUEST  */
			dhcp_type = 5;			/*  DHCPACK  */
			opt = net_ip_dhcp_option(packet, len, 50);
			if (opt != NULL && opt[0] == 4) {
				if (memcmp(opt + 1, client_ipv4, 4) != 0)
					dhcp_type = 6;	/*  DHCPNAK  */
			} else if (memcmp(packet + 54, zero_ipv4, 4) != 0 &&
			    memcmp(packet + 54, client_ipv4, 4) != 0)
				dhcp_type = 6;		/*  DHCPNAK  */
			break;
		case 8:	/*  DHCPINFORM  */
			dhcp_type = 5;			/*  DHCPACK  */
			inform = 1;
			break;
		default:/*  DHCPDECLINE, DHCPRELEASE  */
			return;
		}
	}

	/*  A DHCP answer has at least the 300 bytes of a BOOTP message:  */
	reply_len = dhcp_type ? 14 + 20 + 8 + 300 : 307;
        lp = net_allocate_ethernet_packet_link(net, nic, reply_len);

        /*  From old packet, copy everything before options field:  */
        memcpy(lp->data, packet, 278);
	memset(lp->data + 278, 0, reply_len - 278);

	/*  We are sending to the client, from the gateway:  */
	memcpy(lp->data + 0, packet + 6, 6);
	memcpy(lp->data + 6, net->gateway_ethernet_addr, 6);

	/*  Set IP length:  */
	lp->data[16] = (reply_len - 14) >> 8;
	lp->data[17] = (reply_len - 14) & 0xff;

        /*  Set IP addresses (an ack goes to the address the client has):  */
	memcpy(lp->data + 26, &net->gateway_ipv4_addr[0], 4);
	if (dhcp_type == 5 && memcmp(packet + 54, zero_ipv4, 4) != 0)
		memcpy(lp->data + 30, packet + 54, 4);
	else
		memset(lp->data + 30, 0xff, 4);

	/*  Switch src and dst ports:  */
	memcpy(lp->data + 34, packet + 36, 2);
	memcpy(lp->data + 36, packet + 34, 2);

	/*  Set UDP length:  */
	lp->data[38] = (reply_len - 34) >> 8;
	lp->data[39] = (reply_len - 34) & 0xff;

	/*  Client's (yiaddr) IPv4 address:  */
	if (dhcp_type == 6 || inform)
		memset(lp->data + 58, 0, 4);
	else
		memcpy(lp->data + 58, client_ipv4, 4);

	if (dhcp_type == 0) {
		/*  Server's IPv4 address:  (giaddr)  */
		memcpy(lp->data + 66, &net->gateway_ipv4_addr[0], 4);
	} else if (dhcp_type != 6) {
		/*  Server's IPv4 address:  (siaddr)  */
		memcpy(lp->data + 62, &net->gateway_ipv4_addr[0], 4);
	}

	/*  This is a Reply:  */
	lp->data[42] = 0x02;

	snprintf((char *)lp->data + 70+16+64, 8, "gxemul");

	/*  Options field at offset 278:  */
	lp->data[278] = 99;
	lp->data[279] = 130;
	lp->data[280] = 83;
	lp->data[281] = 99;
	i = 282;

	/*  DHCP options, http://tools.ietf.org/html/rfc1533  */
	if (dhcp_type != 0) {
		lp->data[i++] = 53; /* DHCP message type */
		lp->data[i++] = 1;
		lp->data[i++] = dhcp_type;
		lp->data[i++] = 54; /* server identifier */
		lp->data[i++] = 4;
		memcpy(lp->data + i, &net->gateway_ipv4_addr[0], 4);
		i += 4;
		if (!inform && dhcp_type != 6) {
			lp->data[i++] = 51; /* lease time: infinite */
			lp->data[i++] = 4;
			memset(lp->data + i, 0xff, 4);
			i += 4;
		}
	}
	if (dhcp_type != 6) {
		lp->data[i++] = 1; /* subnet mask */
		lp->data[i++] = 4;
		memcpy(lp->data + i, client_mask, 4);
		i += 4;
		lp->data[i++] = 3; /* router */
		lp->data[i++] = 4;
		memcpy(lp->data + i, &net->gateway_ipv4_addr[0], 4);
		i += 4;
		lp->data[i++] = 6; /* domain name server */
		lp->data[i++] = 4;
		memcpy(lp->data + i, &net->gateway_ipv4_addr[0], 4);
		i += 4;
	}
	if (dhcp_type == 0) {
		lp->data[i++] = 54; /* server identifier */
		lp->data[i++] = 4;
		memcpy(lp->data + i, &net->gateway_ipv4_addr[0], 4);
		i += 4;
	}
	lp->data[i] = 255; /* end */

	/*  Recalculate IP header checksum:  */
	net_ip_checksum(lp->data + 14, 10, 20);

	/*  ... and the UDP checksum:  */
        net_ip_tcp_checksum(lp->data + 34, 6, reply_len - 34,
	    lp->data + 26, lp->data + 30, 1);

	if (ENOUGH_VERBOSITY(SUBSYS_NET, VERBOSITY_DEBUG)) {
		char s[4000];
		s[0] = '\0';

		packet = lp->data;

		for (i=0; i<14; i++) {
			if (strlen(s) > sizeof(s) - 5)
				break;
			snprintf(s+strlen(s), sizeof(s)-strlen(s), "%02x", packet[i]);
		}
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "ver=%02x ", packet[14]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "tos=%02x ", packet[15]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "len=%02x%02x ", packet[16], packet[17]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "id=%02x%02x ",  packet[18], packet[19]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "ofs=%02x%02x ", packet[20], packet[21]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "ttl=%02x ", packet[22]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "p=%02x ", packet[23]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "sum=%02x%02x ", packet[24], packet[25]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "src=%02x%02x%02x%02x ",
		    packet[26], packet[27], packet[28], packet[29]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "dst=%02x%02x%02x%02x ",
		    packet[30], packet[31], packet[32], packet[33]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "op=%02x ", packet[42]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "htype=%02x ", packet[43]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "hlen=%02x ", packet[44]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "hops=%02x ", packet[45]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "xid=%02x%02x%02x%02x ",
			packet[46], packet[47], packet[48], packet[49]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "secs=%02x%02x ", packet[50], packet[51]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "flags=%02x%02x ", packet[52], packet[53]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "ciaddr=%02x%02x%02x%02x ",
			packet[54], packet[55], packet[56], packet[57]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "yiaddr=%02x%02x%02x%02x ",
			packet[58], packet[59], packet[60], packet[61]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "siaddr=%02x%02x%02x%02x ",
			packet[62], packet[63], packet[64], packet[65]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "giaddr=%02x%02x%02x%02x ",
			packet[66], packet[67], packet[68], packet[69]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "chaddr=");
		for (i=70; i<70+16; i++) {
			if (strlen(s) > sizeof(s) - 5)
				break;

			snprintf(s+strlen(s), sizeof(s)-strlen(s), "%02x", packet[i]);
		}

		debugmsg(SUBSYS_NET, "IPv4 DHCP REPLY", VERBOSITY_DEBUG, "%s", s);
	}
}


/*
 *  net_ip_broadcast():
 *
 *  Handle an IP broadcast packet, coming from the emulated NIC.
 *  (This is usually a DHCP request, or similar.)
 */
void net_ip_broadcast(struct net *net, struct nic_data *nic,
	unsigned char *packet, int len)
{
	unsigned char *p = (unsigned char *) &net->netmask_ipv4;
	uint32_t x, y;
	int i, xl, warning = 0, match = 0;

	if (ENOUGH_VERBOSITY(SUBSYS_NET, VERBOSITY_DEBUG)) {
		char s[4000];
		
		snprintf(s, sizeof(s), "ver=%02x ", packet[14]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "tos=%02x ", packet[15]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "len=%02x%02x ", packet[16], packet[17]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "id=%02x%02x ",  packet[18], packet[19]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "ofs=%02x%02x ", packet[20], packet[21]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "ttl=%02x ", packet[22]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "p=%02x ", packet[23]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "sum=%02x%02x ", packet[24], packet[25]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "src=%02x%02x%02x%02x ",
		    packet[26], packet[27], packet[28], packet[29]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "dst=%02x%02x%02x%02x ",
		    packet[30], packet[31], packet[32], packet[33]);
		for (i=34; i<len; i++) {
			if (strlen(s) > sizeof(s) - 5)
				break;
			snprintf(s+strlen(s), sizeof(s)-strlen(s), "%02x", packet[i]);
		}

		debugmsg(SUBSYS_NET, "ip BROADCAST", VERBOSITY_DEBUG, "%s", s);
	}

	/*  Check for 10.0.0.255 first, maybe some guest OSes think that
	    it's a /24 network, regardless of what it actually is.  */
	y = (packet[30] << 24) + (packet[31] << 16) +
	    (packet[32] << 8) + packet[33];

	x = (p[0] << 24) + (p[1] << 16) + (p[2] << 8) + p[3];
	/*  Example: x = 10.0.0.0  */
	x |= 255;

	if (x == y) {
		warning = 1;
		match = 1;
	}

	xl = 32 - net->netmask_ipv4_len;
	x |= (1 << xl) - 1;
	/*  x = 10.255.255.255  */

	if (x == y)
		match = 1;
	if (y == 0xffffffff)
		match = 1;

	if (warning)
		debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ net_ip_broadcast(): warning: broadcast to "
		    "0x%08x, expecting broadcast to 0x%08x or "
		    "0xffffffff ]", y, x);

	/*  Cut off overflowing tail data:  */
	if (len > 14 + packet[16]*256 + packet[17])
		len = 14 + packet[16]*256 + packet[17];

	/*  Check for known packets:  */
	if (packet[14] == 0x45 &&			/*  IPv4  */
	    packet[23] == 0x11 &&			/*  UDP  */
	    packet[34] == 0 && packet[35] == 68 &&	/*  DHCP client  */
	    packet[36] == 0 && packet[37] == 67) {	/*  DHCP server  */
		net_ip_broadcast_dhcp(net, nic, packet, len);
		return;
	}

	/*  NetBSD seems to send this during shutdown:  */
	if (packet[14] == 0x45 &&			/*  IPv4  */
	    packet[23] == 0x11 &&			/*  UDP  */
	    packet[34] == 0xff && packet[35] == 0xff &&	/*  0xffff  */
	    packet[36] == 0x00 && packet[37] == 0x6f) {	/*  ipx-in-ip  */
		// Let's ignore for now.
		return;
	}

	/*  Unknown packet:  */
	if (ENOUGH_VERBOSITY(SUBSYS_NET, VERBOSITY_ERROR)) {
		char s[4000];
		
		snprintf(s, sizeof(s), "ver=%02x ", packet[14]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "tos=%02x ", packet[15]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "len=%02x%02x ", packet[16], packet[17]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "id=%02x%02x ",  packet[18], packet[19]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "ofs=%02x%02x ", packet[20], packet[21]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "ttl=%02x ", packet[22]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "p=%02x ", packet[23]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "sum=%02x%02x ", packet[24], packet[25]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "src=%02x%02x%02x%02x ",
		    packet[26], packet[27], packet[28], packet[29]);
		snprintf(s+strlen(s), sizeof(s)-strlen(s), "dst=%02x%02x%02x%02x ",
		    packet[30], packet[31], packet[32], packet[33]);
		for (i=34; i<len; i++) {
			if (strlen(s) > sizeof(s) - 50)
				break;
			snprintf(s+strlen(s), sizeof(s)-strlen(s), "%02x", packet[i]);
		}
		snprintf(s+strlen(s), sizeof(s)-strlen(s), " (match=%i)", match);

		debugmsg(SUBSYS_NET, "ip UNIMPLEMENTED BROADCAST", VERBOSITY_ERROR, "%s", s);
	}
}


/*
 *  net_udp_rx_avail():
 *
 *  Receive any available UDP packets (from the outside world).
 */
void net_udp_rx_avail(struct net *net, struct nic_data *nic)
{
	int received_packets_this_tick = 0;
	int max_packets_this_tick = 200;
	int con_id;

	for (con_id=0; con_id<MAX_UDP_CONNECTIONS; con_id++) {
		ssize_t res;
		unsigned char buf[66000];
		unsigned char udp_data[66008];
		struct sockaddr_in from;
		socklen_t from_len = sizeof(from);
		int ip_len, udp_len;
		struct ethernet_packet_link *lp;
		int max_per_packet;
		int bytes_converted = 0;
		int this_packets_data_length;
		int fragment_ofs = 0;

		if (received_packets_this_tick > max_packets_this_tick)
			break;

		if (!net->udp_connections[con_id].in_use)
			continue;

		if (net->udp_connections[con_id].socket < 0) {
			fatal_error(SUBSYS_NET, "net_ip", "INTERNAL ERROR in net.c, udp socket < 0 "
			    "but in use?\n");
			continue;
		}

		res = recvfrom(net->udp_connections[con_id].socket, (char *) buf,
		    sizeof(buf), 0, (struct sockaddr *)&from, &from_len);

		/*  No more incoming UDP on this connection?  */
		if (res < 0)
			continue;

		net->timestamp ++;
		net->udp_connections[con_id].last_used_timestamp =
		    net->timestamp;

		net->udp_connections[con_id].udp_id ++;

		/*
		 *  Special case for the nameserver:  If a UDP packet is
		 *  received from the nameserver (if the nameserver's IP is
		 *  known), fake it so that it comes from the gateway instead.
		 */
		if (net->udp_connections[con_id].fake_ns) {
			memcpy(((unsigned char *)(&from))+4,
			    &net->gateway_ipv4_addr[0], 4);
			res = net_dns_synth_ptr(buf, res, sizeof(buf));
		}

		/*
		 *  We now have a UDP packet of size 'res' which we need
		 *  turn into one or more ethernet packets for the emulated
		 *  operating system.  Ethernet packets are at most 1518
		 *  bytes long. With some margin, that means we can have
		 *  about 1500 bytes per packet.
		 *
		 *	Ethernet = 14 bytes
		 *	IP = 20 bytes
		 *	(UDP = 8 bytes + data)
		 *
		 *  So data can be at most max_per_packet - 34. For UDP
		 *  fragments, each multiple should (?) be a multiple of
		 *  8 bytes, except the last which doesn't have any such
		 *  restriction.
		 */
		max_per_packet = 1500;

		/*  UDP:  */
		udp_len = res + 8;
		/*  from[2..3] = outside_udp_port  */
		udp_data[0] = ((unsigned char *)&from)[2];
		udp_data[1] = ((unsigned char *)&from)[3];
		udp_data[2] = (net->udp_connections[con_id].
		    inside_udp_port >> 8) & 0xff;
		udp_data[3] = net->udp_connections[con_id].
		    inside_udp_port & 0xff;
		udp_data[4] = udp_len >> 8;
		udp_data[5] = udp_len & 0xff;
		udp_data[6] = 0;
		udp_data[7] = 0;
		memcpy(udp_data + 8, buf, res);
		/*
		 *  TODO:  UDP checksum, if necessary. At least NetBSD
		 *  and OpenBSD accept UDP packets with 0x0000 in the
		 *  checksum field anyway.
		 */

		while (bytes_converted < udp_len) {
			this_packets_data_length = udp_len - bytes_converted;

			/*  Do we need to fragment?  */
			if (this_packets_data_length > max_per_packet-34) {
				this_packets_data_length =
				    max_per_packet - 34;
				while (this_packets_data_length & 7)
					this_packets_data_length --;
			}

			ip_len = 20 + this_packets_data_length;

			lp = net_allocate_ethernet_packet_link(net, nic,
			    14 + 20 + this_packets_data_length);

			/*  Ethernet header:  */
			memcpy(lp->data + 0, net->udp_connections[con_id].
			    ethernet_address, 6);
			memcpy(lp->data + 6, net->gateway_ethernet_addr, 6);
			lp->data[12] = 0x08;	/*  IP = 0x0800  */
			lp->data[13] = 0x00;

			/*  IP header:  */
			lp->data[14] = 0x45;	/*  ver  */
			lp->data[15] = 0x00;	/*  tos  */
			lp->data[16] = ip_len >> 8;
			lp->data[17] = ip_len & 0xff;
			lp->data[18] = net->udp_connections[con_id].udp_id >> 8;
			lp->data[19] = net->udp_connections[con_id].udp_id
			    & 0xff;
			lp->data[20] = (fragment_ofs >> 8);
			if (bytes_converted + this_packets_data_length
			    < udp_len)
				lp->data[20] |= 0x20;	/*  More fragments  */
			lp->data[21] = fragment_ofs & 0xff;
			lp->data[22] = 0x40;	/*  ttl  */
			lp->data[23] = 17;	/*  p = UDP  */
			lp->data[26] = ((unsigned char *)&from)[4];
			lp->data[27] = ((unsigned char *)&from)[5];
			lp->data[28] = ((unsigned char *)&from)[6];
			lp->data[29] = ((unsigned char *)&from)[7];
			memcpy(lp->data + 30, net->udp_connections[con_id].
			    inside_ip_address, 4);
			net_ip_checksum(lp->data + 14, 10, 20);

			memcpy(lp->data+34, udp_data + bytes_converted,
			    this_packets_data_length);

			bytes_converted += this_packets_data_length;
			fragment_ofs = bytes_converted / 8;

			received_packets_this_tick ++;
		}

		/*  This makes sure we check this connection AGAIN
		    for more incoming UDP packets, before moving to the
		    next connection:  */
		con_id --;
	}
}


/*
 *  net_tcp_rx_avail():
 *
 *  Receive any available TCP packets (from the outside world).
 */
void net_tcp_rx_avail(struct net *net, struct nic_data *nic)
{
	int received_packets_this_tick = 0;
	int max_packets_this_tick = 200;
	int con_id;

	for (con_id=0; con_id<MAX_TCP_CONNECTIONS; con_id++) {
		ssize_t res, res2;
		fd_set rfds, efds;
		struct timeval tv;
		int sock;

		if (received_packets_this_tick > max_packets_this_tick)
			break;

		if (!net->tcp_connections[con_id].in_use)
			continue;

		if (net->tcp_connections[con_id].socket < 0) {
			fatal_error(SUBSYS_NET, "net_ip", "INTERNAL ERROR in net.c, tcp socket < 0"
			    " but in use?\n");
			continue;
		}

		if (net->tcp_connections[con_id].incoming_buf == NULL)
			CHECK_ALLOCATION(net->tcp_connections[con_id].
			    incoming_buf = (unsigned char *) malloc(TCP_INCOMING_BUF_LEN));

		/*  Our FIN (at outside_seqnr) can be lost, as data can:
		    send it again until the guest acknowledges it.  */
		if (net->tcp_connections[con_id].state ==
		    TCP_OUTSIDE_DISCONNECTED) {
			struct tcp_connection *c = &net->tcp_connections[con_id];
			int64_t now = tcp_now_ms();
			if ((int32_t) (c->inside_acknr - c->outside_seqnr) < 1 &&
			    now - c->last_progress_ms > TCP_RETRANSMIT_MS) {
				c->last_progress_ms = now;
				net_ip_tcp_connectionreply(net, nic, con_id, 0,
				    NULL, 0, 0);
			}
		}

		if (net->tcp_connections[con_id].state >=
		    TCP_OUTSIDE_DISCONNECTED)
			continue;

		/*
		 *  Is the socket available for output?  While connecting,
		 *  this (or, on Windows, an exception) means that connect()
		 *  has finished, and SO_ERROR tells whether it succeeded.
		 */
		sock = net->tcp_connections[con_id].socket;
		FD_ZERO(&rfds);		/*  write  */
		FD_SET(sock, &rfds);
		FD_ZERO(&efds);
		FD_SET(sock, &efds);
		tv.tv_sec = tv.tv_usec = 0;
		res = select(sock + 1, NULL, &rfds, &efds, &tv);

		if (net->tcp_connections[con_id].state ==
		    TCP_OUTSIDE_TRYINGTOCONNECT) {
			int err = 0;
			socklen_t errlen = sizeof(err);

			if (net->tcp_connections[con_id].connect_error != 0)
				err = net->tcp_connections[con_id].
				    connect_error;
			else if (res < 1)
				continue;
			else if (getsockopt(sock, SOL_SOCKET, SO_ERROR,
			    (char *) &err, &errlen) != 0 && err == 0)
				err = socket_error();

			if (err != 0) {
				debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ TCP: connection %i failed (error "
				    "%i) ]\n", con_id, err);

				/*  Answer the guest's SYN with a RST:  */
				net->tcp_connections[con_id].outside_acknr =
				    net->tcp_connections[con_id].
				    inside_seqnr + 1;
				net_ip_tcp_connectionreply(net, nic, con_id,
				    0, NULL, 0, 1);
				tcp_closeconnection(net, con_id);
				continue;
			}

			net->tcp_connections[con_id].state =
			    TCP_OUTSIDE_CONNECTED;
			debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "CHANGING TO TCP_OUTSIDE_CONNECTED\n");
			net_ip_tcp_connectionreply(net, nic, con_id, 1,
			    NULL, 0, 0);
			net->tcp_connections[con_id].incoming_buf_len = 0;
			net->tcp_connections[con_id].incoming_buf_seqnr =
			    net->tcp_connections[con_id].outside_seqnr;
			net->tcp_connections[con_id].last_progress_ms =
			    tcp_now_ms();
		}

		{
			struct tcp_connection *c = &net->tcp_connections[con_id];
			int64_t now = tcp_now_ms();
			int n_sent = 0;

			/*  Fill the buffer from the socket:  */
			if (!c->outside_eof &&
			    c->incoming_buf_len < TCP_INCOMING_BUF_LEN) {
				FD_ZERO(&rfds);		/*  read  */
				FD_SET(sock, &rfds);
				tv.tv_sec = tv.tv_usec = 0;
				res2 = select(sock + 1, &rfds, NULL, NULL,
				    &tv);
				if (res2 > 0) {
					res = recv(sock, (char *)
					    c->incoming_buf +
					    c->incoming_buf_len,
					    TCP_INCOMING_BUF_LEN -
					    c->incoming_buf_len, 0);
					if (res > 0)
						c->incoming_buf_len += res;
					else {
						if (res < 0)
							debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ TCP: read "
							    "error %i ]\n",
							    socket_error());
						c->outside_eof = 1;
					}
				}
			}

			/*  Nothing acknowledged for a while? Then send
			    again from the first unacknowledged byte:  */
			if (c->outside_seqnr != c->incoming_buf_seqnr &&
			    c->incoming_buf_len != 0 &&
			    now - c->last_progress_ms > TCP_RETRANSMIT_MS) {
				debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "[ TCP: resending from %u (was at "
				    "%u) ]\n", c->incoming_buf_seqnr,
				    c->outside_seqnr);
				c->outside_seqnr = c->incoming_buf_seqnr;
				c->last_progress_ms = now;
			}

			/*  Send what the guest's window allows (with a
			    closed window, one segment at a time, as a
			    probe, after each timeout):  */
			while (n_sent < TCP_SEGMENTS_PER_ROUND) {
				uint32_t off = c->outside_seqnr -
				    c->incoming_buf_seqnr;
				uint32_t seg = TCP_SEGMENT_SIZE;

				if (off >= (uint32_t) c->incoming_buf_len)
					break;
				if (seg > c->incoming_buf_len - off)
					seg = c->incoming_buf_len - off;
				if (off + seg > c->inside_window) {
					if (off != 0)
						break;
					if (c->inside_window > 0 &&
					    c->inside_window < seg)
						seg = c->inside_window;
				}
				if (off == 0)
					c->last_progress_ms = now;
				net_ip_tcp_connectionreply(net, nic, con_id,
				    0, c->incoming_buf + off, seg, 0);
				n_sent ++;
				received_packets_this_tick ++;
			}

			/*  The outside has closed, and the guest has all of
			    it: send a FIN.  */
			if (c->outside_eof && c->incoming_buf_len == 0) {
				c->state = TCP_OUTSIDE_DISCONNECTED;
				c->last_progress_ms = now;
				debugmsg(SUBSYS_NET, "net_ip", VERBOSITY_DEBUG, "CHANGING TO TCP_OUTSIDE_DISCONNECTED\n");
				net_ip_tcp_connectionreply(net, nic, con_id, 0,
				    NULL, 0, 0);
			}

			if (n_sent == 0 && !c->outside_eof)
				continue;
		}

		net->timestamp ++;
		net->tcp_connections[con_id].last_used_timestamp =
		    net->timestamp;
	}
}


