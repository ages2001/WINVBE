#!/bin/sh
# build WINVBE.DRV for Windows 1.x/2.x (OLDWIN): obj2/winvbe2.drv
set -e
cd "$(dirname "$0")"
. ../owenv.sh
python3 ../tools/mkres.py ../res >/dev/null
mkdir -p obj2
python3 ../tools/mkres2.py ../res obj2/res >/dev/null
mkdir -p obj2
CFLAGS="-q -ml -zu -s -3 -ox -zl -zls -wx -we -wcd=303 -wcd=202 -bt=windows -fpi -zq -i=. -DOLDWIN $XCFLAGS"
for f in util vbe color rop surf blt cursor enable robj output text dib; do
  wcc $CFLAGS -fo=obj2/$f.obj $f.c
done
nasm -f obj -DOLDWIN -o obj2/entry.obj entry.asm
cd obj2
wlink @../display2.lnk file entry.obj,util.obj,vbe.obj,color.obj,rop.obj,surf.obj,blt.obj,cursor.obj,enable.obj,robj.obj,output.obj,text.obj,dib.obj
cd ..
wrc -q -r -bt=windows -30 -i=obj2/res -fo=obj2/winvbe2.res obj2/res/winvbe2.rc
wrc -q -bt=windows -30 obj2/winvbe2.res obj2/winvbe2.drv -fe=obj2/winvbe2.drv
python3 ../tools/nefix.py obj2/winvbe2.drv 0x201
nasm -f bin -o obj2/winvbe.lgo winvbe.asm
ls -l obj2/winvbe2.drv
