#!/bin/sh
# build WINVBE.DRV
set -e
cd "$(dirname "$0")"
. ../owenv.sh
RES=${RES:-../res}
python3 ../tools/mkres.py ../res >/dev/null
mkdir -p obj
CFLAGS="$XCF -q -ml -zu -s -3 -ox -zl -zls -wx -we -wcd=303 -wcd=202 -bt=windows -fpi -zq -i=."
for f in util vbe color rop surf blt cursor enable robj output text dib; do
  wcc $CFLAGS -fo=obj/$f.obj $f.c
done
nasm -f obj -o obj/entry.obj entry.asm
cd obj
wlink @../display.lnk file entry.obj,util.obj,vbe.obj,color.obj,rop.obj,surf.obj,blt.obj,cursor.obj,enable.obj,robj.obj,output.obj,text.obj,dib.obj
cd ..
wrc -q -r -bt=windows -30 -i=$RES -fo=obj/winvbe.res $RES/winvbe.rc
wrc -q -bt=windows -30 obj/winvbe.res obj/winvbe.drv -fe=obj/winvbe.drv
ls -l obj/winvbe.drv
