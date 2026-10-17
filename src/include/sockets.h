#ifndef	SOCKETS_H
#define	SOCKETS_H

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
 *
 *
 *  Sockets, with BSD sockets or Winsock 2 (on Windows).  Winsock has the
 *  same functions, except for closing, non-blocking mode and error codes,
 *  which are used through the definitions here.
 */

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>

#define	socket_close(s)		closesocket(s)
#define	socket_error()		WSAGetLastError()
#define	socket_clear_error()	WSASetLastError(0)

#define	SOCKET_EWOULDBLOCK	WSAEWOULDBLOCK
#define	SOCKET_EINTR		WSAEINTR
#define	SOCKET_EADDRINUSE	WSAEADDRINUSE

/*  A non-blocking connect() which hasn't finished yet:  */
#define	socket_connect_pending(e)	\
	((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS)

static inline int socket_set_nonblocking(int s)
{
	u_long one = 1;
	return ioctlsocket(s, FIONBIO, &one);
}

#else

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>

#define	socket_close(s)		close(s)
#define	socket_error()		errno
#define	socket_clear_error()	(errno = 0)

#define	SOCKET_EWOULDBLOCK	EWOULDBLOCK
#define	SOCKET_EINTR		EINTR
#define	SOCKET_EADDRINUSE	EADDRINUSE

/*  A non-blocking connect() which hasn't finished yet:  */
#define	socket_connect_pending(e)	((e) == EINPROGRESS || (e) == EINTR)

static inline int socket_set_nonblocking(int s)
{
	return fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
}

#endif

/*  EAGAIN and EWOULDBLOCK may differ:  */
#ifdef EAGAIN
#define	socket_would_block(e)	((e) == SOCKET_EWOULDBLOCK || (e) == EAGAIN)
#else
#define	socket_would_block(e)	((e) == SOCKET_EWOULDBLOCK)
#endif

#endif	/*  SOCKETS_H  */
