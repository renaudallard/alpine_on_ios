/*
 * elf2macho: convert an AOT-patched AArch64 ELF to a Mach-O dylib.
 *
 * Produces a minimal arm64 dylib that iOS can dlopen().  Preserves
 * the relative offsets between code and data segments so ADRP
 * instructions remain correct.  The dylib is unsigned; run codesign
 * after generation.
 *
 * Usage: elf2macho <input.elf> <output.dylib>
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- ELF definitions ---- */

#define EI_NIDENT	16
#define ELFMAG		"\177ELF"
#define ELFCLASS64	2
#define ELFDATA2LSB	1
#define EM_AARCH64	183
#define ET_DYN		3
#define PT_LOAD		1
#define PT_INTERP	3
#define PF_X		1
#define PF_W		2
#define PF_R		4

typedef struct {
	uint8_t		e_ident[EI_NIDENT];
	uint16_t	e_type, e_machine;
	uint32_t	e_version;
	uint64_t	e_entry, e_phoff, e_shoff;
	uint32_t	e_flags;
	uint16_t	e_ehsize, e_phentsize, e_phnum;
	uint16_t	e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
	uint32_t	p_type, p_flags;
	uint64_t	p_offset, p_vaddr, p_paddr;
	uint64_t	p_filesz, p_memsz, p_align;
} Elf64_Phdr;

/* ---- Mach-O definitions ---- */

#define MH_MAGIC_64		0xFEEDFACF
#define CPU_TYPE_ARM64		(0x0100000C)
#define CPU_SUBTYPE_ARM64_ALL	0
#define MH_DYLIB		6
#define MH_DYLDLINK		0x4
#define MH_TWOLEVEL		0x80
#define MH_PIE			0x200000

#define LC_SEGMENT_64		0x19
#define LC_ID_DYLIB		0x0D
#define LC_UUID			0x1B
#define LC_BUILD_VERSION	0x32
#define LC_SYMTAB		0x02
#define LC_DYSYMTAB		0x0B
#define LC_CODE_SIGNATURE	0x1D
#define LC_DYLD_EXPORTS_TRIE	0x80000033

#define VM_PROT_READ	1
#define VM_PROT_WRITE	2
#define VM_PROT_EXEC	4

#define PLATFORM_IOS	2

#define PAGE_SZ		0x4000	/* 16K for arm64 iOS */
#define ALIGN_UP(x, a)	(((x) + (a) - 1) & ~((uint64_t)(a) - 1))

/* Mach-O structs (matching Apple's layout) */
typedef struct {
	uint32_t magic;
	int32_t  cputype, cpusubtype;
	uint32_t filetype, ncmds, sizeofcmds, flags, reserved;
} mach_header_64;

typedef struct {
	uint32_t cmd, cmdsize;
	char     segname[16];
	uint64_t vmaddr, vmsize, fileoff, filesize;
	int32_t  maxprot, initprot;
	uint32_t nsects, flags;
} segment_command_64;

typedef struct {
	char     sectname[16];
	char     segname[16];
	uint64_t addr, size;
	uint32_t offset, align, reloff, nreloc, flags;
	uint32_t reserved1, reserved2, reserved3;
} section_64;

typedef struct {
	uint32_t cmd, cmdsize;
	uint32_t name_offset, timestamp;
	uint32_t current_version, compat_version;
} dylib_command;

typedef struct {
	uint32_t cmd, cmdsize;
	uint8_t  uuid[16];
} uuid_command;

typedef struct {
	uint32_t cmd, cmdsize;
	uint32_t platform, minos, sdk, ntools;
} build_version_command;

typedef struct {
	uint32_t cmd, cmdsize;
	uint32_t symoff, nsyms, stroff, strsize;
} symtab_command;

typedef struct {
	uint32_t cmd, cmdsize;
	uint32_t ilocalsym, nlocalsym;
	uint32_t iextdefsym, nextdefsym;
	uint32_t iundefsym, nundefsym;
	uint32_t tocoff, ntoc;
	uint32_t modtaboff, nmodtab;
	uint32_t extrefsymoff, nextrefsyms;
	uint32_t indirectsymoff, nindirectsyms;
	uint32_t extreloff, nextrel;
	uint32_t locreloff, nlocrel;
} dysymtab_command;

typedef struct {
	uint32_t cmd, cmdsize;
	uint32_t dataoff, datasize;
} linkedit_data_command;

/* ---- Helpers ---- */

static void
wbuf(uint8_t **p, const void *src, size_t n) { memcpy(*p, src, n); *p += n; }

static void
wpad(uint8_t **p, size_t n) { memset(*p, 0, n); *p += n; }

int
main(int argc, char **argv)
{
	int		 fd;
	struct stat	 st;
	uint8_t		*elf, *out, *wp;
	Elf64_Ehdr	*ehdr;
	Elf64_Phdr	*phdrs;
	uint64_t	 vmin, vmax, text_vaddr, text_filesz, text_memsz;
	uint64_t	 data_vaddr, data_filesz, data_memsz;
	uint64_t	 text_fileoff_elf, data_fileoff_elf;
	uint64_t	 entry;
	int		 has_text, has_data;
	int		 i;

	/* Output layout offsets */
	uint64_t	 hdr_size, text_off;
	uint64_t	 data_off;
	uint64_t	 linkedit_off, linkedit_sz;
	uint64_t	 total_sz;

	if (argc != 3) {
		fprintf(stderr, "usage: elf2macho <input.elf> <output.dylib>\n");
		return 1;
	}

	/* Read ELF */
	fd = open(argv[1], O_RDONLY);
	if (fd < 0) { perror(argv[1]); return 1; }
	if (fstat(fd, &st) < 0) { perror("fstat"); close(fd); return 1; }
	elf = malloc((size_t)st.st_size);
	if (!elf) { perror("malloc"); close(fd); return 1; }
	if (read(fd, elf, (size_t)st.st_size) != st.st_size) {
		perror("read"); free(elf); close(fd); return 1;
	}
	close(fd);

	/* Validate ELF */
	ehdr = (Elf64_Ehdr *)elf;
	if (memcmp(ehdr->e_ident, ELFMAG, 4) != 0 ||
	    ehdr->e_ident[4] != ELFCLASS64 ||
	    ehdr->e_machine != EM_AARCH64) {
		fprintf(stderr, "%s: not an AArch64 ELF\n", argv[1]);
		free(elf); return 1;
	}

	if (ehdr->e_type != ET_DYN) {
		fprintf(stderr, "%s: not ET_DYN (PIE required)\n", argv[1]);
		free(elf); return 1;
	}

	/* Find PT_LOAD segments (expect one RX + one RW). */
	phdrs = (Elf64_Phdr *)(elf + ehdr->e_phoff);
	has_text = has_data = 0;
	vmin = UINT64_MAX;
	vmax = 0;

	for (i = 0; i < ehdr->e_phnum; i++) {
		uint64_t	end;

		if (phdrs[i].p_type != PT_LOAD)
			continue;

		if (phdrs[i].p_vaddr < vmin)
			vmin = phdrs[i].p_vaddr;
		end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
		if (end > vmax)
			vmax = end;

		if ((phdrs[i].p_flags & PF_X) && !has_text) {
			text_vaddr = phdrs[i].p_vaddr;
			text_filesz = phdrs[i].p_filesz;
			text_memsz = phdrs[i].p_memsz;
			text_fileoff_elf = phdrs[i].p_offset;
			has_text = 1;
		} else if ((phdrs[i].p_flags & PF_W) && !has_data) {
			data_vaddr = phdrs[i].p_vaddr;
			data_filesz = phdrs[i].p_filesz;
			data_memsz = phdrs[i].p_memsz;
			data_fileoff_elf = phdrs[i].p_offset;
			has_data = 1;
		}
	}

	if (!has_text) {
		fprintf(stderr, "%s: no executable PT_LOAD segment\n", argv[1]);
		free(elf); return 1;
	}

	entry = ehdr->e_entry;

	/*
	 * Mach-O layout strategy: shift the entire ELF up by one page
	 * so the header has room.  All sections use file offsets that
	 * equal their VM addresses (section.addr/.offset invariant).
	 *
	 * Layout:
	 *   File 0:                 mach_header + load commands
	 *   File PAGE_SZ:           code (originally at text_vaddr)
	 *   File data_vaddr+SHIFT:  data
	 *
	 * SHIFT = PAGE_SZ - text_vaddr_page_aligned (so text_off is
	 * 16K-aligned in the file).  Runtime loader uses
	 * base = img_addr + SHIFT to find segments.
	 */
	uint64_t shift, text_seg_fileoff, text_seg_vmaddr;
	uint64_t text_seg_filesize, text_seg_vmsize;
	uint64_t data_seg_fileoff, data_seg_vmaddr;
	uint64_t data_seg_filesize, data_seg_vmsize;
	uint64_t text_vaddr_page = text_vaddr & ~(uint64_t)(PAGE_SZ - 1);

	/* Shift up so code starts at page boundary >= one full page. */
	shift = PAGE_SZ - text_vaddr_page;

	text_seg_fileoff = 0;
	text_seg_vmaddr = 0;
	text_seg_filesize = ALIGN_UP(shift + text_vaddr + text_filesz, PAGE_SZ);
	text_seg_vmsize = ALIGN_UP(shift + text_vaddr + text_memsz, PAGE_SZ);
	if (text_seg_vmsize < text_seg_filesize)
		text_seg_vmsize = text_seg_filesize;

	if (has_data) {
		uint64_t data_pos = shift + data_vaddr;
		data_seg_vmaddr = data_pos & ~(uint64_t)(PAGE_SZ - 1);
		data_seg_fileoff = data_seg_vmaddr;
		if (data_seg_fileoff < text_seg_filesize) {
			fprintf(stderr,
			    "%s: data overlaps text segment\n", argv[1]);
			free(elf); return 1;
		}
		data_seg_filesize = ALIGN_UP(
		    (data_pos - data_seg_vmaddr) + data_filesz, PAGE_SZ);
		data_seg_vmsize = ALIGN_UP(
		    (data_pos - data_seg_vmaddr) + data_memsz, PAGE_SZ);
		if (data_seg_vmsize < data_seg_filesize)
			data_seg_vmsize = data_seg_filesize;
	} else {
		data_seg_fileoff = text_seg_filesize;
		data_seg_vmaddr = text_seg_vmsize;
		data_seg_filesize = 0;
		data_seg_vmsize = 0;
	}

	/* LINKEDIT after __DATA. */
	linkedit_off = data_seg_fileoff + data_seg_filesize;
	if (linkedit_off < text_seg_filesize)
		linkedit_off = text_seg_filesize;
	linkedit_off = ALIGN_UP(linkedit_off, PAGE_SZ);
	linkedit_sz = PAGE_SZ;

	total_sz = linkedit_off + linkedit_sz;

	/* Header sanity: must fit before code at file offset PAGE_SZ. */
	hdr_size = 32 + (size_t)(72 + 80) +
	    (has_data ? (72 + 80) : 0) +
	    72 +
	    24 + 32 +
	    24 + 24 + 24 + 80 + 16 + 16;
	if (hdr_size > PAGE_SZ) {
		fprintf(stderr, "%s: header too large (%zu > %d)\n",
		    argv[1], hdr_size, PAGE_SZ);
		free(elf); return 1;
	}

	text_off = shift + text_vaddr;
	data_off = data_seg_fileoff + (shift + data_vaddr - data_seg_vmaddr);

	/* Allocate output. */
	out = calloc(1, (size_t)total_sz);
	if (!out) { perror("calloc"); free(elf); return 1; }

	/* Copy code segment to text_off (== text_vaddr). */
	if (text_fileoff_elf + text_filesz <= (uint64_t)st.st_size)
		memcpy(out + text_off, elf + text_fileoff_elf, text_filesz);

	/* Copy data segment. */
	if (has_data && data_fileoff_elf + data_filesz <= (uint64_t)st.st_size)
		memcpy(out + data_off, elf + data_fileoff_elf, data_filesz);

	/*
	 * Build Mach-O headers.
	 */
	wp = out;

	/* Install name. */
	const char *install_name = "@rpath/guest.dylib";
	uint32_t name_len = (uint32_t)strlen(install_name) + 1;
	uint32_t id_cmdsize = ALIGN_UP(24 + name_len, 8);

	/* Count load commands. */
	uint32_t ncmds = 1 + (has_data ? 1 : 0) + 1 + 7;
	/* __TEXT, optional __DATA, __LINKEDIT, then 7 other commands */

	uint32_t sizeofcmds =
	    (72 + 80) +				/* __TEXT + 1 section */
	    (has_data ? (72 + 80) : 0) +	/* __DATA + 1 section */
	    72 +				/* __LINKEDIT */
	    id_cmdsize +			/* LC_ID_DYLIB */
	    24 +				/* LC_BUILD_VERSION */
	    24 +				/* LC_UUID */
	    24 +				/* LC_SYMTAB */
	    80 +				/* LC_DYSYMTAB */
	    16 +				/* LC_DYLD_EXPORTS_TRIE */
	    16;					/* LC_CODE_SIGNATURE */

	/* mach_header_64 */
	mach_header_64 mh = {
		.magic = MH_MAGIC_64,
		.cputype = CPU_TYPE_ARM64,
		.cpusubtype = CPU_SUBTYPE_ARM64_ALL,
		.filetype = MH_DYLIB,
		.ncmds = ncmds,
		.sizeofcmds = sizeofcmds,
		.flags = MH_DYLDLINK | MH_TWOLEVEL | MH_PIE,
		.reserved = 0
	};
	wbuf(&wp, &mh, sizeof(mh));

	/* LC_SEGMENT_64 __TEXT */
	{
		segment_command_64 seg = {
			.cmd = LC_SEGMENT_64,
			.cmdsize = 72 + 80,
			.vmaddr = text_seg_vmaddr,
			.vmsize = text_seg_vmsize,
			.fileoff = text_seg_fileoff,
			.filesize = text_seg_filesize,
			.maxprot = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC,
			.initprot = VM_PROT_READ | VM_PROT_EXEC,
			.nsects = 1,
			.flags = 0
		};
		memcpy(seg.segname, "__TEXT", 6);
		wbuf(&wp, &seg, 72);

		section_64 sect = {
			.addr = shift + text_vaddr,
			.size = text_filesz,
			.offset = (uint32_t)(shift + text_vaddr),
			.align = 2,
		};
		memcpy(sect.sectname, "__text", 7);
		memcpy(sect.segname, "__TEXT", 6);
		wbuf(&wp, &sect, 80);
	}

	/* LC_SEGMENT_64 __DATA */
	if (has_data) {
		segment_command_64 seg = {
			.cmd = LC_SEGMENT_64,
			.cmdsize = 72 + 80,
			.vmaddr = data_seg_vmaddr,
			.vmsize = data_seg_vmsize,
			.fileoff = data_seg_fileoff,
			.filesize = data_seg_filesize,
			.maxprot = VM_PROT_READ | VM_PROT_WRITE,
			.initprot = VM_PROT_READ | VM_PROT_WRITE,
			.nsects = 1,
			.flags = 0
		};
		memcpy(seg.segname, "__DATA", 7);
		wbuf(&wp, &seg, 72);

		section_64 sect = {
			.addr = shift + data_vaddr,
			.size = data_filesz,
			.offset = (uint32_t)(shift + data_vaddr),
			.align = 3,
		};
		memcpy(sect.sectname, "__data", 7);
		memcpy(sect.segname, "__DATA", 7);
		wbuf(&wp, &sect, 80);
	}

	/* LC_SEGMENT_64 __LINKEDIT */
	{
		segment_command_64 seg = {
			.cmd = LC_SEGMENT_64,
			.cmdsize = 72,
			.vmaddr = linkedit_off,	/* same as fileoff */
			.vmsize = ALIGN_UP(linkedit_sz, PAGE_SZ),
			.fileoff = linkedit_off,
			.filesize = linkedit_sz,
			.maxprot = VM_PROT_READ,
			.initprot = VM_PROT_READ,
			.nsects = 0,
			.flags = 0
		};
		memcpy(seg.segname, "__LINKEDIT", 11);
		wbuf(&wp, &seg, 72);
	}

	/* LC_ID_DYLIB */
	{
		dylib_command dc = {
			.cmd = LC_ID_DYLIB,
			.cmdsize = id_cmdsize,
			.name_offset = 24,
			.timestamp = 1,
			.current_version = 0x00010000,
			.compat_version = 0x00010000
		};
		wbuf(&wp, &dc, 24);
		wbuf(&wp, install_name, name_len);
		wpad(&wp, id_cmdsize - 24 - name_len);
	}

	/* LC_BUILD_VERSION (iOS 15.0) */
	{
		build_version_command bv = {
			.cmd = LC_BUILD_VERSION,
			.cmdsize = 24,
			.platform = PLATFORM_IOS,
			.minos = 0x000F0000,	/* 15.0 */
			.sdk = 0x000F0000,
			.ntools = 0
		};
		wbuf(&wp, &bv, 24);
	}

	/* LC_UUID */
	{
		uuid_command uc = {
			.cmd = LC_UUID,
			.cmdsize = 24,
		};
		/* Generate a deterministic UUID from the entry point. */
		memset(uc.uuid, 0, 16);
		memcpy(uc.uuid, &entry, sizeof(entry));
		uc.uuid[6] = (uc.uuid[6] & 0x0F) | 0x40; /* version 4 */
		uc.uuid[8] = (uc.uuid[8] & 0x3F) | 0x80; /* variant 1 */
		wbuf(&wp, &uc, 24);
	}

	/* LINKEDIT content: exports trie + strtab */
	uint32_t exports_off = (uint32_t)linkedit_off;
	uint32_t strtab_off = exports_off + 2;
	out[exports_off] = 0;		/* empty trie */
	out[exports_off + 1] = 0;
	out[strtab_off] = 0;		/* empty string table */

	/* LC_SYMTAB */
	{
		symtab_command sc = {
			.cmd = LC_SYMTAB,
			.cmdsize = 24,
			.symoff = 0,
			.nsyms = 0,
			.stroff = strtab_off,
			.strsize = 1
		};
		wbuf(&wp, &sc, 24);
	}

	/* LC_DYSYMTAB */
	{
		dysymtab_command dc;
		memset(&dc, 0, sizeof(dc));
		dc.cmd = LC_DYSYMTAB;
		dc.cmdsize = 80;
		wbuf(&wp, &dc, 80);
	}

	/* LC_DYLD_EXPORTS_TRIE */
	{
		linkedit_data_command lc = {
			.cmd = LC_DYLD_EXPORTS_TRIE,
			.cmdsize = 16,
			.dataoff = exports_off,
			.datasize = 2
		};
		wbuf(&wp, &lc, 16);
	}

	/* LC_CODE_SIGNATURE (placeholder, codesign fills it). */
	{
		uint32_t sig_off = strtab_off + 16;
		sig_off = (uint32_t)ALIGN_UP(sig_off, 16);
		linkedit_data_command lc = {
			.cmd = LC_CODE_SIGNATURE,
			.cmdsize = 16,
			.dataoff = sig_off,
			.datasize = (uint32_t)(total_sz - sig_off)
		};
		wbuf(&wp, &lc, 16);
	}

	/* Write output. */
	fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (fd < 0) { perror(argv[2]); free(out); free(elf); return 1; }
	if (write(fd, out, (size_t)total_sz) != (ssize_t)total_sz) {
		perror("write");
		close(fd); free(out); free(elf); return 1;
	}
	close(fd);

	/* Print metadata for diagnosis. */
	printf("entry=0x%llx text=0x%llx-0x%llx data=0x%llx-0x%llx\n",
	    (unsigned long long)entry,
	    (unsigned long long)text_seg_vmaddr,
	    (unsigned long long)(text_seg_vmaddr + text_seg_vmsize),
	    (unsigned long long)data_seg_vmaddr,
	    (unsigned long long)(data_seg_vmaddr + data_seg_vmsize));

	free(out);
	free(elf);
	return 0;
}
