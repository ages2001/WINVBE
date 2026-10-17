#!/bin/sh
set -e
cd "$(dirname "$0")"
D=../drv
gcc -g -O0 -fsanitize=address,undefined -Wall -Wno-unused -Wno-pointer-sign -Wno-unknown-pragmas -I. -I$D -o test test.c host.c $D/color.c $D/rop.c $D/surf.c $D/blt.c $D/cursor.c $D/robj.c $D/output.c $D/text.c $D/dib.c
