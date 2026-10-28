#!/bin/sh
# build32.sh : DirectDraw / Direct3D / DCI / OpenGL / GDI test programs, DBGRUN (mingw-w64, no C runtime)
set -e
cd "$(dirname "$0")"
CC="i686-w64-mingw32-gcc -Os -march=i486 -mwindows -nostdlib -nostartfiles -e _start@0 -s -Wall"
$CC dd/ddtest.c   -o dd/ddtest.exe   -lddraw -luser32 -lgdi32 -lkernel32 -ldxguid
$CC dd/dcitest.c  -o dd/dcitest.exe  -luser32 -lkernel32
$CC gdi32/w32gdi.c -o gdi32/w32gdi.exe -lcomctl32 -luser32 -lgdi32 -lkernel32
$CC gdi32/dbgrun.c -o gdi32/dbgrun.exe -luser32 -lkernel32
$CC d3d/d3dtest.c -o d3d/d3dtest.exe -lddraw -luser32 -lgdi32 -lkernel32 -ldxguid
$CC d3d/gltest.c  -o d3d/gltest.exe  -lopengl32 -luser32 -lgdi32 -lkernel32 \
    -Wl,--subsystem,windows:3.10 -Wl,--major-os-version,1 -Wl,--minor-os-version,0
