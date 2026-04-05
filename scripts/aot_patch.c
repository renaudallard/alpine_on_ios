/*
 * AOT patcher: scan ELF aarch64 binaries and replace:
 *   SVC #0          (0xD4000001) -> BRK #0x0001  (0xD4200020)
 *   MSR TPIDR_EL0   (0xD51BD04x) -> BRK #(0x0100|Rn)
 *   MRS TPIDR_EL0   (0xD53BD04x) -> BRK #(0x0200|Rn)
 *
 * Usage: aot_patch <file> [<file> ...]
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define EI_NIDENT	16
#define ELFMAG		"\177ELF"
#define ELFCLASS64	2
#define ELFDATA2LSB	1
#define EM_AARCH64	183
#define PT_LOAD		1
#define PF_X		1

typedef struct {
	uint8_t		e_ident[EI_NIDENT];
	uint16_t	e_type;
	uint16_t	e_machine;
	uint32_t	e_version;
	uint64_t	e_entry;
	uint64_t	e_phoff;
	uint64_t	e_shoff;
	uint32_t	e_flags;
	uint16_t	e_ehsize;
	uint16_t	e_phentsize;
	uint16_t	e_phnum;
	uint16_t	e_shentsize;
	uint16_t	e_shnum;
	uint16_t	e_shstrndx;
} Elf64_Ehdr;

typedef struct {
	uint32_t	p_type;
	uint32_t	p_flags;
	uint64_t	p_offset;
	uint64_t	p_vaddr;
	uint64_t	p_paddr;
	uint64_t	p_filesz;
	uint64_t	p_memsz;
	uint64_t	p_align;
} Elf64_Phdr;

static int
patch_file(const char *path)
{
	int		 fd, patched;
	struct stat	 st;
	uint8_t		*map;
	Elf64_Ehdr	*ehdr;
	Elf64_Phdr	*phdr;
	int		 i;

	fd = open(path, O_RDWR);
	if (fd < 0)
		return -1;

	if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
		close(fd);
		return -1;
	}

	map = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE,
	    MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return -1;
	}

	ehdr = (Elf64_Ehdr *)map;

	/* Validate ELF aarch64. */
	if (memcmp(ehdr->e_ident, ELFMAG, 4) != 0 ||
	    ehdr->e_ident[4] != ELFCLASS64 ||
	    ehdr->e_ident[5] != ELFDATA2LSB ||
	    ehdr->e_machine != EM_AARCH64) {
		munmap(map, st.st_size);
		close(fd);
		return -1;
	}

	patched = 0;

	for (i = 0; i < ehdr->e_phnum; i++) {
		uint32_t	*insns;
		size_t		 count, j;
		uint64_t	 off, sz;

		phdr = (Elf64_Phdr *)(map + ehdr->e_phoff +
		    (uint64_t)i * ehdr->e_phentsize);

		if (phdr->p_type != PT_LOAD)
			continue;
		if (!(phdr->p_flags & PF_X))
			continue;

		off = phdr->p_offset;
		sz = phdr->p_filesz;
		if (off + sz > (uint64_t)st.st_size)
			continue;

		insns = (uint32_t *)(map + off);
		count = sz / 4;

		for (j = 0; j < count; j++) {
			uint32_t insn = insns[j];
			int rn;

			if (insn == 0xD4000001) {
				/* SVC #0 -> BRK #0x0001 */
				insns[j] = 0xD4200020;
				patched++;
			} else if ((insn & 0xFFFFFFE0) == 0xD51BD040) {
				/* MSR TPIDR_EL0, Xn */
				rn = insn & 0x1F;
				insns[j] = 0xD4200000 |
				    ((0x0100 | rn) << 5);
				patched++;
			} else if ((insn & 0xFFFFFFE0) == 0xD53BD040) {
				/* MRS Xn, TPIDR_EL0 */
				rn = insn & 0x1F;
				insns[j] = 0xD4200000 |
				    ((0x0200 | rn) << 5);
				patched++;
			}
		}
	}

	munmap(map, st.st_size);
	close(fd);

	if (patched > 0)
		printf("  %s: %d patches\n", path, patched);

	return patched;
}

int
main(int argc, char **argv)
{
	int	i, total;

	if (argc < 2) {
		fprintf(stderr, "usage: aot_patch <file> ...\n");
		return 1;
	}

	total = 0;
	for (i = 1; i < argc; i++) {
		int n = patch_file(argv[i]);
		if (n > 0)
			total += n;
	}

	printf("Total: %d patches\n", total);
	return 0;
}
