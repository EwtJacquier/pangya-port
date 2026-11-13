#!/usr/bin/env python3
"""Apply code/data patches to the decrypted PSP PRX and emit an equivalent CWCheat file.

Addresses are PSP runtime addresses (image base 0x08804000, same as Ghidra and PPSSPP).

Patch file format (text):
    [Mod name]                 # starts a group (becomes one CWCheat entry)
    0x0880ABCD 00000000        # one or more 32-bit words, hex, written little-endian
    0x0880ABD1 03E00008 00000000
    # comments allowed anywhere

Outputs:
  - patched ELF (to put back into the ISO with iso_replace.py)
  - optional CWCheat .ini (for quick tests in PPSSPP, no ISO rebuild needed)

Warns when a patched word is covered by a PRX relocation: the loader would add the load
address on top of the patched bytes. In that case use the cheat, or pick another spot.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

IMAGE_BASE = 0x08804000
CHEAT_BASE = 0x08800000
SHT_PRXRELOC = 0x700000A0


def parse_patches(text):
    groups, current = [], None
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        m = re.fullmatch(r"\[(.+)\]", line)
        if m:
            current = {"name": m.group(1).strip(), "writes": []}
            groups.append(current)
            continue
        parts = line.split()
        if current is None:
            current = {"name": "Patch", "writes": []}
            groups.append(current)
        try:
            addr = int(parts[0], 16)
            words = [int(w, 16) for w in parts[1:]]
        except ValueError:
            sys.exit(f"line {lineno}: expected 'ADDR WORD [WORD...]' in hex: {raw!r}")
        if not words or addr % 4:
            sys.exit(f"line {lineno}: need at least one word and a 4-byte aligned address")
        for i, w in enumerate(words):
            current["writes"].append((addr + 4 * i, w & 0xFFFFFFFF))
    return groups


class Prx:
    def __init__(self, data):
        self.data = bytearray(data)
        if data[:4] != b"\x7fELF":
            sys.exit("not an ELF (did you pass the decrypted EBOOT?)")
        (e_phoff, e_shoff) = struct.unpack_from("<II", data, 0x1C)
        e_phentsize, e_phnum, e_shentsize, e_shnum = struct.unpack_from("<HHHH", data, 0x2A)
        self.segments = []
        for i in range(e_phnum):
            p_type, p_off, p_vaddr, _p_paddr, p_filesz, p_memsz = struct.unpack_from(
                "<IIIIII", data, e_phoff + i * e_phentsize)
            if p_type == 1:
                self.segments.append((p_off, p_vaddr, p_filesz, p_memsz))
        self.relocated = set()
        for i in range(e_shnum):
            _name, sh_type, _flags, _addr, sh_off, sh_size = struct.unpack_from(
                "<IIIIII", data, e_shoff + i * e_shentsize)
            if sh_type != SHT_PRXRELOC:
                continue
            for j in range(sh_off, sh_off + sh_size, 8):
                r_offset, r_info = struct.unpack_from("<II", data, j)
                rtype, ofs_seg = r_info & 0xFF, (r_info >> 8) & 0xFF
                if rtype == 0 or ofs_seg >= len(self.segments):
                    continue
                self.relocated.add(IMAGE_BASE + self.segments[ofs_seg][1] + r_offset)

    def file_offset(self, addr):
        rel = addr - IMAGE_BASE
        for p_off, p_vaddr, p_filesz, _ in self.segments:
            if p_vaddr <= rel and rel + 4 <= p_vaddr + p_filesz:
                return p_off + rel - p_vaddr
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf", type=Path, help="decrypted EBOOT (ELF/PRX)")
    ap.add_argument("patches", type=Path)
    ap.add_argument("-o", "--out", type=Path, help="patched ELF output")
    ap.add_argument("--cheats", type=Path, help="write CWCheat .ini (e.g. ULUS10438.ini)")
    ap.add_argument("--game-id", default="ULUS-10438")
    ap.add_argument("--title", default="PANGYA : Fantasy Golf")
    args = ap.parse_args()

    prx = Prx(args.elf.read_bytes())
    groups = parse_patches(args.patches.read_text(encoding="utf-8"))
    warnings = 0

    for g in groups:
        for addr, word in g["writes"]:
            off = prx.file_offset(addr)
            if off is None:
                sys.exit(f"[{g['name']}] 0x{addr:08X} is outside the file-backed segments (bss?)")
            old = struct.unpack_from("<I", prx.data, off)[0]
            flag = ""
            if addr in prx.relocated:
                flag = "  !! RELOCATED: loader will modify this word; ELF patch unreliable here"
                warnings += 1
            print(f"[{g['name']}] 0x{addr:08X} (file 0x{off:06X}) {old:08X} -> {word:08X}{flag}")
            struct.pack_into("<I", prx.data, off, word)

    if args.out:
        args.out.write_bytes(prx.data)
        print(f"patched ELF -> {args.out}")
    if args.cheats:
        lines = [f"_S {args.game_id}", f"_G {args.title}"]
        for g in groups:
            lines.append(f"_C0 {g['name']}")
            for addr, word in g["writes"]:
                lines.append(f"_L 0x2{addr - CHEAT_BASE:07X} 0x{word:08X}")
        args.cheats.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"CWCheat -> {args.cheats}")
    if warnings:
        print(f"{warnings} relocation warning(s)", file=sys.stderr)


if __name__ == "__main__":
    main()
