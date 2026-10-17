#!/usr/bin/env python3
"""pefix.py image [noentry] : final touches on a WINVBE NT image -
optionally clear the entry point (NT 3.x display DLL) and set the checksum."""
import sys, pefile
f = sys.argv[1]
pe = pefile.PE(f)
if 'noentry' in sys.argv[2:]:
    pe.OPTIONAL_HEADER.AddressOfEntryPoint = 0
pe.OPTIONAL_HEADER.CheckSum = 0
pe.OPTIONAL_HEADER.DllCharacteristics = 0
pe.OPTIONAL_HEADER.MajorLinkerVersion = 3        # like the NT 4.0 DDK linker
pe.OPTIONAL_HEADER.MinorLinkerVersion = 10
if 'nonpaged' in sys.argv[2:]:
    for sec in pe.sections:
        if sec.Name.rstrip(b'\0') in (b'.text', b'.data', b'.rdata', b'.idata', b'.bss'):
            sec.Characteristics |= 0x08000000         # IMAGE_SCN_MEM_NOT_PAGED
if f.lower().endswith('.sys'):
    pe.FILE_HEADER.Characteristics &= ~0x2000          # not a DLL
# NT <= 4 maps images with a section alignment below the page size 1:1 from
# the file: every section must start at a file offset equal to its RVA
if pe.OPTIONAL_HEADER.SectionAlignment < 0x1000:
    for sec in pe.sections:
        if sec.SizeOfRawData and sec.PointerToRawData != sec.VirtualAddress:
            raise SystemExit('%s: section %s: raw %x != rva %x' % (f, sec.Name, sec.PointerToRawData, sec.VirtualAddress))
        if not sec.SizeOfRawData and sec.Misc_VirtualSize:
            raise SystemExit('%s: section %s has no raw data' % (f, sec.Name))
data = bytearray(pe.write())
pe.close()
pe = pefile.PE(data=bytes(data))
pe.OPTIONAL_HEADER.CheckSum = pe.generate_checksum()
open(f, 'wb').write(pe.write())
