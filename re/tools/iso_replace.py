#!/usr/bin/env python3
"""Copy a PSP ISO and replace files inside the copy, in place (no full rebuild).

The new file must fit in the sectors the old one already occupies; the directory
record size is updated. The original ISO is never modified.

  python3 iso_replace.py original.iso modded.iso \
      PSP_GAME/SYSDIR/EBOOT.BIN=EBOOT_patched.elf \
      PSP_GAME/USRDIR/clientsetting.txt=my_clientsetting.txt

  python3 iso_replace.py --in-place work.iso PSP_GAME/SYSDIR/EBOOT.BIN=EBOOT_patched.elf
      (edits an existing working copy without copying 1.5 GB; never use on the original)

PPSSPP and custom firmware run an unencrypted ELF EBOOT.BIN directly.
Note: PPSSPP's extracted-folder mode does NOT work for this game (sector-seek ioctl on
pspdata.pak fails), so mods must go into an ISO.
"""
import shutil
import struct
import sys
from pathlib import Path

SECTOR = 2048


def read_record(buf, pos):
    length = buf[pos]
    if length == 0:
        return None
    lba = struct.unpack_from("<I", buf, pos + 2)[0]
    size = struct.unpack_from("<I", buf, pos + 10)[0]
    flags = buf[pos + 25]
    name_len = buf[pos + 32]
    name = buf[pos + 33:pos + 33 + name_len].decode("latin1").split(";")[0]
    return length, lba, size, flags, name


def find(fh, path):
    """Return (record_file_offset, lba, size) for an ISO9660 path, case-insensitive."""
    fh.seek(16 * SECTOR)
    pvd = fh.read(SECTOR)
    if pvd[1:6] != b"CD001":
        sys.exit("not an ISO9660 image")
    root = read_record(pvd, 156)
    lba, size, rec_off = root[1], root[2], None
    for part in path.strip("/").split("/"):
        fh.seek(lba * SECTOR)
        buf = fh.read(size)
        found = None
        pos = 0
        while pos < len(buf):
            rec = read_record(buf, pos)
            if rec is None:  # records never cross sectors; skip to next one
                pos = (pos // SECTOR + 1) * SECTOR
                continue
            if rec[4].upper() == part.upper():
                found = (lba * SECTOR + pos, rec[1], rec[2])
                break
            pos += rec[0]
        if not found:
            sys.exit(f"not found in ISO: {path} (at {part!r})")
        rec_off, lba, size = found
    return rec_off, lba, size


def main():
    argv = sys.argv[1:]
    in_place = bool(argv) and argv[0] == "--in-place"
    if in_place:
        # Edits a working copy directly (fast iteration). Never point this at the original ISO.
        argv = argv[1:]
        if len(argv) < 2:
            sys.exit(__doc__)
        dst = Path(argv[0])
        rest = argv[1:]
    else:
        if len(argv) < 3:
            sys.exit(__doc__)
        src, dst = Path(argv[0]), Path(argv[1])
        if src.resolve() == dst.resolve():
            sys.exit("output must be a different file than the original")
        rest = argv[2:]
    pairs = [a.split("=", 1) for a in rest]
    if any(len(p) != 2 for p in pairs):
        sys.exit("replacements must look like ISO/PATH=local_file")

    if not in_place:
        shutil.copyfile(src, dst)
    with dst.open("r+b") as fh:
        for iso_path, local in pairs:
            data = Path(local).read_bytes()
            rec_off, lba, old_size = find(fh, iso_path)
            capacity = -(-old_size // SECTOR) * SECTOR if old_size else SECTOR
            if len(data) > capacity:
                if not in_place:
                    dst.unlink()
                sys.exit(f"{iso_path}: new file {len(data)} B > available {capacity} B; needs a full ISO rebuild")
            fh.seek(lba * SECTOR)
            fh.write(data + b"\0" * (capacity - len(data)))
            fh.seek(rec_off + 10)
            fh.write(struct.pack("<I", len(data)) + struct.pack(">I", len(data)))
            print(f"{iso_path}: {old_size} -> {len(data)} bytes (LBA {lba})")
    print(f"modded ISO -> {dst}")


if __name__ == "__main__":
    main()
