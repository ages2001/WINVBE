#!/bin/sh
# build GDITEST.EXE - GDI test program used to verify the driver
set -e
cd "$(dirname "$0")"
. ../owenv.sh
wcc -q -ml -zW -bt=windows -2 -ox -fo=gditest.obj gditest.c
wlink system windows option quiet name gditest.exe option stack=8k file gditest.obj
