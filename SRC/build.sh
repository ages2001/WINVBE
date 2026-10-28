#!/bin/sh
# Build everything: WINVBE.DRV (3.x, 2.x, 1.x), WINVBE.386, WINVBECF.EXE, WVBPOWER.SCR,
# INSTALL.EXE, 915PATCH.EXE, 915PATCH.VXD, GDITEST.EXE and the Windows NT
# drivers (nt/build.sh).
# Requires Open Watcom 2 (set WATCOM), NASM and Python 3; for NT mingw-w64.
set -e
cd "$(dirname "$0")"
sh drv/build.sh
sh drv/build2.sh
sh drv/build1.sh
sh cfg/build.sh
sh patch/build.sh
sh vxd/build.sh
sh nt/build.sh
sh test/build.sh
mkdir -p bin
python3 tools/mkfon.py bin
cp drv/obj/winvbe.drv bin/WINVBE.DRV
mkdir -p bin/WIN30 bin/WIN9X
cp drv/obj/winvbe30.drv bin/WIN30/WINVBE.DRV
cp drv/obj/winvbe9x.drv bin/WIN9X/WINVBE.DRV
cp cfg/obj/winvbecf.exe bin/WINVBECF.EXE
cp cfg/obj/wvbpower.scr bin/WVBPOWER.SCR
cp cfg/obj/install.exe bin/INSTALL.EXE
cp test/gditest.exe bin/GDITEST.EXE
cp patch/obj/915patch.exe bin/915PATCH.EXE
cp patch/obj/915patch.vxd bin/915PATCH.VXD
cp vxd/obj/winvbe.386 bin/WINVBE.386
mkdir -p bin/NT/I386/NT3X bin/NT/I386/NT4-XP bin/NT/AMD64
cp nt/obj/i386/winvbe.sys nt/obj/i386/winvbecf.exe bin/NT/I386/NT3X/
cp nt/obj/i386/winvbe3.dll bin/NT/I386/NT3X/winvbe.dll
cp nt/obj/i386/winvbe.sys nt/obj/i386/winvbe.dll nt/obj/i386/winvbecf.exe bin/NT/I386/NT4-XP/
cp nt/obj/amd64/winvbe.sys nt/obj/amd64/winvbe.dll nt/obj/amd64/winvbecf.exe bin/NT/AMD64/
mkdir -p bin/WIN2X bin/WIN1X
cp drv/obj2/winvbe2.drv bin/WIN2X/WINVBE.DRV
cp drv/obj2/winvbe.lgo bin/WIN2X/WINVBE.LGO
cp drv/obj1/winvbe1.drv bin/WIN1X/WINVBE.DRV
cp drv/obj2/winvbe.lgo bin/WIN1X/WINVBE.LGO
cp cfg/obj/winvbecf2.exe bin/WIN2X/WINVBECF.EXE
cp cfg/obj/winvbecf1.exe bin/WIN1X/WINVBECF.EXE
ls -l bin
