#!/usr/bin/env python3
"""
AOT patcher: scan ELF aarch64 binaries and replace:
  SVC #0          (0xD4000001) -> BRK #0x0001  (0xD4200020)
  MSR TPIDR_EL0   (0xD51BD04x) -> BRK #(0x0100|Rn)
  MRS TPIDR_EL0   (0xD53BD04x) -> BRK #(0x0200|Rn)

Usage: aot_patch.py <file> [<file> ...]

Pure Python (no external deps) so it does not need to be
compiled and signed on every macOS 26 build, where freshly
compiled unsigned binaries get SIGKILLed by the kernel.
"""

import os
import struct
import sys

ELFMAG = b"\x7fELF"
ELFCLASS64 = 2
ELFDATA2LSB = 1
EM_AARCH64 = 183
PT_LOAD = 1
PF_X = 1


def patch_file(path):
    """Return number of patches applied, or -1 on error."""
    try:
        fd = os.open(path, os.O_RDWR)
    except OSError as e:
        print(f"aot_patch: open({path}): {e.strerror}", file=sys.stderr)
        return -1

    try:
        st = os.fstat(fd)
        if st.st_size < 64:
            # Too small to be an ELF64; skip silently.
            return 0
        data = os.read(fd, st.st_size)
        if len(data) != st.st_size:
            print(f"aot_patch: short read on {path}", file=sys.stderr)
            return -1

        # Parse ELF64 header.
        if data[:4] != ELFMAG:
            return 0  # not an ELF
        if data[4] != ELFCLASS64 or data[5] != ELFDATA2LSB:
            return 0  # not 64-bit little-endian
        e_machine = struct.unpack_from("<H", data, 18)[0]
        if e_machine != EM_AARCH64:
            return 0  # not aarch64

        e_phoff, e_shoff = struct.unpack_from("<QQ", data, 32)
        e_phentsize, e_phnum = struct.unpack_from("<HH", data, 54)

        # Validate program-header table fits in the file.
        if e_phentsize < 56 or e_phoff > st.st_size or \
           e_phnum * e_phentsize > st.st_size - e_phoff:
            print(f"aot_patch: {path} malformed phdr table "
                  f"(phoff={e_phoff} phnum={e_phnum} "
                  f"phentsize={e_phentsize} size={st.st_size})",
                  file=sys.stderr)
            return -1

        patched = 0
        for i in range(e_phnum):
            base = e_phoff + i * e_phentsize
            p_type, p_flags = struct.unpack_from("<II", data, base)
            if p_type != PT_LOAD or not (p_flags & PF_X):
                continue
            p_offset, p_vaddr, p_paddr, p_filesz = \
                struct.unpack_from("<QQQQ", data, base + 8)
            if p_offset + p_filesz > st.st_size:
                continue

            # Walk 4-byte instructions in the executable segment.
            count = p_filesz // 4
            for j in range(count):
                ioff = p_offset + j * 4
                insn = struct.unpack_from("<I", data, ioff)[0]
                if insn == 0xD4000001:
                    # SVC #0 -> BRK #0x0001
                    new_insn = 0xD4200020
                elif (insn & 0xFFFFFFE0) == 0xD51BD040:
                    # MSR TPIDR_EL0, Xn
                    rn = insn & 0x1F
                    new_insn = 0xD4200000 | ((0x0100 | rn) << 5)
                elif (insn & 0xFFFFFFE0) == 0xD53BD040:
                    # MRS Xn, TPIDR_EL0
                    rn = insn & 0x1F
                    new_insn = 0xD4200000 | ((0x0200 | rn) << 5)
                else:
                    continue
                # Write the 4-byte patch back.
                os.pwrite(fd, struct.pack("<I", new_insn), ioff)
                patched += 1

        return patched
    finally:
        os.close(fd)


def main():
    if len(sys.argv) < 2:
        print("usage: aot_patch.py <file> ...", file=sys.stderr)
        return 1

    total = 0
    had_error = False
    for path in sys.argv[1:]:
        n = patch_file(path)
        if n < 0:
            had_error = True
            continue
        if n > 0:
            print(f"  {path}: {n} patches")
            total += n

    print(f"Total: {total} patches")
    return 1 if had_error else 0


if __name__ == "__main__":
    sys.exit(main())
