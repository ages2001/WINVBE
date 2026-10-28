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
# one driver per Windows family, the same code with the OEM resources
# (bitmaps, cursors, icons) of that Windows' own VGA driver
# (tools/stockres.py; without msfiles/stock the WINVBE artwork from res/):
#   winvbe30.drv  Windows 3.0      VGA.DRV 3.0
#   winvbe.drv    Windows 3.1/WfW  VGA.DRV 3.1
#   winvbe9x.drv  Windows 9x       none: USER uses its own, as with FRAMEBUF.DRV
STOCK=../msfiles/stock
mv obj/winvbe.drv obj/winvbe.bin
for v in 30:w3:VGA30.DRV 31:w3:VGA31.DRV 9x:w9:-; do
  n=${v%%:*}; m=${v#*:}; m=${m%%:*}; f=${v##*:}
  out=obj/winvbe$n.drv; [ $n = 31 ] && out=obj/winvbe.drv
  python3 ../tools/stockres.py $m $STOCK/$f $RES obj/res$n
  wrc -q -r -bt=windows -30 -i=obj/res$n -fo=obj/winvbe$n.res obj/res$n/winvbe.rc
  cp obj/winvbe.bin $out
  wrc -q -bt=windows -30 obj/winvbe$n.res $out -fe=$out
done
ls -l obj/winvbe.drv obj/winvbe30.drv obj/winvbe9x.drv
