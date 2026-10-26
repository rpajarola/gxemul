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
 *  gunzip_file():  Decompresses a gzip file (RFC 1952) into another file,
 *  with puff.c (from zlib's contrib/puff, by Mark Adler) doing the work.
 *  Several gzip members, one after another, are decompressed one after
 *  another, as gunzip does.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "misc.h"
#include "puff.h"


#define	GZIP_FHCRC	0x02
#define	GZIP_FEXTRA	0x04
#define	GZIP_FNAME	0x08
#define	GZIP_FCOMMENT	0x10


uint32_t crc32_update(uint32_t crc, const unsigned char *p, size_t n)
{
	static uint32_t table[256];

	if (table[1] == 0) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? 0xedb88320 ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
	}

	crc = ~crc;
	while (n-- > 0)
		crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}


static uint32_t get32le(const unsigned char *p)
{
	return p[0] + (p[1] << 8) + (p[2] << 16) + ((uint32_t) p[3] << 24);
}


/*
 *  Returns the length of the gzip member header at in[0..len-1], or 0 if
 *  it isn't one.
 */
static size_t gzip_header_len(const unsigned char *in, size_t len)
{
	size_t pos = 10;
	int flags;

	if (len < 10 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8)
		return 0;

	flags = in[3];

	if (flags & GZIP_FEXTRA) {
		if (pos + 2 > len)
			return 0;
		pos += 2 + in[pos] + (in[pos + 1] << 8);
	}

	if (flags & GZIP_FNAME) {
		while (pos < len && in[pos] != 0)
			pos ++;
		pos ++;
	}

	if (flags & GZIP_FCOMMENT) {
		while (pos < len && in[pos] != 0)
			pos ++;
		pos ++;
	}

	if (flags & GZIP_FHCRC)
		pos += 2;

	return pos <= len ? pos : 0;
}


/*
 *  gunzip_file():
 *
 *  Returns true on success. On failure, an error message has been printed.
 */
bool gunzip_file(const char *from, const char *to)
{
	unsigned char *in = NULL, *out = NULL;
	size_t in_len = 0, pos = 0;
	FILE *f;
	bool ok = false;
	long size;

	f = fopen(from, "rb");
	if (f == NULL) {
		perror(from);
		return false;
	}

	if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0 &&
	    fseek(f, 0, SEEK_SET) == 0) {
		CHECK_ALLOCATION(in = (unsigned char *) malloc(size));
		in_len = fread(in, 1, size, f);
	}
	fclose(f);

	f = fopen(to, "wb");
	if (f == NULL) {
		perror(to);
		free(in);
		return false;
	}

	while (pos < in_len) {
		size_t header = gzip_header_len(in + pos, in_len - pos);
		unsigned long src_len, out_len = 0;
		uint32_t expected_len;

		if (header == 0 || in_len - pos < header + 8) {
			/*  gunzip ignores trailing zeroes (padding):  */
			if (pos > 0 && in[pos] == 0)
				ok = true;
			else
				fprintf(stderr, "%s: not in gzip format\n",
				    from);
			break;
		}

		/*  The uncompressed size (mod 2^32) is at the end of the
		    member, but the end isn't known until the data has been
		    decompressed, so the size is first found with no output
		    (dest NIL):  */
		src_len = in_len - pos - header;
		if (puff(NIL, &out_len, in + pos + header, &src_len) != 0) {
			fprintf(stderr, "%s: corrupt gzip data\n", from);
			break;
		}

		if (pos + header + src_len + 8 > in_len) {
			fprintf(stderr, "%s: truncated gzip file\n", from);
			break;
		}

		expected_len = get32le(in + pos + header + src_len + 4);
		CHECK_ALLOCATION(out = (unsigned char *)
		    realloc(out, out_len > 0 ? out_len : 1));
		src_len = in_len - pos - header;
		if (puff(out, &out_len, in + pos + header, &src_len) != 0 ||
		    (uint32_t) out_len != expected_len ||
		    crc32_update(0, out, out_len) !=
		    get32le(in + pos + header + src_len)) {
			fprintf(stderr, "%s: corrupt gzip data\n", from);
			break;
		}

		if (fwrite(out, 1, out_len, f) != out_len) {
			perror(to);
			break;
		}

		pos += header + src_len + 8;
		ok = pos >= in_len;
	}

	if (fclose(f) != 0) {
		perror(to);
		ok = false;
	}

	free(in);
	free(out);
	return ok;
}
