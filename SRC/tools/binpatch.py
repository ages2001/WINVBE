#!/usr/bin/env python3
"""
binpatch.py <WIN?00.BIN> <WIN?00.OVL> <driver.drv> [out-prefix]

Prototype of the direct installation of a display driver into an installed
Windows 1.x/2.x (the same algorithm is implemented in C in INSTALL.EXE).

Windows 1.x/2.x Setup links the core modules into WIN100.BIN / WIN200.BIN
(a DOS EXE: KERNEL's MZ header, then the NE modules one after the other)
and WIN100.OVL / WIN200.OVL:
  * per module in the .BIN: the NE header with its tables (without the
    non-resident names), then the PRELOAD segments (with their relocation
    records) and the PRELOAD resources, each on a paragraph boundary;
    NE+08h (the CRC field) holds the paragraph of the next module (0 = end),
  * per module in the .OVL: the non-resident names (NE+2Ch = paragraph in
    the .OVL), the segments that are not preloaded, and a copy of every
    resource and of every preloaded discardable segment (to reload them),
  * offsets of segments/resources (shift 4) are paragraphs in the .BIN for
    preloaded objects and in the .OVL otherwise; for preloaded discardable
    segments the "minimum allocation" field holds the paragraph of the .OVL
    copy, for preloaded resources the reserved "handle" field does.
The DISPLAY module is rebuilt from the driver file; its .OVL part is
appended at the end of the .OVL (the old part stays unused), the modules
after it in the .BIN are moved and their .BIN offsets adjusted.
"""
import struct
import sys

PRELOAD, DISCARD, RELOC = 0x40, 0x1000, 0x100


def w(d, o):
    return struct.unpack_from('<H', d, o)[0]


def modules(b):
    ne, out = 0x400, []
    while True:
        rn = w(b, ne + 0x26)
        name = bytes(b[ne + rn + 1:ne + rn + 1 + b[ne + rn]]).decode('latin-1')
        nxt = w(b, ne + 8)
        out.append((name, ne, nxt))
        if not nxt:
            return out
        ne = nxt << 4


def para(buf):
    while len(buf) % 16:
        buf.append(0)
    return len(buf) >> 4


def build_display(drv, at_para, ovl):
    """new DISPLAY block for the .BIN starting at paragraph at_para; its
    .OVL part is appended to ovl (bytearray)"""
    ne = struct.unpack_from('<I', drv, 0x3C)[0]
    nonres = struct.unpack_from('<I', drv, ne + 0x2C)[0]
    nonreslen = w(drv, ne + 0x20)
    shift = w(drv, ne + 0x32)
    hdr = bytearray(drv[ne:nonres])         # NE header and its tables
    blk = bytearray(hdr)
    segtab, restab, resnam = w(hdr, 0x22), w(hdr, 0x24), w(hdr, 0x26)
    # non-resident names
    struct.pack_into('<I', blk, 0x2C, para(ovl))
    ovl += drv[nonres:nonres + nonreslen]
    # segments
    for i in range(w(hdr, 0x1C)):
        e = segtab + i * 8
        off, ln, fl = w(hdr, e), w(hdr, e + 2), w(hdr, e + 4)
        if not off:
            continue
        p = off << shift
        size = ln or 0x10000
        data = drv[p:p + size]
        if fl & RELOC:
            n = w(drv, p + size)
            data += drv[p + size:p + size + 2 + n * 8]
        if fl & PRELOAD:
            struct.pack_into('<H', blk, e, at_para + para(blk))
            blk += data
            if fl & DISCARD:
                struct.pack_into('<H', blk, e + 6, para(ovl))
                ovl += data
        else:
            struct.pack_into('<H', blk, e, para(ovl))
            ovl += data
    # resources
    if restab != resnam:
        rs = w(hdr, restab)
        q = restab + 2
        while w(hdr, q):
            cnt = w(hdr, q + 2)
            q += 8
            for j in range(cnt):
                off, ln, fl = w(hdr, q), w(hdr, q + 2), w(hdr, q + 4)
                data = drv[off << rs:(off + ln) << rs]
                if rs != 4:
                    raise SystemExit('resource alignment must be 4')
                if fl & PRELOAD:
                    struct.pack_into('<H', blk, q, at_para + para(blk))
                    blk += data
                    struct.pack_into('<H', blk, q + 8, para(ovl))
                    ovl += data
                else:
                    struct.pack_into('<H', blk, q, para(ovl))
                    struct.pack_into('<H', blk, q + 8, 0)
                    ovl += data
                q += 12
    para(blk)
    para(ovl)
    struct.pack_into('<H', blk, 0x32, 4)
    return blk


def shift_module(b, ne, delta):
    """add delta paragraphs to the .BIN offsets of the module at ne"""
    if w(b, ne + 8):
        struct.pack_into('<H', b, ne + 8, w(b, ne + 8) + delta)
    segtab, restab, resnam = w(b, ne + 0x22), w(b, ne + 0x24), w(b, ne + 0x26)
    for i in range(w(b, ne + 0x1C)):
        e = ne + segtab + i * 8
        if w(b, e) and w(b, e + 4) & PRELOAD:
            struct.pack_into('<H', b, e, w(b, e) + delta)
    if restab != resnam:
        q = ne + restab + 2
        while w(b, q):
            cnt = w(b, q + 2)
            q += 8
            for j in range(cnt):
                if w(b, q + 4) & PRELOAD:
                    struct.pack_into('<H', b, q, w(b, q) + delta)
                q += 12


def patch(b, ovl, drv):
    b, ovl = bytearray(b), bytearray(ovl)
    mods = modules(b)
    k = [m[0] for m in mods].index('DISPLAY')
    start = mods[k][1]
    end = mods[k][2] << 4
    blk = build_display(drv, start >> 4, ovl)
    delta = (len(blk) - (end - start)) >> 4
    out = b[:start] + blk + b[end:]
    struct.pack_into('<H', out, start + 8, (start + len(blk)) >> 4)
    for name, ne, nxt in mods[k + 1:]:
        shift_module(out, ne + (delta << 4), delta)
    # KERNEL's DOS header describes the whole file
    struct.pack_into('<HH', out, 2, len(out) % 512, (len(out) + 511) // 512)
    return out, ovl


if __name__ == '__main__':
    b = open(sys.argv[1], 'rb').read()
    o = open(sys.argv[2], 'rb').read()
    d = open(sys.argv[3], 'rb').read()
    nb, no = patch(b, o, d)
    pre = sys.argv[4] if len(sys.argv) > 4 else 'out'
    open(pre + '.BIN', 'wb').write(nb)
    open(pre + '.OVL', 'wb').write(no)
    print(len(b), '->', len(nb), ';', len(o), '->', len(no))
