#!/bin/sh
# build-host.sh - build and run the Linux-side ICD test (icdhost): the same
# Mesa sources as build.sh, plus icd.c and wvbdrv.c on a fake Win32 GDI.
# Mesa asserts are enabled here (no NDEBUG).  Needs ../build.sh to have run
# once (for the generated GLSL built-in library headers in ../obj/gen).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
MESA=$TOP/mesa-src
OBJ=$TOP/obj/hosttest
HOSTCC=${HOSTCC:-gcc}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 2)}
mkdir -p "$OBJ/mesa"
[ -s "$TOP/obj/gen/library/slang_core_gc.h" ] || { echo "run ../build.sh first" >&2; exit 1; }

SOURCES=$(sed -n '/^SOURCES="/,/^"/p' "$TOP/build.sh" | sed '1d;$d' | sed 's,\$M/,mesa/,g')
INCS="-I$MESA/include -I$MESA/src/mesa -I$MESA/src/mesa/main -I$MESA/src/mesa/glapi \
      -I$MESA/src/mesa/shader -I$MESA/src/mesa/shader/slang -I$TOP/obj/gen"
CFLAGS="-std=gnu99 -O1 -g -fno-strict-aliasing -fcommon -w $INCS"
export HOSTCC CFLAGS MESA OBJ
for f in $SOURCES; do echo $f; done | xargs -P "$JOBS" -I{} sh -c '
    src=$MESA/src/{}
    o=$OBJ/mesa/$(echo {} | sed "s,/,_,g; s,\.c$,.o,")
    if [ ! -f "$o" ] || [ "$src" -nt "$o" ]; then
        echo "HOSTCC {}"
        $HOSTCC $CFLAGS -c "$src" -o "$o" || exit 255
    fi'
WFLAGS="-std=gnu99 -O1 -g -fno-strict-aliasing -Wall -Wno-unused-function -Wno-array-parameter -I$HERE $INCS"
$HOSTCC $WFLAGS -c "$TOP/wvbdrv.c" -o "$OBJ/wvbdrv.o"
$HOSTCC $WFLAGS -c "$TOP/icd.c" -o "$OBJ/icd.o"
$HOSTCC $WFLAGS -Wno-unused-variable -c "$HERE/icdhost.c" -o "$OBJ/icdhost.o"
$HOSTCC -o "$OBJ/icdhost" "$OBJ/icdhost.o" "$OBJ/icd.o" "$OBJ/wvbdrv.o" "$OBJ"/mesa/*.o -lm
"$OBJ/icdhost" "$OBJ/icdhost.txt"
