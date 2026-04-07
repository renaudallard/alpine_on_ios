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
#define PT_DYNAMIC	2
#define PT_INTERP	3
#define PF_X		1
#define PF_W		2
#define PF_R		4

#define DT_NULL		0
#define DT_NEEDED	1
#define DT_PLTRELSZ	2
#define DT_PLTGOT	3
#define DT_STRTAB	5
#define DT_SYMTAB	6
#define DT_RELA		7
#define DT_RELASZ	8
#define DT_RELAENT	9
#define DT_STRSZ	10
#define DT_SYMENT	11
#define DT_PLTREL	20
#define DT_JMPREL	23
#define DT_RELR		36
#define DT_RELRSZ	35
#define DT_RELRENT	37

#define R_AARCH64_NONE		0
#define R_AARCH64_ABS64		257
#define R_AARCH64_GLOB_DAT	1025
#define R_AARCH64_JUMP_SLOT	1026
#define R_AARCH64_RELATIVE	1027

#define STB_LOCAL	0
#define STB_GLOBAL	1
#define STB_WEAK	2
#define STT_NOTYPE	0
#define STT_OBJECT	1
#define STT_FUNC	2
#define STT_SECTION	3
#define STT_FILE	4
#define ELF64_ST_BIND(i)	((i) >> 4)
#define ELF64_ST_TYPE(i)	((i) & 0xf)
#define ELF64_R_SYM(i)		((i) >> 32)
#define ELF64_R_TYPE(i)		((i) & 0xffffffff)

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

typedef struct {
	int64_t		d_tag;
	uint64_t	d_val;
} Elf64_Dyn;

typedef struct {
	uint64_t	r_offset;
	uint64_t	r_info;
	int64_t		r_addend;
} Elf64_Rela;

typedef struct {
	uint32_t	st_name;
	uint8_t		st_info;
	uint8_t		st_other;
	uint16_t	st_shndx;
	uint64_t	st_value;
	uint64_t	st_size;
} Elf64_Sym;

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
#define LC_LOAD_DYLIB		0x0C
#define LC_UUID			0x1B
#define LC_BUILD_VERSION	0x32
#define LC_SYMTAB		0x02
#define LC_DYSYMTAB		0x0B
#define LC_CODE_SIGNATURE	0x1D
#define LC_DYLD_INFO_ONLY	0x80000022
#define LC_DYLD_EXPORTS_TRIE	0x80000033

/* nlist_64 type flags */
#define N_UNDF		0x0
#define N_SECT		0xe
#define N_EXT		0x01

/* Bind opcodes */
#define BIND_TYPE_POINTER			1
#define BIND_OPCODE_DONE			0x00
#define BIND_OPCODE_SET_DYLIB_ORDINAL_IMM	0x10
#define BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM 0x40
#define BIND_OPCODE_SET_TYPE_IMM		0x50
#define BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB	0x70
#define BIND_OPCODE_DO_BIND			0x90
#define BIND_OPCODE_ADD_ADDR_ULEB		0x80

/* Rebase opcodes */
#define REBASE_TYPE_POINTER			1
#define REBASE_OPCODE_DONE			0x00
#define REBASE_OPCODE_SET_TYPE_IMM		0x10
#define REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB 0x20
#define REBASE_OPCODE_DO_REBASE_ULEB_TIMES	0x60

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

typedef struct {
	uint32_t cmd, cmdsize;
	uint32_t rebase_off, rebase_size;
	uint32_t bind_off, bind_size;
	uint32_t weak_bind_off, weak_bind_size;
	uint32_t lazy_bind_off, lazy_bind_size;
	uint32_t export_off, export_size;
} dyld_info_command;

typedef struct {
	uint32_t n_strx;
	uint8_t  n_type;
	uint8_t  n_sect;
	uint16_t n_desc;
	uint64_t n_value;
} nlist_64;

/* ULEB128 encoder. */
static size_t
write_uleb(uint8_t *p, uint64_t v)
{
	size_t n = 0;
	do {
		uint8_t b = v & 0x7f;
		v >>= 7;
		if (v != 0) b |= 0x80;
		p[n++] = b;
	} while (v != 0);
	return n;
}

/* ---- ELF dynamic section parsing ---- */

#define MAX_NEEDED	16

struct dynamic_info {
	/* DT_NEEDED entries (library names) */
	const char	*needed[MAX_NEEDED];
	int		 n_needed;

	/* String table */
	const char	*strtab;	/* points into elf buffer */
	uint64_t	 strsz;

	/* Symbol table */
	Elf64_Sym	*symtab;	/* points into elf buffer */
	uint64_t	 nsyms;		/* count derived from PLTGOT/RELA */
	uint64_t	 syment;	/* sizeof entry, should be 24 */

	/* RELA: regular dynamic relocations */
	Elf64_Rela	*rela;
	uint64_t	 relasz;	/* in bytes */

	/* JMPREL: PLT relocations */
	Elf64_Rela	*jmprel;
	uint64_t	 pltrelsz;	/* in bytes */
	int		 pltrel_type;	/* DT_RELA or DT_REL (we expect RELA) */

	/* RELR: compact relative relocations */
	uint64_t	*relr;
	uint64_t	 relrsz;	/* in bytes */

	/* PLTGOT: address of GOT for PLT entries */
	uint64_t	 pltgot;
};

/*
 * Convert an ELF vaddr to a pointer in the loaded ELF buffer.
 * Walks PT_LOAD segments to find which one contains vaddr.
 */
static void *
elf_vaddr_to_ptr(uint8_t *elf, Elf64_Phdr *phdrs, int phnum, uint64_t vaddr)
{
	int i;
	for (i = 0; i < phnum; i++) {
		if (phdrs[i].p_type != PT_LOAD)
			continue;
		if (vaddr >= phdrs[i].p_vaddr &&
		    vaddr < phdrs[i].p_vaddr + phdrs[i].p_filesz) {
			return elf + phdrs[i].p_offset +
			    (vaddr - phdrs[i].p_vaddr);
		}
	}
	return NULL;
}

/* ---- Mach-O symbol table builder ---- */

#define MAX_SYMS 4096

struct mach_symbol {
	const char	*name;		/* without leading underscore */
	uint64_t	 vaddr;		/* unshifted, ELF vaddr */
	uint8_t		 type;		/* nlist_64.n_type */
	uint8_t		 sect;		/* nlist_64.n_sect (1-based) */
	uint16_t	 desc;		/* library ordinal in low 8 bits */
	uint32_t	 strx;		/* string table offset (set by build) */
};

struct symtab_builder {
	struct mach_symbol *syms;	/* sorted: locals, exts, undefs */
	int		 nsyms;
	int		 ilocal, nlocal;
	int		 iext, next;
	int		 iundef, nundef;
	uint8_t		*strtab;
	uint32_t	 strsz;
	uint32_t	 strcap;
};

static uint32_t
strtab_add(struct symtab_builder *sb, const char *name)
{
	size_t len = strlen(name) + 1;
	if (sb->strsz + len > sb->strcap) {
		sb->strcap = (sb->strcap + len + 4096) * 2;
		sb->strtab = realloc(sb->strtab, sb->strcap);
	}
	uint32_t off = sb->strsz;
	memcpy(sb->strtab + off, name, len);
	sb->strsz += len;
	return off;
}

/*
 * Build Mach-O symbol table from ELF dynamic symbols.
 * - Defined globals → external defs (exported)
 * - Undefined globals → undefs (imported, will be bound)
 * - Locals are skipped (Mach-O doesn't need them for dyld)
 */
static int
build_symtab(struct dynamic_info *dyn, struct symtab_builder *sb)
{
	uint64_t i;
	int idx = 0;

	memset(sb, 0, sizeof(*sb));
	if (dyn->symtab == NULL || dyn->strtab == NULL || dyn->nsyms == 0)
		return 0;

	sb->syms = calloc(dyn->nsyms + 1, sizeof(*sb->syms));
	if (sb->syms == NULL) return -1;

	/* Initialize string table with leading null. */
	sb->strcap = 4096;
	sb->strtab = calloc(1, sb->strcap);
	if (sb->strtab == NULL) { free(sb->syms); return -1; }
	sb->strsz = 1;	/* index 0 is empty string */

	/* First pass: external defined symbols. */
	sb->iext = idx;
	for (i = 1; i < dyn->nsyms; i++) {
		Elf64_Sym *s = &dyn->symtab[i];
		int bind = ELF64_ST_BIND(s->st_info);
		int type = ELF64_ST_TYPE(s->st_info);
		const char *name;

		if (bind != STB_GLOBAL && bind != STB_WEAK)
			continue;
		if (s->st_shndx == 0)	/* SHN_UNDEF */
			continue;
		if (type == STT_SECTION || type == STT_FILE)
			continue;
		if (s->st_name >= dyn->strsz)
			continue;
		name = dyn->strtab + s->st_name;
		if (name[0] == '\0')
			continue;

		sb->syms[idx].name = name;
		sb->syms[idx].vaddr = s->st_value;
		sb->syms[idx].type = N_SECT | N_EXT;
		/* Section number: 1=__text, 2=__data (Mach-O 1-based). */
		sb->syms[idx].sect = (type == STT_FUNC) ? 1 : 2;
		sb->syms[idx].desc = 0;
		idx++;
	}
	sb->next = idx - sb->iext;

	/* Second pass: undefined symbols (imports). */
	sb->iundef = idx;
	for (i = 1; i < dyn->nsyms; i++) {
		Elf64_Sym *s = &dyn->symtab[i];
		int bind = ELF64_ST_BIND(s->st_info);
		const char *name;

		if (bind != STB_GLOBAL && bind != STB_WEAK)
			continue;
		if (s->st_shndx != 0)	/* not SHN_UNDEF */
			continue;
		if (s->st_name >= dyn->strsz)
			continue;
		name = dyn->strtab + s->st_name;
		if (name[0] == '\0')
			continue;

		sb->syms[idx].name = name;
		sb->syms[idx].vaddr = 0;
		sb->syms[idx].type = N_UNDF | N_EXT;
		sb->syms[idx].sect = 0;
		/* Library ordinal 1 = first LC_LOAD_DYLIB */
		sb->syms[idx].desc = 0x0100;
		idx++;
	}
	sb->nundef = idx - sb->iundef;
	sb->nsyms = idx;

	/* Build string table: each symbol gets an underscore prefix. */
	for (i = 0; i < (uint64_t)sb->nsyms; i++) {
		char buf[256];
		const char *name = sb->syms[i].name;
		size_t len = strlen(name);
		if (len > sizeof(buf) - 2) len = sizeof(buf) - 2;
		buf[0] = '_';
		memcpy(buf + 1, name, len);
		buf[len + 1] = '\0';
		sb->syms[i].strx = strtab_add(sb, buf);
	}

	return 0;
}

static void
free_symtab(struct symtab_builder *sb)
{
	free(sb->syms);
	free(sb->strtab);
	memset(sb, 0, sizeof(*sb));
}

/* ---- Bind opcode generation ---- */

struct bind_builder {
	uint8_t		*buf;
	size_t		 size;
	size_t		 cap;
};

static void
bind_grow(struct bind_builder *bb, size_t need)
{
	if (bb->size + need > bb->cap) {
		bb->cap = (bb->cap + need + 256) * 2;
		bb->buf = realloc(bb->buf, bb->cap);
	}
}

static void
bind_byte(struct bind_builder *bb, uint8_t b)
{
	bind_grow(bb, 1);
	bb->buf[bb->size++] = b;
}

static void
bind_uleb(struct bind_builder *bb, uint64_t v)
{
	bind_grow(bb, 10);
	bb->size += write_uleb(bb->buf + bb->size, v);
}

static void
bind_str(struct bind_builder *bb, const char *s)
{
	size_t len = strlen(s) + 1;
	bind_grow(bb, len);
	memcpy(bb->buf + bb->size, s, len);
	bb->size += len;
}

/*
 * Find the index of an undefined symbol in the symtab by ELF symbol idx.
 * Returns -1 if not found.
 */
static int
find_undef_by_elf_sym(struct dynamic_info *dyn, struct symtab_builder *sb,
    uint64_t elf_sym_idx)
{
	const char *name;
	int i;

	if (elf_sym_idx == 0 || elf_sym_idx >= dyn->nsyms)
		return -1;
	if (dyn->symtab[elf_sym_idx].st_name >= dyn->strsz)
		return -1;
	name = dyn->strtab + dyn->symtab[elf_sym_idx].st_name;
	if (name[0] == '\0')
		return -1;

	/* Linear search in undefined section. */
	for (i = sb->iundef; i < sb->iundef + sb->nundef; i++) {
		if (strcmp(sb->syms[i].name, name) == 0)
			return i;
	}
	return -1;
}

/*
 * Build bind opcodes for R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT,
 * and R_AARCH64_ABS64 relocations.  Each binds an undefined symbol
 * to its loaded address by writing into the data segment.
 *
 * data_seg_idx: 0-based segment index of __DATA in the load commands
 *               (typically 1 if __TEXT is segment 0)
 * data_seg_vmaddr: vmaddr of __DATA segment (used to compute offset)
 */
static int
build_binds(struct dynamic_info *dyn, struct symtab_builder *sb,
    struct bind_builder *bb, int data_seg_idx, uint64_t data_seg_vmaddr,
    uint64_t shift)
{
	uint64_t i, n;
	Elf64_Rela *rels[2];
	uint64_t nrels[2];

	memset(bb, 0, sizeof(*bb));

	rels[0] = dyn->rela;
	nrels[0] = dyn->relasz / sizeof(Elf64_Rela);
	rels[1] = dyn->jmprel;
	nrels[1] = dyn->pltrelsz / sizeof(Elf64_Rela);

	/* Set type once for all binds. */
	bind_byte(bb, BIND_OPCODE_SET_TYPE_IMM | BIND_TYPE_POINTER);
	/* Set library ordinal: 1 = first LC_LOAD_DYLIB. */
	bind_byte(bb, BIND_OPCODE_SET_DYLIB_ORDINAL_IMM | 1);

	for (int r = 0; r < 2; r++) {
		for (i = 0, n = nrels[r]; i < n; i++) {
			Elf64_Rela *rl = &rels[r][i];
			uint32_t type = ELF64_R_TYPE(rl->r_info);
			uint64_t sym = ELF64_R_SYM(rl->r_info);
			int undef_idx;
			uint64_t target_vaddr, seg_off;

			if (type != R_AARCH64_GLOB_DAT &&
			    type != R_AARCH64_JUMP_SLOT &&
			    type != R_AARCH64_ABS64)
				continue;

			undef_idx = find_undef_by_elf_sym(dyn, sb, sym);
			if (undef_idx < 0)
				continue;

			/* Target address (in shifted dylib coordinates). */
			target_vaddr = rl->r_offset + shift;
			if (target_vaddr < data_seg_vmaddr)
				continue;
			seg_off = target_vaddr - data_seg_vmaddr;

			/* Symbol with leading underscore. */
			{
				char buf[256];
				const char *name = sb->syms[undef_idx].name;
				size_t len = strlen(name);
				if (len > sizeof(buf) - 2) len = sizeof(buf) - 2;
				buf[0] = '_';
				memcpy(buf + 1, name, len);
				buf[len + 1] = '\0';
				bind_byte(bb,
				    BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM | 0);
				bind_str(bb, buf);
			}
			bind_byte(bb,
			    BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB |
			    (data_seg_idx & 0xf));
			bind_uleb(bb, seg_off);
			bind_byte(bb, BIND_OPCODE_DO_BIND);
		}
	}

	bind_byte(bb, BIND_OPCODE_DONE);
	return 0;
}

static void
free_binds(struct bind_builder *bb)
{
	free(bb->buf);
	memset(bb, 0, sizeof(*bb));
}

/*
 * Parse PT_DYNAMIC segment and fill struct dynamic_info.
 * Returns 0 on success, -1 if no dynamic section found.
 */
static int
parse_dynamic(uint8_t *elf, Elf64_Phdr *phdrs, int phnum,
    struct dynamic_info *dyn)
{
	int i;
	Elf64_Phdr *pdyn = NULL;
	Elf64_Dyn *d;
	uint64_t needed_off[MAX_NEEDED];
	int n_needed = 0;

	memset(dyn, 0, sizeof(*dyn));

	for (i = 0; i < phnum; i++) {
		if (phdrs[i].p_type == PT_DYNAMIC) {
			pdyn = &phdrs[i];
			break;
		}
	}
	if (pdyn == NULL)
		return -1;

	d = (Elf64_Dyn *)(elf + pdyn->p_offset);
	for (; d->d_tag != DT_NULL; d++) {
		switch (d->d_tag) {
		case DT_NEEDED:
			if (n_needed < MAX_NEEDED)
				needed_off[n_needed++] = d->d_val;
			break;
		case DT_STRTAB:
			dyn->strtab = (const char *)elf_vaddr_to_ptr(
			    elf, phdrs, phnum, d->d_val);
			break;
		case DT_STRSZ:
			dyn->strsz = d->d_val;
			break;
		case DT_SYMTAB:
			dyn->symtab = (Elf64_Sym *)elf_vaddr_to_ptr(
			    elf, phdrs, phnum, d->d_val);
			break;
		case DT_SYMENT:
			dyn->syment = d->d_val;
			break;
		case DT_RELA:
			dyn->rela = (Elf64_Rela *)elf_vaddr_to_ptr(
			    elf, phdrs, phnum, d->d_val);
			break;
		case DT_RELASZ:
			dyn->relasz = d->d_val;
			break;
		case DT_JMPREL:
			dyn->jmprel = (Elf64_Rela *)elf_vaddr_to_ptr(
			    elf, phdrs, phnum, d->d_val);
			break;
		case DT_PLTRELSZ:
			dyn->pltrelsz = d->d_val;
			break;
		case DT_PLTREL:
			dyn->pltrel_type = (int)d->d_val;
			break;
		case DT_RELR:
			dyn->relr = (uint64_t *)elf_vaddr_to_ptr(
			    elf, phdrs, phnum, d->d_val);
			break;
		case DT_RELRSZ:
			dyn->relrsz = d->d_val;
			break;
		case DT_PLTGOT:
			dyn->pltgot = d->d_val;
			break;
		}
	}

	/* Resolve DT_NEEDED string offsets to names. */
	dyn->n_needed = n_needed;
	for (i = 0; i < n_needed; i++) {
		if (dyn->strtab != NULL)
			dyn->needed[i] = dyn->strtab + needed_off[i];
		else
			dyn->needed[i] = NULL;
	}

	/*
	 * Compute approximate symbol count.  We don't have DT_HASH
	 * size readily available; estimate from RELA + JMPREL highest
	 * symbol index used.
	 */
	{
		uint64_t max_sym = 0;
		uint64_t n;

		if (dyn->rela != NULL) {
			n = dyn->relasz / sizeof(Elf64_Rela);
			for (uint64_t k = 0; k < n; k++) {
				uint64_t s = ELF64_R_SYM(dyn->rela[k].r_info);
				if (s > max_sym) max_sym = s;
			}
		}
		if (dyn->jmprel != NULL) {
			n = dyn->pltrelsz / sizeof(Elf64_Rela);
			for (uint64_t k = 0; k < n; k++) {
				uint64_t s = ELF64_R_SYM(dyn->jmprel[k].r_info);
				if (s > max_sym) max_sym = s;
			}
		}
		dyn->nsyms = max_sym + 1;
	}

	return 0;
}

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
	struct dynamic_info dyn;
	struct symtab_builder sb;
	struct bind_builder bb;
	int		 has_dyn;

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

	/* Parse dynamic section. */
	has_dyn = (parse_dynamic(elf, phdrs, ehdr->e_phnum, &dyn) == 0);
	memset(&sb, 0, sizeof(sb));
	if (has_dyn) {
		if (build_symtab(&dyn, &sb) != 0) {
			fprintf(stderr, "%s: build_symtab failed\n", argv[1]);
			free(elf); return 1;
		}
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

	/* Build bind opcodes now that we know the layout. */
	memset(&bb, 0, sizeof(bb));
	if (has_dyn && has_data) {
		build_binds(&dyn, &sb, &bb, /*data_seg_idx=*/1,
		    data_seg_vmaddr, shift);
	}

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

	if (has_dyn) {
		uint64_t nrela = dyn.relasz / sizeof(Elf64_Rela);
		uint64_t njmprel = dyn.pltrelsz / sizeof(Elf64_Rela);
		uint64_t nrelr = dyn.relrsz / sizeof(uint64_t);
		printf("dyn: needed=%d syms=%llu rela=%llu jmprel=%llu relr=%llu\n",
		    dyn.n_needed,
		    (unsigned long long)dyn.nsyms,
		    (unsigned long long)nrela,
		    (unsigned long long)njmprel,
		    (unsigned long long)nrelr);
		for (i = 0; i < dyn.n_needed; i++)
			printf("  needed: %s\n", dyn.needed[i] ?
			    dyn.needed[i] : "(null)");
		printf("symtab: nsyms=%d (ext=%d undef=%d) strsz=%u\n",
		    sb.nsyms, sb.next, sb.nundef, sb.strsz);
		printf("binds: %zu bytes\n", bb.size);
	}

	/* Print metadata for diagnosis. */
	printf("entry=0x%llx text=0x%llx-0x%llx data=0x%llx-0x%llx\n",
	    (unsigned long long)entry,
	    (unsigned long long)text_seg_vmaddr,
	    (unsigned long long)(text_seg_vmaddr + text_seg_vmsize),
	    (unsigned long long)data_seg_vmaddr,
	    (unsigned long long)(data_seg_vmaddr + data_seg_vmsize));

	free_symtab(&sb);
	free_binds(&bb);
	free(out);
	free(elf);
	return 0;
}
