#!/bin/sh
# verify_icd_table.sh - check that icdtable.h lists the 336 OpenGL 1.1 entry
# points in Microsoft's opengl32 ICD dispatch order.
#
# Usage: sh verify_icd_table.sh [reactos-glfuncs.h]
#
# Compares icdtable.h with:
#   1. Mesa glapi dispatch offsets 0..335 (src/mesa/glapi/glapitable.h)
#   2. offset="N" attributes in src/mesa/glapi/gen/gl_API.xml
#   3. Mesa 6.x ICD table src/mesa/drivers/windows/icd/icdlist.h
#   4. optionally ReactOS dll/opengl/opengl32/glfuncs.h (name, ICD offset
#      and x86 stdcall stack size), an independent reimplementation of
#      Microsoft's opengl32.dll; the stack sizes are also compared with the
#      _glXxx@N decorations of Mesa's compiled glapi_dispatch.o.

HERE=$(cd "$(dirname "$0")" && pwd)
MESA=$HERE/mesa-src/src/mesa
T=${TMPDIR:-/tmp}/icdverify.$$
mkdir -p "$T"
trap 'rm -rf "$T"' EXIT
fail=0

sed -n 's,^ICD_ENTRY(\([A-Za-z0-9_]*\)) /\* \([0-9]*\) \*/,\2 \1,p' "$HERE/icdtable.h" > "$T/ours"
n=$(wc -l < "$T/ours")
echo "icdtable.h: $n entries"
[ "$n" -eq 336 ] || { echo "FAIL: expected 336 entries"; fail=1; }
awk '{ if ($1 != NR-1) { print "FAIL: index", $1, "at line", NR; exit 1 } }' "$T/ours" || fail=1

sed -n 's,^ *[A-Za-z_ *]*(GLAPIENTRYP \([A-Za-z0-9_]*\))(.*/\* \([0-9]*\) \*/$,\2 \1,p' \
    "$MESA/glapi/glapitable.h" | awk '$1<336' > "$T/table"
if cmp -s "$T/ours" "$T/table"; then echo "glapitable.h offsets 0..335: match"
else echo "FAIL: glapitable.h differs"; diff "$T/ours" "$T/table" | head; fail=1; fi

tr '\n' ' ' < "$MESA/glapi/gen/gl_API.xml" | sed 's,<function ,\n<function ,g' |
    sed -n 's,^<function name="\([A-Za-z0-9_]*\)"[^>]*offset="\([0-9]*\)".*,\2 \1,p' |
    awk '$1<336' | sort -n > "$T/xml"
if cmp -s "$T/ours" "$T/xml"; then echo "gl_API.xml offsets 0..335: match"
else echo "FAIL: gl_API.xml differs"; diff "$T/ours" "$T/xml" | head; fail=1; fi

sed -n 's,^ICD_ENTRY(\([A-Za-z0-9_]*\)).*/\* *\([0-9]*\) *\*/.*,\2 \1,p' \
    "$MESA/drivers/windows/icd/icdlist.h" > "$T/old"
if cmp -s "$T/ours" "$T/old"; then echo "Mesa 6.x icd/icdlist.h: match"
else echo "FAIL: icd/icdlist.h differs"; diff "$T/ours" "$T/old" | head; fail=1; fi

if [ -n "$1" ]; then
    sed -n 's|^USE_GL_FUNC\(_RET\)\{0,1\}(\([A-Za-z0-9_]*\),.*, \([0-9]*\), \([0-9]*\))$|\3 \2 \4|p' "$1" |
        awk '$1<336' | sort -n > "$T/ros"
    cut -d' ' -f1,2 "$T/ros" > "$T/rosnames"
    if cmp -s "$T/ours" "$T/rosnames"; then echo "ReactOS glfuncs.h ICD offsets: match"
    else echo "FAIL: ReactOS glfuncs.h differs"; diff "$T/ours" "$T/rosnames" | head; fail=1; fi
    O=$HERE/obj/mesa/mesa_glapi_glapi_dispatch.o
    if [ -f "$O" ]; then
        i686-w64-mingw32-nm "$O" | sed -n 's,^[0-9a-f]* T _gl\([A-Za-z0-9_]*\)@\([0-9]*\)$,\1 \2,p' | sort > "$T/sym"
        awk '{print $2, $3}' "$T/ros" | sort > "$T/rosstack"
        bad=$(join "$T/rosstack" "$T/sym" | awk '$2 != $3' | wc -l)
        cnt=$(join "$T/rosstack" "$T/sym" | wc -l)
        echo "stdcall stack sizes checked: $cnt, mismatches: $bad"
        [ "$cnt" -eq 336 ] && [ "$bad" -eq 0 ] || fail=1
    fi
fi

[ $fail -eq 0 ] && echo "RESULT: OK" || echo "RESULT: FAILED"
exit $fail
