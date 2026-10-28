#!/bin/sh
# Build WINVBE for Windows NT: WINVBE.SYS (miniport), WINVBE.DLL (display
# driver) and WINVBECF.EXE, for i386 and amd64.
# Requires mingw-w64 (i686 and x86_64), python3 with pefile.
# i386 code is built for the 386 (-march=i386): mingw's default (Pentium Pro,
# CMOV) stops 386/486/Pentium machines with STOP 7F/1E.
# XCFLAGS=-DTEST440FX builds the 915 patch test version.
set -e
cd "$(dirname "$0")"
O=obj
mkdir -p $O/i386 $O/amd64 $O/lib $O/stub
echo "" > $O/stub/d3dnthal.h
cp common/ddrawint.h common/dvp.h $O/stub/

# import libraries
i686-w64-mingw32-dlltool -k -d lib/winsrv31.def -l $O/lib/libwinsrv.a
x86_64-w64-mingw32-dlltool -d lib/videoprt64.def -l $O/lib/libvideoprt64.a
x86_64-w64-mingw32-dlltool -d lib/win32k64.def -l $O/lib/libwin32k64.a

python3 tools/mkld.py i686-w64-mingw32-ld $O/i386.x
python3 tools/mkld.py x86_64-w64-mingw32-ld $O/amd64.x

KFLAGS="-O2 -fno-common -fno-zero-initialized-in-bss -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
 -mno-sse -mno-mmx -mno-stack-arg-probe -fno-builtin -Wall -Wno-unused-function -Wno-pointer-sign \
 -I$O/stub $XCFLAGS"

for A in i386 amd64; do
  if [ $A = i386 ]; then CC=i686-w64-mingw32-gcc; LD=i686-w64-mingw32-ld; ENT=_DriverEntry@8; DENT=_DrvEnableDriver@12;
     VPLIB="-L/usr/i686-w64-mingw32/lib -lvideoprt -lntoskrnl"; W32K="-L/usr/i686-w64-mingw32/lib -lwin32k"; AF="-D_X86_=1 -march=i386 -mtune=i686"
  else CC=x86_64-w64-mingw32-gcc; LD=x86_64-w64-mingw32-ld; ENT=DriverEntry; DENT=DrvEnableDriver;
     VPLIB="-L$O/lib -lvideoprt64 -L/usr/x86_64-w64-mingw32/lib -lntoskrnl"; W32K="-L$O/lib -lwin32k64"; AF=-D_AMD64_=1; fi

  # miniport
  $CC $KFLAGS $AF -c mini/winvbe.c -o $O/$A/mini.o
  $CC $KFLAGS $AF -c ../patch/i915core.c -o $O/$A/i915core.o
  ${CC%gcc}windres mini/winvbe.rc -o $O/$A/minirc.o
  $LD -T $O/$A.x --subsystem native -e $ENT -shared --exclude-all-symbols --image-base 0x10000 --file-alignment 0x200 --section-alignment 0x200 \
      --major-os-version 1 --minor-os-version 0 --major-subsystem-version 1 --minor-subsystem-version 0 \
      $O/$A/mini.o $O/$A/i915core.o $O/$A/minirc.o $VPLIB -o $O/$A/winvbe.sys
  python3 tools/pefix.py $O/$A/winvbe.sys nonpaged
  [ $A = i386 ] && python3 tools/chkimp.py $O/$A/winvbe.sys

  # display driver (kernel mode, NT 4.0 and later)
  $CC $KFLAGS $AF -c disp/winvbe.c -o $O/$A/disp.o
  ${CC%gcc}windres disp/winvbe.rc -o $O/$A/disprc.o
  $LD -T $O/$A.x --subsystem native -e $DENT -shared --exclude-all-symbols --image-base ${DLLBASE:-0x10000} --file-alignment ${DLLALIGN:-0x200} --section-alignment ${DLLALIGN:-0x200} \
      --major-os-version 4 --minor-os-version 0 --major-subsystem-version 4 --minor-subsystem-version 0 \
      $O/$A/disp.o $O/$A/disprc.o $W32K -o $O/$A/winvbe.dll
  python3 tools/pefix.py $O/$A/winvbe.dll nonpaged
done

# display driver for NT 3.x (user mode in CSRSS, GDI engine in WINSRV.DLL)
i686-w64-mingw32-gcc $KFLAGS -D_X86_=1 -march=i386 -mtune=i686 -DNT3X -c disp/winvbe.c -o $O/i386/disp3.o
python3 ../tools/mkres.py ../res >/dev/null
# NT 3.1 VGA.DLL bitmaps, cursors, icons and colours (tools/stockres.py)
python3 ../tools/stockres.py nt3 ../msfiles/stock/VGANT31.DLL ../res $O/res3
i686-w64-mingw32-windres --include-dir $O/res3 disp/winvbe3.rc -o $O/i386/disp3rc.o
i686-w64-mingw32-ld -T $O/i386.x --subsystem native -e _DrvEnableDriver@12 -shared --image-base 0x6F000000 \
    --file-alignment 0x200 --section-alignment 0x1000 \
    --major-os-version 1 --minor-os-version 0 --major-subsystem-version 1 --minor-subsystem-version 0 \
    --kill-at $O/i386/disp3.o $O/i386/disp3rc.o -L$O/lib -lwinsrv -L/usr/i686-w64-mingw32/lib -lkernel32 -o $O/i386/winvbe3.dll
python3 tools/pefix.py $O/i386/winvbe3.dll noentry
# settings program (Win32, no C runtime; runs on NT 3.1)
CFGF="-Os -s -nostdlib -fno-builtin -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wno-unused-function"
i686-w64-mingw32-windres cfg/winvbecf.rc -o $O/i386/cfgrc.o
i686-w64-mingw32-gcc $CFGF -march=i386 -mtune=i686 -mno-stack-arg-probe -Wl,-e,_start@0 -Wl,--subsystem,windows:3.10 \
    -Wl,--major-os-version,1 -Wl,--minor-os-version,0 cfg/winvbecf.c $O/i386/cfgrc.o \
    -lkernel32 -luser32 -lgdi32 -ladvapi32 -o $O/i386/winvbecf.exe
x86_64-w64-mingw32-windres cfg/winvbecf.rc -o $O/amd64/cfgrc.o
x86_64-w64-mingw32-gcc $CFGF -Wl,-e,start -Wl,--subsystem,windows:5.02 \
    cfg/winvbecf.c $O/amd64/cfgrc.o -lkernel32 -luser32 -lgdi32 -ladvapi32 -o $O/amd64/winvbecf.exe
ls -l $O/i386 $O/amd64 | grep -v "\.o$"
# no Pentium Pro instructions in the i386 files (see -march above)
for f in $O/i386/winvbe.sys $O/i386/winvbe.dll $O/i386/winvbe3.dll $O/i386/winvbecf.exe; do
  if i686-w64-mingw32-objdump -d $f 2>/dev/null | grep -q -E '[[:space:]](cmov|fcomi|fucomi)'; then
    echo "$f: instructions the 386/486/Pentium do not have"; exit 1
  fi
done
