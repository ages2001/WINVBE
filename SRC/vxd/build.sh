#!/bin/sh
# build WINVBE.386 (Windows 3.x 386 enhanced mode / 9x helper VxD); DEBUG=1: port E9h trace
# The x86 emulator for video BIOS calls (emuvxd.c with libx86emu from
# ../nt/mini/x86emu) is compiled with mingw, converted to ELF and linked as
# a flat image at the address the VxD includes it (EMU_OFS in winvbe.asm),
# once per org: tools/mkvxd32.py derives the relocations from the two images.
set -e
cd "$(dirname "$0")"
mkdir -p obj/emu
D=; [ -n "$DEBUG" ] && D=-DDEBUGE9
X=../nt/mini/x86emu
CF="-Os -march=i386 -mtune=i686 -ffreestanding -fno-common -fno-stack-protector -fno-asynchronous-unwind-tables \
 -mno-sse -mno-mmx -mno-stack-arg-probe -fno-builtin -fno-jump-tables -DWINVBE -I$X/kinc -I$X/include -w"
O=obj/emu
for f in api decode mem ops ops2 prim_ops; do i686-w64-mingw32-gcc $CF -c $X/$f.c -o $O/$f.o; done
i686-w64-mingw32-gcc $CF -c emuvxd.c -o $O/emuvxd.o
i686-w64-mingw32-as emuentry.s -o $O/entry.o
OFS=$(sed -n 's/^EMU_OFS equ \([0-9A-Fa-f]*\)h.*/\1/p' winvbe.asm)
for i in 0 1; do
    B=$((0x$OFS + i * 0x10000))
    # one PE at a fixed base (its first section at B), then its memory image
    i686-w64-mingw32-ld -e _ve_entry --image-base $(printf '0x%X' $((B - 0x1000))) --section-alignment 0x1000 \
        --file-alignment 0x200 --disable-dynamicbase --no-seh -nostdlib \
        -o $O/emu_$i.exe $O/entry.o $O/emuvxd.o $O/api.o $O/decode.o $O/mem.o $O/ops.o $O/ops2.o $O/prim_ops.o \
        $(i686-w64-mingw32-gcc -print-libgcc-file-name)
    python3 ../tools/pe2flat.py $O/emu_$i.exe $B $O/emu_$i.bin
done
nasm -f bin -DORG=0 $D -o obj/a0.bin winvbe.asm
nasm -f bin -DORG=0x10000 $D -o obj/a1.bin winvbe.asm
python3 ../tools/mkvxd32.py WINVBE obj/a0.bin obj/a1.bin obj/winvbe.386
