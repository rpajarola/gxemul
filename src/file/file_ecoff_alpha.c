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
 *  Alpha ECOFF (DEC OSF/1, early NetBSD/alpha). Included from file.c.
 *
 *  Unlike MIPS ECOFF, the Alpha variant has 64-bit addresses and sizes in
 *  all headers, so it has its own loader. All fields are little-endian.
 *
 *	file header	 24 bytes
 *	a.out header	 80 bytes
 *	section headers	 64 bytes each
 *	symbolic header	144 bytes (magic 0x1992), external symbols 24 bytes
 *
 *  Compressed executables (magic 0x188) are not supported.
 */

#define	ECOFF_MAGIC_ALPHA		0x183
#define	ECOFF_MAGIC_ALPHA_COMPRESSED	0x188
#define	ECOFF_ALPHA_SYMMAGIC		0x1992

#define	ECOFF_ALPHA_FILHSZ		24
#define	ECOFF_ALPHA_AOUTSZ		80
#define	ECOFF_ALPHA_SCNHSZ		64
#define	ECOFF_ALPHA_SYMHSZ		144
#define	ECOFF_ALPHA_EXTSZ		24

/*  Section flags:  */
#define	ECOFF_STYP_NOLOAD		0x00000002
#define	ECOFF_STYP_BSS			0x00000080
#define	ECOFF_STYP_SBSS			0x00000400


static uint16_t ea_le16(const unsigned char *p)
{
	return p[0] | (p[1] << 8);
}

static uint32_t ea_le32(const unsigned char *p)
{
	return ea_le16(p) | ((uint32_t)ea_le16(p + 2) << 16);
}

static uint64_t ea_le64(const unsigned char *p)
{
	return ea_le32(p) | ((uint64_t)ea_le32(p + 4) << 32);
}


static void ea_read(FILE *f, const char *filename, off_t pos,
	unsigned char *buf, size_t len, const char *what)
{
	if (fseeko(f, pos, SEEK_SET) != 0 || fread(buf, 1, len, f) != len) {
		fprintf(stderr, "%s: Alpha ECOFF: can't read the %s\n",
		    filename, what);
		exit(1);
	}
}


/*
 *  Load the external symbols, so that they show up in traces and in the
 *  debugger.
 */
static void file_load_ecoff_alpha_symbols(struct machine *m, FILE *f,
	const char *filename, off_t symptr)
{
	unsigned char hdr[ECOFF_ALPHA_SYMHSZ];
	uint32_t iextMax, issExtMax, i;
	uint64_t cbSsExtOffset, cbExtOffset;
	unsigned char *ext;
	char *strings;
	int n = 0;

	ea_read(f, filename, symptr, hdr, sizeof(hdr), "symbolic header");
	if (ea_le16(hdr) != ECOFF_ALPHA_SYMMAGIC) {
		debug("unknown symbolic header magic 0x%04x, no symbols\n",
		    ea_le16(hdr));
		return;
	}

	issExtMax = ea_le32(hdr + 32);
	iextMax = ea_le32(hdr + 44);
	cbSsExtOffset = ea_le64(hdr + 112);
	cbExtOffset = ea_le64(hdr + 136);
	if (iextMax == 0 || issExtMax == 0)
		return;

	CHECK_ALLOCATION(strings = (char *) malloc(issExtMax + 1));
	CHECK_ALLOCATION(ext = (unsigned char *)
	    malloc((size_t)iextMax * ECOFF_ALPHA_EXTSZ));
	ea_read(f, filename, cbSsExtOffset, (unsigned char *)strings,
	    issExtMax, "external strings");
	strings[issExtMax] = '\0';
	ea_read(f, filename, cbExtOffset, ext,
	    (size_t)iextMax * ECOFF_ALPHA_EXTSZ, "external symbols");

	for (i = 0; i < iextMax; i++) {
		unsigned char *e = ext + i * ECOFF_ALPHA_EXTSZ;
		uint64_t value = ea_le64(e + 8);
		uint32_t iss = ea_le32(e + 16);

		if (value == 0 || iss >= issExtMax)
			continue;
		add_symbol_name(&m->symbol_context, value, 0, strings + iss,
		    0, -1);
		n ++;
	}

	debug("%i symbols\n", n);
	free(ext);
	free(strings);
}


/*
 *  file_load_ecoff_alpha():
 *
 *  Loads an Alpha ECOFF executable into emulated memory.
 */
static void file_load_ecoff_alpha(struct machine *m, struct memory *mem,
	char *filename, uint64_t *entrypointp, uint64_t *gpp,
	int *byte_orderp)
{
	unsigned char fh[ECOFF_ALPHA_FILHSZ + ECOFF_ALPHA_AOUTSZ];
	unsigned char *ah = fh + ECOFF_ALPHA_FILHSZ;
	unsigned char sh[ECOFF_ALPHA_SCNHSZ];
	unsigned char buf[8192];
	uint16_t f_magic, f_nscns, f_opthdr;
	uint64_t f_symptr, a_entry, a_gp, end_addr = 0;
	off_t scnpos;
	FILE *f;
	int i;

	f = fopen(filename, "rb");
	if (f == NULL) {
		perror(filename);
		exit(1);
	}

	ea_read(f, filename, 0, fh, sizeof(fh), "file and a.out headers");

	f_magic = ea_le16(fh);
	if (f_magic == ECOFF_MAGIC_ALPHA_COMPRESSED) {
		fprintf(stderr, "%s: compressed Alpha ECOFF executables are not"
		    " supported yet\n", filename);
		exit(1);
	}
	f_nscns = ea_le16(fh + 2);
	f_symptr = ea_le64(fh + 8);
	f_opthdr = ea_le16(fh + 20);

	a_entry = ea_le64(ah + 32);
	a_gp = ea_le64(ah + 72);

	debug("Alpha ECOFF, %i sections, a.out magic 0%o, entry 0x%016"
	    PRIx64 ", gp 0x%016" PRIx64 "\n", f_nscns, ea_le16(ah),
	    a_entry, a_gp);
	debug("text 0x%" PRIx64 " bytes @ 0x%016" PRIx64 ", data 0x%" PRIx64
	    " bytes @ 0x%016" PRIx64 ", bss 0x%" PRIx64 " bytes @ 0x%016"
	    PRIx64 "\n", ea_le64(ah + 8), ea_le64(ah + 40), ea_le64(ah + 16),
	    ea_le64(ah + 48), ea_le64(ah + 24), ea_le64(ah + 56));

	scnpos = ECOFF_ALPHA_FILHSZ + f_opthdr;
	for (i = 0; i < f_nscns; i++) {
		char name[9];
		uint64_t s_vaddr, s_size, s_scnptr, done = 0;
		uint32_t s_flags;

		ea_read(f, filename, scnpos + i * ECOFF_ALPHA_SCNHSZ, sh,
		    sizeof(sh), "section headers");
		memcpy(name, sh, 8);
		name[8] = '\0';
		s_vaddr = ea_le64(sh + 16);
		s_size = ea_le64(sh + 24);
		s_scnptr = ea_le64(sh + 32);
		s_flags = ea_le32(sh + 60);

		debug("section %-8s 0x%08" PRIx64 " bytes @ 0x%016" PRIx64
		    ", file offset 0x%" PRIx64 ", flags 0x%x\n", name, s_size,
		    s_vaddr, s_scnptr, s_flags);

		if (s_size == 0 || (s_flags & ECOFF_STYP_NOLOAD))
			continue;
		if (s_vaddr + s_size > end_addr)
			end_addr = s_vaddr + s_size;

		/*  bss is zero already, as is all of emulated memory.  */
		if (s_scnptr == 0 ||
		    (s_flags & (ECOFF_STYP_BSS | ECOFF_STYP_SBSS)))
			continue;

		if (fseeko(f, s_scnptr, SEEK_SET) != 0) {
			fprintf(stderr, "%s: can't seek to section %s\n",
			    filename, name);
			exit(1);
		}
		while (done < s_size) {
			size_t len = sizeof(buf);
			if (len > s_size - done)
				len = s_size - done;
			if (fread(buf, 1, len, f) != len) {
				fprintf(stderr, "%s: section %s is truncated\n",
				    filename, name);
				exit(1);
			}
			m->cpus[0]->memory_rw(m->cpus[0], mem, s_vaddr + done,
			    buf, len, MEM_WRITE, NO_EXCEPTIONS);
			done += len;
		}
	}

	if (f_symptr != 0)
		file_load_ecoff_alpha_symbols(m, f, filename, f_symptr);

	fclose(f);

	*entrypointp = a_entry;
	*gpp = a_gp;
	*byte_orderp = EMUL_LITTLE_ENDIAN;
	m->file_loaded_end_addr = end_addr;

	n_executables_loaded ++;
}
