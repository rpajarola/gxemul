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
#else
#include <sys/ioctl.h>
#include <dlfcn.h>
#endif
#include "thirdparty/pcap/pcap.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "misc.h"
#include "net.h"


/*
 *  libpcap (Npcap's wpcap.dll on Windows), loaded at runtime, so that it is
 *  only needed when the network is bridged to a host interface:
 */
static int (*p_pcap_findalldevs)(pcap_if_t **, char *);
static void (*p_pcap_freealldevs)(pcap_if_t *);
static pcap_t *(*p_pcap_open_live)(const char *, int, int, int, char *);
static void (*p_pcap_close)(pcap_t *);
static int (*p_pcap_sendpacket)(pcap_t *, const u_char *, int);
static char *(*p_pcap_geterr)(pcap_t *);
static int (*p_pcap_setnonblock)(pcap_t *, int, char *);
static int (*p_pcap_next_ex)(pcap_t *, struct pcap_pkthdr **,
	const u_char **);

#ifdef _WIN32
#define	PCAP_NAME	"Npcap (wpcap.dll)"
#else
#define	PCAP_NAME	"libpcap"
#endif

static void *pcap_sym(void *lib, const char *name)
{
#ifdef _WIN32
	return (void *) GetProcAddress((HMODULE) lib, name);
#else
	return dlsym(lib, name);
#endif
}

static bool pcap_load(void)
{
	static void *lib;
#ifdef _WIN32
	char sysdir[MAX_PATH], path[MAX_PATH + 32];
#else
	static const char *names[] = {
#ifdef __APPLE__
	    "/usr/lib/libpcap.A.dylib", "libpcap.A.dylib", "libpcap.dylib",
#else
	    "libpcap.so.1", "libpcap.so.0.8", "libpcap.so",
#endif
	    NULL };
	int i;
#endif

	if (lib != NULL)
		return true;

#ifdef _WIN32
	if (GetSystemDirectoryA(sysdir, sizeof(sysdir)) > 0) {
		snprintf(path, sizeof(path), "%s\\Npcap\\wpcap.dll", sysdir);
		lib = (void *) LoadLibraryExA(path, NULL,
		    LOAD_WITH_ALTERED_SEARCH_PATH);
	}
	if (lib == NULL)
		lib = (void *) LoadLibraryA("wpcap.dll");
#else
	for (i = 0; lib == NULL && names[i] != NULL; i++)
		lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
#endif
	if (lib == NULL)
		return false;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#ifdef _WIN32
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
	p_pcap_findalldevs = (int (*)(pcap_if_t **, char *))
	    pcap_sym(lib, "pcap_findalldevs");
	p_pcap_freealldevs = (void (*)(pcap_if_t *))
	    pcap_sym(lib, "pcap_freealldevs");
	p_pcap_open_live = (pcap_t *(*)(const char *, int, int, int, char *))
	    pcap_sym(lib, "pcap_open_live");
	p_pcap_close = (void (*)(pcap_t *)) pcap_sym(lib, "pcap_close");
	p_pcap_sendpacket = (int (*)(pcap_t *, const unsigned char *, int))
	    pcap_sym(lib, "pcap_sendpacket");
	p_pcap_geterr = (char *(*)(pcap_t *)) pcap_sym(lib, "pcap_geterr");
	p_pcap_setnonblock = (int (*)(pcap_t *, int, char *))
	    pcap_sym(lib, "pcap_setnonblock");
	p_pcap_next_ex = (int (*)(pcap_t *, struct pcap_pkthdr **,
	    const unsigned char **)) pcap_sym(lib, "pcap_next_ex");
#pragma GCC diagnostic pop

	if (p_pcap_findalldevs == NULL || p_pcap_freealldevs == NULL ||
	    p_pcap_open_live == NULL || p_pcap_close == NULL ||
	    p_pcap_sendpacket == NULL || p_pcap_geterr == NULL ||
	    p_pcap_setnonblock == NULL || p_pcap_next_ex == NULL) {
#ifdef _WIN32
		FreeLibrary((HMODULE) lib);
#else
		dlclose(lib);
#endif
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

/*
 *  Interfaces which can be bridged: not loopback, and on Linux and macOS,
 *  not the pseudo interfaces which libpcap also lists (which aren't
 *  Ethernet):
 */
static bool pcap_usable(const pcap_if_t *d)
{
#ifndef _WIN32
	static const char *skip[] = { "any", "nflog", "nfqueue", "dbus",
	    "bluetooth", "usbmon", "utun", "gif", "stf", "awdl", "llw",
	    "anpi", "pktap", "ap1", NULL };
	int i;

	for (i = 0; skip[i] != NULL; i++)
		if (strncmp(d->name, skip[i], strlen(skip[i])) == 0)
			return false;
#endif
	return !(d->flags & PCAP_IF_LOOPBACK);
}

/*  The interface which a tapdev setting names, or NULL:  */
static pcap_if_t *pcap_find(pcap_if_t *alldevs, const char *tapdev)
{
	pcap_if_t *d;
	char *end;
	long num = strtol(tapdev, &end, 10);
	int idx = 0;

	for (d = alldevs; d != NULL; d = d->next) {
		if (!pcap_usable(d))
			continue;
		idx++;
		if (strcasecmp(d->name, tapdev) == 0 ||
		    strcasecmp(npcap_short_name(d->name), tapdev) == 0 ||
		    (d->description != NULL &&
		    strcasecmp(d->description, tapdev) == 0) ||
		    (*tapdev != '\0' && *end == '\0' && num == idx))
			return d;
	}
	if (strcasecmp(tapdev, "list") == 0)
		return NULL;
	for (d = alldevs; d != NULL; d = d->next)
		if (pcap_usable(d) && (str_contains_ci(d->name, tapdev) ||
		    (d->description != NULL &&
		    str_contains_ci(d->description, tapdev))))
			return d;
	return NULL;
}

/*  How to get permission to capture, if pcap_open_live() failed:  */
static const char *pcap_permission_hint(const char *errbuf)
{
#ifdef _WIN32
	(void) errbuf;
	return "";
#else
	if (strstr(errbuf, "ermission") == NULL &&
	    strstr(errbuf, "not permitted") == NULL)
		return "";
#ifdef __APPLE__
	return " (gxemul needs to be able to read /dev/bpf*: install "
	    "Wireshark's ChmodBPF, or run gxemul with sudo)";
#else
	return " (gxemul needs the CAP_NET_RAW capability: sudo setcap "
	    "cap_net_raw,cap_net_admin=eip gxemul, or run it with sudo)";
#endif
#endif
}


/*
 *  net_tap_list():
 *
 *  Lists the host's network interfaces which the network can be bridged
 *  to (with libpcap, or Npcap on Windows).  Returns the number of
 *  interfaces (>= 0), or -1 if libpcap is not available.  The caller frees
 *  the strings and arrays.
 */
int net_tap_list(char ***names, char ***descriptions)
{
	pcap_if_t *alldevs, *d;
	char errbuf[PCAP_ERRBUF_SIZE];
	int n = 0;

	*names = NULL;
	*descriptions = NULL;

	if (!pcap_load())
		return -1;
	if (p_pcap_findalldevs(&alldevs, errbuf) == -1)
		return -1;

	for (d = alldevs; d != NULL; d = d->next) {
		if (!pcap_usable(d))
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
}


/*
 *  net_tap_check():
 *
 *  Checks that the network can be bridged to a host interface (or tap
 *  device): returns NULL if so, or else why not (which the caller frees).
 */
char *net_tap_check(const char *tapdev)
{
	pcap_if_t *alldevs, *d;
	pcap_t *pcap;
	char errbuf[PCAP_ERRBUF_SIZE], msg[PCAP_ERRBUF_SIZE + 300];
	char *r = NULL;

	if (tapdev == NULL || tapdev[0] == '\0')
		return NULL;
#ifndef _WIN32
	if (tapdev[0] == '/') {
		int fd = open(tapdev, O_RDWR);
		if (fd >= 0) {
			close(fd);
			return NULL;
		}
		snprintf(msg, sizeof(msg), "can't open %s: %s", tapdev,
		    strerror(errno));
		CHECK_ALLOCATION(r = strdup(msg));
		return r;
	}
#endif
	if (!pcap_load()) {
		snprintf(msg, sizeof(msg), "%s is not installed", PCAP_NAME);
		CHECK_ALLOCATION(r = strdup(msg));
		return r;
	}
	if (p_pcap_findalldevs(&alldevs, errbuf) == -1) {
		snprintf(msg, sizeof(msg), "%s", errbuf);
		CHECK_ALLOCATION(r = strdup(msg));
		return r;
	}
	d = pcap_find(alldevs, tapdev);
	if (d == NULL)
		snprintf(msg, sizeof(msg), "no network interface \"%s\"",
		    tapdev);
	else if ((pcap = p_pcap_open_live(d->name, 65536, 1, 1, errbuf)) ==
	    NULL) {
		/*  (libpcap's messages often start with the name already.)  */
		const char *e = errbuf;
		size_t n = strlen(d->name);
		if (strncmp(e, d->name, n) == 0 && e[n] == ':' && e[n + 1] == ' ')
			e += n + 2;
		snprintf(msg, sizeof(msg), "can't capture on %s: %s%s",
		    npcap_short_name(d->name), e, pcap_permission_hint(errbuf));
	}
	else {
		p_pcap_close(pcap);
		msg[0] = '\0';
	}
	p_pcap_freealldevs(alldevs);
	if (msg[0] != '\0')
		CHECK_ALLOCATION(r = strdup(msg));
	return r;
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
#ifndef _WIN32
		unsigned char buf[1518];
#endif
		const unsigned char *pkt;
		ssize_t bytes_read;
		int i;

		if (received_packets_this_tick > max_packets_this_tick)
			break;

		if (net->tap_pcap != NULL) {
			struct pcap_pkthdr *header;
			if (p_pcap_next_ex((pcap_t *) net->tap_pcap, &header,
			    &pkt) != 1)
				break;
			bytes_read = header->caplen;
		} else {
#ifdef _WIN32
			break;
#else
			/* Read one packet from the tap device. */
			bytes_read = read(net->tap_fd, buf, sizeof(buf));
			if (bytes_read < 0) {
				/* No more packets available on the tap. */
				break;
			}
			pkt = buf;
#endif
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
			    pkt, bytes_read);
		}
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
	if (net->tap_pcap != NULL)
		(void) p_pcap_sendpacket((pcap_t *) net->tap_pcap, packet, len);
#ifndef _WIN32
	else {
		int ignored = write(net->tap_fd, packet, len);
		(void)ignored;
	}
#endif
}

/*
 *  net_tap_init():
 *
 *  Initialize the tap interface.  Returns true if successful, false otherwise.
 */
bool net_tap_init(struct net *net, const char *tapdev)
{
	pcap_if_t *alldevs, *d, *chosen;
	pcap_t *pcap;
	char errbuf[PCAP_ERRBUF_SIZE];
	int idx;

#ifndef _WIN32
	/*  A tap device (e.g. /dev/tap0), rather than a host interface:  */
	if (tapdev[0] == '/') {
		int fd, one = 1;

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
	}
#endif

	if (!pcap_load()) {
		debugmsg(SUBSYS_NET, "tap", VERBOSITY_ERROR,
		    "%s is not installed", PCAP_NAME);
		return false;
	}

	if (p_pcap_findalldevs(&alldevs, errbuf) == -1) {
		debugmsg(SUBSYS_NET, "tap", VERBOSITY_ERROR,
		    "pcap_findalldevs: %s", errbuf);
		return false;
	}

	chosen = pcap_find(alldevs, tapdev);
	if (chosen == NULL) {
		if (strcasecmp(tapdev, "list") != 0)
			debugmsg(SUBSYS_NET, "tap", VERBOSITY_ERROR,
			    "unknown network interface '%s'", tapdev);
		fprintf(stderr, "Available network interfaces:\n");
		for (d = alldevs, idx = 0; d != NULL; d = d->next) {
			if (!pcap_usable(d))
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
		    "unable to capture on '%s': %s%s",
		    npcap_short_name(chosen->name), errbuf,
		    pcap_permission_hint(errbuf));
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
}
