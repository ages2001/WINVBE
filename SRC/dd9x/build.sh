#!/bin/sh
# build WVBDD32.DLL, the 32-bit part of the Windows 9x DirectDraw HAL (mingw-w64).
# The image base is in the shared arena (above 2 GB): Windows 9x maps such a
# DLL into every process at the same address, so DirectDraw can call its
# callbacks from any process (it checks them with IsBadCodePtr). Windows 9x
# does that for images whose sections are all shared (tools/peshared.py).
set -e
cd "$(dirname "$0")"
mkdir -p obj
i686-w64-mingw32-gcc -O2 -march=i486 -Wall -Wno-missing-braces -Wno-unused-function -fno-asynchronous-unwind-tables -fno-builtin -shared -nostdlib \
    -e _DllMain@12 -Wl,--kill-at -Wl,--image-base,0xB00B0000 -Wl,--file-alignment,0x1000 -Wl,--disable-dynamicbase -Wl,--disable-nxcompat -s \
    -o obj/wvbdd32.dll wvbdd32.c -lkernel32 -lgcc
python3 ../tools/peshared.py obj/wvbdd32.dll
ls -l obj/wvbdd32.dll
