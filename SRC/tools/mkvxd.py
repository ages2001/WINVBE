#!/usr/bin/env python3
"""Build a minimal static VxD (LE format) around a real mode initialization
image: mkvxd.py NAME rm.bin memsize out.vxd

Object 1 (32-bit): the Device Descriptor Block NAME_DDB and a control
procedure that only returns success (clc / ret).
Object 2 (16-bit): the real mode image; the module start address (start
object 2, EIP 0) is the real mode initialization entry point, which the
Windows loader calls in real mode.
"""
import struct, sys

PAGE = 4096

def le_vxd(name, rm, memsize):
    name8 = name.upper().encode()[:8].ljust(8, b' ')
    # ---- object 1: DDB + control proc
    ddb = struct.pack('<IHHBBH8sIIIIIIIII',
                      0,          # DDB_Next
                      0x030A,     # DDB_SDK_Version (Windows 3.1 style)
                      0,          # DDB_Req_Device_Number (undefined)
                      1, 0,       # version 1.0
                      0,          # DDB_Flags
                      name8,
                      0x80000000, # DDB_Init_Order (undefined)
                      0x38,       # DDB_Control_Proc (fixup below)
                      0, 0, 0, 0, 0, 0, 0)
    assert len(ddb) == 0x38
    obj1 = ddb + b'\xF8\xC3'        # clc ; ret
    obj1_fix = [(0x18, 1, 0x38)]    # (offset in page, target object, target offset)

    rm = rm.ljust(memsize, b'\0')
    if len(rm) > 0x4000:     # Windows 95 does not run a larger one
        raise SystemExit('real mode image too large')

    # (data, virtual size, flags, segment type tag in the reserved field:
    #  the Windows 9x loader uses it, as LINK386 writes it)
    objs = [
        (obj1, len(obj1), 0x2000 | 0x40 | 0x04 | 0x01, b'LCOD'),  # 32-bit locked code, preload
        (rm, len(rm), 0x1000 | 0x04 | 0x01, b'RCOD'),             # 16-bit real mode init
    ]

    # pages
    pages = []          # (data, object index)
    pt_first = []
    for i, (d, vs, fl, tag) in enumerate(objs):
        pt_first.append(len(pages) + 1)
        n = (len(d) + PAGE - 1) // PAGE
        for p in range(n):
            pages.append(d[p * PAGE:(p + 1) * PAGE])
    npages = len(pages)
    preload_pages = sum((len(d) + PAGE - 1) // PAGE for d, vs, fl, tag in objs if fl & 0x40)

    HDR = 0xC4
    objtab = b''
    for i, (d, vs, fl, tag) in enumerate(objs):
        n = (len(d) + PAGE - 1) // PAGE
        objtab += struct.pack('<IIIII', vs, 0, fl, pt_first[i], n) + tag
    pagetab = b''
    for i in range(npages):
        pagetab += bytes([0, (i + 1) >> 8 & 0xFF, (i + 1) & 0xFF, 0])  # page number (big endian), flags 0
    # resident names: the module name; the DDB export (ordinal 1) is in the
    # nonresident name table, as LINK386 and wlink write it
    mod = name.upper().encode()
    resnames = bytes([len(mod)]) + mod + b'\0\0' + b'\0'
    ddbn = mod + b'_DDB'
    nonres = bytes([len(ddbn)]) + ddbn + b'\1\0' + b'\0'
    entry = struct.pack('<BBH', 1, 3, 1) + struct.pack('<BI', 3, 0) + b'\0'

    # fixups: only in page 1
    fixrec = b''
    for off, tobj, toff in obj1_fix:
        # source 07h = 32-bit offset, flags 00h = internal reference, 16-bit target offset
        fixrec += struct.pack('<BBHBH', 0x07, 0x00, off, tobj, toff)
    fixpage = struct.pack('<I', 0) + struct.pack('<I', len(fixrec)) * npages

    off = HDR
    o_obj = off; off += len(objtab)
    o_pt = off; off += len(pagetab)
    o_res = off; off += len(resnames)
    o_ent = off; off += len(entry)
    loader_size = off - o_obj
    o_fpt = off; off += len(fixpage)
    o_frt = off; off += len(fixrec)
    fixup_size = off - o_fpt
    o_imp = off; o_impproc = off
    off += 1   # empty import procedure name table
    le_size = off

    mz = bytearray(0x80)
    mz[0:2] = b'MZ'
    struct.pack_into('<HHHHHHHHHHHHH', mz, 2, 0x80, 1, 0, 4, 0x10, 0xFFFF, 0, 0xB8, 0, 0, 0, 0x40, 0)
    stub = b'\x0e\x1f\xba\x0e\x00\xb4\x09\xcd\x21\xb8\x01\x4c\xcd\x21' + b'This is a Windows virtual device driver.\r\n$'
    mz[0x40:0x40 + len(stub)] = stub
    struct.pack_into('<I', mz, 0x3C, 0x80)
    le_off = 0x80
    data_off = le_off + le_size
    data_off = (data_off + 0x1FF) & ~0x1FF   # sector aligned, as LINK386 does

    hdr = bytearray(HDR)
    hdr[0:2] = b'LE'
    lastpage = len(pages[-1])
    vals = {
        0x08: ('H', 2),          # 386
        0x0A: ('H', 4),          # Windows 386 (VxD)
        0x0C: ('I', 0),
        0x10: ('I', 0x00038000), # virtual device driver
        0x14: ('I', npages),
        0x18: ('I', len(objs) if len(objs) > 1 else 0),          # start object: real mode init
        0x1C: ('I', 0),          # start offset
        0x20: ('I', 0), 0x24: ('I', 0),
        0x28: ('I', PAGE),
        0x2C: ('I', lastpage),
        0x30: ('I', fixup_size),
        0x38: ('I', loader_size),
        0x40: ('I', o_obj),
        0x44: ('I', len(objs)),
        0x48: ('I', o_pt),
        0x4C: ('I', 0),
        0x50: ('I', 0), 0x54: ('I', 0),
        0x58: ('I', o_res),
        0x5C: ('I', o_ent),
        0x60: ('I', 0), 0x64: ('I', 0),
        0x68: ('I', o_fpt),
        0x6C: ('I', o_frt),
        0x70: ('I', o_imp),
        0x74: ('I', 0),
        0x78: ('I', o_impproc),
        0x7C: ('I', 0),
        0x80: ('I', data_off),
        0x84: ('I', preload_pages),
        0x88: ('I', 0), 0x8C: ('I', 0),
        0x94: ('I', 0),
        0xC0: ('I', 0x030A0000),   # DDK version 3.10, device ID 0
    }
    for o, (f, v) in vals.items():
        struct.pack_into('<' + f, hdr, o, v)
    le = bytes(hdr) + objtab + pagetab + resnames + entry + fixpage + fixrec + b'\0'
    assert len(le) == le_size
    out = bytes(mz) + le
    out = out.ljust(data_off, b'\0')
    for i, p in enumerate(pages):
        out += p if i == npages - 1 else p.ljust(PAGE, b'\0')
    nonres_off = len(out)
    out += nonres
    out = bytearray(out)
    struct.pack_into('<II', out, le_off + 0x88, nonres_off, len(nonres))
    return bytes(out)

if __name__ == '__main__':
    name, rmf, memsize, outf = sys.argv[1], sys.argv[2], int(sys.argv[3], 0), sys.argv[4]
    rm = open(rmf, 'rb').read()
    open(outf, 'wb').write(le_vxd(name, rm, max(memsize, len(rm))))
