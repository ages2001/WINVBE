#!/usr/bin/env python3
"""Convert raw NE resource dumps (from neres.py) into .bmp/.ico/.cur files + .rc
(used only for local testing with stock resources)."""
import sys, os, struct, glob

src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
rc = []

def rd(n):
    return open(os.path.join(src, n), 'rb').read()

# bitmaps
for f in sorted(glob.glob(os.path.join(src, '2_*.bin'))):
    rid = int(os.path.basename(f)[2:-4])
    d = open(f, 'rb').read()
    hs = struct.unpack_from('<I', d, 0)[0]
    if hs == 12:
        bits = struct.unpack_from('<H', d, 10)[0]
        ncol = (1 << bits) if bits <= 8 else 0
        pal = ncol * 3
    else:
        bits = struct.unpack_from('<H', d, 14)[0]
        used = struct.unpack_from('<I', d, 32)[0]
        ncol = used or ((1 << bits) if bits <= 8 else 0)
        pal = ncol * 4
    off = 14 + hs + pal
    bf = b'BM' + struct.pack('<IHHI', 14 + len(d), 0, 0, off)
    open(os.path.join(out, f'b{rid}.bmp'), 'wb').write(bf + d)
    rc.append(f'{rid} BITMAP b{rid}.bmp')

def group(kind, gtype, etype, ext):
    for f in sorted(glob.glob(os.path.join(src, f'{gtype}_*.bin'))):
        gid = int(os.path.basename(f).split('_')[1][:-4])
        g = open(f, 'rb').read()
        res, typ, cnt = struct.unpack_from('<HHH', g, 0)
        entries = []
        for i in range(cnt):
            e = g[6 + i * 14: 6 + i * 14 + 14]
            if kind == 'icon':
                w, h, cc, r, planes, bc, size, nid = struct.unpack('<BBBBHHIH', e)
                data = rd(f'{etype}_{nid}.bin')
                entries.append((w, h, cc, planes, bc, data, 0, 0))
            else:
                w, h, planes, bc, size, nid = struct.unpack('<HHHHIH', e)
                data = rd(f'{etype}_{nid}.bin')
                hx, hy = struct.unpack_from('<HH', data, 0)
                entries.append((w, h // 2, 0, hx, hy, data[4:], 0, 0))
        hdr = struct.pack('<HHH', 0, 1 if kind == 'icon' else 2, len(entries))
        off = 6 + 16 * len(entries)
        dirs = b''
        blob = b''
        for e in entries:
            if kind == 'icon':
                w, h, cc, planes, bc, data, _, _ = e
                dirs += struct.pack('<BBBBHHII', w, h, cc, 0, planes, bc, len(data), off + len(blob))
            else:
                w, h, _, hx, hy, data, _, _ = e
                dirs += struct.pack('<BBBBHHII', w, h, 0, 0, hx, hy, len(data), off + len(blob))
            blob += data
        name = f'{kind[0]}{gid}.{ext}'
        open(os.path.join(out, name), 'wb').write(hdr + dirs + blob)
        rc.append(f'{gid} {kind.upper()} {name}')

group('icon', 14, 3, 'ico')
group('cursor', 12, 1, 'cur')
for i in (1, 2, 3):
    open(os.path.join(out, f'oem{i}.bin'), 'wb').write(rd(f'28678_{i}.bin'))
open(os.path.join(out, 'nametab.bin'), 'wb').write(rd('15_1.bin'))
open(os.path.join(out, 'stock.rc'), 'w').write('\n'.join(rc) + '\n')
print(len(rc), 'resources')
