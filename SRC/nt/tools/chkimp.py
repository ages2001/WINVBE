#!/usr/bin/env python3
"""chkimp.py <winvbe.sys> : the i386 miniport must load on Windows NT 3.1 -
every import has to exist in NT 3.1's VIDEOPRT.SYS / NTOSKRNL.EXE.
Functions of later versions are looked up at run time instead
(VIDEO_PORT_CONFIG_INFO.VideoPortGetProcAddress, Windows 2000 and later)."""
import struct, sys

NT31_VIDEOPRT = set(['VideoPortCompareMemory', 'VideoPortDisableInterrupt', 'VideoPortEnableInterrupt', 'VideoPortFreeDeviceBase', 'VideoPortGetDeviceBase', 'VideoPortGetDeviceData', 'VideoPortGetRegistryParameters', 'VideoPortInitialize', 'VideoPortInt10', 'VideoPortLogError', 'VideoPortMapMemory', 'VideoPortMoveMemory', 'VideoPortReadPortBufferUchar', 'VideoPortReadPortBufferUlong', 'VideoPortReadPortBufferUshort', 'VideoPortReadPortUchar', 'VideoPortReadPortUlong', 'VideoPortReadPortUshort', 'VideoPortReadRegisterBufferUchar', 'VideoPortReadRegisterBufferUlong', 'VideoPortReadRegisterBufferUshort', 'VideoPortReadRegisterUchar', 'VideoPortReadRegisterUlong', 'VideoPortReadRegisterUshort', 'VideoPortScanRom', 'VideoPortSetRegistryParameters', 'VideoPortSetTrappedEmulatorPorts', 'VideoPortStallExecution', 'VideoPortSynchronizeExecution', 'VideoPortUnmapMemory', 'VideoPortVerifyAccessRanges', 'VideoPortWritePortBufferUchar', 'VideoPortWritePortBufferUlong', 'VideoPortWritePortBufferUshort', 'VideoPortWritePortUchar', 'VideoPortWritePortUlong', 'VideoPortWritePortUshort', 'VideoPortWriteRegisterBufferUchar', 'VideoPortWriteRegisterBufferUlong', 'VideoPortWriteRegisterBufferUshort', 'VideoPortWriteRegisterUchar', 'VideoPortWriteRegisterUlong', 'VideoPortWriteRegisterUshort', 'VideoPortZeroMemory'])
# NTOSKRNL.EXE imports checked against NT 3.1 (add new ones only after checking)
NT31_NTOSKRNL = {'IoGetCurrentProcess', 'MmIsAddressValid'}

def main(p):
    d = open(p, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    opt = pe + 24
    dd = opt + 96
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    sopt = struct.unpack_from('<H', d, pe + 20)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + sopt + i * 40
        vs, va, rs, ra = struct.unpack_from('<IIII', d, o + 8)
        secs.append((va, max(vs, rs), ra))
    def r(rva):
        for va, sz, ra in secs:
            if va <= rva < va + sz:
                return rva - va + ra
        raise ValueError(hex(rva))
    def cstr(o):
        return d[o:d.index(b'\0', o)].decode()
    io = r(struct.unpack_from('<I', d, dd + 8)[0])
    bad = []
    while True:
        oft, ts, fc, name, ft = struct.unpack_from('<IIIII', d, io)
        if not name:
            break
        dll = cstr(r(name)).lower()
        allow = NT31_VIDEOPRT if dll == 'videoprt.sys' else NT31_NTOSKRNL if dll == 'ntoskrnl.exe' else set()
        t = r(oft or ft)
        while True:
            v = struct.unpack_from('<I', d, t)[0]
            if not v:
                break
            fn = '#%d' % (v & 0xffff) if v & 0x80000000 else cstr(r(v) + 2)
            if fn not in allow:
                bad.append(dll + '!' + fn)
            t += 4
        io += 20
    if bad:
        print('%s: imports missing on Windows NT 3.1: %s' % (p, ', '.join(bad)))
        sys.exit(1)

main(sys.argv[1])
