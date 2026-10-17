#!/bin/sh
# pack.sh <out.zip> : build the WINVBE10 distribution tree and zip it
set -e
cd "$(dirname "$0")/.."
OUT=$(realpath -m "${1:-WINVBE_v1.0.zip}")
ST=$(mktemp -d)/WINVBE10
mkdir -p $ST
python3 tools/mkinf.py dist >/dev/null
for f in INSTALL.EXE GDITEST.EXE 915PATCH.EXE 915PATCH.VXD; do cp bin/$f $ST/; done
cp README.TXT BENIOKU.TXT $ST/
mkdir -p $ST/TEST
for f in dd/ddtest dd/dcitest d3d/d3dtest d3d/gltest; do
    cp test/$f.exe $ST/TEST/$(basename $f | tr a-z A-Z).EXE
done
# one folder per Windows version; INSTALL.EXE takes the files of the folder
# that matches the Windows it finds.  WIN30/WIN31/WIN2X also hold the
# Microsoft files Setup would ask the Windows disks for (msfiles/).
for v in WIN30 WIN31; do
  mkdir -p $ST/$v
  cp dist/$v/OEMSETUP.INF $ST/$v/
  cp bin/WINVBE.DRV bin/WINVBE.386 bin/WINVBECF.EXE bin/WVBSYS.FON bin/WVBFIX.FON bin/WVBOEM.FON $ST/$v/
  cp msfiles/$v/* $ST/$v/
done
cp bin/WVBPOWER.SCR $ST/WIN31/
mkdir -p $ST/WIN9X
cp dist/WIN9X/WINVBE.INF bin/WINVBE.DRV bin/WINVBE.386 bin/WINVBECF.EXE bin/WVBPOWER.SCR $ST/WIN9X/
cp bin/915PATCH.VXD $ST/WIN9X/
mkdir -p $ST/WIN2X $ST/WIN1X
cp bin/WIN2X/WINVBE.DRV bin/WIN2X/WINVBE.LGO bin/WIN2X/WINVBECF.EXE dist/WIN2X/PREP2X.BAT msfiles/WIN2X/* $ST/WIN2X/
cp bin/WIN1X/WINVBE.DRV bin/WIN1X/WINVBE.LGO bin/WIN1X/WINVBECF.EXE dist/WIN1X/PATCH1X.BAT $ST/WIN1X/
# Windows NT: 3.1/3.5/3.51 (NT\I386\NT3X), 4.0-XP/2003 (NT\I386\NT4-XP), x64 (NT\AMD64)
mkdir -p $ST/NT/I386/NT3X $ST/NT/I386/NT4-XP $ST/NT/AMD64
python3 nt/inf/mkinfnt.py $ST
for f in WINVBE.SYS WINVBE.DLL WINVBECF.EXE; do
  l=$(echo $f | tr A-Z a-z)
  cp bin/NT/I386/NT3X/$l $ST/NT/I386/NT3X/$f
  cp bin/NT/I386/NT4-XP/$l $ST/NT/I386/NT4-XP/$f
  cp bin/NT/AMD64/$l $ST/NT/AMD64/$f
done
# sources
S=$ST/SRC
mkdir -p $S/drv $S/cfg $S/patch $S/hosttest $S/test $S/tools $S/res $S/dist/WIN1X $S/dist/WIN2X $S/vxd
mkdir -p $S/nt/mini $S/nt/disp $S/nt/common $S/nt/cfg $S/nt/lib $S/nt/inf $S/nt/tools
cp build.sh owenv.sh README.TXT README.md BENIOKU.TXT $S/
mkdir -p $S/docs/images; cp docs/images/*.png $S/docs/images/
cp drv/*.c drv/*.h drv/*.asm drv/*.lnk drv/build*.sh $S/drv/
cp cfg/*.c cfg/*.h cfg/*.rc cfg/*.ico cfg/*.asm cfg/*.lnk cfg/build.sh $S/cfg/
cp patch/*.c patch/*.h patch/*.asm patch/build.sh $S/patch/
cp hosttest/*.c hosttest/*.h hosttest/build.sh $S/hosttest/
cp test/gditest.c test/build.sh $S/test/
mkdir -p $S/test/dd $S/test/d3d
cp test/build32.sh $S/test/
cp test/dd/*.c $S/test/dd/
cp test/d3d/*.c $S/test/d3d/
cp tools/*.py tools/pack.sh $S/tools/
cp res/* $S/res/
cp vxd/*.asm vxd/build.sh $S/vxd/
cp nt/build.sh $S/nt/
cp nt/mini/*.c nt/mini/*.rc $S/nt/mini/; cp nt/disp/*.c nt/disp/*.rc $S/nt/disp/; cp nt/common/*.h $S/nt/common/
cp nt/cfg/*.c nt/cfg/*.h nt/cfg/*.rc $S/nt/cfg/; cp nt/lib/*.def $S/nt/lib/; cp nt/inf/*.py $S/nt/inf/
cp nt/tools/*.py nt/tools/*.c $S/nt/tools/
cp dist/WIN1X/PATCH1X.BAT $S/dist/WIN1X/; cp dist/WIN2X/PREP2X.BAT $S/dist/WIN2X/
# DOS text files with CR LF
for t in $ST/*.TXT $S/README.TXT $ST/WIN30/OEMSETUP.INF $ST/WIN31/OEMSETUP.INF $ST/WIN9X/WINVBE.INF; do
  python3 -c "import sys; p=sys.argv[1]; d=open(p,'rb').read().replace(b'\r\n',b'\n').replace(b'\n',b'\r\n'); open(p,'wb').write(d)" $t
done
rm -f "$OUT"
(cd $(dirname $ST) && zip -q -r -X "$OUT.tmp" WINVBE10)
mv "$OUT.tmp" "$OUT"
ls -l "$OUT"
