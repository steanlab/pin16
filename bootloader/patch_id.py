#!/usr/bin/env python3
"""Writes the variant id byte into the version word of the bootloader's info struct (the last byte of the version).

The application reads it back (4F 0D answers "BTL main=... props=... id=03"): 03 = the Solum tags.
The info struct is found through the pointer at word 13 of the vector table (the first bytes of the image).
Reads and writes Motorola S-record files (S1/S2/S3 data, any order); needs no extra Python package.
Usage: patch_id.py <in.s37> <out.s37> [id, default 3]
"""
import sys

def read_srec(path):
    mem = {}
    for ln, line in enumerate(open(path), 1):
        line = line.strip()
        if not line or line[0] != "S":
            continue
        t = line[1]
        if t not in "123":
            continue
        raw = bytes.fromhex(line[2:])
        if (sum(raw) & 0xFF) != 0xFF:
            sys.exit("patch_id: checksum error in %s line %d" % (path, ln))
        al = int(t) + 1
        addr = int.from_bytes(raw[1:1 + al], "big")
        for i, v in enumerate(raw[1 + al:-1]):
            mem[addr + i] = v
    return mem

def write_srec(mem, path):
    out = ["S00600004844521B"]
    addrs = sorted(mem)
    i = 0
    while i < len(addrs):
        j = i
        while j + 1 < len(addrs) and addrs[j + 1] == addrs[j] + 1 and j + 1 - i < 32:
            j += 1
        chunk = bytes(mem[a] for a in addrs[i:j + 1])
        body = bytes([len(chunk) + 5]) + addrs[i].to_bytes(4, "big") + chunk
        out.append("S3" + (body + bytes([(~sum(body)) & 0xFF])).hex().upper())
        i = j + 1
    out.append("S70500000000FA")
    open(path, "w").write("\n".join(out) + "\n")

def word(mem, a):
    return int.from_bytes(bytes(mem[a + k] for k in range(4)), "little")

def main(src, dst, idbyte):
    mem = read_srec(src)
    base = min(mem)
    p = word(mem, base + 13 * 4)
    if p not in mem or p + 36 - 1 not in mem:
        sys.exit("patch_id: info struct pointer %#x is outside the image" % p)
    if word(mem, p) != 0xFA79B713:
        sys.exit("patch_id: no info struct at %#x (magic %#x)" % (p, word(mem, p)))
    if (word(mem, p + 28) & 0xFF) != 0x40:
        sys.exit("patch_id: the info struct is not that of a bootloader (type %#x)" % (word(mem, p + 28) & 0xFF))
    ver = word(mem, p + 32)
    new = (ver & 0xFFFFFF00) | idbyte
    for k, v in enumerate(new.to_bytes(4, "little")):
        mem[p + 32 + k] = v
    write_srec(mem, dst)
    print("patch_id: version word %#010x -> %#010x" % (ver, new))

if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2], int(sys.argv[3], 0) if len(sys.argv) > 3 else 3)
