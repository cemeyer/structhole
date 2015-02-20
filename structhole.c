/*
 * Copyright (c) 2014, EMC Isilon storage division
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <sys/types.h>
#include <sys/stat.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#ifdef __linux__
#define __dead2
#else
#include <bsd/sys/cdefs.h>
#endif

#include <dwarf.h>
#include <elf.h>
#include <elfutils/libdw.h>
#include <elfutils/libdwfl.h>
#include <libelf.h>

/* Bizarrely, elfutils doesn't make error values publically visible. */
enum {
	DWARF_E_NO_REGFILE = 3,
};

static const char *argv0, *structname, *binary;
static size_t cachelinesize = 64;
static size_t pointer_size = sizeof(void *);

static void
usage(void)
{

	printf("Usage: %s <structname> <binary>\n", argv0);
	exit(EX_USAGE);
}

static void __dead2
_dwarf_err(const char *fn, unsigned ln, const char *func, int ex, int error,
    const char *fmt, ...)
{
	va_list ap;

	printf("%s:%u: %s: ", fn, ln, func);

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);

	/*
	 * Bizarrely, this clears the global DWARF error, breaking
	 * dwarf_errmsg(-1):
	 */
	if (error == -1)
		error = dwarf_errno();
	printf(": %s(%d)\n", dwarf_errmsg(error), error);
	exit(ex);
}
#define dwarf_err(ex, fmt, ...) \
    _dwarf_err(__FILE__, __LINE__, __func__, (ex), (-1), (fmt), ##__VA_ARGS__)
#define dwarf_err_errno(ex, no, fmt, ...) \
    _dwarf_err(__FILE__, __LINE__, __func__, (ex), (no), (fmt), ##__VA_ARGS__)

static inline bool
isstruct(int dwtag)
{

	if (dwtag == DW_TAG_structure_type || dwtag == DW_TAG_class_type ||
	    dwtag == DW_TAG_interface_type)
		return (true);
	return (false);
}

static inline void
get_uleb128(Dwarf_Word *out, unsigned char *dat)
{
	unsigned char b;
	unsigned shift = 0;

	*out = 0;
	while (true) {
		b = *dat;

		*out |= ((Dwarf_Word)(b & 0x7f) << shift);
		if ((b & 0x80) == 0)
			break;

		shift += 7;
		dat++;
	}
}

static int
get_member_offset(Dwarf_Die *memdie, Dwarf_Word *off_out)
{
	Dwarf_Attribute loc_attr;
	Dwarf_Block block;

	if (dwarf_attr_integrate(memdie, DW_AT_data_member_location, &loc_attr)
	    == NULL)
		dwarf_err(EX_DATAERR, "dwarf_attr_integrate(%s/loc)",
		    dwarf_diename(memdie));

	switch (dwarf_whatform(&loc_attr)) {
	case DW_FORM_block:
	case DW_FORM_block1:
	case DW_FORM_block2:
	case DW_FORM_block4:
		if (dwarf_formblock(&loc_attr, &block))
		    dwarf_err(EX_DATAERR, "dwarf_formblock(%s)",
			dwarf_diename(memdie));
		assert(block.data[0] == DW_OP_plus_uconst ||
		    block.data[0] == DW_OP_constu);
		get_uleb128(off_out, &block.data[1]);
		return (0);
	default:
		printf("ZZZ!\n");
		return (-1);
	}
}

static int
get_member_size(Dwarf_Die *type_die, Dwarf_Word *msize_out)
{
	if (dwarf_aggregate_size(type_die, msize_out) != -1)
		return (0);

	if (dwarf_tag(type_die) == DW_TAG_pointer_type)
		return (pointer_size);

	dwarf_err(EX_DATAERR, "dwarf_aggregate_size");
	return (-1);
}

static void
get_dwarf_attr(Dwarf_Die *parent, int attr, Dwarf_Attribute *attr_out,
    Dwarf_Die *die_out)
{

	if (dwarf_attr_integrate(parent, attr, attr_out) == NULL)
		dwarf_err(EX_DATAERR, "dwarf_attr_integrate(%s/%d)",
		    dwarf_diename(parent), attr);

	if (dwarf_formref_die(attr_out, die_out) == NULL)
		dwarf_err(EX_DATAERR, "dwarf_formref_die(%s)",
		    dwarf_diename(parent));
}

static void
structprobe(Dwarf *dw, Dwarf_Die *structdie)
{
	Dwarf_Die memdie;
	Dwarf_Word lastoff = 0, structsize;
	unsigned cline, members, nholes;
	size_t memsz, holesz;
	int x;

	(void)dw;

	cline = members = nholes = 0;
	memsz = holesz = 0;

	printf("struct %s {\n", dwarf_diename(structdie));

	if (dwarf_aggregate_size(structdie, &structsize) == -1)
		dwarf_err(EX_DATAERR, "dwarf_aggregate_size");

	if (dwarf_child(structdie, &memdie)) {
		printf("XXX ???\n");
		exit(EX_DATAERR);
	}

	do {
		Dwarf_Attribute type_attr, base_type_attr;
		Dwarf_Die type_die, base_type_die;
		char type_name[128], mem_name[128], ptr_suffix[32] = { '\0' };
		const char *type_tag = "";
		const char *type = NULL;
		unsigned type_ptrlevel = 0;

		Dwarf_Word msize, off;

		if (dwarf_tag(&memdie) != DW_TAG_member)
			continue;

		members++;
		/*
		 * TODO: Handle bitfield members. DW_AT_bit_offset,
		 * DW_AT_bit_size;
		 */

	 	/* Chase down the type die of this member */
		get_dwarf_attr(&memdie, DW_AT_type, &type_attr, &type_die);

		/* Member offset ... */
		if (get_member_offset(&memdie, &off) == -1)
			dwarf_err(EX_DATAERR, "%s", dwarf_diename(&memdie));

		/* Member size. */
		if (get_member_size(&type_die, &msize) == -1)
			dwarf_err(EX_DATAERR, "get_member_size");

		/* Format name; 'struct foo', 'enum bar', 'char **', etc. */
		if (isstruct(dwarf_tag(&type_die))) {
			type_tag = "struct ";
			type = dwarf_diename(&type_die);
		} else if (dwarf_tag(&type_die) == DW_TAG_enumeration_type) {
			type_tag = "enum ";
			type = dwarf_diename(&type_die);
		} else if (dwarf_tag(&type_die) == DW_TAG_pointer_type) {
			unsigned i;

			do {
				if (dwarf_tag(&type_die) == DW_TAG_pointer_type)
					type_ptrlevel++;
				else if (isstruct(dwarf_tag(&type_die)))
					type_tag = "struct ";
				else if (dwarf_tag(&type_die) == DW_TAG_enumeration_type)
					type_tag = "enum ";
				else
					printf("!!! XXX ignored pointer qualifier TAG %#x\n",
					    dwarf_tag(&type_die));

				/*
				 * Pointers to basic types still need some
				 * work. Clang doesn't emit an AT_TYPE for
				 * 'void*,' for example.
				 */
				if (!dwarf_hasattr(&type_die, DW_AT_type))
					break;

				get_dwarf_attr(&type_die, DW_AT_type,
				    &base_type_attr, &base_type_die);
				type_die = base_type_die;
				type_attr = base_type_attr;
			} while (dwarf_tag(&type_die) != DW_TAG_base_type);

			type = dwarf_diename(&type_die);
			if (type_ptrlevel > sizeof(ptr_suffix) - 2)
				type_ptrlevel = sizeof(ptr_suffix) - 2;
			ptr_suffix[0] = ' ';
			for (i = 1; i <= type_ptrlevel; i++)
				ptr_suffix[i] = '*';
			ptr_suffix[i] = '\0';
		} else
			type = dwarf_diename(&type_die);

		if (type == NULL)
			type = "<anonymous>";

		snprintf(type_name, sizeof(type_name), "%s%s%s", type_tag,
		    type, ptr_suffix);

		if (off != lastoff) {
			printf("\n\t/* XXX %ld bytes hole, try to pack */\n\n", off - lastoff);
			nholes++;
			holesz += (off - lastoff);
		}

		snprintf(mem_name, sizeof(mem_name), "%s;",
		    dwarf_diename(&memdie));

		printf("\t%-27s%-21s /* %5ld %5ld */\n", type_name, mem_name,
		    (long)off, (long)msize);
		memsz += msize;

		lastoff = off + msize;
		if (lastoff / cachelinesize > cline) {
			int ago = lastoff % cachelinesize;
			cline = lastoff / cachelinesize;

			if (ago)
				printf("\t/* --- cacheline %u boundary (%ld "
				    "bytes) was %d bytes ago --- */\n", cline,
				    (long)cline * cachelinesize, ago);
			else
				printf("\t/* --- cacheline %u boundary (%ld "
				    "bytes) --- */\n", cline, (long)cline *
				    cachelinesize);
		}
	} while ((x = dwarf_siblingof(&memdie, &memdie)) == 0);
	if (x == -1)
		dwarf_err(EX_DATAERR, "dwarf_siblingof");

	printf("\n\t/* size: %lu, cachelines: %u, members: %u */\n",
	    structsize, cline + 1, members);
	printf("\t/* sum members: %zu, holes: %u, sum holes: %zu */\n", memsz,
	    nholes, holesz);
	printf("\t/* last cacheline: %lu bytes */\n", lastoff % cachelinesize);

	printf("};\n");
}

static void
get_elf_pointer_size(Dwarf *dw)
{
	Elf *elf;
	char *elf_ident;
	size_t elf_nident;

	elf = dwarf_getelf(dw);

	elf_ident = elf_getident(elf, &elf_nident);
	assert(elf_ident != NULL && elf_nident > EI_CLASS);

	switch ((uint8_t)elf_ident[EI_CLASS]) {
	case ELFCLASS32:
		pointer_size = 4;
		break;
	case ELFCLASS64:
		pointer_size = 8;
		break;
	case ELFCLASSNONE:
	default:
		errx(EX_DATAERR, "Bogus ELF ident header, machine class=%u.\n",
		    (unsigned)elf_ident[EI_CLASS]);
	}
}

int
main(int argc, char **argv)
{
	Dwarf_Off off, lastoff;
	Dwarf *dw;

	size_t hdr_size;
	int cufd, error;

	argv0 = argv[0];
	if (argc < 3)
		usage();

	structname = argv[1];
	binary = argv[2];

	elf_version(EV_CURRENT);

	cufd = open(binary, O_RDONLY);
	if (cufd == -1)
		err(EX_USAGE, "open");

	dw = dwarf_begin(cufd, DWARF_C_READ);
	if (dw == NULL) {
		error = dwarf_errno();
		if (error == DWARF_E_NO_REGFILE)
			errx(EX_USAGE, "%s: Not a regular file", binary);
		dwarf_err_errno(EX_DATAERR, error, "dwarf_begin");
	}

	get_elf_pointer_size(dw);

	/* XXX worry about .debug_types sections later. */

	lastoff = off = 0;
	while (dwarf_nextcu(dw, off, &off, &hdr_size, NULL, NULL, NULL) == 0) {
		Dwarf_Die cu_die, die;
		int x;

		if (dwarf_offdie(dw, lastoff + hdr_size, &cu_die) == NULL)
			continue;
		lastoff = off;

		/*
		 * A CU may be empty because e.g. an empty (or fully #if0'd)
		 * file is compiled.
		 */
		if (dwarf_child(&cu_die, &die))
			continue;

		/* Loop through all DIEs in the CU. */
		do {
			if (isstruct(dwarf_tag(&die)) &&
			    dwarf_haschildren(&die) &&
			    dwarf_diename(&die) &&
			    strcmp(dwarf_diename(&die), structname) == 0) {
				structprobe(dw, &die);
				goto out;
			}

		} while ((x = dwarf_siblingof(&die, &die)) == 0);
		if (x == -1)
			dwarf_err(EX_DATAERR, "dwarf_siblingof");
	}

out:
	if (dwarf_end(dw))
		dwarf_err(EX_SOFTWARE, "dwarf_end");

	return (EX_OK);
}
