#!/bin/sh
# build WINVBE.DRV
set -e
cd "$(dirname "$0")"
. ../owenv.sh
RES=${RES:-../res}
python3 ../tools/mkres.py ../res >/dev/null
mkdir -p obj
# -zc: string literals in the code segment (DGROUP is full)
CFLAGS="$XCF -q -ml -zc -zu -s -3 -ox -zl -zls -wx -we -wcd=303 -wcd=202 -bt=windows -fpi -zq -i=."
for f in util vbe color rop surf blt cursor enable robj output text dib dibeng ddhal; do
  wcc $CFLAGS -fo=obj/$f.obj $f.c
done
nasm -f obj -o obj/entry.obj entry.asm
cd obj
wlink @../display.lnk file entry.obj,util.obj,vbe.obj,color.obj,rop.obj,surf.obj,blt.obj,cursor.obj,enable.obj,robj.obj,output.obj,text.obj,dib.obj,dibeng.obj,ddhal.obj
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
  # version resource (Windows 3.1 and later show it: DXDIAG, file properties)
  [ $n = 30 ] || cat >> obj/res$n/winvbe.rc <<'VEOF'
1 VERSIONINFO
FILEVERSION 1,2,0,0
PRODUCTVERSION 1,2,0,0
FILEFLAGSMASK 0x3F
FILEFLAGS 0
FILEOS 0x1
FILETYPE 3
FILESUBTYPE 4
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904E4"
        BEGIN
            VALUE "CompanyName", "WINVBE\0"
            VALUE "FileDescription", "WINVBE Universal VESA/VGA/SVGA display driver\0"
            VALUE "FileVersion", "1.2\0"
            VALUE "InternalName", "DISPLAY\0"
            VALUE "LegalCopyright", "GNU General Public License v3\0"
            VALUE "OriginalFilename", "WINVBE.DRV\0"
            VALUE "ProductName", "WINVBE\0"
            VALUE "ProductVersion", "1.2\0"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1252
    END
END
VEOF
  wrc -q -r -bt=windows -30 -i=obj/res$n -fo=obj/winvbe$n.res obj/res$n/winvbe.rc
  cp obj/winvbe.bin $out
  wrc -q -bt=windows -30 obj/winvbe$n.res $out -fe=$out
done
ls -l obj/winvbe.drv obj/winvbe30.drv obj/winvbe9x.drv
