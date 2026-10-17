#!/usr/bin/env python3
"""mkvxd32.py NAME a0.bin a1.bin out.vxd
Build a static 32-bit VxD (LE format) from a flat binary assembled twice,
at org 0 (a0.bin) and at org 10000h (a1.bin): the dwords that differ by
10000h are the relocations. The binary starts with the DDB (control
procedure at DDB+18h); one object (code + data, preloaded)."""
import struct, sys

PAGE = 4096

def build(name, a0, a1):
    assert len(a0) == len(a1)
    relocs = []
    i = 0
    while i + 4 <= len(a0):
        v0, = struct.unpack_from('<I', a0, i)
        v1, = struct.unpack_from('<I', a1, i)
        if v1 - v0 == 0x10000:
            relocs.append((i, v0))
            i += 4
        else:
            i += 1
    data = a0
    npages = (len(data) + PAGE - 1) // PAGE
    # fixup records per page
    recs = [b''] * npages
    for off, tgt in relocs:
        pg = off // PAGE
        if (off % PAGE) + 4 > PAGE:
            raise SystemExit('relocation across a page boundary at %x' % off)
        if tgt < 0x10000:
            recs[pg] += struct.pack('<BBHBH', 0x07, 0x00, off % PAGE, 1, tgt)
        else:
            recs[pg] += struct.pack('<BBHBI', 0x07, 0x10, off % PAGE, 1, tgt)
    fixrec = b''.join(recs)
    fixpage = b''
    acc = 0
    for r in recs:
        fixpage += struct.pack('<I', acc); acc += len(r)
    fixpage += struct.pack('<I', acc)

    name8 = name.upper().encode()
    objtab = struct.pack('<IIIII', len(data), 0, 0x2000 | 0x40 | 0x04 | 0x02 | 0x01, 1, npages) + b'LCOD'
    pagetab = b''.join(bytes([0, (i + 1) >> 8 & 0xFF, (i + 1) & 0xFF, 0]) for i in range(npages))
    resnames = bytes([len(name8)]) + name8 + b'\0\0' + b'\0'
    ddbn = name8 + b'_DDB'
    nonres = bytes([len(ddbn)]) + ddbn + b'\1\0' + b'\0'
    entry = struct.pack('<BBH', 1, 3, 1) + struct.pack('<BI', 3, 0) + b'\0'
    HDR = 0xC4
    off = HDR
    o_obj = off; off += len(objtab)
    o_pt = off; off += len(pagetab)
    o_res = off; off += len(resnames)
    o_ent = off; off += len(entry)
    loader_size = off - o_obj
    o_fpt = off; off += len(fixpage)
    o_frt = off; off += len(fixrec)
    fixup_size = off - o_fpt
    o_imp = off
    off += 1
    le_size = off
    mz = bytearray(0x80)
    mz[0:2] = b'MZ'
    struct.pack_into('<HHHHHHHHHHHHH', mz, 2, 0x80, 1, 0, 4, 0x10, 0xFFFF, 0, 0xB8, 0, 0, 0, 0x40, 0)
    stub = b'\x0e\x1f\xba\x0e\x00\xb4\x09\xcd\x21\xb8\x01\x4c\xcd\x21' + b'This is a Windows virtual device driver.\r\n$'
    mz[0x40:0x40 + len(stub)] = stub
    struct.pack_into('<I', mz, 0x3C, 0x80)
    data_off = (0x80 + le_size + 0x1FF) & ~0x1FF
    hdr = bytearray(HDR)
    hdr[0:2] = b'LE'
    lastpage = len(data) - (npages - 1) * PAGE
    vals = {0x08: ('H', 2), 0x0A: ('H', 4), 0x10: ('I', 0x00038000), 0x14: ('I', npages),
            0x28: ('I', PAGE), 0x2C: ('I', lastpage), 0x30: ('I', fixup_size), 0x38: ('I', loader_size),
            0x40: ('I', o_obj), 0x44: ('I', 1), 0x48: ('I', o_pt), 0x58: ('I', o_res), 0x5C: ('I', o_ent),
            0x68: ('I', o_fpt), 0x6C: ('I', o_frt), 0x70: ('I', o_imp), 0x78: ('I', o_imp),
            0x80: ('I', data_off), 0x84: ('I', npages), 0xC0: ('I', 0x03000000)}
    for o, (f, v) in vals.items():
        struct.pack_into('<' + f, hdr, o, v)
    le = bytes(hdr) + objtab + pagetab + resnames + entry + fixpage + fixrec + b'\0'
    assert len(le) == le_size
    out = bytearray(bytes(mz) + le)
    out = out.ljust(data_off, b'\0')
    for i in range(npages):
        p = data[i * PAGE:(i + 1) * PAGE]
        out += p if i == npages - 1 else p.ljust(PAGE, b'\0')
    nonres_off = len(out)
    out += nonres
    struct.pack_into('<II', out, 0x80 + 0x88, nonres_off, len(nonres))
    return bytes(out), relocs

if __name__ == '__main__':
    name, f0, f1, outf = sys.argv[1:5]
    out, relocs = build(name, open(f0, 'rb').read(), open(f1, 'rb').read())
    open(outf, 'wb').write(out)
    print('%s: %d bytes, %d relocations' % (outf, len(out), len(relocs)))
