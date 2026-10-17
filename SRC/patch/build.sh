#!/bin/sh
# build 915PATCH.EXE (DOS) and 915PATCH.VXD (Windows real mode init)
# XCFLAGS=-DTEST440FX: test build that also accepts QEMU's 440FX
set -e
cd "$(dirname "$0")"
. ../owenv.sh
export INCLUDE=$WATCOM/h
mkdir -p obj/v
wcc -q -ml -bt=dos -3 -os -wx -wcd=303 -wcd=202 $XCFLAGS -fo=obj/i915core.obj i915core.c
wcc -q -ml -bt=dos -3 -os -wx -wcd=303 -wcd=202 $XCFLAGS -fo=obj/915patch.obj 915patch.c
wlink system dos option quiet name obj/915patch.exe file obj/915patch.obj,obj/i915core.obj
ls -l obj/915patch.exe

# VxD: small model, code and data in one segment (DGROUP), own startup
F="-q -ms -bt=dos -3 -os -s -zl -wx -wcd=303 -wcd=202 -DVXDINIT $XCFLAGS"
wcc $F -fo=obj/v/i915core.obj i915core.c
wcc $F -fo=obj/v/915patch.obj 915patch.c
wcc $F -fo=obj/v/vxdinit.obj vxdinit.c
nasm -f obj $NASMFLAGS -o obj/v/vxdrm.obj vxdrm.asm
wlink format raw bin option quiet, offset=0, map=obj/v/rm.map, nodefaultlibs name obj/v/rm.bin \
    file obj/v/vxdrm.obj,obj/v/vxdinit.obj,obj/v/915patch.obj,obj/v/i915core.obj \
    library $WATCOM/lib286/dos/clibs.lib
# memory size = end of the STACK segment (BSS is not in the raw file)
MEM=$(awk '$1=="STACK"{print "0x" $4 " 0x" $5}' obj/v/rm.map | { read a b; echo $((a + b)); })
# the Windows 95 loader skips a real mode init object larger than 16 KB
[ $MEM -le 16384 ] || { echo "real mode image too large: $MEM"; exit 1; }
python3 ../tools/mkvxd.py 915PATCH obj/v/rm.bin $MEM obj/915patch.vxd
ls -l obj/915patch.vxd
