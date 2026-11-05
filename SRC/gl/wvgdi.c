/*
 * wvgdi.c - WVGDI.DLL, a GDI32 stand-in for OpenGL programs on Windows NT 3.1.
 *
 * Windows NT 3.1's GDI32.DLL has none of the OpenGL pixel-format or buffer
 * functions (ChoosePixelFormat, SetPixelFormat, DescribePixelFormat,
 * GetPixelFormat, SwapBuffers) - they arrived in NT 3.5. An OpenGL program
 * imports them from GDI32 and therefore fails to load on NT 3.1.
 *
 * WVGDI.DLL exports every function NT 3.1's GDI32 has, each as a forwarder to
 * GDI32 (so it behaves exactly like GDI32), plus the five OpenGL functions,
 * forwarded to WINVBE's OPENGL32.DLL (gl/ogl3x.c), which implements them. The
 * GL31PAT.EXE tool renames a program's "GDI32.DLL" import to "WVGDI.DLL" (both
 * nine characters, changed in place), so the five OpenGL imports resolve from
 * OPENGL32 and every other GDI call still reaches GDI32 unchanged. The whole
 * export table is forwarders (wvgdi.def); this file only supplies the DLL
 * entry point. No C runtime.
 *
 * NT 3.5x and later already have these functions in GDI32, so no patching and
 * no WVGDI.DLL are needed there.
 *
 * Build:  i686-w64-mingw32-gcc -shared -nostartfiles -nostdlib \
 *             -Wl,-e,_WvbGdiEntry@12 -o wvgdi.dll wvgdi.def wvgdi.c -lkernel32
 */

#include <windows.h>

BOOL WINAPI WvbGdiEntry(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void) h; (void) reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        /* DisableThreadLibraryCalls is not on NT 3.1's KERNEL32; resolve it at
           run time (a forwarder DLL has no per-thread work anyway). */
        HMODULE k = GetModuleHandleA("KERNEL32.DLL");
        BOOL (WINAPI *dtlc)(HMODULE) = k ? (BOOL (WINAPI *)(HMODULE))
            GetProcAddress(k, "DisableThreadLibraryCalls") : NULL;
        if (dtlc)
            dtlc(h);
    }
    return TRUE;
}
