#!/usr/bin/env python3
"""Build heman.pak from main.obb (copy it to ux0:data/heman/).

Every file <= 256 KB from screen-large/, sounds/ and the root of the OBB goes
into one file, so the port opens the memory card once instead of once per
asset (1-3.5 ms each). Format (little endian), read by source/reimpl/memfile.c:
  "HMPK", u32 version=1, u32 count, u32 names_size
  count x {u32 hash, u32 offset, u32 size, u32 name_offset}
  names: NUL-terminated lowercase paths relative to the data dir
  file data
hash = FNV-1a 32 of the name.

usage: make_pak.py main.obb heman.pak
"""
import struct, sys, zipfile

MAX_SIZE = 256 * 1024
DIRS = ("screen-large/", "sounds/")

def fnv1a(s):
    h = 2166136261
    for b in s.encode():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h

def wanted(name):
    return name.lower().startswith(DIRS) or "/" not in name

def main(obb, out):
    z = zipfile.ZipFile(obb)
    infos = [i for i in z.infolist()
             if not i.is_dir() and i.file_size <= MAX_SIZE and wanted(i.filename)]
    names, seen = [], set()
    for i in infos:
        key = "/".join(p for p in i.filename.replace("\\", "/").lower().split("/") if p)
        if key in seen:
            sys.exit("duplicate name (case): " + key)
        seen.add(key)
        names.append(key)
    blob = bytearray()
    name_offs = []
    for n in names:
        name_offs.append(len(blob))
        blob += n.encode() + b"\0"
    header_size = 16 + 16 * len(infos) + len(blob)
    data_start = (header_size + 15) & ~15
    entries, off = [], data_start
    for i, n, no in zip(infos, names, name_offs):
        entries.append(struct.pack("<IIII", fnv1a(n), off, i.file_size, no))
        off = (off + i.file_size + 15) & ~15
    with open(out, "wb") as f:
        f.write(b"HMPK" + struct.pack("<III", 1, len(infos), len(blob)))
        f.write(b"".join(entries))
        f.write(blob)
        f.write(b"\0" * (data_start - header_size))
        for i in infos:
            data = z.read(i)
            assert len(data) == i.file_size
            f.write(data)
            f.write(b"\0" * ((-len(data)) % 16))
    print(f"{out}: {len(infos)} files, {off / 1048576:.1f} MB")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
