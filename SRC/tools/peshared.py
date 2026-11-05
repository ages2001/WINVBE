#!/usr/bin/env python3
"""peshared.py <file.dll> : mark every section of a 32-bit PE image shared
(IMAGE_SCN_MEM_SHARED), as MS LINK /SECTION:...,S would. Windows 9x then loads
a DLL whose image base is above 2 GB into the shared arena, at the same
address in every process (what a DirectDraw HAL DLL needs). Updates the PE
checksum."""
import struct, sys

p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
pe = struct.unpack_from('<I', d, 0x3C)[0]
assert d[pe:pe + 4] == b'PE\0\0', 'not PE'
nsec, = struct.unpack_from('<H', d, pe + 6)
optsz, = struct.unpack_from('<H', d, pe + 20)
opt = pe + 24
assert struct.unpack_from('<H', d, opt)[0] == 0x10B, 'not PE32'
base, = struct.unpack_from('<I', d, opt + 28)
assert base >= 0x80000000, 'image base must be above 2 GB'
sec = opt + optsz
for i in range(nsec):
    o = sec + i * 40 + 36
    c, = struct.unpack_from('<I', d, o)
    struct.pack_into('<I', d, o, c | 0x10000000)
# PE checksum
struct.pack_into('<I', d, opt + 64, 0)
s = 0
for i in range(0, len(d) & ~1, 2):
    s += d[i] | (d[i + 1] << 8)
    s = (s & 0xFFFF) + (s >> 16)
if len(d) & 1:
    s += d[-1]
    s = (s & 0xFFFF) + (s >> 16)
s = (s & 0xFFFF) + (s >> 16)
struct.pack_into('<I', d, opt + 64, (s + len(d)) & 0xFFFFFFFF)
open(p, 'wb').write(d)
