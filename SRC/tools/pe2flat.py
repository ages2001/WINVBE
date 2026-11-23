#!/usr/bin/env python3
"""pe2flat.py in.exe base out.bin : the memory image of a PE file from the
virtual address `base` (its first section) to the end of its last section,
uninitialized data as zeros. Used for the x86 emulator blob of WINVBE.386,
linked by mingw ld at a fixed image base (no relocations needed)."""
import sys, pefile
pe = pefile.PE(sys.argv[1])
base = int(sys.argv[2], 0)
ib = pe.OPTIONAL_HEADER.ImageBase
secs = [s for s in pe.sections if s.Name.rstrip(b'\0') not in (b'.reloc', b'.idata', b'.edata')]
end = max(ib + s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData) for s in secs)
img = bytearray(end - base)
for s in secs:
    va = ib + s.VirtualAddress
    if va < base:
        raise SystemExit('section %s below the base' % s.Name)
    data = s.get_data()[:max(s.Misc_VirtualSize, 0) or len(s.get_data())]
    img[va - base:va - base + len(data)] = data
if hasattr(pe, 'DIRECTORY_ENTRY_IMPORT'):
    raise SystemExit('the emulator blob must not import anything')
open(sys.argv[3], 'wb').write(bytes(img))
print('%s: %d bytes at %x' % (sys.argv[3], len(img), base))
