#!/bin/sh
set -e
cd "$(dirname "$0")"
. ../owenv.sh
mkdir -p obj
cp ../res/winvbecf.ico . 2>/dev/null || true
wcc -q -ml -zW -bt=windows -2 -ox -wx -wcd=303 -fo=obj/vbeq.obj vbeq.c
wcc -q -ml -zW -bt=windows -2 -ox -wx -wcd=303 -fo=obj/winvbecf.obj winvbecf.c
wcc -q -ml -zW -bt=windows -3 -ox -wx -wcd=303 -fo=obj/i915core.obj ../patch/i915core.c
wrc -q -r -bt=windows -30 -i=$WATCOM/h/win -fo=obj/winvbecf.res winvbecf.rc
wlink system windows option quiet name obj/winvbecf.exe option stack=10k file obj/winvbecf.obj,obj/vbeq.obj,obj/i915core.obj
wrc -q -bt=windows -30 obj/winvbecf.res obj/winvbecf.exe -fe=obj/winvbecf.exe
ls -l obj/winvbecf.exe
# WVBPOWER.SCR - monitor power saver (screen saver) for Windows 3.1 and later
wcc -q -ml -zW -bt=windows -2 -ox -wx -wcd=303 -fo=obj/wvbpower.obj wvbpower.c
wrc -q -r -bt=windows -30 -i=$WATCOM/h/win -fo=obj/wvbpower.res wvbpower.rc
wlink @wvbpower.lnk
wrc -q -bt=windows -30 obj/wvbpower.res obj/wvbpower.scr -fe=obj/wvbpower.scr
ls -l obj/wvbpower.scr
export INCLUDE=$WATCOM/h
wcc -q -ml -bt=dos -0 -os -wx -wcd=303 -wcd=202 -fo=obj/vbeq_d.obj vbeq.c
wcc -q -ml -bt=dos -0 -os -wx -wcd=303 -wcd=202 -fo=obj/install.obj install.c
wlink system dos option quiet option stack=8k name obj/install.exe file obj/install.obj,obj/vbeq_d.obj
ls -l obj/install.exe
# WINVBECF for Windows 1.x/2.x (OLDWIN): obj/winvbecf1.exe, obj/winvbecf2.exe
mkdir -p objo
export INCLUDE="$WATCOM/h:$WATCOM/h/win"
OCF="-q -ml -zW -bt=windows -0 -os -s -zl -wx -wcd=303 -DOLDWIN $XCFLAGS"
wcc $OCF -fo=objo/winvbecf.obj winvbecf.c
wcc $OCF -fo=objo/vbeq.obj vbeq.c
wcc $OCF -fo=objo/oldlib.obj oldlib.c
nasm -f obj $NASMFLAGS -o objo/cfgold.obj cfgold.asm
(cd objo && wlink @../winvbeo.lnk file cfgold.obj,winvbecf.obj,vbeq.obj,oldlib.obj)
wrc -q -r -bt=windows -30 -fo=objo/winvbeo.res winvbeo.rc
wrc -q -bt=windows -30 objo/winvbeo.res objo/winvbeo.exe -fe=objo/winvbeo.exe
cp objo/winvbeo.exe obj/winvbecf2.exe
cp objo/winvbeo.exe obj/winvbecf1.exe
python3 ../tools/nefix.py obj/winvbecf2.exe 0x201
python3 ../tools/nefix.py obj/winvbecf1.exe 0
ls -l obj/winvbecf1.exe obj/winvbecf2.exe
