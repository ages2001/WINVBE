#!/bin/sh
# build.sh - build WVBGL32.DLL, the WINVBE OpenGL ICD (Mesa 7.8.2 classic
# software rasterizer rendering through GDI) and the ICDTEST.EXE test program.
#
# Target: Windows 95/98/ME and Windows NT 4.0/2000/XP, 32-bit x86, or
# (with "x64") Windows XP x64 / Server 2003 x64, 64-bit.
# Toolchain: mingw-w64 (i686-w64-mingw32-gcc, x86_64-w64-mingw32-gcc for
# x64), plus the host gcc for the GLSL built-in library generator.
# No make/scons/python needed.
#
# Usage:  sh build.sh [sse2|x64] [clean]
#
# 32-bit Windows gets two builds of the renderer and WVBGL32.DLL, which
# loads the SSE2 one where the CPU and the OS allow it (loader.c):
#   sh build.sh       obj/wvbglx.dll (x87, Pentium), obj/wvbgl32.dll (the
#                     loader), obj/icdtest.exe
#   sh build.sh sse2  objsse/wvbgls.dll (SSE2)
#   sh build.sh x64   obj64/wvbgl32.dll (x64, SSE2), obj64/icdtest.exe

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
MESA=$HERE/mesa-src
X64=0 SSE2=0
if [ "$1" = x64 ]; then X64=1; shift; fi
if [ "$1" = sse2 ]; then SSE2=1; shift; fi
if [ $X64 = 1 ]; then OBJ=$HERE/obj64; TRIPLE=x86_64-w64-mingw32; DLL=wvbgl32.dll
elif [ $SSE2 = 1 ]; then OBJ=$HERE/objsse; TRIPLE=i686-w64-mingw32; DLL=wvbgls.dll
else OBJ=$HERE/obj; TRIPLE=i686-w64-mingw32; DLL=wvbglx.dll; fi
GEN=$OBJ/gen
HOST=$OBJ/host
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 2)}

CC=${CC:-$TRIPLE-gcc}
STRIP=${STRIP:-$TRIPLE-strip}
OBJDUMP=${OBJDUMP:-$TRIPLE-objdump}
HOSTCC=${HOSTCC:-gcc}

if [ "$1" = clean ]; then
    rm -rf "$OBJ"
    exit 0
fi

if [ ! -d "$MESA/src/mesa" ]; then
    echo "Mesa 7.8.2 source tree not found in $MESA" >&2
    echo "git clone --depth 1 --branch mesa-7.8.2 https://github.com/mirror/mesa $MESA" >&2
    exit 1
fi

mkdir -p "$OBJ/mesa" "$GEN/library" "$HOST"

# --------------------------------------------------------------------------
# 1. GLSL built-in library: build the generator for the host and turn the
#    slang_*.gc sources into the *_gc.h byte arrays slang_compile.c includes.
# --------------------------------------------------------------------------
GLSL=$MESA/src/glsl
if [ ! -x "$HOST/glsl_compile" ]; then
    echo "HOSTCC glsl_compile"
    $HOSTCC -O1 -w -o "$HOST/glsl_compile" \
        $GLSL/pp/sl_pp_context.c $GLSL/pp/sl_pp_define.c $GLSL/pp/sl_pp_dict.c \
        $GLSL/pp/sl_pp_error.c $GLSL/pp/sl_pp_expression.c $GLSL/pp/sl_pp_extension.c \
        $GLSL/pp/sl_pp_if.c $GLSL/pp/sl_pp_line.c $GLSL/pp/sl_pp_macro.c \
        $GLSL/pp/sl_pp_pragma.c $GLSL/pp/sl_pp_process.c $GLSL/pp/sl_pp_purify.c \
        $GLSL/pp/sl_pp_token.c $GLSL/pp/sl_pp_token_util.c $GLSL/pp/sl_pp_version.c \
        $GLSL/cl/sl_cl_parse.c $GLSL/apps/compile.c
fi
LIBDIR=$MESA/src/mesa/shader/slang/library
for spec in \
    "fragment slang_120_core" \
    "fragment slang_builtin_120_common" \
    "fragment slang_builtin_120_fragment" \
    "fragment slang_common_builtin" \
    "fragment slang_core" \
    "fragment slang_fragment_builtin" \
    "vertex slang_vertex_builtin"
do
    set -- $spec
    if [ ! -s "$GEN/library/$2_gc.h" ] || [ "$LIBDIR/$2.gc" -nt "$GEN/library/$2_gc.h" ]; then
        echo "GLSL $2.gc"
        "$HOST/glsl_compile" $1 "$LIBDIR/$2.gc" "$GEN/library/$2_gc.h"
    fi
done

# --------------------------------------------------------------------------
# 2. Mesa core (classic swrast path).  Paths relative to $MESA/src.
# --------------------------------------------------------------------------
M=mesa
SOURCES="
$M/main/api_arrayelt.c $M/main/api_exec.c $M/main/api_loopback.c $M/main/api_noop.c
$M/main/api_validate.c $M/main/accum.c $M/main/attrib.c $M/main/arrayobj.c
$M/main/blend.c $M/main/bufferobj.c $M/main/buffers.c $M/main/clear.c $M/main/clip.c
$M/main/colortab.c $M/main/condrender.c $M/main/context.c $M/main/convolve.c
$M/main/cpuinfo.c $M/main/debug.c $M/main/depth.c $M/main/depthstencil.c
$M/main/dlist.c $M/main/dlopen.c $M/main/drawpix.c $M/main/enable.c $M/main/enums.c
$M/main/eval.c $M/main/execmem.c $M/main/extensions.c $M/main/fbobject.c
$M/main/feedback.c $M/main/ffvertex_prog.c $M/main/fog.c $M/main/formats.c
$M/main/framebuffer.c $M/main/get.c $M/main/getstring.c $M/main/hash.c $M/main/hint.c
$M/main/histogram.c $M/main/image.c $M/main/imports.c $M/main/light.c $M/main/lines.c
$M/main/matrix.c $M/main/mipmap.c $M/main/mm.c $M/main/multisample.c $M/main/pixel.c
$M/main/pixelstore.c $M/main/points.c $M/main/polygon.c $M/main/queryobj.c
$M/main/rastpos.c $M/main/rbadaptors.c $M/main/readpix.c $M/main/remap.c
$M/main/renderbuffer.c $M/main/scissor.c $M/main/shaders.c $M/main/shared.c
$M/main/state.c $M/main/stencil.c $M/main/syncobj.c $M/main/texcompress.c
$M/main/texcompress_s3tc.c $M/main/texcompress_fxt1.c $M/main/texenv.c
$M/main/texenvprogram.c $M/main/texfetch.c $M/main/texformat.c $M/main/texgen.c
$M/main/texgetimage.c $M/main/teximage.c $M/main/texobj.c $M/main/texparam.c
$M/main/texrender.c $M/main/texstate.c $M/main/texstore.c $M/main/varray.c
$M/main/version.c $M/main/viewport.c $M/main/vtxfmt.c

$M/glapi/glapi.c $M/glapi/glapi_dispatch.c $M/glapi/glapi_getproc.c
$M/glapi/glapi_nop.c $M/glapi/glthread.c

$M/math/m_debug_clip.c $M/math/m_debug_norm.c $M/math/m_debug_xform.c
$M/math/m_eval.c $M/math/m_matrix.c $M/math/m_translate.c $M/math/m_vector.c
$M/math/m_xform.c

$M/swrast/s_aaline.c $M/swrast/s_aatriangle.c $M/swrast/s_accum.c $M/swrast/s_alpha.c
$M/swrast/s_atifragshader.c $M/swrast/s_bitmap.c $M/swrast/s_blend.c $M/swrast/s_blit.c
$M/swrast/s_clear.c $M/swrast/s_copypix.c $M/swrast/s_context.c $M/swrast/s_depth.c
$M/swrast/s_drawpix.c $M/swrast/s_feedback.c $M/swrast/s_fog.c $M/swrast/s_fragprog.c
$M/swrast/s_lines.c $M/swrast/s_logic.c $M/swrast/s_masking.c $M/swrast/s_points.c
$M/swrast/s_readpix.c $M/swrast/s_span.c $M/swrast/s_stencil.c $M/swrast/s_texcombine.c
$M/swrast/s_texfilter.c $M/swrast/s_triangle.c $M/swrast/s_zoom.c

$M/swrast_setup/ss_context.c $M/swrast_setup/ss_triangle.c

$M/tnl/t_context.c $M/tnl/t_pipeline.c $M/tnl/t_draw.c $M/tnl/t_rasterpos.c
$M/tnl/t_vb_program.c $M/tnl/t_vb_render.c $M/tnl/t_vb_texgen.c $M/tnl/t_vb_texmat.c
$M/tnl/t_vb_vertex.c $M/tnl/t_vb_cull.c $M/tnl/t_vb_fog.c $M/tnl/t_vb_light.c
$M/tnl/t_vb_normals.c $M/tnl/t_vb_points.c $M/tnl/t_vp_build.c $M/tnl/t_vertex.c
$M/tnl/t_vertex_generic.c

$M/vbo/vbo_context.c $M/vbo/vbo_exec.c $M/vbo/vbo_exec_api.c $M/vbo/vbo_exec_array.c
$M/vbo/vbo_exec_draw.c $M/vbo/vbo_exec_eval.c $M/vbo/vbo_rebase.c $M/vbo/vbo_split.c
$M/vbo/vbo_split_copy.c $M/vbo/vbo_split_inplace.c $M/vbo/vbo_save.c
$M/vbo/vbo_save_api.c $M/vbo/vbo_save_draw.c $M/vbo/vbo_save_loopback.c

$M/shader/arbprogparse.c $M/shader/arbprogram.c $M/shader/atifragshader.c
$M/shader/hash_table.c $M/shader/lex.yy.c $M/shader/nvfragparse.c $M/shader/nvprogram.c
$M/shader/nvvertparse.c $M/shader/program.c $M/shader/program_parse.tab.c
$M/shader/program_parse_extra.c $M/shader/prog_cache.c $M/shader/prog_execute.c
$M/shader/prog_instruction.c $M/shader/prog_noise.c $M/shader/prog_optimize.c
$M/shader/prog_parameter.c $M/shader/prog_parameter_layout.c $M/shader/prog_print.c
$M/shader/prog_statevars.c $M/shader/prog_uniform.c $M/shader/programopt.c
$M/shader/symbol_table.c $M/shader/shader_api.c

$M/shader/slang/slang_builtin.c $M/shader/slang/slang_codegen.c
$M/shader/slang/slang_compile.c $M/shader/slang/slang_compile_function.c
$M/shader/slang/slang_compile_operation.c $M/shader/slang/slang_compile_struct.c
$M/shader/slang/slang_compile_variable.c $M/shader/slang/slang_emit.c
$M/shader/slang/slang_ir.c $M/shader/slang/slang_label.c $M/shader/slang/slang_link.c
$M/shader/slang/slang_log.c $M/shader/slang/slang_mem.c $M/shader/slang/slang_print.c
$M/shader/slang/slang_simplify.c $M/shader/slang/slang_storage.c
$M/shader/slang/slang_typeinfo.c $M/shader/slang/slang_vartable.c
$M/shader/slang/slang_utility.c

glsl/pp/sl_pp_context.c glsl/pp/sl_pp_define.c glsl/pp/sl_pp_dict.c
glsl/pp/sl_pp_error.c glsl/pp/sl_pp_expression.c glsl/pp/sl_pp_extension.c
glsl/pp/sl_pp_if.c glsl/pp/sl_pp_line.c glsl/pp/sl_pp_macro.c glsl/pp/sl_pp_pragma.c
glsl/pp/sl_pp_process.c glsl/pp/sl_pp_purify.c glsl/pp/sl_pp_token.c
glsl/pp/sl_pp_token_util.c glsl/pp/sl_pp_version.c
glsl/cl/sl_cl_parse.c

$M/drivers/common/driverfuncs.c $M/drivers/common/meta.c
"

# Code generation: plain x87 floating point, Pentium-compatible integer code
# (no CMOV, MMX, SSE), no Mesa x86 assembly.
# sse2: scalar and vector SSE2 (Pentium 4 instruction set, tuned for later
# CPUs as well); Win32 only keeps the stack 4-byte aligned, so functions
# that need 16 bytes align it themselves.
# x64: the baseline x86-64 ISA (SSE2), which every x64 CPU has.
if [ $X64 = 1 ]; then
    ARCH="-mtune=generic"
elif [ $SSE2 = 1 ]; then
    ARCH="-march=pentium4 -mtune=generic -mfpmath=sse -msse2 -mincoming-stack-boundary=2"
else
    ARCH="-march=i586 -mtune=pentium2 -mfpmath=387 -mno-mmx -mno-sse -mno-3dnow"
fi
# WIN32_THREADS: current context/dispatch live in TlsAlloc() slots once a
# second thread makes a context current (no __thread/__declspec(thread)).
# NO_FAST_MATH: Mesa would switch the x87 to single precision while it
# renders (START_FAST_MATH), which breaks its own IEEE floor/ceil tricks
# (texel addresses one off: GL_NEAREST textures sampled the border)
DEFS="-DNDEBUG -DNO_FAST_MATH -DWIN32_THREADS -DMESA_MINWARN -D__USE_MINGW_ANSI_STDIO=0 -D__MSVCRT_VERSION__=0x0600 \
      -DWINVER=0x0400 -D_WIN32_WINNT=0x0400 -D_WIN32_IE=0x0400"
INCS="-I$MESA/include -I$MESA/src/mesa -I$MESA/src/mesa/main -I$MESA/src/mesa/glapi \
      -I$MESA/src/mesa/shader -I$MESA/src/mesa/shader/slang -I$GEN"
CFLAGS="-std=gnu99 -O2 $ARCH -fno-strict-aliasing -fno-math-errno -fcommon -fno-stack-protector \
        -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections \
        -w $DEFS $INCS"
export CC CFLAGS MESA OBJ

echo "Compiling Mesa core with $JOBS jobs"
for f in $SOURCES; do echo $f; done | xargs -P "$JOBS" -I{} sh -c '
    src=$MESA/src/{}
    o=$OBJ/mesa/$(echo {} | sed "s,/,_,g; s,\.c$,.o,")
    if [ ! -f "$o" ] || [ "$src" -nt "$o" ]; then
        echo "CC {}"
        $CC $CFLAGS -c "$src" -o "$o" || exit 255
    fi'

# --------------------------------------------------------------------------
# 3. WINVBE GDI driver + ICD entry layer, link the DLL.
# --------------------------------------------------------------------------
WFLAGS="-std=gnu99 -O2 $ARCH -fno-strict-aliasing -fno-math-errno -fno-stack-protector \
        -fno-asynchronous-unwind-tables -Wall -Wno-unused-function -Wno-array-parameter $DEFS $INCS"
for f in wvbdrv icd compat; do
    if [ ! -f "$OBJ/$f.o" ] || [ "$HERE/$f.c" -nt "$OBJ/$f.o" ] || [ "$HERE/wvbdrv.h" -nt "$OBJ/$f.o" ] ||
       [ "$HERE/icdtable.h" -nt "$OBJ/$f.o" ]; then
        echo "CC $f.c"
        $CC $WFLAGS -c "$HERE/$f.c" -o "$OBJ/$f.o"
    fi
done

# x64 has one calling convention: the exports carry no @N decoration.
if [ $X64 = 1 ]; then
    sed 's/ = [A-Za-z_0-9]*@[0-9]*$//' "$HERE/wvbgl32.def" > "$OBJ/wvbgl32.def"
    DEF=$OBJ/wvbgl32.def ENTRY=WvbDllEntry BASE=0x6a000000
    SUBVER="-Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,2 -Wl,--major-os-version,5"
else
    sed "s/^LIBRARY .*/LIBRARY $(echo $DLL | tr a-z A-Z)/" "$HERE/wvbgl32.def" > "$OBJ/wvbgl32.def"
    DEF=$OBJ/wvbgl32.def ENTRY=_WvbDllEntry@12 BASE=0x6a000000
    # subsystem/OS version 3.10: Windows NT 3.1 refuses to load an image that
    # asks for a newer Windows (its loader fails with "bad EXE format"). 3.10
    # is a minimum, so these binaries still run on NT 3.5x-XP and 95-ME.
    SUBVER="-Wl,--major-subsystem-version,3 -Wl,--minor-subsystem-version,10 -Wl,--major-os-version,3 -Wl,--minor-os-version,10"
fi

# C runtime: 32-bit CRTDLL.DLL, which every 32-bit Windows has (NT 3.1 - XP,
# 95 - ME, WOW64; MSVCRT.DLL is missing on NT 3.x and retail Windows 95),
# x64 MSVCRT.DLL
if [ $X64 = 1 ]; then CRT=-lmsvcrt; CRTDLL=msvcrt.dll; else CRT=-lcrtdll; CRTDLL=crtdll.dll; fi
echo "LD $DLL"
# -nostartfiles: no mingw-w64 DLL CRT startup (no TLS directory, no atexit
# table); icd.c provides the DLL entry point and initialises what Mesa needs.
$CC -shared -nostartfiles -nostdlib -static-libgcc -Wl,--gc-sections \
    -Wl,-e,$ENTRY -Wl,--enable-stdcall-fixup \
    -Wl,--image-base,$BASE -Wl,--subsystem,windows $SUBVER \
    -Wl,--disable-dynamicbase -Wl,--disable-nxcompat \
    -Wl,--no-insert-timestamp \
    -o "$OBJ/$DLL" "$DEF" \
    "$OBJ/icd.o" "$OBJ/wvbdrv.o" "$OBJ/compat.o" "$OBJ"/mesa/*.o \
    $CRT -lgdi32 -luser32 -lkernel32 -lgcc $CRT -lkernel32
$STRIP --strip-unneeded "$OBJ/$DLL"

# Imports must stay within what Windows NT 3.51, 95, 98, ME, 2000 and XP
# provide (checked against NT 3.51's DLLs).  Anything not on these lists
# stops the build.
OK_DLLS="GDI32.dll KERNEL32.dll $CRTDLL USER32.dll"
OK_FUNCS="BitBlt CreateCompatibleDC CreateDIBSection DeleteDC DeleteObject GetDeviceCaps SelectObject ExtEscape GetPixelFormat GetVersion SetDIBitsToDevice
 DeleteCriticalSection DisableThreadLibraryCalls EnterCriticalSection FreeLibrary GetCurrentThreadId
 GetLastError GetModuleHandleA GetProcAddress InitializeCriticalSection LeaveCriticalSection
 LoadLibraryA OutputDebugStringA SetLastError TlsAlloc TlsFree TlsGetValue TlsSetValue
 GetClientRect GetDC IsWindow ReleaseDC WindowFromDC
 __iob_func _errno _iob _strdup _vsnprintf abort asin atan atoi bsearch calloc clearerr cos exit exp fclose
 ferror fopen fprintf fputc fputs fread free fwrite getc getenv ldexp log malloc memcmp memcpy
 memmove memset perror pow printf putchar puts qsort realloc sin sprintf strchr strcmp strcpy
 strlen strncmp strncpy strstr strtod strtol vsprintf"
$OBJDUMP -p "$OBJ/$DLL" > "$OBJ/wvbgl32.pe.txt"
bad=0
for d in $(sed -n 's/^\tDLL Name: //p' "$OBJ/wvbgl32.pe.txt"); do
    case " $OK_DLLS " in *" $d "*) ;; *) echo "unexpected import DLL: $d" >&2; bad=1 ;; esac
done
for f in $(sed -n '/^\tDLL Name:/,/^$/s/^\t[0-9a-f]*\t *[0-9]*  \([A-Za-z_0-9]*\)$/\1/p' "$OBJ/wvbgl32.pe.txt"); do
    case " $(echo $OK_FUNCS) " in *" $f "*) ;; *) echo "unexpected import: $f" >&2; bad=1 ;; esac
done
if grep -q "^The .tls\|TLS Directory" "$OBJ/wvbgl32.pe.txt"; then echo "DLL has a TLS directory" >&2; bad=1; fi
[ $bad -eq 0 ] || { echo "import check failed" >&2; exit 1; }
echo "import check passed"
# the x87 build must run on a Pentium: no CMOV, FCOMI, FCMOV (P6) or SSE
if [ $X64 = 0 ] && [ $SSE2 = 0 ] && $OBJDUMP -d "$OBJ/$DLL" | grep -qE '\s(cmov|fcmov|fcomi|fucomi|movaps|movups|xorps|pxor|movd|movq)'; then
    echo "$DLL: instructions the Pentium does not have" >&2; exit 1
fi

# --------------------------------------------------------------------------
# 4. 32-bit: WVBGL32.DLL, the loader (KERNEL32 only); the SSE2 build has
#    none of its own and no test program
# --------------------------------------------------------------------------
[ $SSE2 = 1 ] && { ls -l "$OBJ/$DLL"; exit 0; }
if [ $X64 = 0 ]; then
    echo "LD wvbgl32.dll (loader)"
    $CC -std=gnu99 -Os -march=i386 -mno-sse -Wall -fno-stack-protector -fno-asynchronous-unwind-tables \
        -fno-builtin -shared -nostartfiles -nostdlib -Wl,-e,_WvbLoaderEntry@12 -Wl,--enable-stdcall-fixup \
        -Wl,--image-base,0x69f00000 -Wl,--subsystem,windows $SUBVER \
        -Wl,--disable-dynamicbase -Wl,--disable-nxcompat -Wl,--no-insert-timestamp \
        -o "$OBJ/wvbgl32.dll" "$HERE/wvbgl32.def" "$HERE/loader.c" -lkernel32
    $STRIP --strip-unneeded "$OBJ/wvbgl32.dll"
    $OBJDUMP -p "$OBJ/wvbgl32.dll" > "$OBJ/loader.pe.txt"
    # the SSE2 probe is the only SSE instruction in it
    if $OBJDUMP -d "$OBJ/wvbgl32.dll" | grep -E '\s(cmov|fcmov|fcomi|fucomi|movaps|xorps|movd|movq)' | grep -q .; then
        echo "loader: instructions the 386/Pentium does not have" >&2; exit 1
    fi
    if [ "$(sed -n 's/^\tDLL Name: //p' "$OBJ/loader.pe.txt")" != "KERNEL32.dll" ]; then
        echo "loader: unexpected imports" >&2; exit 1
    fi
fi

# --------------------------------------------------------------------------
# 4b. 32-bit: OPENGL32.DLL for Windows NT 3.1/3.5/3.51 (ogl3x.c; full OpenGL
#     2.1 for programs on the ICD), its gl* stubs made from icdtable.h
# --------------------------------------------------------------------------
if [ $X64 = 0 ]; then
    echo "LD opengl32.dll (NT 3.x)"
    mkdir -p "$OBJ/nt3x"
    S=$OBJ/nt3x/ogl3x_stubs.s
    { echo ".text"
      sed -n 's,^ICD_ENTRY(\([A-Za-z0-9]*\)) /\* \([0-9]*\) \*/,\2,p' "$HERE/icdtable.h" | while read n; do
        echo ".globl _wvbo_$n"
        echo "_wvbo_$n: cmpl \$0, _wvbo_ready"
        echo "    jne 1f"
        echo "    call _wvbo_init"
        echo "1:  jmp *_wvbo_tab+4*$n"
      done; } > "$S"
    { echo "LIBRARY OPENGL32.DLL"; echo "EXPORTS"
      sed -n 's,^ICD_ENTRY(\([A-Za-z0-9]*\)) /\* \([0-9]*\) \*/,    gl\1 = wvbo_\2,p' "$HERE/icdtable.h"
      for f in wglCopyContext@12 wglCreateContext@4 wglCreateLayerContext@8 wglDeleteContext@4 \
               wglDescribeLayerPlane@20 wglGetCurrentContext@0 wglGetCurrentDC@0 wglGetLayerPaletteEntries@20 \
               wglGetProcAddress@4 wglMakeCurrent@8 wglRealizeLayerPalette@12 wglSetLayerPaletteEntries@20 \
               wglShareLists@8 wglSwapBuffers@4 wglSwapLayerBuffers@8 wglUseFontBitmapsA@16 wglUseFontBitmapsW@16 \
               wglUseFontOutlinesA@32 wglUseFontOutlinesW@32; do
          echo "    ${f%@*} = $f"
      done
      # NT 3.1: GDI32 has no pixel-format or SwapBuffers functions; export them
      # here (internal names wvb_*) so gl31patch can redirect a program's GDI32
      # imports to this DLL.
      echo "    ChoosePixelFormat = wvb_ChoosePixelFormat@8"
      echo "    DescribePixelFormat = wvb_DescribePixelFormat@16"
      echo "    GetPixelFormat = wvb_GetPixelFormat@4"
      echo "    SetPixelFormat = wvb_SetPixelFormat@12"
      echo "    SwapBuffers = wvb_SwapBuffers@4"; } > "$OBJ/nt3x/opengl32.def"
    $CC -std=gnu99 -Os -march=i386 -mno-sse -Wall -fno-stack-protector -fno-asynchronous-unwind-tables \
        -Wno-attributes -fno-builtin -I"$MESA/include" -I"$HERE" -shared -nostartfiles -nostdlib -Wl,-e,_WvboEntry@12 \
        -Wl,--enable-stdcall-fixup -Wl,--image-base,0x69e00000 -Wl,--subsystem,windows $SUBVER \
        -Wl,--disable-dynamicbase -Wl,--disable-nxcompat -Wl,--no-insert-timestamp \
        -o "$OBJ/nt3x/opengl32.dll" "$OBJ/nt3x/opengl32.def" "$HERE/ogl3x.c" "$S" -lgdi32 -lkernel32
    $STRIP --strip-unneeded "$OBJ/nt3x/opengl32.dll"
    $OBJDUMP -p "$OBJ/nt3x/opengl32.dll" > "$OBJ/nt3x/opengl32.pe.txt"
    if $OBJDUMP -d "$OBJ/nt3x/opengl32.dll" | grep -qE '\s(cmov|fcmov|fcomi|fucomi|movaps|xorps|pxor)'; then
        echo "opengl32.dll: instructions the 386/Pentium does not have" >&2; exit 1
    fi

    # WVGDI.DLL (NT 3.1 only): a GDI32 stand-in - every NT 3.1 GDI32 function
    # forwarded to GDI32, plus the five OpenGL functions forwarded to our
    # OPENGL32.DLL. wvgdi.def is generated from NT 3.1's GDI32 export list.
    # (subsystem 3.10 via $SUBVER, so NT 3.1's loader accepts it.)
    echo "LD wvgdi.dll (NT 3.1)"
    $CC -std=gnu99 -Os -march=i386 -mno-sse -Wall -fno-stack-protector -fno-asynchronous-unwind-tables \
        -fno-builtin -shared -nostartfiles -nostdlib -Wl,-e,_WvbGdiEntry@12 -Wl,--enable-stdcall-fixup \
        -Wl,--subsystem,windows $SUBVER -Wl,--disable-dynamicbase -Wl,--disable-nxcompat \
        -Wl,--no-insert-timestamp -o "$OBJ/wvgdi.dll" "$HERE/wvgdi.def" "$HERE/wvgdi.c" -lkernel32
    $STRIP --strip-unneeded "$OBJ/wvgdi.dll"

    # GL31PAT.EXE (an 8.3 name for NT 3.1 FAT volumes): points a program's
    # GDI32 OpenGL imports at WVGDI.DLL so it loads on NT 3.1. A GUI program
    # (reports through a message box) that imports only KERNEL32 and USER32.
    echo "LD gl31pat.exe (NT 3.1 import patcher)"
    $CC -std=gnu99 -Os -march=i586 -mno-sse -mno-mmx -Wall -fno-stack-protector \
        -fno-asynchronous-unwind-tables -fno-builtin -nostdlib -nostartfiles -Wl,-e,_start@0 -mwindows \
        $SUBVER -Wl,--disable-dynamicbase -Wl,--disable-nxcompat -Wl,--no-insert-timestamp \
        -o "$OBJ/gl31pat.exe" "$HERE/../tools/gl31patch.c" -luser32 -lkernel32 -lgcc
    $STRIP "$OBJ/gl31pat.exe"
fi

# --------------------------------------------------------------------------
# 5. ICDTEST.EXE (no C runtime; imports opengl32/gdi32/user32/kernel32)
# --------------------------------------------------------------------------
echo "CC icdtest.c"
if [ $X64 = 1 ]; then TARCH=""; TENTRY=start; else TARCH="-march=i586 -mno-sse -mno-mmx"; TENTRY=_start@0; fi
$CC -std=gnu99 -Os $TARCH -Wall -fno-stack-protector \
    -fno-asynchronous-unwind-tables -fno-builtin -I"$MESA/include" \
    -nostdlib -nostartfiles -Wl,-e,$TENTRY -mwindows \
    $SUBVER -Wl,--disable-dynamicbase -Wl,--disable-nxcompat \
    -o "$OBJ/icdtest.exe" "$HERE/icdtest.c" -lopengl32 -lgdi32 -luser32 -lkernel32 -lgcc
$STRIP "$OBJ/icdtest.exe"

ls -l "$OBJ"/*.dll "$OBJ/icdtest.exe"
