#!/bin/sh
# build WINVBE.386 (Windows 3.x 386 enhanced mode helper VxD); DEBUG=1: port E9h trace
set -e
cd "$(dirname "$0")"
mkdir -p obj
D=; [ -n "$DEBUG" ] && D=-DDEBUGE9
nasm -f bin -DORG=0 $D -o obj/a0.bin winvbe.asm
nasm -f bin -DORG=0x10000 $D -o obj/a1.bin winvbe.asm
python3 ../tools/mkvxd32.py WINVBE obj/a0.bin obj/a1.bin obj/winvbe.386
