/*
 * icd.c - WVBGL32.DLL OpenGL Installable Client Driver (ICD) entry layer.
 *
 * Windows' own opengl32.dll loads this DLL as the ICD of the display driver
 * and calls the Drv* functions below; the GL calls themselves go through the
 * GLCLTPROCTABLE returned by DrvSetContext (OpenGL 1.1 entry points) and
 * through DrvGetProcAddress (everything newer).
 *
 * Modelled on Mesa 6.x src/mesa/drivers/windows/icd/icd.c (Gregor Anich),
 * on top of the WINVBE GDI driver in wvbdrv.c.  The wgl* functions of
 * wmesa/wgl.c are not used or exported: opengl32.dll provides those.
 *
 * The Mesa-derived code is under the MIT license, see MESA-LICENSE.TXT.
 */

#include <windows.h>
#include "wvbdrv.h"
#include "main/glheader.h"
#include "glapi/glapi.h"

/* opengl32 <-> ICD interface types (Windows DDK "icd.h"). */
#define OPENGL_VERSION_110_ENTRIES 336

typedef struct _GLCLTPROCTABLE {
    int  cEntries;
    PROC glDispatchTable[OPENGL_VERSION_110_ENTRIES];
} GLCLTPROCTABLE, *PGLCLTPROCTABLE;

typedef VOID (APIENTRY *PFN_SETPROCTABLE)(PGLCLTPROCTABLE);
typedef DWORD_PTR DHGLRC;

/* The OpenGL 1.1 dispatch table in opengl32 order: Mesa's gl* dispatch
 * functions (glapi_dispatch.c), which call through the current context's
 * dispatch table (global, or a TlsAlloc() slot once several threads are
 * in use). */
static GLCLTPROCTABLE wvb_cltproctable = {
    OPENGL_VERSION_110_ENTRIES,
    {
#define ICD_ENTRY(name) (PROC) gl##name,
#include "icdtable.h"
#undef ICD_ENTRY
    }
};

/* NT 3.5x: OPENGL32.DLL is OpenGL 1.0 (ICD interface 1) and keeps the first
 * 306 entries; glEnd draws at once (wvb_flush_vertices). */
#define OPENGL_VERSION_100_ENTRIES 306
static GLCLTPROCTABLE wvb_cltproctable_nt3;

static void (APIENTRY *wvb_real_glEnd)(void);

static void APIENTRY wvb_glEnd_nt3(void)
{
    wvb_real_glEnd();
    wvb_flush_vertices();
}

/* Callbacks opengl32 may hand us (Windows 2000 and later); not needed. */
static PROC wvb_callbacks[16];
static int wvb_ncallbacks;

/* ------------------------------------------------------------------------
 * WGL extensions reachable through wglGetProcAddress
 * ------------------------------------------------------------------------ */

static const char wvb_wgl_extensions[] = "WGL_ARB_extensions_string WGL_EXT_extensions_string";

static const char *WINAPI wvb_wglGetExtensionsStringARB(HDC hdc)
{
    (void) hdc;
    return wvb_wgl_extensions;
}

static const char *WINAPI wvb_wglGetExtensionsStringEXT(void)
{
    return wvb_wgl_extensions;
}

/* ------------------------------------------------------------------------
 * ICD entry points (stdcall, exported undecorated through wvbgl32.def)
 * ------------------------------------------------------------------------ */

BOOL APIENTRY DrvValidateVersion(ULONG ulVersion)
{
    (void) ulVersion;
    return TRUE;
}

void APIENTRY DrvSetCallbackProcs(INT nProcs, PROC *pProcs)
{
    int i;
    if (nProcs < 0 || !pProcs)
        return;
    wvb_ncallbacks = nProcs < 16 ? nProcs : 16;
    for (i = 0; i < wvb_ncallbacks; i++)
        wvb_callbacks[i] = pProcs[i];
}

LONG APIENTRY DrvDescribePixelFormat(HDC hdc, LONG iPixelFormat, ULONG cjpfd,
                                     PIXELFORMATDESCRIPTOR *ppfd)
{
    PIXELFORMATDESCRIPTOR pfd;
    int count = wvb_pixelformat_count();

    if (!ppfd)
        return count;
    if (iPixelFormat < 1 || iPixelFormat > count || cjpfd < sizeof(WORD) * 2) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    wvb_describe_pixelformat(hdc, (int) iPixelFormat, &pfd);
    memcpy(ppfd, &pfd, cjpfd < sizeof(pfd) ? cjpfd : sizeof(pfd));
    return count;
}

BOOL APIENTRY DrvSetPixelFormat(HDC hdc, LONG iPixelFormat)
{
    if (!wvb_set_pixelformat(hdc, (int) iPixelFormat)) {
        SetLastError(ERROR_INVALID_PIXEL_FORMAT);
        return FALSE;
    }
    return TRUE;
}

DHGLRC APIENTRY DrvCreateContext(HDC hdc)
{
    return (DHGLRC) wvb_create_context(hdc);
}

DHGLRC APIENTRY DrvCreateLayerContext(HDC hdc, int iLayerPlane)
{
    if (iLayerPlane != 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return DrvCreateContext(hdc);
}

BOOL APIENTRY DrvDeleteContext(DHGLRC dhglrc)
{
    if (!dhglrc)
        return FALSE;
    return wvb_destroy_context((struct wvb_context *) dhglrc);
}

BOOL APIENTRY DrvCopyContext(DHGLRC dhrcSource, DHGLRC dhrcDest, UINT fuMask)
{
    return wvb_copy_context((struct wvb_context *) dhrcSource,
                            (struct wvb_context *) dhrcDest, fuMask);
}

BOOL APIENTRY DrvShareLists(DHGLRC dhglrc1, DHGLRC dhglrc2)
{
    return wvb_share_lists((struct wvb_context *) dhglrc1,
                           (struct wvb_context *) dhglrc2);
}

PGLCLTPROCTABLE APIENTRY DrvSetContext(HDC hdc, DHGLRC dhglrc, PFN_SETPROCTABLE pfnSetProcTable)
{
    (void) pfnSetProcTable;
    if (!hdc || !dhglrc) {
        wvb_release_current(NULL);
        return NULL;
    }
    if (!wvb_make_current((struct wvb_context *) dhglrc, hdc))
        return NULL;
    if (wvb_nt3()) {
        if (!wvb_cltproctable_nt3.cEntries) {
            int i;
            for (i = 0; i < OPENGL_VERSION_100_ENTRIES; i++)
                wvb_cltproctable_nt3.glDispatchTable[i] = wvb_cltproctable.glDispatchTable[i];
            wvb_real_glEnd = (void (APIENTRY *)(void)) wvb_cltproctable.glDispatchTable[43];  /* glEnd */
            wvb_cltproctable_nt3.glDispatchTable[43] = (PROC) wvb_glEnd_nt3;
            wvb_cltproctable_nt3.cEntries = OPENGL_VERSION_100_ENTRIES;
        }
        return &wvb_cltproctable_nt3;
    }
    return &wvb_cltproctable;
}

BOOL APIENTRY DrvReleaseContext(DHGLRC dhglrc)
{
    return wvb_release_current((struct wvb_context *) dhglrc);
}

BOOL APIENTRY DrvSwapBuffers(HDC hdc)
{
    return wvb_swap_buffers(hdc);
}

BOOL APIENTRY DrvSwapLayerBuffers(HDC hdc, UINT fuPlanes)
{
    if (fuPlanes & WGL_SWAP_MAIN_PLANE)
        return wvb_swap_buffers(hdc);
    return TRUE;
}

PROC APIENTRY DrvGetProcAddress(LPCSTR lpszProc)
{
    PROC p = NULL;

    if (!lpszProc)
        return NULL;
    if (!strcmp(lpszProc, "wglGetExtensionsStringARB"))
        return (PROC) wvb_wglGetExtensionsStringARB;
    if (!strcmp(lpszProc, "wglGetExtensionsStringEXT"))
        return (PROC) wvb_wglGetExtensionsStringEXT;
    if (lpszProc[0] == 'g' && lpszProc[1] == 'l')
        p = (PROC) _glapi_get_proc_address(lpszProc);
    /* NT 3.x (WINVBE's OPENGL32.DLL): glEnd draws at once, see above */
    if (p && wvb_nt3() && !strcmp(lpszProc, "glEnd")) {
        wvb_real_glEnd = (void (APIENTRY *)(void)) p;
        p = (PROC) wvb_glEnd_nt3;
    }
    if (!p)
        SetLastError(ERROR_PROC_NOT_FOUND);
    return p;
}

BOOL APIENTRY DrvDescribeLayerPlane(HDC hdc, int iPixelFormat, int iLayerPlane,
                                    UINT nBytes, LPLAYERPLANEDESCRIPTOR plpd)
{
    (void) hdc; (void) iPixelFormat; (void) iLayerPlane; (void) nBytes; (void) plpd;
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

int APIENTRY DrvSetLayerPaletteEntries(HDC hdc, int iLayerPlane, int iStart,
                                       int cEntries, CONST COLORREF *pcr)
{
    (void) hdc; (void) iLayerPlane; (void) iStart; (void) cEntries; (void) pcr;
    return 0;
}

int APIENTRY DrvGetLayerPaletteEntries(HDC hdc, int iLayerPlane, int iStart,
                                       int cEntries, COLORREF *pcr)
{
    (void) hdc; (void) iLayerPlane; (void) iStart; (void) cEntries; (void) pcr;
    return 0;
}

BOOL APIENTRY DrvRealizeLayerPalette(HDC hdc, int iLayerPlane, BOOL bRealize)
{
    (void) hdc; (void) iLayerPlane; (void) bRealize;
    return FALSE;
}

/* ------------------------------------------------------------------------
 * DLL entry point (linked with -nostartfiles: no CRT startup code)
 * ------------------------------------------------------------------------ */

BOOL WINAPI WvbDllEntry(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        /* not on NT 3.1's KERNEL32: resolve at run time (an optimisation only) */
        HMODULE k = GetModuleHandleA("KERNEL32.DLL");
        BOOL (WINAPI *dtlc)(HMODULE) = k ? (BOOL (WINAPI *)(HMODULE))
            GetProcAddress(k, "DisableThreadLibraryCalls") : NULL;
        if (dtlc)
            dtlc(hinst);
        wvb_process_attach();
        break;
    }
    case DLL_PROCESS_DETACH:
        /* At process exit (reserved != NULL) other threads are gone and
         * the process is torn down anyway: leave everything alone. */
        if (!reserved)
            wvb_process_detach();
        break;
    }
    return TRUE;
}
