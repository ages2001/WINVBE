#!/usr/bin/env python3
"""rsgraft.py dst.dll src.dll out.dll
Test helper: replace the resources of dst with the resource section of src
(NT 3.1 USER loads its cursors, icons and OEM bitmaps from the display
driver).  WINVBECF does the same on the user's machine (graft_rsrc)."""
import struct, sys

def rd(fn): return bytearray(open(fn, 'rb').read())

def graft(dst, src):
    def hdr(b):
        pe = struct.unpack_from('<I', b, 0x3c)[0]
        nsec = struct.unpack_from('<H', b, pe + 6)[0]
        opt = pe + 24
        sopt = struct.unpack_from('<H', b, pe + 20)[0]
        return pe, nsec, opt, opt + sopt
    spe, sn, sopt, ssec = hdr(src)
    rva, size = struct.unpack_from('<II', src, sopt + 96 + 2 * 8)
    for i in range(sn):
        o = ssec + i * 40
        vs, va, rs, raw = struct.unpack_from('<IIII', src, o + 8)
        if va <= rva < va + max(vs, rs):
            break
    else:
        raise SystemExit('no resource section')
    blob = bytearray(src[raw:raw + max(vs, rs)][:rs] + bytes(max(0, vs - rs)))
    dpe, dn, dopt, dsec = hdr(dst)
    salign, falign = struct.unpack_from('<II', dst, dopt + 32)
    last = dsec + (dn - 1) * 40
    lvs, lva, lrs, lraw = struct.unpack_from('<IIII', dst, last + 8)
    nva = (lva + max(lvs, lrs) + salign - 1) // salign * salign
    nraw = (len(dst) + falign - 1) // falign * falign
    delta = nva - va
    # relocate IMAGE_RESOURCE_DATA_ENTRY.OffsetToData
    base = rva - va       # directory offsets are relative to its start
    def walk(off):
        n = sum(struct.unpack_from('<HH', blob, base + off + 12))
        for k in range(n):
            name, ptr = struct.unpack_from('<II', blob, base + off + 16 + k * 8)
            if ptr & 0x80000000:
                walk(ptr & 0x7fffffff)
            else:
                d = struct.unpack_from('<I', blob, base + ptr)[0]
                struct.pack_into('<I', blob, base + ptr, d + delta)
    walk(0)
    if dsec + (dn + 1) * 40 > struct.unpack_from('<I', dst, dopt + 60)[0]:
        raise SystemExit('no room for a section header')
    rawsize = (len(blob) + falign - 1) // falign * falign
    sh = struct.pack('<8sIIIIIIHHI', b'.rsrc3', len(blob), nva, rawsize, nraw, 0, 0, 0, 0, 0x40000040)
    dst[dsec + dn * 40:dsec + dn * 40 + 40] = sh
    struct.pack_into('<H', dst, dpe + 6, dn + 1)
    struct.pack_into('<II', dst, dopt + 96 + 2 * 8, nva + (rva - va), size)
    struct.pack_into('<I', dst, dopt + 56, (nva + len(blob) + salign - 1) // salign * salign)
    struct.pack_into('<I', dst, dopt + 64, 0)       # checksum
    dst += bytes(nraw - len(dst)) + blob + bytes(rawsize - len(blob))
    struct.pack_into('<I', dst, dopt + 64, pe_checksum(dst, dopt + 64))
    return dst

def pe_checksum(b, csoff):
    s = 0
    n = len(b) & ~1
    for i in range(0, n, 2):
        if i == csoff or i == csoff + 2: continue
        s += b[i] | b[i + 1] << 8
        s = (s & 0xffff) + (s >> 16)
    if len(b) & 1: s += b[-1]; s = (s & 0xffff) + (s >> 16)
    s = (s & 0xffff) + (s >> 16)
    return s + len(b)

if __name__ == '__main__':
    out = graft(rd(sys.argv[1]), rd(sys.argv[2]))
    open(sys.argv[3], 'wb').write(out)
