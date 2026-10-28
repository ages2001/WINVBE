#!/usr/bin/env python3
"""
WINVBE - take the default OEM resources (bitmaps, cursors, icons, default
colours) from a standard Microsoft display driver, so that WINVBE looks
exactly like the stock VGA driver of each Windows version.

  stockres.py w3  <stock.drv> <res dir> <out dir>   Windows 3.0/3.1
      writes .bmp/.cur/.ico files for every bitmap, cursor and icon of the
      stock driver and winvbe.rc; the OEMBIN resources (metrics, default
      colours, fonts) and the name table come from <res dir>
  stockres.py w9  <stock.drv> <res dir> <out dir>   Windows 95/98/ME
      no bitmaps, cursors or icons at all: like FRAMEBUF.DRV, USER then
      uses its own (Windows 9x) ones
  stockres.py nt3 <VGA.DLL>   <res dir> <out dir>   Windows NT 3.x
      the same for the NT 3.1 CSRSS display DLL: nt3.rc, and its RCDATA 1/2
      (metrics, default colours, colour dialog table)
  stockres.py old <stock.drv> <out dir>             Windows 1.x/2.x
      replaces the bitmaps, cursors and icons that mkres2.py wrote into
      <out dir> by the stock ones (pre-3.0 formats, copied unchanged)

If <stock> does not exist, the WINVBE artwork from <res dir> (mkres.py) is
used instead: w3/nt3 copy it, old leaves <out dir> as it is.  The stock
drivers are Microsoft files and are not part of the source tree; put them
into msfiles/stock (see README.TXT, "Building").
"""
import glob
import os
import shutil
import struct
import sys


# --------------------------------------------------------------------------
# readers: {(type, id): bytes}; type/id are ints or strings

def ne_resources(path):
    d = open(path, 'rb').read()
    ne = struct.unpack_from('<H', d, 0x3C)[0]
    rp = ne + struct.unpack_from('<H', d, ne + 0x24)[0]
    shift = struct.unpack_from('<H', d, rp)[0]
    p = rp + 2
    out = {}

    def name(v):
        if v & 0x8000:
            return v & 0x7FFF
        return d[rp + v + 1: rp + v + 1 + d[rp + v]].decode('latin-1')
    while True:
        tid = struct.unpack_from('<H', d, p)[0]
        if tid == 0:
            break
        cnt = struct.unpack_from('<H', d, p + 2)[0]
        p += 8
        t = name(tid)
        for _ in range(cnt):
            off, ln, _fl, rid = struct.unpack_from('<HHHH', d, p)
            p += 12
            out[(t, name(rid))] = d[off << shift:(off << shift) + (ln << shift)]
    return out


def pe_resources(path):
    import pefile
    pe = pefile.PE(path)
    out = {}
    for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        tn = t.id if t.id is not None else str(t.name)
        for e in t.directory.entries:
            en = e.id if e.id is not None else str(e.name)
            de = e.directory.entries[0].data.struct
            out[(tn, en)] = pe.get_data(de.OffsetToData, de.Size)
    return out


# --------------------------------------------------------------------------
# 3.x / Win32 resources -> .bmp / .ico / .cur files

def dib_size(d):
    """length of a DIB resource (header + palette + bits); NE resources are
    padded to the alignment unit"""
    hs = struct.unpack_from('<I', d, 0)[0]
    if hs == 12:
        w, h, _pl, bits = struct.unpack_from('<HHHH', d, 4)
        ncol = (1 << bits) if bits <= 8 else 0
        pal = ncol * 3
    else:
        w, h, _pl, bits = struct.unpack_from('<iiHH', d, 4)
        used = struct.unpack_from('<I', d, 32)[0]
        ncol = used or ((1 << bits) if bits <= 8 else 0)
        pal = ncol * 4
    return hs, pal, hs + pal + ((w * bits + 31) // 32) * 4 * abs(h)


def write_bmp(path, d):
    hs, pal, size = dib_size(d)
    d = d[:size]
    open(path, 'wb').write(b'BM' + struct.pack('<IHHI', 14 + len(d), 0, 0, 14 + hs + pal) + d)


def icon_entry_size(d):
    """an icon/cursor image is a DIB with the height doubled (XOR + AND)"""
    hs, w, h, _pl, bits = struct.unpack_from('<IiiHH', d, 0)
    ncol = struct.unpack_from('<I', d, 32)[0] or ((1 << bits) if bits <= 8 else 0)
    xor = ((w * bits + 31) // 32) * 4 * (h // 2)
    andm = ((w + 31) // 32) * 4 * (h // 2)
    return hs + ncol * 4 + xor + andm


def write_group(path, res, gdata, etype, cursor):
    cnt = struct.unpack_from('<H', gdata, 4)[0]
    entries = []
    for i in range(cnt):
        e = gdata[6 + i * 14: 20 + i * 14]
        if cursor:
            w, h, _pl, _bc, _size, nid = struct.unpack('<HHHHIH', e)
            data = res[(etype, nid)]
            hx, hy = struct.unpack_from('<HH', data, 0)
            img = data[4:]
            img = img[:icon_entry_size(img)]
            entries.append((struct.pack('<BBBBHH', w & 0xFF, (h // 2) & 0xFF, 0, 0, hx, hy), img))
        else:
            w, h, cc, _r, planes, bc, _size, nid = struct.unpack('<BBBBHHIH', e)
            img = res[(etype, nid)]
            img = img[:icon_entry_size(img)]
            entries.append((struct.pack('<BBBBHH', w, h, cc, 0, planes, bc), img))
    out = struct.pack('<HHH', 0, 2 if cursor else 1, len(entries))
    off = 6 + 16 * len(entries)
    blob = b''
    for hdr, img in entries:
        out += hdr + struct.pack('<II', len(img), off + len(blob))
        blob += img
    open(path, 'wb').write(out + blob)


def art_files(res, out, quote):
    """writes all bitmaps, cursors and icons of <res>; returns the rc lines"""
    rc = []
    q = '"' if quote else ''
    for (t, rid), d in sorted(res.items(), key=lambda x: str(x[0])):
        if t == 2 and isinstance(rid, int):
            write_bmp(os.path.join(out, f'b{rid}.bmp'), d)
            rc.append(f'{rid} BITMAP {q}b{rid}.bmp{q}')
    for gt, et, kind, ext in ((12, 1, 'CURSOR', 'cur'), (14, 3, 'ICON', 'ico')):
        for (t, rid), d in sorted(res.items(), key=lambda x: str(x[0])):
            if t == gt and isinstance(rid, int):
                fn = f'{ext[0]}{rid}.{ext}'
                write_group(os.path.join(out, fn), res, d, et, gt == 12)
                rc.append(f'{rid} {kind} {q}{fn}{q}')
    return rc


def generated_art(resdir, out, rcname):
    rc = []
    for line in open(os.path.join(resdir, rcname)):
        if line.split()[1:2] and line.split()[1] in ('BITMAP', 'CURSOR', 'ICON'):
            shutil.copy(os.path.join(resdir, line.split()[2].strip('"')), out)
            rc.append(line.rstrip())
    return rc


def main():
    mode, stock, *rest = sys.argv[1:]
    have = os.path.isfile(stock)
    if mode in ('w3', 'w9'):
        resdir, out = rest
        os.makedirs(out, exist_ok=True)
        for f in ('oem1.bin', 'oem2.bin', 'oem3.bin', 'nametab.bin'):
            shutil.copy(os.path.join(resdir, f), out)
        if have:
            res = ne_resources(stock)
            rc = art_files(res, out, False) if mode == 'w3' else []
        else:
            rc = generated_art(resdir, out, 'winvbe.rc') if mode == 'w3' else []
        rc += ['1 28678 PRELOAD FIXED oem1.bin', '2 28678 oem2.bin',
               '3 28678 PRELOAD oem3.bin', '1 15 nametab.bin']
        open(os.path.join(out, 'winvbe.rc'), 'w').write('\n'.join(rc) + '\n')
    elif mode == 'nt3':
        resdir, out = rest
        os.makedirs(out, exist_ok=True)
        for f in ('nt1.bin', 'nt2.bin'):
            shutil.copy(os.path.join(resdir, f), out)
        if have:
            res = pe_resources(stock)
            rc = art_files(res, out, True)
            for i in (1, 2):    # RCDATA 1/2: metrics, default colours, colour table
                if (10, i) in res:
                    open(os.path.join(out, f'nt{i}.bin'), 'wb').write(res[(10, i)])
        else:
            rc = generated_art(resdir, out, 'nt3.rc')
        rc += ['1 RCDATA "nt1.bin"', '2 RCDATA "nt2.bin"']
        open(os.path.join(out, 'nt3.rc'), 'w').write('\n'.join(rc) + '\n')
    elif mode == 'old':
        out = rest[0]
        if not have:
            return
        res = ne_resources(stock)
        for f in glob.glob(os.path.join(out, '[bci]*.bin')):
            b = os.path.basename(f)
            t = {'b': 2, 'c': 1, 'i': 3}[b[0]]
            key = (t, int(b[1:-4]))
            if key in res:
                open(f, 'wb').write(res[key])
    else:
        sys.exit('usage: see the header of stockres.py')


if __name__ == '__main__':
    main()
