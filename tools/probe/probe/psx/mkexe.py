#!/usr/bin/env python3
"""The probe's ELF as a PS-X EXE (psx-spx, CDROM File Formats, "PS-X EXE"),
and a check of one.

  mkexe.py probe.elf probe.exe      write the EXE
  mkexe.py --check probe.elf probe.exe
                                    check its header against the ELF and the
                                    memory map (docs/probe.md, "Memory map")

The header is 800h bytes: "PS-X EXE", the initial PC, GP 0, the load
address and size (a multiple of 800h), no data or BSS fill (crt0.S clears
BSS), the stack at 801FFFF0h; the region marker is left zero (psx-spx: the
BIOS does not check it). Then the ELF's loadable sections, from the load
address up. Python 3, standard library only.
"""
import struct
import sys

LOAD = 0x80010000
STACK = 0x801FFFF0
STACK_RESERVE = 0x10000
RAM_END = 0x80200000


def read_elf(path):
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        sys.exit(f"{path}: not a 32-bit little-endian ELF")
    (e_type, e_machine, _, e_entry, _, e_shoff, _, _, _, _, e_shentsize, e_shnum,
     e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    if e_machine != 8:
        sys.exit(f"{path}: not MIPS")
    secs = []
    for i in range(e_shnum):
        secs.append(struct.unpack_from("<IIIIIIIIII", data, e_shoff + i * e_shentsize))
    strtab = secs[e_shstrndx]

    def name(off):
        s = data[strtab[4] + off:]
        return s[:s.index(b"\0")].decode()

    loads, symbols = [], {}
    for s in secs:
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size = s[:6]
        if sh_flags & 2 and sh_size:  # SHF_ALLOC
            loads.append((name(sh_name), sh_type, sh_addr, sh_offset, sh_size))
        if sh_type == 2:  # SHT_SYMTAB
            link = secs[s[6]]
            for k in range(sh_size // 16):
                st_name, st_value, _, _, _, _ = struct.unpack_from("<IIIBBH", data, sh_offset + 16 * k)
                n = data[link[4] + st_name:]
                symbols[n[:n.index(b"\0")].decode()] = st_value
    return data, e_entry, loads, symbols


def image(path):
    data, entry, loads, symbols = read_elf(path)
    end = LOAD
    for _, sh_type, addr, _, size in loads:
        if sh_type != 8:  # not NOBITS
            end = max(end, addr + size)
    body = bytearray(end - LOAD)
    for nm, sh_type, addr, off, size in loads:
        if sh_type == 8:
            continue
        if addr < LOAD:
            sys.exit(f"{path}: section {nm} at {addr:#x} is below {LOAD:#x}")
        body[addr - LOAD:addr - LOAD + size] = data[off:off + size]
    body += bytes(-len(body) % 0x800)
    return entry, bytes(body), loads, symbols


def header(entry, size):
    h = bytearray(0x800)
    h[0:8] = b"PS-X EXE"
    struct.pack_into("<IIIIIIIIII", h, 0x10, entry, 0, LOAD, size, 0, 0, 0, 0, STACK, 0)
    return bytes(h)


def check(elf, exe):
    entry, body, loads, symbols = image(elf)
    got = open(exe, "rb").read()
    errors = []
    if got[:8] != b"PS-X EXE" or any(got[8:16]):
        errors.append("ID")
    pc, gp, taddr, tsize, daddr, dsize, baddr, bsize, spbase, spoff = struct.unpack_from("<10I", got, 0x10)
    if pc != entry or not LOAD <= pc < LOAD + tsize:
        errors.append(f"initial PC {pc:#x} (the ELF's entry is {entry:#x})")
    if gp != 0:
        errors.append(f"GP {gp:#x}")
    if taddr != LOAD:
        errors.append(f"load address {taddr:#x}")
    if tsize % 0x800 or tsize != len(got) - 0x800 or tsize != len(body):
        errors.append(f"size {tsize:#x} (file {len(got):#x} bytes, image {len(body):#x})")
    if (daddr, dsize, baddr, bsize) != (0, 0, 0, 0):
        errors.append("data or BSS fill set")
    if (spbase, spoff) != (STACK, 0):
        errors.append(f"stack {spbase:#x} + {spoff:#x}")
    if any(got[0x38:0x800]):
        errors.append("reserved or marker bytes not zero")
    if got[0x800:] != body:
        errors.append("the code is not the ELF's")
    if "_start" not in symbols or symbols["_start"] != entry:
        errors.append("the entry is not _start")
    bss_end = symbols.get("__bss_end", 0)
    if not LOAD + tsize <= bss_end + 0x800 or bss_end > STACK + 0x10 - STACK_RESERVE:
        errors.append(f"BSS ends at {bss_end:#x}, past {STACK + 0x10 - STACK_RESERVE:#x}")
    for nm, sh_type, addr, _, size in loads:
        if addr < LOAD or addr + size > RAM_END:
            errors.append(f"section {nm} at {addr:#x}+{size:#x} is outside {LOAD:#x}-{RAM_END:#x}")
    if errors:
        for e in errors:
            print(f"{exe}: {e}")
        sys.exit(1)
    print(f"{exe}: PS-X EXE, PC {pc:#010x}, {tsize:#x} bytes at {taddr:#010x} "
          f"(code and data end {LOAD + len(body.rstrip(bytes(1))):#010x}), BSS to {bss_end:#010x}, "
          f"stack {spbase:#010x} with {STACK + 0x10 - bss_end:#x} bytes free below it")


def main():
    args = sys.argv[1:]
    if len(args) == 3 and args[0] == "--check":
        check(args[1], args[2])
    elif len(args) == 2:
        entry, body, _, _ = image(args[0])
        with open(args[1], "wb") as f:
            f.write(header(entry, len(body)) + body)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
