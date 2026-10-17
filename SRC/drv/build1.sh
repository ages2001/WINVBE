#!/bin/sh
# build WINVBE.DRV for Windows 1.0x (OLDWIN, 1.x resources): obj1/winvbe1.drv
# The objects are the 2.x ones (build2.sh first); only the export list, the
# resources and the expected version differ.
set -e
cd "$(dirname "$0")"
. ../owenv.sh
mkdir -p obj1
nasm -f obj -DOLDWIN $NASMFLAGS -o obj1/entry.obj entry.asm
python3 ../tools/mkres2.py ../res obj1/res oldwin1 >/dev/null
cd obj1
wlink @../display1.lnk file entry.obj,../obj2/util.obj,../obj2/vbe.obj,../obj2/color.obj,../obj2/rop.obj,../obj2/surf.obj,../obj2/blt.obj,../obj2/cursor.obj,../obj2/enable.obj,../obj2/robj.obj,../obj2/output.obj,../obj2/text.obj,../obj2/dib.obj
cd ..
wrc -q -r -bt=windows -30 -i=obj1/res -fo=obj1/winvbe1.res obj1/res/winvbe2.rc
wrc -q -bt=windows -30 obj1/winvbe1.res obj1/winvbe1.drv -fe=obj1/winvbe1.drv
python3 ../tools/nefix.py obj1/winvbe1.drv 0
ls -l obj1/winvbe1.drv
