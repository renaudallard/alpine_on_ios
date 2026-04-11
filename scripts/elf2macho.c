/*
 * elf2macho: convert an AOT-patched AArch64 ELF to a Mach-O dylib
 * that iOS dyld can load via dlopen().
 *
 * --- Why this exists ---
 *
 * iOS only executes code from properly signed Mach-O files.  ELF
 * files cannot be made executable on iOS (PROT_EXEC is rejected).
 * To run Linux binaries natively, we wrap each ELF in a Mach-O
 * dylib at build time, codesign it, and let dyld load it at runtime.
 *
 * --- Layout strategy ---
 *
 * The Mach-O header must live at file offset 0.  The ELF code starts
 * at vaddr 0 (typical for PIE binaries), so we cannot place code at
 * file offset 0 without overwriting the header.  We "shift" all
 * sections up by one PAGE_SZ (16K) so the header has room:
 *
 *   File offset 0..PAGE_SZ:  Mach-O header + load commands
 *   File offset PAGE_SZ:     code (was at ELF vaddr 0)
 *   File offset PAGE_SZ+V:   data (was at ELF vaddr V)
 *
 * Each section's file offset equals its VM address within the dylib
 * (so the Mach-O invariant section.addr - segment.vmaddr ==
 * section.offset - segment.fileoff holds with vmaddr = fileoff).
 *
 * --- Address resolution at runtime ---
 *
 * dyld loads the dylib at some kernel-chosen address (`loadAddress`)
 * and applies a "slide".  Both nlist_64.n_value and export_trie
 * symbol offsets contain `vaddr + shift` (the file-layout-shifted
 * address).  dyld computes the runtime address as:
 *   resolved = loadAddress + (vaddr + shift)
 * which lands on the correct code/data because file offset 0 maps
 * to loadAddress and the actual content lives at file offset
 * (vaddr + shift).  See feedback_macho_audit_lessons.md for why
 * the shift MUST appear in trie offsets (common audit confusion).
 *
 * --- Dynamic linking ---
 *
 * For each ELF DT_NEEDED, we emit an LC_LOAD_DYLIB pointing to
 * `@rpath/<lib>.dylib`.  LC_RPATH = `@loader_path` so dyld finds
 * sibling dylibs.  ELF dynamic relocations are translated:
 *   R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT  -> Mach-O bind opcodes
 *   R_AARCH64_RELATIVE, DT_RELR              -> Mach-O rebase opcodes
 * dyld walks these opcode streams at load time and patches the
 * data segment.
 *
 * --- Code signing ---
 *
 * Output dylib is unsigned.  Run `codesign --force --sign -` after
 * generation (done by patch_rootfs_aot.sh on macOS CI runners).
 *
 * Usage: elf2macho <input.elf> <output.dylib>
 */

#define _POSIX_C_SOURCE 200809L

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
#define LC_RPATH		0x8000001C

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
#define REBASE_OPCODE_ADD_ADDR_ULEB		0x30
#define REBASE_OPCODE_DO_REBASE_IMM_TIMES	0x50
#define REBASE_OPCODE_DO_REBASE_ULEB_TIMES	0x60

#define VM_PROT_READ	1
#define VM_PROT_WRITE	2
#define VM_PROT_EXEC	4

#define PLATFORM_MACOS		1
#define PLATFORM_IOS		2
#define PLATFORM_IOSSIMULATOR	7

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
	uint32_t path_offset;
} rpath_command;

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

/*
 * ULEB128 encoder.  Used throughout Mach-O dyld_info opcodes and
 * the export trie.  Each byte holds 7 bits with the high bit
 * indicating "more bytes follow".  Value 0 encodes as one byte 0x00.
 */
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

/* ---- ELF dynamic section parsing ----
 *
 * The PT_DYNAMIC segment contains an array of (tag, value) pairs
 * describing the binary's dynamic linking requirements: dependencies,
 * symbol table, relocations, etc.  We parse it once into struct
 * dynamic_info and then use that to generate Mach-O equivalents.
 *
 * Pointers in the parsed info point into the loaded ELF buffer
 * (no copies); callers must not free the ELF buffer until done
 * with the dynamic_info.
 */

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

/*
 * Check if a symbol name contains a version suffix ("@" or "@@").
 * Returns 1 if versioned (caller should skip or strip).
 */
static int
is_versioned_sym(const char *name)
{
	return strchr(name, '@') != NULL;
}

/* ---- Mach-O symbol table builder ----
 *
 * Mach-O nlist_64 symbol entries must be sorted in 3 groups in order:
 * (1) local symbols, (2) external defined, (3) external undefined.
 * LC_DYSYMTAB stores the index and count of each group.
 *
 * dyld uses external defined symbols (via the export trie, but the
 * symbol table is still required) and external undefined symbols
 * (which are bound at load time via the bind opcodes).
 *
 * String table convention: byte 0 is null, then null-terminated names.
 * Each Mach-O symbol gets a leading underscore (e.g. ELF "malloc"
 * becomes Mach-O "_malloc").
 */

#define MAX_SYMS 4096

struct mach_symbol {
	const char	*name;		/* without leading underscore */
	uint64_t	 vaddr;		/* unshifted, ELF vaddr */
	uint8_t		 type;		/* nlist_64.n_type */
	uint8_t		 sect;		/* nlist_64.n_sect (1-based) */
	uint16_t	 desc;		/* library ordinal << 8 | flags */
	uint32_t	 strx;		/* string table offset (set by build) */
	int		 weak;		/* 1 if STB_WEAK undef */
};

/* nlist_64 n_desc flags (see <mach-o/nlist.h>). */
#define N_WEAK_REF	0x0040
#define N_WEAK_DEF	0x0080

/* BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM imm bits. */
#define BIND_SYMBOL_FLAGS_WEAK_IMPORT	0x1

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
 *
 * text_lo/hi and data_lo/hi are the original ELF vaddr ranges
 * used to assign section numbers based on st_value.
 */
static int
build_symtab(struct dynamic_info *dyn, struct symtab_builder *sb,
    uint64_t text_lo, uint64_t text_hi,
    uint64_t data_lo, uint64_t data_hi)
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
		/* Skip versioned symbols (only use the default version
		 * which has no @ suffix). */
		if (is_versioned_sym(name))
			continue;

		sb->syms[idx].name = name;
		sb->syms[idx].vaddr = s->st_value;
		sb->syms[idx].type = N_SECT | N_EXT;
		/* Section number: 1=__text, 2=__data (Mach-O 1-based).
		 * Determine by address, not symbol type. */
		if (s->st_value >= text_lo && s->st_value < text_hi)
			sb->syms[idx].sect = 1;
		else if (s->st_value >= data_lo && s->st_value < data_hi)
			sb->syms[idx].sect = 2;
		else
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
		if (is_versioned_sym(name))
			continue;

		sb->syms[idx].name = name;
		sb->syms[idx].vaddr = 0;
		sb->syms[idx].type = N_UNDF | N_EXT;
		sb->syms[idx].sect = 0;
		/*
		 * desc: library ordinal in the high byte (1 = first
		 * LC_LOAD_DYLIB) and flags in the low byte.  Mark
		 * STB_WEAK undefs with N_WEAK_REF so dyld treats them
		 * as weak imports and accepts them remaining unresolved
		 * (NULL) at load time, matching ELF semantics.
		 */
		sb->syms[idx].desc = 0x0100;
		sb->syms[idx].weak = (bind == STB_WEAK);
		if (sb->syms[idx].weak)
			sb->syms[idx].desc |= N_WEAK_REF;
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

/* ---- Bind opcode generation ----
 *
 * dyld walks a stream of opcodes at load time to bind external
 * symbols.  Each opcode is one byte: 4-bit opcode + 4-bit immediate.
 * Some opcodes are followed by ULEB128 values or null-terminated
 * strings.  dyld maintains a cursor (segment_index + offset) and
 * a current symbol/library/type as it walks the stream.
 *
 * For each symbol bind we emit:
 *   SET_SYMBOL_TRAILING_FLAGS_IMM | 0  (followed by symbol name)
 *   SET_SEGMENT_AND_OFFSET_ULEB | seg  (followed by ULEB offset)
 *   DO_BIND
 * DO_BIND uses the current cursor (segment, offset) and writes
 * the resolved symbol address there, then advances the cursor by
 * sizeof(pointer)=8 bytes.  We reset the cursor before each bind
 * via SET_SEGMENT_AND_OFFSET so they don't need to be sorted.
 *
 * The bind type (POINTER) and library ordinal (1 = first
 * LC_LOAD_DYLIB) are set once at the start.
 */

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
				uint8_t flags = 0;
				if (len > sizeof(buf) - 2) len = sizeof(buf) - 2;
				buf[0] = '_';
				memcpy(buf + 1, name, len);
				buf[len + 1] = '\0';
				/*
				 * Weak undef in the ELF -> weak import in
				 * Mach-O.  dyld will leave it as NULL if
				 * the symbol is not exported by any loaded
				 * dylib instead of failing the load.
				 */
				if (sb->syms[undef_idx].weak)
					flags |= BIND_SYMBOL_FLAGS_WEAK_IMPORT;
				bind_byte(bb,
				    BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM |
				    flags);
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

/* ---- Rebase opcode generation ----
 *
 * Rebase opcodes tell dyld which pointers in the data segment need
 * to have the slide added (R_AARCH64_RELATIVE in ELF terms).  The
 * file already contains the unrelocated addend at each offset;
 * dyld adds its slide and writes the result back.
 *
 * Sources:
 * - DT_RELA entries with R_AARCH64_RELATIVE type
 * - DT_RELR: a compact bitmap encoding for runs of relative
 *   relocations.  Format alternates address-entries (LSB=0, the
 *   address itself is a relocation site) and bitmap-entries (LSB=1,
 *   bits 1..63 indicate offsets at base + (i-1)*8 for the next 63
 *   slots starting at the previous address).
 *
 * The opcode stream is similar to bind: a cursor is set with
 * SET_SEGMENT_AND_OFFSET, then DO_REBASE_ULEB_TIMES advances it
 * by N pointers.  ADD_ADDR_ULEB skips ahead between non-contiguous
 * runs.  We sort offsets ascending and emit one opcode per
 * relocation (not optimal but simple and correct).
 */

struct rebase_builder {
	uint8_t		*buf;
	size_t		 size;
	size_t		 cap;
};

static void
rebase_byte(struct rebase_builder *rb, uint8_t b)
{
	if (rb->size + 1 > rb->cap) {
		rb->cap = (rb->cap + 1 + 256) * 2;
		rb->buf = realloc(rb->buf, rb->cap);
	}
	rb->buf[rb->size++] = b;
}

static void
rebase_uleb(struct rebase_builder *rb, uint64_t v)
{
	if (rb->size + 10 > rb->cap) {
		rb->cap = (rb->cap + 10 + 256) * 2;
		rb->buf = realloc(rb->buf, rb->cap);
	}
	rb->size += write_uleb(rb->buf + rb->size, v);
}

/*
 * Build rebase opcodes for R_AARCH64_RELATIVE and DT_RELR entries.
 * Each rebase: dyld adds the slide to the existing value at offset.
 *
 * Sorts offsets and emits efficient opcode runs.
 */
static int
build_rebases(struct dynamic_info *dyn, struct rebase_builder *rb,
    int data_seg_idx, uint64_t data_seg_vmaddr, uint64_t shift)
{
	uint64_t i, n;
	uint64_t *offsets = NULL;
	size_t n_offsets = 0, cap_offsets = 0;

	memset(rb, 0, sizeof(*rb));

	/* Collect all relative relocation offsets. */

	/* From DT_RELA: R_AARCH64_RELATIVE entries.  The final value is
	 * base + r_addend, matched by leaving r_addend+shift in the
	 * slot and letting dyld add the slide.  apply_self_reloc_values()
	 * pre-writes that; we just add the offset to the rebase list. */
	if (dyn->rela != NULL) {
		n = dyn->relasz / sizeof(Elf64_Rela);
		for (i = 0; i < n; i++) {
			Elf64_Rela *rl = &dyn->rela[i];
			if (ELF64_R_TYPE(rl->r_info) != R_AARCH64_RELATIVE)
				continue;
			if (n_offsets >= cap_offsets) {
				cap_offsets = (cap_offsets + 1) * 2;
				offsets = realloc(offsets,
				    cap_offsets * sizeof(uint64_t));
			}
			offsets[n_offsets++] = rl->r_offset;
		}
	}

	/*
	 * Intentionally NOT emitting rebase opcodes for DT_RELR
	 * entries.  Musl's _dlstart_c in ld-musl-aarch64.so.1 runs
	 * its own RELR self-relocation loop during startup, and
	 * that loop is an "add base" operation on the existing slot:
	 *
	 *     *reloc_addr += ldso.base;
	 *
	 * If dyld has already processed the same slots via a rebase
	 * opcode, the slot holds (load_base + ELF_vaddr) when musl
	 * runs, and musl's += base turns it into
	 * (2*load_base + ELF_vaddr), corrupting every pointer in
	 * .data.rel.ro / .got.  Every pointer subsequently
	 * dereferenced in ld-musl's internal init then crashes
	 * before any syscall (BRK) can even fire, which on iOS
	 * shows up as a silent process death with no .ips file.
	 *
	 * Leave the RELR slots untouched in the Mach-O.  dyld will
	 * see them as zero-initialized pointers; musl will then
	 * apply the relative adjustment exactly once and the pointers
	 * end up correct.  R_AARCH64_RELATIVE in RELA format is
	 * safe to double-apply because musl's handler overwrites
	 * with (base + addend), not `+= base`; same for GLOB_DAT,
	 * JUMP_SLOT and ABS64.  So the bug is specifically RELR.
	 */

	/*
	 * GLOB_DAT/JUMP_SLOT/ABS64 relocations targeting a DEFINED
	 * (local) symbol.  An ordinary ELF dynamic linker would resolve
	 * these to the symbol's load-time address; on Mach-O they
	 * cannot use the bind opcode stream (the ordinal lookup is for
	 * foreign libraries) so we treat them as rebases of a value
	 * that apply_self_reloc_values() pre-writes into the data
	 * segment.  Without this, a self-contained dylib like ld-musl
	 * ships with its own GOT/PLT entries holding 0 / PLT0-vaddr
	 * garbage and crashes the first time any internal call goes
	 * through them.
	 */
	{
		Elf64_Rela	*rels2[2];
		uint64_t	 nrels2[2];
		int		 r;

		rels2[0] = dyn->rela;
		nrels2[0] = dyn->rela ?
		    dyn->relasz / sizeof(Elf64_Rela) : 0;
		rels2[1] = dyn->jmprel;
		nrels2[1] = dyn->jmprel ?
		    dyn->pltrelsz / sizeof(Elf64_Rela) : 0;

		for (r = 0; r < 2; r++) {
			for (i = 0; i < nrels2[r]; i++) {
				Elf64_Rela	*rl = &rels2[r][i];
				uint32_t	 type = ELF64_R_TYPE(rl->r_info);
				uint64_t	 sym_idx = ELF64_R_SYM(rl->r_info);
				Elf64_Sym	*s;

				if (type != R_AARCH64_GLOB_DAT &&
				    type != R_AARCH64_JUMP_SLOT &&
				    type != R_AARCH64_ABS64)
					continue;
				if (sym_idx == 0 || sym_idx >= dyn->nsyms)
					continue;
				s = &dyn->symtab[sym_idx];
				if (s->st_shndx == 0)
					continue;	/* undef -> bind path */
				if (n_offsets >= cap_offsets) {
					cap_offsets = (cap_offsets + 1) * 2;
					offsets = realloc(offsets,
					    cap_offsets * sizeof(uint64_t));
				}
				offsets[n_offsets++] = rl->r_offset;
			}
		}
	}

	if (n_offsets == 0) {
		free(offsets);
		rebase_byte(rb, REBASE_OPCODE_DONE);
		return 0;
	}

	/* Sort offsets ascending. */
	for (i = 0; i + 1 < n_offsets; i++) {
		size_t j;
		for (j = i + 1; j < n_offsets; j++) {
			if (offsets[j] < offsets[i]) {
				uint64_t t = offsets[i];
				offsets[i] = offsets[j];
				offsets[j] = t;
			}
		}
	}

	/* Drop duplicates so we never rebase the same slot twice. */
	{
		size_t w = 0;
		for (i = 0; i < n_offsets; i++) {
			if (w == 0 || offsets[i] != offsets[w - 1])
				offsets[w++] = offsets[i];
		}
		n_offsets = w;
	}

	/* Emit opcodes. */
	rebase_byte(rb, REBASE_OPCODE_SET_TYPE_IMM | REBASE_TYPE_POINTER);

	uint64_t cur_off = 0;
	int set_seg = 0;
	for (i = 0; i < n_offsets; i++) {
		uint64_t target = offsets[i] + shift;
		if (target < data_seg_vmaddr)
			continue;
		uint64_t seg_off = target - data_seg_vmaddr;

		if (!set_seg) {
			rebase_byte(rb,
			    REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB |
			    (data_seg_idx & 0xf));
			rebase_uleb(rb, seg_off);
			cur_off = seg_off;
			set_seg = 1;
		} else if (seg_off > cur_off) {
			/* Skip ahead. */
			uint64_t skip = seg_off - cur_off;
			rebase_byte(rb, REBASE_OPCODE_ADD_ADDR_ULEB);
			rebase_uleb(rb, skip);
			cur_off = seg_off;
		}

		/* Do one rebase, advance cur_off by 8. */
		rebase_byte(rb, REBASE_OPCODE_DO_REBASE_ULEB_TIMES);
		rebase_uleb(rb, 1);
		cur_off += 8;
	}

	rebase_byte(rb, REBASE_OPCODE_DONE);
	free(offsets);
	return 0;
}

static void
free_rebases(struct rebase_builder *rb)
{
	free(rb->buf);
	memset(rb, 0, sizeof(*rb));
}

/*
 * Pre-write the resolved value for each RELA entry whose final
 * runtime address is base+offset+shift:
 *   - R_AARCH64_RELATIVE: value = r_addend + shift
 *   - R_AARCH64_GLOB_DAT / JUMP_SLOT / ABS64 targeting a DEFINED
 *     local symbol: value = sym->st_value + r_addend + shift
 *
 * build_rebases() already added these offsets to the rebase
 * opcode stream so dyld will add the load slide at load time,
 * and together they resolve to the correct runtime address.
 *
 * Without this, a self-contained dylib like ld-musl-aarch64.so.1
 * ships with its own GOT/PLT entries holding 0 / PLT0-vaddr
 * garbage and crashes the first time any internal call goes
 * through them.  RELR-format relatives are unaffected because
 * their slot already holds the unslid target.
 */
static void
apply_self_reloc_values(struct dynamic_info *dyn, uint8_t *out,
    uint64_t out_size, uint64_t data_seg_fileoff,
    uint64_t data_seg_filesize, uint64_t shift)
{
	Elf64_Rela	*rels[2];
	uint64_t	 nrels[2];
	uint64_t	 i;
	int		 r;

	rels[0] = dyn->rela;
	nrels[0] = dyn->rela ? dyn->relasz / sizeof(Elf64_Rela) : 0;
	rels[1] = dyn->jmprel;
	nrels[1] = dyn->jmprel ? dyn->pltrelsz / sizeof(Elf64_Rela) : 0;

	for (r = 0; r < 2; r++) {
		for (i = 0; i < nrels[r]; i++) {
			Elf64_Rela	*rl = &rels[r][i];
			uint32_t	 type = ELF64_R_TYPE(rl->r_info);
			uint64_t	 sym_idx = ELF64_R_SYM(rl->r_info);
			Elf64_Sym	*s;
			uint64_t	 value, file_off;

			if (type == R_AARCH64_RELATIVE) {
				value = (uint64_t)rl->r_addend + shift;
			} else if (type == R_AARCH64_GLOB_DAT ||
			    type == R_AARCH64_JUMP_SLOT ||
			    type == R_AARCH64_ABS64) {
				if (dyn->symtab == NULL)
					continue;
				if (sym_idx == 0 || sym_idx >= dyn->nsyms)
					continue;
				s = &dyn->symtab[sym_idx];
				if (s->st_shndx == 0)
					continue;  /* undef -> bind path */
				value = s->st_value +
				    (uint64_t)rl->r_addend + shift;
			} else {
				continue;
			}

			file_off = rl->r_offset + shift;

			/* Bounds: must land in the __DATA file range. */
			if (file_off < data_seg_fileoff)
				continue;
			if (file_off + 8 >
			    data_seg_fileoff + data_seg_filesize)
				continue;
			if (file_off + 8 > out_size)
				continue;

			memcpy(out + file_off, &value, 8);
		}
	}
}

/* ---- Export trie generation ----
 *
 * The export trie is a compressed prefix tree of exported symbols.
 * dyld uses it to resolve dlsym() and inter-dylib symbol lookups.
 * It is more compact than a flat symbol list when many symbols
 * share prefixes (e.g. "pthread_*", "__pthread_*").
 *
 * Encoded format for each node:
 *   terminal_size (uleb128)
 *     if non-zero (i.e. this node is a complete symbol):
 *       flags (uleb128)         -- 0 for regular symbols
 *       symbol_offset (uleb128) -- vaddr (with file shift) of the symbol
 *   n_children (1 byte)
 *   for each child (sorted alphabetically by edge):
 *     edge string (null-terminated)
 *     child_offset (uleb128, absolute byte offset from trie start)
 *
 * IMPORTANT: children must be sorted alphabetically by edge string,
 * and the symbol_offset must include the file shift (so dyld's
 * computed runtime address loadAddress + offset lands on the actual
 * code/data, since the file has the content at offset (vaddr+shift)).
 *
 * The trie is built by inserting symbols one at a time; each insert
 * walks from root, descending matching edges, splitting an edge if
 * a partial match is found, and adding new children otherwise.
 *
 * Sizing: trie_compute_sizes() walks the tree and computes each
 * node's file_offset.  Since each node encodes its children's
 * offsets as ULEB128 (whose size depends on the value), changing
 * a child's offset can change the parent's size, which can cascade.
 * We iterate the sizing pass until total size converges.
 */

struct trie_node {
	char		*edge;		/* string from parent */
	struct trie_node **children;
	int		 n_children;
	int		 cap_children;
	int		 is_terminal;
	uint64_t	 sym_offset;
	uint64_t	 file_offset;	/* set during encoding */
	size_t		 node_size;	/* set during sizing */
};

static struct trie_node *
trie_new(const char *edge)
{
	struct trie_node *n = calloc(1, sizeof(*n));
	if (n == NULL) return NULL;
	if (edge != NULL) n->edge = strdup(edge);
	return n;
}

static void
trie_free(struct trie_node *n)
{
	int i;
	if (n == NULL) return;
	for (i = 0; i < n->n_children; i++)
		trie_free(n->children[i]);
	free(n->children);
	free(n->edge);
	free(n);
}

static void
trie_add_child(struct trie_node *parent, struct trie_node *child)
{
	if (parent->n_children >= parent->cap_children) {
		parent->cap_children = (parent->cap_children + 1) * 2;
		parent->children = realloc(parent->children,
		    parent->cap_children * sizeof(*parent->children));
	}
	parent->children[parent->n_children++] = child;
}

/* Insert a symbol into the trie. */
static void
trie_insert(struct trie_node *root, const char *name, uint64_t sym_off)
{
	struct trie_node *node = root;
	const char *p = name;

	while (*p != '\0') {
		int i, found = 0;
		for (i = 0; i < node->n_children; i++) {
			struct trie_node *c = node->children[i];
			const char *e = c->edge;
			if (e[0] != p[0])
				continue;

			/* Find common prefix length. */
			size_t common = 0;
			while (e[common] != '\0' && p[common] != '\0' &&
			    e[common] == p[common])
				common++;

			if (e[common] == '\0') {
				/* Edge fully matched, descend. */
				node = c;
				p += common;
			} else {
				/* Edge partially matched: split. */
				struct trie_node *split = trie_new(NULL);
				split->edge = strndup(e, common);

				/* Old child becomes a child of split. */
				char *old_remainder = strdup(e + common);
				free(c->edge);
				c->edge = old_remainder;
				trie_add_child(split, c);

				/* Replace c in node->children with split. */
				node->children[i] = split;

				node = split;
				p += common;
			}
			found = 1;
			break;
		}
		if (!found) {
			/* Add new child with remaining string as edge. */
			struct trie_node *nc = trie_new(p);
			trie_add_child(node, nc);
			node = nc;
			p += strlen(p);
		}
	}

	node->is_terminal = 1;
	node->sym_offset = sym_off;
}

static size_t
uleb_size(uint64_t v)
{
	size_t n = 0;
	do { n++; v >>= 7; } while (v != 0);
	return n;
}

/* Sort children alphabetically by edge string (dyld requires this). */
static void
trie_sort_children(struct trie_node *node)
{
	int i, j;
	for (i = 0; i + 1 < node->n_children; i++) {
		for (j = i + 1; j < node->n_children; j++) {
			if (strcmp(node->children[j]->edge,
			    node->children[i]->edge) < 0) {
				struct trie_node *t = node->children[i];
				node->children[i] = node->children[j];
				node->children[j] = t;
			}
		}
	}
	for (i = 0; i < node->n_children; i++)
		trie_sort_children(node->children[i]);
}

/* First pass: compute size of each node assuming current child offsets.
 * Returns total trie size.  May need multiple passes to converge. */
static size_t
trie_compute_sizes(struct trie_node *node, size_t base_off)
{
	int i;
	size_t off = base_off;
	size_t self_size;

	/* Self size. */
	if (node->is_terminal) {
		size_t info_size = uleb_size(0) /* flags */
		    + uleb_size(node->sym_offset);
		self_size = uleb_size(info_size) + info_size;
	} else {
		self_size = uleb_size(0);	/* terminal_size = 0 */
	}
	self_size += 1;	/* n_children */
	for (i = 0; i < node->n_children; i++) {
		self_size += strlen(node->children[i]->edge) + 1;
		self_size += uleb_size(node->children[i]->file_offset);
	}
	node->node_size = self_size;
	node->file_offset = off;
	off += self_size;

	for (i = 0; i < node->n_children; i++)
		off = trie_compute_sizes(node->children[i], off);

	return off;
}

static void
trie_encode_node(struct trie_node *node, uint8_t *buf, size_t *pos)
{
	int i;
	if (node->is_terminal) {
		size_t info_size = uleb_size(0) + uleb_size(node->sym_offset);
		*pos += write_uleb(buf + *pos, info_size);
		*pos += write_uleb(buf + *pos, 0);	/* flags */
		*pos += write_uleb(buf + *pos, node->sym_offset);
	} else {
		buf[(*pos)++] = 0;	/* terminal_size = 0 */
	}
	buf[(*pos)++] = (uint8_t)node->n_children;
	for (i = 0; i < node->n_children; i++) {
		struct trie_node *c = node->children[i];
		size_t len = strlen(c->edge) + 1;
		memcpy(buf + *pos, c->edge, len);
		*pos += len;
		*pos += write_uleb(buf + *pos, c->file_offset);
	}
	for (i = 0; i < node->n_children; i++)
		trie_encode_node(node->children[i], buf, pos);
}

/*
 * Build the export trie from defined external symbols.
 * Returns malloc'd buffer; caller frees.
 */
static uint8_t *
build_export_trie(struct symtab_builder *sb, size_t *out_size, uint64_t shift)
{
	struct trie_node *root = trie_new(NULL);
	int i;
	uint8_t *buf;
	size_t total, prev_total;

	for (i = sb->iext; i < sb->iext + sb->next; i++) {
		char buf_name[256];
		const char *name = sb->syms[i].name;
		size_t len = strlen(name);
		if (len > sizeof(buf_name) - 2) len = sizeof(buf_name) - 2;
		buf_name[0] = '_';
		memcpy(buf_name + 1, name, len);
		buf_name[len + 1] = '\0';
		/* Symbol offset is the export's address (shifted). */
		trie_insert(root, buf_name, sb->syms[i].vaddr + shift);
	}

	/* Sort children alphabetically (dyld requirement). */
	trie_sort_children(root);

	/* Iterate sizing to convergence (offsets affect sizes). */
	total = trie_compute_sizes(root, 0);
	for (int iter = 0; iter < 10; iter++) {
		prev_total = total;
		total = trie_compute_sizes(root, 0);
		if (total == prev_total) break;
	}

	buf = calloc(1, total + 16);
	if (buf == NULL) { trie_free(root); return NULL; }
	size_t pos = 0;
	trie_encode_node(root, buf, &pos);
	*out_size = pos;

	trie_free(root);
	return buf;
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
	struct rebase_builder rb;
	uint8_t		*export_trie = NULL;
	size_t		 export_trie_size = 0;
	int		 has_dyn;

	/* Output layout offsets */
	uint64_t	 hdr_size, text_off;
	uint64_t	 data_off;
	uint64_t	 linkedit_off, linkedit_sz;
	uint64_t	 total_sz;

	uint32_t macho_platform = PLATFORM_IOS;
	if (argc >= 2 && strcmp(argv[1], "--simulator") == 0) {
		macho_platform = PLATFORM_IOSSIMULATOR;
		argc--;
		argv++;
	} else if (argc >= 2 && strcmp(argv[1], "--macos") == 0) {
		macho_platform = PLATFORM_MACOS;
		argc--;
		argv++;
	}
	if (argc != 3) {
		fprintf(stderr, "usage: elf2macho [--simulator|--macos] "
		    "<input.elf> <output.dylib>\n");
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
		uint64_t t_lo = text_vaddr;
		uint64_t t_hi = text_vaddr + text_memsz;
		uint64_t d_lo = has_data ? data_vaddr : 0;
		uint64_t d_hi = has_data ? data_vaddr + data_memsz : 0;
		if (build_symtab(&dyn, &sb, t_lo, t_hi, d_lo, d_hi) != 0) {
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

	/* Shift the code so the Mach-O header gets its own leading
	 * page.  If the ELF's text is already at or past file offset
	 * PAGE_SZ (text_vaddr_page != 0) the header fits in front of
	 * it without moving anything; otherwise we push the text up
	 * by exactly one page.  The previous formula
	 *   shift = PAGE_SZ - text_vaddr_page
	 * underflowed to a huge garbage value whenever
	 * text_vaddr_page >= PAGE_SZ.  elf_loader.c applies the same
	 * rule at runtime so the two must stay in sync. */
	shift = (text_vaddr_page == 0) ? PAGE_SZ : 0;

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

	/* LINKEDIT after __DATA - compute base offset only for now.
	 * Actual size depends on the dyld_info opcodes, symtab, etc.
	 * which we compute below. */
	linkedit_off = data_seg_fileoff + data_seg_filesize;
	if (linkedit_off < text_seg_filesize)
		linkedit_off = text_seg_filesize;
	linkedit_off = ALIGN_UP(linkedit_off, PAGE_SZ);
	linkedit_sz = PAGE_SZ; /* placeholder, recomputed below */

	/* Header sanity: must fit before code at file offset PAGE_SZ. */
	hdr_size = 32 + (size_t)(72 + 80) +
	    (has_data ? (72 + 80) : 0) +
	    72 +
	    24 + 32 +
	    24 + 24 + 24 + 80 + 16 + 16;
	if (hdr_size > PAGE_SZ) {
		fprintf(stderr, "%s: header too large (%llu > %d)\n",
		    argv[1], (unsigned long long)hdr_size, PAGE_SZ);
		free(elf); return 1;
	}

	text_off = shift + text_vaddr;
	data_off = data_seg_fileoff + (shift + data_vaddr - data_seg_vmaddr);

	/* Build bind opcodes now that we know the layout. */
	memset(&bb, 0, sizeof(bb));
	memset(&rb, 0, sizeof(rb));
	if (has_dyn) {
		/* Use __DATA seg if present, else __TEXT (binds rarely
		 * target text but the Mach-O format requires a segment). */
		int seg_idx = has_data ? 1 : 0;
		uint64_t seg_vmaddr = has_data ? data_seg_vmaddr :
		    text_seg_vmaddr;
		build_binds(&dyn, &sb, &bb, seg_idx, seg_vmaddr, shift);
		build_rebases(&dyn, &rb, seg_idx, seg_vmaddr, shift);
	}

	/* Build export trie for libraries (binaries have no exports). */
	if (has_dyn && sb.next > 0) {
		export_trie = build_export_trie(&sb, &export_trie_size,
		    shift);
	}

	/*
	 * Compute LINKEDIT internal layout.  All offsets are relative
	 * to the start of LINKEDIT (which itself sits at file offset
	 * `linkedit_off` in the dylib).  Sections are concatenated in
	 * a fixed order with appropriate alignment:
	 *
	 *   rebase opcodes (8-byte aligned tail)
	 *   bind opcodes (8-byte aligned tail)
	 *   export trie (8-byte aligned tail)
	 *   symbol table (nlist_64[])
	 *   string table (8-byte aligned tail)
	 *   code signature placeholder (16-byte aligned start)
	 *
	 * codesign overwrites the codesig area when it signs the file.
	 * We reserve 4KB which is enough for ad-hoc signatures of any
	 * size we'll produce.
	 */
	uint64_t le_rebase_off = 0;
	uint64_t le_rebase_sz = (rb.size + 7) & ~(uint64_t)7;
	uint64_t le_bind_off = le_rebase_off + le_rebase_sz;
	uint64_t le_bind_sz = (bb.size + 7) & ~(uint64_t)7;
	uint64_t le_export_off = le_bind_off + le_bind_sz;
	uint64_t le_export_sz = (export_trie_size + 7) & ~(uint64_t)7;
	uint64_t le_symtab_off = le_export_off + le_export_sz;
	uint64_t le_symtab_sz = (uint64_t)sb.nsyms * sizeof(nlist_64);
	uint64_t le_strtab_off = le_symtab_off + le_symtab_sz;
	uint64_t le_strtab_sz = (sb.strsz + 7) & ~(uint64_t)7;
	if (le_strtab_sz == 0) le_strtab_sz = 8;
	uint64_t le_codesig_off = le_strtab_off + le_strtab_sz;
	le_codesig_off = (le_codesig_off + 15) & ~(uint64_t)15;
	uint64_t le_codesig_sz = 4096; /* placeholder space for codesign */
	uint64_t le_total = le_codesig_off + le_codesig_sz;

	linkedit_sz = ALIGN_UP(le_total, PAGE_SZ);
	total_sz = linkedit_off + linkedit_sz;

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
	 * Pre-write self-reloc values into the data segment.  Must run
	 * after the data copy above (which would otherwise overwrite
	 * them with the raw ELF GOT bytes) and before any header
	 * emission, since dyld's rebase pass reads what we leave here.
	 */
	if (has_dyn && has_data)
		apply_self_reloc_values(&dyn, out, total_sz,
		    data_seg_fileoff, data_seg_filesize, shift);

	/*
	 * Build Mach-O headers.
	 */
	wp = out;

	/* Install name (basename of input file with .dylib suffix). */
	char install_name[256];
	{
		const char *base = strrchr(argv[1], '/');
		base = base ? base + 1 : argv[1];
		snprintf(install_name, sizeof(install_name),
		    "@rpath/%s.dylib", base);
	}
	uint32_t name_len = (uint32_t)strlen(install_name) + 1;
	uint32_t id_cmdsize = ALIGN_UP(24 + name_len, 8);

	/* LC_LOAD_DYLIB for each dependency. */
	uint32_t load_dylib_cmdsizes[MAX_NEEDED];
	char load_dylib_names[MAX_NEEDED][256];
	int n_load_dylib = 0;
	if (has_dyn) {
		for (int j = 0; j < dyn.n_needed; j++) {
			if (dyn.needed[j] == NULL) continue;
			snprintf(load_dylib_names[n_load_dylib],
			    sizeof(load_dylib_names[0]),
			    "@rpath/%s.dylib", dyn.needed[j]);
			uint32_t l = (uint32_t)strlen(
			    load_dylib_names[n_load_dylib]) + 1;
			load_dylib_cmdsizes[n_load_dylib] =
			    ALIGN_UP(24 + l, 8);
			n_load_dylib++;
		}
	}

	/*
	 * LC_RPATH search path.  We emit several so dyld can find
	 * libraries regardless of whether the loading binary lives in
	 *   /alpine/bin/         (busybox itself, sees ../lib)
	 *   /alpine/sbin/        (sees ../lib)
	 *   /alpine/usr/bin/     (sees ../lib and ../../lib)
	 *   /alpine/usr/sbin/    (sees ../lib and ../../lib)
	 *   /alpine/usr/lib/foo/ (sees ../, ../../lib)
	 * etc.
	 *
	 * Each entry is 4-byte aligned including its trailing NUL,
	 * but the cmdsize itself must be 8-byte aligned.
	 */
	static const char *rpath_strs[] = {
		"@loader_path",
		"@loader_path/../lib",
		"@loader_path/../../lib",
		"@loader_path/../usr/lib",
		"@loader_path/../../usr/lib",
	};
	const int n_rpath = (int)(sizeof(rpath_strs) / sizeof(rpath_strs[0]));
	uint32_t rpath_cmdsizes[5];
	for (int j = 0; j < n_rpath; j++)
		rpath_cmdsizes[j] = (uint32_t)ALIGN_UP(
		    12 + strlen(rpath_strs[j]) + 1, 8);
	int has_rpath = (n_load_dylib > 0);

	/* Count load commands. */
	uint32_t ncmds = 1 + (has_data ? 1 : 0) + 1 + 8 + n_load_dylib +
	    (has_rpath ? n_rpath : 0);
	/* segments + LC_ID_DYLIB + LC_BUILD_VERSION + LC_UUID
	 * + LC_DYLD_INFO_ONLY + LC_SYMTAB + LC_DYSYMTAB
	 * + LC_DYLD_EXPORTS_TRIE + LC_CODE_SIGNATURE + n_load_dylib
	 * + n_rpath * LC_RPATH */

	uint32_t sizeofcmds =
	    (72 + 80) +				/* __TEXT + 1 section */
	    (has_data ? (72 + 80) : 0) +	/* __DATA + 1 section */
	    72 +				/* __LINKEDIT */
	    id_cmdsize +			/* LC_ID_DYLIB */
	    24 +				/* LC_BUILD_VERSION */
	    24 +				/* LC_UUID */
	    48 +				/* LC_DYLD_INFO_ONLY */
	    24 +				/* LC_SYMTAB */
	    80 +				/* LC_DYSYMTAB */
	    16 +				/* LC_DYLD_EXPORTS_TRIE */
	    16;					/* LC_CODE_SIGNATURE */
	for (int j = 0; j < n_load_dylib; j++)
		sizeofcmds += load_dylib_cmdsizes[j];
	if (has_rpath) {
		for (int j = 0; j < n_rpath; j++)
			sizeofcmds += rpath_cmdsizes[j];
	}

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
			.platform = macho_platform,
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

	/* LC_LOAD_DYLIB for each dependency. */
	for (int j = 0; j < n_load_dylib; j++) {
		dylib_command dc = {
			.cmd = LC_LOAD_DYLIB,
			.cmdsize = load_dylib_cmdsizes[j],
			.name_offset = 24,
			.timestamp = 1,
			.current_version = 0x00010000,
			.compat_version = 0x00010000
		};
		wbuf(&wp, &dc, 24);
		uint32_t l = (uint32_t)strlen(load_dylib_names[j]) + 1;
		wbuf(&wp, load_dylib_names[j], l);
		wpad(&wp, load_dylib_cmdsizes[j] - 24 - l);
	}

	/* LC_RPATH entries so dependencies are found relative to
	 * where this dylib was loaded from. */
	if (has_rpath) {
		for (int j = 0; j < n_rpath; j++) {
			uint32_t l = (uint32_t)strlen(rpath_strs[j]) + 1;
			rpath_command rc = {
				.cmd = LC_RPATH,
				.cmdsize = rpath_cmdsizes[j],
				.path_offset = 12
			};
			wbuf(&wp, &rc, 12);
			wbuf(&wp, rpath_strs[j], l);
			wpad(&wp, rpath_cmdsizes[j] - 12 - l);
		}
	}

	/*
	 * Compute absolute file offsets within LINKEDIT.
	 * Then write the LINKEDIT content into the output buffer.
	 */
	uint32_t abs_rebase_off = (uint32_t)(linkedit_off + le_rebase_off);
	uint32_t abs_bind_off = (uint32_t)(linkedit_off + le_bind_off);
	uint32_t abs_export_off = (uint32_t)(linkedit_off + le_export_off);
	uint32_t abs_symtab_off = (uint32_t)(linkedit_off + le_symtab_off);
	uint32_t abs_strtab_off = (uint32_t)(linkedit_off + le_strtab_off);
	uint32_t abs_codesig_off = (uint32_t)(linkedit_off + le_codesig_off);

	if (rb.size > 0)
		memcpy(out + abs_rebase_off, rb.buf, rb.size);
	if (bb.size > 0)
		memcpy(out + abs_bind_off, bb.buf, bb.size);
	if (export_trie != NULL && export_trie_size > 0)
		memcpy(out + abs_export_off, export_trie, export_trie_size);

	/* Write nlist_64 entries to symtab. */
	for (int k = 0; k < sb.nsyms; k++) {
		nlist_64 nl;
		nl.n_strx = sb.syms[k].strx;
		nl.n_type = sb.syms[k].type;
		nl.n_sect = sb.syms[k].sect;
		nl.n_desc = sb.syms[k].desc;
		nl.n_value = sb.syms[k].vaddr +
		    (sb.syms[k].type & N_SECT ? shift : 0);
		memcpy(out + abs_symtab_off + (size_t)k * sizeof(nl),
		    &nl, sizeof(nl));
	}
	if (sb.strsz > 0)
		memcpy(out + abs_strtab_off, sb.strtab, sb.strsz);

	/* LC_DYLD_INFO_ONLY */
	{
		dyld_info_command dc = {
			.cmd = LC_DYLD_INFO_ONLY,
			.cmdsize = 48,
			.rebase_off = rb.size > 0 ? abs_rebase_off : 0,
			.rebase_size = (uint32_t)rb.size,
			.bind_off = bb.size > 0 ? abs_bind_off : 0,
			.bind_size = (uint32_t)bb.size,
			.export_off = export_trie_size > 0 ? abs_export_off : 0,
			.export_size = (uint32_t)export_trie_size,
		};
		wbuf(&wp, &dc, 48);
	}

	/* LC_SYMTAB */
	{
		symtab_command sc = {
			.cmd = LC_SYMTAB,
			.cmdsize = 24,
			.symoff = sb.nsyms > 0 ? abs_symtab_off : 0,
			.nsyms = (uint32_t)sb.nsyms,
			.stroff = abs_strtab_off,
			.strsize = sb.strsz > 0 ? sb.strsz : 1
		};
		wbuf(&wp, &sc, 24);
	}

	/* LC_DYSYMTAB */
	{
		dysymtab_command dc;
		memset(&dc, 0, sizeof(dc));
		dc.cmd = LC_DYSYMTAB;
		dc.cmdsize = 80;
		dc.ilocalsym = 0;
		dc.nlocalsym = 0;
		dc.iextdefsym = sb.iext;
		dc.nextdefsym = sb.next;
		dc.iundefsym = sb.iundef;
		dc.nundefsym = sb.nundef;
		wbuf(&wp, &dc, 80);
	}

	/* LC_DYLD_EXPORTS_TRIE (also points to the same trie data;
	 * dyld may use either). */
	{
		linkedit_data_command lc = {
			.cmd = LC_DYLD_EXPORTS_TRIE,
			.cmdsize = 16,
			.dataoff = export_trie_size > 0 ? abs_export_off : 0,
			.datasize = (uint32_t)export_trie_size
		};
		wbuf(&wp, &lc, 16);
	}

	/* LC_CODE_SIGNATURE (placeholder, codesign fills it). */
	{
		linkedit_data_command lc = {
			.cmd = LC_CODE_SIGNATURE,
			.cmdsize = 16,
			.dataoff = abs_codesig_off,
			.datasize = (uint32_t)le_codesig_sz
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
		printf("binds: %zu bytes  rebases: %zu bytes\n",
		    bb.size, rb.size);
		printf("export trie: %zu bytes\n", export_trie_size);
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
	free_rebases(&rb);
	free(export_trie);
	free(out);
	free(elf);
	return 0;
}
