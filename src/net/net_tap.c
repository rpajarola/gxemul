/*  
 *  Copyright (C) 2020 Jason R. Thorpe.  All rights reserved.
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
 *  Support for Ethernet tap interfaces.
 *
 *  A single tap instance is used for the entire simulated network.
 *  We treat this as sort of virtual Ethernet switch, with the tap
 *  being the upstream port.  This is very simple, conceptually, and
 *  fits in nicely with the rest of the network simulation model in
 *  GXemul.
 *
 *  Use of the tap interface is completely optional, but if it is used
 *  then all of the virtual IP network support is bypassed completely.
 */

#include <sys/types.h>
#ifdef _WIN32
#define	WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include "thirdparty/pcap/pcap.h"
#else
#include <sys/ioctl.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "misc.h"
#include "net.h"

#ifdef _WIN32

/*
 *  Npcap (wpcap.dll) loaded at runtime on Windows, so that Npcap is only
 *  needed when a tap / bridge interface is used.
 */
static int (*p_pcap_findalldevs)(pcap_if_t **, char *);
static void (*p_pcap_freealldevs)(pcap_if_t *);
static pcap_t *(*p_pcap_open_live)(const char *, int, int, int, char *);
static int (*p_pcap_sendpacket)(pcap_t *, const u_char *, int);
static char *(*p_pcap_geterr)(pcap_t *);
static int (*p_pcap_setnonblock)(pcap_t *, int, char *);
static int (*p_pcap_next_ex)(pcap_t *, struct pcap_pkthdr **,
	const u_char **);

static bool npcap_load(void)
{
	static HMODULE lib;
	char sysdir[MAX_PATH], path[MAX_PATH + 32];

	if (lib != NULL)
		return true;

	if (GetSystemDirectoryA(sysdir, sizeof(sysdir)) > 0) {
		snprintf(path, sizeof(path), "%s\\Npcap\\wpcap.dll", sysdir);
		lib = LoadLibraryExA(path, NULL,
		    LOAD_WITH_ALTERED_SEARCH_PATH);
	}
	if (lib == NULL)
		lib = LoadLibraryA("wpcap.dll");
	if (lib == NULL)
		return false;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
	p_pcap_findalldevs = (int (*)(pcap_if_t **, char *))
	    GetProcAddress(lib, "pcap_findalldevs");
	p_pcap_freealldevs = (void (*)(pcap_if_t *))
	    GetProcAddress(lib, "pcap_freealldevs");
	p_pcap_open_live = (pcap_t *(*)(const char *, int, int, int, char *))
	    GetProcAddress(lib, "pcap_open_live");
	p_pcap_sendpacket = (int (*)(pcap_t *, const unsigned char *, int))
	    GetProcAddress(lib, "pcap_sendpacket");
	p_pcap_geterr = (char *(*)(pcap_t *))
	    GetProcAddress(lib, "pcap_geterr");
	p_pcap_setnonblock = (int (*)(pcap_t *, int, char *))
	    GetProcAddress(lib, "pcap_setnonblock");
	p_pcap_next_ex = (int (*)(pcap_t *, struct pcap_pkthdr **,
	    const unsigned char **)) GetProcAddress(lib, "pcap_next_ex");
#pragma GCC diagnostic pop

	if (p_pcap_findalldevs == NULL || p_pcap_freealldevs == NULL ||
	    p_pcap_open_live == NULL || p_pcap_sendpacket == NULL ||
	    p_pcap_geterr == NULL || p_pcap_setnonblock == NULL ||
	    p_pcap_next_ex == NULL) {
		FreeLibrary(lib);
		lib = NULL;
		return false;
	}
	return true;
}

/*
 *  The part of an Npcap device name after "\Device\NPF_", if any:
 */
static const char *npcap_short_name(const char *name)
{
	const char *prefix = "\\Device\\NPF_";

	if (strncasecmp(name, prefix, strlen(prefix)) == 0)
		return name + strlen(prefix);
	return name;
}

static bool str_contains_ci(const char *haystack, const char *needle)
{
	size_t n = strlen(needle), i;

	if (n == 0)
		return false;
	for (i = 0; haystack[i] != '\0'; i++)
		if (strncasecmp(haystack + i, needle, n) == 0)
			return true;
	return false;
}

#endif	/*  _WIN32  */


/*
 *  net_tap_list():
 *
 *  Lists the available Npcap network interfaces on Windows.  Returns the
 *  number of interfaces (>= 0), or -1 if Npcap is not available (and on
 *  non-Windows hosts).  The caller frees the strings and arrays.
 */
int net_tap_list(char ***names, char ***descriptions)
{
	*names = NULL;
	*descriptions = NULL;

#ifdef _WIN32
	pcap_if_t *alldevs, *d;
	char errbuf[PCAP_ERRBUF_SIZE];
	int n = 0;

	if (!npcap_load())
		return -1;
	if (p_pcap_findalldevs(&alldevs, errbuf) == -1)
		return -1;

	for (d = alldevs; d != NULL; d = d->next) {
		if (d->flags & PCAP_IF_LOOPBACK)
			continue;
		CHECK_ALLOCATION(*names = (char **) realloc(*names,
		    sizeof(char *) * (n + 1)));
		CHECK_ALLOCATION(*descriptions = (char **) realloc(
		    *descriptions, sizeof(char *) * (n + 1)));
		CHECK_ALLOCATION((*names)[n] = strdup(
		    npcap_short_name(d->name)));
		CHECK_ALLOCATION((*descriptions)[n] = strdup(
		    d->description != NULL && d->description[0] != '\0' ?
		    d->description : (*names)[n]));
		n++;
	}
	p_pcap_freealldevs(alldevs);
	return n;
#else
	return -1;
#endif
}


/*
 *  net_tap_rx_for_nic():
 *
 *  Receive a packet from the virtual Ethernet switch for this NIC.
 */
static void net_tap_rx_for_nic(struct net *net, struct nic_data *nic,
	const unsigned char *buf, ssize_t size)
{
	struct ethernet_packet_link *lp;

	/*
	 * We should deliver to the interface if:
	 *
	 * ==> The interface is in promiscuous mode.
	 *  -- or --
	 * ==> The packet is broadcast or multicast (the emulated device
	 *     can further apply a multicast filter if it wishes).
	 *  -- or --
	 * ==> The destination MAC address matches the NIC MAC address.
	 *
	 * Note that normally a switch would not know if an interface
	 * is in promiscuous mode, but this is a bit of extra magic
	 * we implement because we can for the sake of convenience.
	 * Also, some emulated interfaces may want to see all packets
	 * so as to implement their own filtering logic.
	 *
	 * Also note that testing for multicast also catches the broadcast
	 * case.
	 */

	if (nic->promiscuous_mode ||
	    net_ether_multicast(buf) || net_ether_eq(nic->mac_address, buf)) {
		lp = net_allocate_ethernet_packet_link(net, nic, (int)size);
		memcpy(lp->data, buf, size);
	}
}

/*
 *  net_tap_rx_avail():
 *
 *  We poll the net-shared tap device and link up any available packets to
 *  their destination interfaces, acting like a virtual Ethernet switch.
 */
void net_tap_rx_avail(struct net *net)
{
	int received_packets_this_tick = 0;
	int max_packets_this_tick = 200;

	for (;;) {
#ifdef _WIN32
		struct pcap_pkthdr *header;
		const unsigned char *pkt_data;
		ssize_t bytes_read;
		int i;

		if (received_packets_this_tick > max_packets_this_tick)
			break;

		if (p_pcap_next_ex((pcap_t *) net->tap_pcap, &header,
		    &pkt_data) != 1)
			break;

		received_packets_this_tick++;
		bytes_read = header->caplen;
		if (bytes_read < 20)
			continue;

		for (i = 0; i < net->n_nics; i++) {
			net_tap_rx_for_nic(net, net->nic_data[i],
			    pkt_data, bytes_read);
		}
#else
		unsigned char buf[1518];
		ssize_t bytes_read;
		int i;

		if (received_packets_this_tick > max_packets_this_tick)
			break;

		/* Read one packet from the tap device. */
		bytes_read = read(net->tap_fd, buf, sizeof(buf));

		if (bytes_read < 0) {
			/* No more packets available on the tap. */
			break;
		}

		received_packets_this_tick++;

		/*
		 * Drop runt packets now; allow other layers to assume
		 * valid Ethernet frames.  This really should be 64, but
		 * 20 is used in the transmit path.
		 */
		if (bytes_read < 20)
			continue;

		for (i = 0; i < net->n_nics; i++) {
			net_tap_rx_for_nic(net, net->nic_data[i],
			    buf, bytes_read);
		}
#endif
	}
}

/*
 *  net_tap_tx():
 *
 *  Transmit an ethernet packet, as seen from the emulated ethernet controller,
 *  to the net-shared tap device.  Even if the packet is destined only for
 *  a NIC on the local virtual Ethernet switch, we always send it to the
 *  tap device so that the host system can monitor traffic by running tcpdump
 *  on its view of the tap.
 */
void net_tap_tx(struct net *net, struct nic_data *nic,
	unsigned char *packet, int len)
{
	int i;

	for (i = 0; i < net->n_nics; i++) {
		if (nic == net->nic_data[i])
			continue;
		net_tap_rx_for_nic(net, net->nic_data[i], packet, len);
	}

	/*
	 * Don't bother checking for errors here.  The tap driver in the
	 * kernel will either take the entire packet or none of it, and
	 * there isn't any useful error recovery for us anyway.
	 */
#ifdef _WIN32
	(void) p_pcap_sendpacket((pcap_t *) net->tap_pcap, packet, len);
#else
	int ignored = write(net->tap_fd, packet, len);
	(void)ignored;
#endif
}

/*
 *  net_tap_init():
 *
 *  Initialize the tap interface.  Returns true if successful, false otherwise.
 */
bool net_tap_init(struct net *net, const char *tapdev)
{
#ifdef _WIN32
	pcap_if_t *alldevs, *d, *chosen = NULL;
	pcap_t *pcap;
	char errbuf[PCAP_ERRBUF_SIZE], *end;
	long num;
	int idx;

	if (!npcap_load()) {
		debugmsg(SUBSYS_NET, "tap", VERBOSITY_ERROR,
		    "Npcap (wpcap.dll) is not installed");
		return false;
	}

	if (p_pcap_findalldevs(&alldevs, errbuf) == -1) {
		debugmsg(SUBSYS_NET, "tap", VERBOSITY_ERROR,
		    "pcap_findalldevs: %s", errbuf);
		return false;
	}

	num = strtol(tapdev, &end, 10);
	for (d = alldevs, idx = 0; d != NULL; d = d->next) {
		if (d->flags & PCAP_IF_LOOPBACK)
			continue;
		idx++;
		if (strcasecmp(d->name, tapdev) == 0 ||
		    strcasecmp(npcap_short_name(d->name), tapdev) == 0 ||
		    (d->description != NULL &&
		    strcasecmp(d->description, tapdev) == 0) ||
		    (*tapdev != '\0' && *end == '\0' && num == idx)) {
			chosen = d;
			break;
		}
	}
	if (chosen == NULL && strcasecmp(tapdev, "list") != 0) {
		for (d = alldevs; d != NULL; d = d->next) {
			if (d->flags & PCAP_IF_LOOPBACK)
				continue;
			if (str_contains_ci(d->name, tapdev) ||
			    (d->description != NULL &&
			    str_contains_ci(d->description, tapdev))) {
				chosen = d;
				break;
			}
		}
	}

	if (chosen == NULL) {
		if (strcasecmp(tapdev, "list") != 0)
			debugmsg(SUBSYS_NET, "tap", VERBOSITY_ERROR,
			    "unknown Npcap interface '%s'", tapdev);
		fprintf(stderr, "Available Npcap interfaces:\n");
		for (d = alldevs, idx = 0; d != NULL; d = d->next) {
			if (d->flags & PCAP_IF_LOOPBACK)
				continue;
			idx++;
			fprintf(stderr, "  %i. %s (%s)\n", idx,
			    npcap_short_name(d->name),
			    d->description != NULL ? d->description :
			    "no description");
		}
		if (idx == 0)
			fprintf(stderr, "  (none)\n");
		p_pcap_freealldevs(alldevs);
		return false;
	}

	pcap = p_pcap_open_live(chosen->name, 65536, 1, 1, errbuf);
	if (pcap == NULL) {
		debugmsg(SUBSYS_NET, "tap", VERBOSITY_ERROR,
		    "unable to open Npcap interface '%s': %s",
		    chosen->name, errbuf);
		p_pcap_freealldevs(alldevs);
		return false;
	}

	if (p_pcap_setnonblock(pcap, 1, errbuf) == -1) {
		debugmsg(SUBSYS_NET, "tap", VERBOSITY_ERROR,
		    "unable to set non-blocking mode on '%s': %s",
		    chosen->name, errbuf);
	}

	CHECK_ALLOCATION(net->tapdev = strdup(chosen->description != NULL &&
	    chosen->description[0] != '\0' ? chosen->description :
	    npcap_short_name(chosen->name)));
	net->tap_pcap = pcap;
	p_pcap_freealldevs(alldevs);

	return true;
#else
	int fd;
	int one = 1;

	fd = open(tapdev, O_RDWR);
	if (fd < 0) {
		debugmsg(SUBSYS_NET, "tap", VERBOSITY_ERROR,
		    "unable to open tap device '%s': %s",
		    tapdev, strerror(errno));
		return false;
	}

	if (ioctl(fd, FIONBIO, &one) < 0) {
		debugmsg(SUBSYS_NET, "tap", VERBOSITY_ERROR,
		    "unable to set non-blocking mode on "
		    "tap device '%s': %s", tapdev, strerror(errno));
		close(fd);
		return false;
	}

	net->tapdev = strdup(tapdev);
	net->tap_fd = fd;

	return true;
#endif
}

