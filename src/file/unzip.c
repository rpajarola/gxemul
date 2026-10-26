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
 *
 *
 *  unzip_find(), unzip_extract():  A file in a zip archive (stored, or
 *  deflated, with puff.c doing the work), e.g. a ROM image in a MAME ROM
 *  set.  Names are matched without case, and without the directories the
 *  file is in inside the archive.  Zip64 archives aren't supported.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "misc.h"
#include "puff.h"


#define	ZIP_MAX_SIZE		(64 * 1048576)

#define	ZIP_END			0x06054b50
#define	ZIP_CENTRAL		0x02014b50
#define	ZIP_LOCAL		0x04034b50

#define	ZIP_STORED		0
#define	ZIP_DEFLATED		8


static uint32_t get16le(const unsigned char *p)
{
	return p[0] + (p[1] << 8);
}

static uint32_t get32le(const unsigned char *p)
{
	return p[0] + (p[1] << 8) + (p[2] << 16) + ((uint32_t) p[3] << 24);
}


/*  Reads a whole file (of at most ZIP_MAX_SIZE bytes):  */
static unsigned char *read_file(const char *path, size_t *lenp)
{
	FILE *f = fopen(path, "rb");
	unsigned char *buf;
	long len;

	if (f == NULL)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 22 ||
	    len > ZIP_MAX_SIZE || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	CHECK_ALLOCATION(buf = (unsigned char *) malloc(len));
	if (fread(buf, 1, len, f) != (size_t) len) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*lenp = len;
	return buf;
}


/*
 *  Finds name in the central directory of the archive in buf[0..len-1],
 *  and returns its entry there, or NULL.
 */
static const unsigned char *find_entry(const unsigned char *buf, size_t len,
	const char *name)
{
	size_t end, pos, cd_end;
	uint32_t i, n;

	/*  The end of central directory record, after which there is at
	    most a 65535 byte comment:  */
	for (end = len - 22; ; end--) {
		if (get32le(buf + end) == ZIP_END)
			break;
		if (end == 0 || len - end > 22 + 65535)
			return NULL;
	}
	n = get16le(buf + end + 10);
	pos = get32le(buf + end + 16);
	cd_end = pos + get32le(buf + end + 12);
	if (cd_end > end || pos > cd_end)
		return NULL;

	for (i = 0; i < n; i++) {
		const unsigned char *e = buf + pos;
		size_t nlen, base;

		if (pos + 46 > cd_end || get32le(e) != ZIP_CENTRAL)
			return NULL;
		nlen = get16le(e + 28);
		if (pos + 46 + nlen > cd_end)
			return NULL;

		/*  Without the directories:  */
		for (base = nlen; base > 0 && e[46 + base - 1] != '/'; base--)
			;
		if (nlen - base == strlen(name) && strncasecmp((const char *)
		    e + 46 + base, name, nlen - base) == 0)
			return e;

		pos += 46 + nlen + get16le(e + 30) + get16le(e + 32);
	}
	return NULL;
}


/*
 *  unzip_find():
 *
 *  Returns true if the zip archive zip has a file called name.
 */
bool unzip_find(const char *zip, const char *name)
{
	unsigned char *buf;
	size_t len;
	bool found;

	if ((buf = read_file(zip, &len)) == NULL)
		return false;
	found = find_entry(buf, len, name) != NULL;
	free(buf);
	return found;
}


/*
 *  unzip_extract():
 *
 *  Writes the file called name in the zip archive zip to the file to.
 *  Returns false (with a message, unless it isn't in the archive) if it
 *  can't.
 */
bool unzip_extract(const char *zip, const char *name, const char *to)
{
	unsigned char *buf, *out = NULL;
	const unsigned char *e, *l;
	unsigned long out_len, src_len;
	size_t len, data;
	uint32_t method, csize, usize;
	bool ok = false;
	FILE *f;

	if ((buf = read_file(zip, &len)) == NULL)
		return false;
	if ((e = find_entry(buf, len, name)) == NULL) {
		free(buf);
		return false;
	}

	method = get16le(e + 10);
	csize = get32le(e + 20);
	usize = get32le(e + 24);
	l = buf + get32le(e + 42);
	if (l + 30 > buf + len || get32le(l) != ZIP_LOCAL) {
		fprintf(stderr, "%s: corrupt zip archive\n", zip);
		goto done;
	}
	data = (l - buf) + 30 + get16le(l + 26) + get16le(l + 28);
	if (data + csize > len) {
		fprintf(stderr, "%s: truncated zip archive\n", zip);
		goto done;
	}

	CHECK_ALLOCATION(out = (unsigned char *) malloc(usize > 0 ? usize : 1));
	out_len = usize;
	src_len = csize;
	if (method == ZIP_STORED && csize == usize)
		memcpy(out, buf + data, usize);
	else if (method == ZIP_DEFLATED) {
		if (puff(out, &out_len, buf + data, &src_len) != 0 ||
		    out_len != usize) {
			fprintf(stderr, "%s: corrupt data for %s\n", zip, name);
			goto done;
		}
	} else {
		fprintf(stderr, "%s: %s is compressed in a way that isn't "
		    "supported (method %u)\n", zip, name, (unsigned) method);
		goto done;
	}
	if (crc32_update(0, out, usize) != get32le(e + 16)) {
		fprintf(stderr, "%s: bad CRC for %s\n", zip, name);
		goto done;
	}

	if ((f = fopen(to, "wb")) == NULL) {
		perror(to);
		goto done;
	}
	ok = fwrite(out, 1, usize, f) == usize;
	if (fclose(f) != 0)
		ok = false;
	if (!ok) {
		perror(to);
		remove(to);
	}

done:
	free(buf);
	free(out);
	return ok;
}
