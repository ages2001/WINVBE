/*
 * ogl3x.c - OPENGL32.DLL for Windows NT 3.1, 3.5 and 3.51 (full OpenGL 2.1
 * for programs, on the WINVBE ICD).
 *
 * NT 3.5x's own OPENGL32.DLL is OpenGL 1.0 and NT 3.1 has none at all, so
 * programs built for OpenGL 1.1 or later do not start there. This one exports
 * the 336 functions of OpenGL 1.1 and the wgl* functions and sends them to
 * the ICD (WVBGL32.DLL -> Mesa 7.8.2) directly: copied next to such a program
 * it gives it OpenGL 1.1 statically and, through wglGetProcAddress, everything
 * else the ICD has - OpenGL 2.1 with GLSL 1.20 (exactly as on NT 4.0 / 2000 /
 * XP, where opengl32.dll also exports only 1.1 and the rest comes through
 * wglGetProcAddress).
 *
 *  - gl*: jumps through a table filled on the first call with the ICD's
 *    functions (DrvGetProcAddress); those find the current context
 *    themselves (per thread), so the table is the same for every context;
 *  - pixel formats and buffer swaps: on NT 3.5x GDI32 has
 *    SetPixelFormat/SwapBuffers and hands them to the display driver
 *    (nt/disp/glnt.c), so this DLL leaves them to GDI32. On NT 3.1 GDI32 has
 *    no such functions, so this DLL exports and implements
 *    ChoosePixelFormat/SetPixelFormat/DescribePixelFormat/GetPixelFormat and
 *    SwapBuffers itself, talking to the ICD (DrvDescribePixelFormat,
 *    DrvSetPixelFormat, DrvSwapBuffers); the ICD renders into a plain buffer
 *    and shows it with SetDIBitsToDevice (wvbdrv.c). A small tool points a
 *    program's GDI32 imports of those five at this DLL (GL31PAT.EXE).
 *  - wglUseFontBitmaps builds the display lists with GetGlyphOutline;
 *    wglUseFontOutlines is not offered.
 *
 * The gl* stubs come from gl/build.sh (icdtable.h -> obj/ogl3x_stubs.s).
 * No C runtime; imports KERNEL32 and GDI32 only (and only functions that NT
 * 3.1 already has: SwapBuffers and DisableThreadLibraryCalls are resolved at
 * run time).
 */

#include <windows.h>
#include <GL/gl.h>

#define NENT 336

/* the stubs (ogl3x_stubs.s) jump through this */
FARPROC wvbo_tab[NENT];
volatile LONG wvbo_ready;

static const char *const wvbo_names[NENT] = {
#define ICD_ENTRY(name) "gl" #name,
#include "icdtable.h"
#undef ICD_ENTRY
};

typedef DWORD_PTR DHGLRC;
static HMODULE icd;
static DHGLRC (APIENTRY *pDrvCreateContext)(HDC);
static BOOL (APIENTRY *pDrvDeleteContext)(DHGLRC);
static PVOID (APIENTRY *pDrvSetContext)(HDC, DHGLRC, PVOID);
static BOOL (APIENTRY *pDrvReleaseContext)(DHGLRC);
static BOOL (APIENTRY *pDrvShareLists)(DHGLRC, DHGLRC);
static BOOL (APIENTRY *pDrvCopyContext)(DHGLRC, DHGLRC, UINT);
static PROC (APIENTRY *pDrvGetProcAddress)(LPCSTR);
static BOOL (APIENTRY *pDrvValidateVersion)(ULONG);
/* pixel formats and swap, used on NT 3.1 (see the header) */
static LONG (APIENTRY *pDrvDescribePixelFormat)(HDC, LONG, ULONG, PIXELFORMATDESCRIPTOR *);
static BOOL (APIENTRY *pDrvSetPixelFormat)(HDC, LONG);
static BOOL (APIENTRY *pDrvSwapBuffers)(HDC);

/* GDI32's SwapBuffers exists on NT 3.5x but not NT 3.1: resolved at run time.
 * nt31_mode is set when GDI32 has no SetPixelFormat (so no OpenGL plumbing);
 * then this DLL does the pixel-format and swap work itself. */
static BOOL (WINAPI *pGdiSwapBuffers)(HDC);
static int nt31_mode;

static CRITICAL_SECTION lk;
static DWORD tls_rc = 0xFFFFFFFF, tls_dc = 0xFFFFFFFF;

/* NT 3.1: remember the pixel format set on each DC (GetPixelFormat) */
#define PF_SLOTS 16
static struct { HDC dc; int pf; } pf_tab[PF_SLOTS];
static int pf_last;

static void pf_remember(HDC dc, int pf)
{
    int i, f = -1;
    EnterCriticalSection(&lk);
    for (i = 0; i < PF_SLOTS; i++) {
        if (pf_tab[i].dc == dc) { f = i; break; }
        if (f < 0 && !pf_tab[i].dc) f = i;
    }
    if (f < 0) f = 0;
    pf_tab[f].dc = dc;
    pf_tab[f].pf = pf;
    pf_last = pf;
    LeaveCriticalSection(&lk);
}

static int pf_lookup(HDC dc)
{
    int i, pf = 0;
    EnterCriticalSection(&lk);
    for (i = 0; i < PF_SLOTS; i++)
        if (pf_tab[i].dc == dc) { pf = pf_tab[i].pf; break; }
    if (!pf) pf = pf_last;
    LeaveCriticalSection(&lk);
    return pf;
}

/* a gl* function the ICD does not have: nothing (cdecl, so that any
   arguments stay balanced - only reached if the ICD is missing) */
static void wvbo_none(void)
{
}

/* the first call of any function: load the ICD, fill the table */
void wvbo_init(void)
{
    int i;
    EnterCriticalSection(&lk);
    if (!wvbo_ready) {
        icd = LoadLibraryA("WVBGL32.DLL");
        if (icd) {
            pDrvCreateContext = (void *) GetProcAddress(icd, "DrvCreateContext");
            pDrvDeleteContext = (void *) GetProcAddress(icd, "DrvDeleteContext");
            pDrvSetContext = (void *) GetProcAddress(icd, "DrvSetContext");
            pDrvReleaseContext = (void *) GetProcAddress(icd, "DrvReleaseContext");
            pDrvShareLists = (void *) GetProcAddress(icd, "DrvShareLists");
            pDrvCopyContext = (void *) GetProcAddress(icd, "DrvCopyContext");
            pDrvGetProcAddress = (void *) GetProcAddress(icd, "DrvGetProcAddress");
            pDrvValidateVersion = (void *) GetProcAddress(icd, "DrvValidateVersion");
            pDrvDescribePixelFormat = (void *) GetProcAddress(icd, "DrvDescribePixelFormat");
            pDrvSetPixelFormat = (void *) GetProcAddress(icd, "DrvSetPixelFormat");
            pDrvSwapBuffers = (void *) GetProcAddress(icd, "DrvSwapBuffers");
            if (pDrvValidateVersion)
                pDrvValidateVersion(1);
        }
        {
            /* NT 3.5x: GDI32 has SetPixelFormat/SwapBuffers (OpenGL plumbing to
               the display driver). NT 3.1: it has neither, so this DLL does
               that work itself. */
            HMODULE g = GetModuleHandleA("GDI32.DLL");
            pGdiSwapBuffers = g ? (BOOL (WINAPI *)(HDC)) GetProcAddress(g, "SwapBuffers") : NULL;
            nt31_mode = !(g && GetProcAddress(g, "SetPixelFormat"));
        }
        for (i = 0; i < NENT; i++) {
            FARPROC p = pDrvGetProcAddress ? (FARPROC) pDrvGetProcAddress(wvbo_names[i]) : NULL;
            wvbo_tab[i] = p ? p : (FARPROC) wvbo_none;
        }
        InterlockedExchange((LONG *) &wvbo_ready, 1);
    }
    LeaveCriticalSection(&lk);
}

#define INIT() do { if (!wvbo_ready) wvbo_init(); } while (0)
#define GLF(i, type) ((type) wvbo_tab[i])

/* ---- contexts --------------------------------------------------------- */

HGLRC WINAPI wglCreateContext(HDC hdc)
{
    INIT();
    if (!pDrvCreateContext) { SetLastError(ERROR_PROC_NOT_FOUND); return NULL; }
    return (HGLRC) pDrvCreateContext(hdc);
}

HGLRC WINAPI wglCreateLayerContext(HDC hdc, int layer)
{
    if (layer != 0) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    return wglCreateContext(hdc);
}

HGLRC WINAPI wglGetCurrentContext(void)
{
    return (HGLRC) TlsGetValue(tls_rc);
}

HDC WINAPI wglGetCurrentDC(void)
{
    return (HDC) TlsGetValue(tls_dc);
}

BOOL WINAPI wglMakeCurrent(HDC hdc, HGLRC rc)
{
    HGLRC cur = (HGLRC) TlsGetValue(tls_rc);
    INIT();
    if (!pDrvSetContext) return FALSE;
    if (!rc) {
        if (cur && pDrvReleaseContext) pDrvReleaseContext((DHGLRC) cur);
        TlsSetValue(tls_rc, NULL);
        TlsSetValue(tls_dc, NULL);
        return TRUE;
    }
    if (!pDrvSetContext(hdc, (DHGLRC) rc, NULL)) return FALSE;
    TlsSetValue(tls_rc, rc);
    TlsSetValue(tls_dc, hdc);
    return TRUE;
}

BOOL WINAPI wglDeleteContext(HGLRC rc)
{
    INIT();
    if (!rc || !pDrvDeleteContext) return FALSE;
    if ((HGLRC) TlsGetValue(tls_rc) == rc) wglMakeCurrent(NULL, NULL);
    return pDrvDeleteContext((DHGLRC) rc);
}

BOOL WINAPI wglShareLists(HGLRC a, HGLRC b)
{
    INIT();
    return pDrvShareLists ? pDrvShareLists((DHGLRC) a, (DHGLRC) b) : FALSE;
}

BOOL WINAPI wglCopyContext(HGLRC a, HGLRC b, UINT mask)
{
    INIT();
    return pDrvCopyContext ? pDrvCopyContext((DHGLRC) a, (DHGLRC) b, mask) : FALSE;
}

PROC WINAPI wglGetProcAddress(LPCSTR name)
{
    INIT();
    return pDrvGetProcAddress && name ? pDrvGetProcAddress(name) : NULL;
}

/* ---- buffers and layers (GDI32 shows the frame through the display driver) */

BOOL WINAPI wglSwapBuffers(HDC hdc)
{
    INIT();
    if (pGdiSwapBuffers)                     /* NT 3.5x: through the display driver */
        return pGdiSwapBuffers(hdc);
    return pDrvSwapBuffers ? pDrvSwapBuffers(hdc) : FALSE;  /* NT 3.1: the ICD */
}

BOOL WINAPI wglSwapLayerBuffers(HDC hdc, UINT planes)
{
    if (planes & WGL_SWAP_MAIN_PLANE) return wglSwapBuffers(hdc);
    return TRUE;
}

/* ---- pixel formats and swap for NT 3.1 ---------------------------------
 * GDI32 on NT 3.1 has no ChoosePixelFormat/SetPixelFormat/DescribePixelFormat/
 * GetPixelFormat/SwapBuffers. These are exported under those names (internal
 * names wvb_*, aliased in the .def) so the GL31PAT.EXE tool can point a
 * program's GDI32 imports of them here. They forward to the ICD. On NT 3.5x
 * GDI32 keeps its own, so a program there is not patched and never calls
 * these. */

int WINAPI wvb_DescribePixelFormat(HDC hdc, int pf, UINT n, LPPIXELFORMATDESCRIPTOR pfd)
{
    INIT();
    return pDrvDescribePixelFormat ? (int) pDrvDescribePixelFormat(hdc, pf, (ULONG) n, pfd) : 0;
}

int WINAPI wvb_GetPixelFormat(HDC hdc)
{
    INIT();
    return pf_lookup(hdc);
}

BOOL WINAPI wvb_SetPixelFormat(HDC hdc, int pf, CONST PIXELFORMATDESCRIPTOR *pfd)
{
    (void) pfd;
    INIT();
    if (!pDrvSetPixelFormat || !pDrvSetPixelFormat(hdc, pf))
        return FALSE;
    pf_remember(hdc, pf);
    return TRUE;
}

/* score a candidate format against what the program asked for */
static long pf_score(const PIXELFORMATDESCRIPTOR *w, const PIXELFORMATDESCRIPTOR *c)
{
    long s = 0;
    if ((w->dwFlags & PFD_DOUBLEBUFFER) && (c->dwFlags & PFD_DOUBLEBUFFER)) s += 1000;
    else if (!(w->dwFlags & PFD_DOUBLEBUFFER) && !(c->dwFlags & PFD_DOUBLEBUFFER)) s += 800;
    if (c->cColorBits >= w->cColorBits) s += 100;
    else s -= (long) (w->cColorBits - c->cColorBits);
    if (w->cDepthBits) { if (c->cDepthBits >= w->cDepthBits) s += 100; else s -= 60; }
    else s += c->cDepthBits ? 5 : 0;
    if (w->cAlphaBits && c->cAlphaBits >= w->cAlphaBits) s += 50;
    if (w->cStencilBits && c->cStencilBits >= w->cStencilBits) s += 20;
    if (w->cAccumBits && c->cAccumBits >= w->cAccumBits) s += 10;
    return s;
}

int WINAPI wvb_ChoosePixelFormat(HDC hdc, CONST PIXELFORMATDESCRIPTOR *want)
{
    PIXELFORMATDESCRIPTOR d;
    int i, n, best = 0;
    long bestscore = -0x7fffffffL;
    INIT();
    if (!pDrvDescribePixelFormat || !want)
        return 0;
    n = (int) pDrvDescribePixelFormat(hdc, 1, sizeof(d), NULL);
    for (i = 1; i <= n; i++) {
        long sc;
        if (!pDrvDescribePixelFormat(hdc, i, sizeof(d), &d))
            continue;
        sc = pf_score(want, &d);
        if (sc > bestscore) { bestscore = sc; best = i; }
    }
    return best;
}

BOOL WINAPI wvb_SwapBuffers(HDC hdc)
{
    return wglSwapBuffers(hdc);
}

BOOL WINAPI wglDescribeLayerPlane(HDC hdc, int pf, int layer, UINT n, LPLAYERPLANEDESCRIPTOR p)
{
    (void) hdc; (void) pf; (void) layer; (void) n; (void) p;
    return FALSE;
}

int WINAPI wglSetLayerPaletteEntries(HDC hdc, int layer, int start, int n, CONST COLORREF *c)
{
    (void) hdc; (void) layer; (void) start; (void) n; (void) c;
    return 0;
}

int WINAPI wglGetLayerPaletteEntries(HDC hdc, int layer, int start, int n, COLORREF *c)
{
    (void) hdc; (void) layer; (void) start; (void) n; (void) c;
    return 0;
}

BOOL WINAPI wglRealizeLayerPalette(HDC hdc, int layer, BOOL on)
{
    (void) hdc; (void) layer; (void) on;
    return FALSE;
}

/* ---- fonts ---------------------------------------------------------------- */

/* glNewList, glEndList, glBitmap, glPixelStorei, glGetIntegerv by index */
#define I_NEWLIST    0
#define I_ENDLIST    1
#define I_BITMAP     8
#define I_PIXELSTOREI 250
#define I_GETINTEGERV 263

static BOOL use_font_bitmaps(HDC hdc, DWORD first, DWORD count, DWORD base, BOOL wide)
{
    static const MAT2 m = { { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };
    GLint align = 4;
    DWORD i;
    BOOL ok = TRUE;
    INIT();
    if (!wglGetCurrentContext()) return FALSE;
    GLF(I_GETINTEGERV, void (APIENTRY *)(GLenum, GLint *))(GL_UNPACK_ALIGNMENT, &align);
    GLF(I_PIXELSTOREI, void (APIENTRY *)(GLenum, GLint))(GL_UNPACK_ALIGNMENT, 4);
    for (i = 0; i < count; i++) {
        GLYPHMETRICS gm;
        DWORD sz, pitch, r;
        BYTE *bits = NULL, *flip = NULL;
        UINT c = first + i;
        sz = wide ? GetGlyphOutlineW(hdc, c, GGO_BITMAP, &gm, 0, NULL, &m)
                  : GetGlyphOutlineA(hdc, c, GGO_BITMAP, &gm, 0, NULL, &m);
        if (sz == GDI_ERROR) { ok = FALSE; continue; }
        if (sz) {
            bits = (BYTE *) LocalAlloc(LMEM_FIXED, sz * 2);
            if (!bits) { ok = FALSE; continue; }
            flip = bits + sz;
            if ((wide ? GetGlyphOutlineW(hdc, c, GGO_BITMAP, &gm, sz, bits, &m)
                      : GetGlyphOutlineA(hdc, c, GGO_BITMAP, &gm, sz, bits, &m)) == GDI_ERROR) {
                LocalFree(bits);
                ok = FALSE;
                continue;
            }
            /* GDI: top row first, DWORD-aligned rows; glBitmap: bottom row first */
            pitch = ((gm.gmBlackBoxX + 31) / 32) * 4;
            for (r = 0; r < gm.gmBlackBoxY && (r + 1) * pitch <= sz; r++) {
                BYTE *d = flip + r * pitch, *s = bits + (gm.gmBlackBoxY - 1 - r) * pitch;
                DWORD k;
                for (k = 0; k < pitch; k++) d[k] = s[k];
            }
        }
        GLF(I_NEWLIST, void (APIENTRY *)(GLuint, GLenum))(base + i, GL_COMPILE);
        GLF(I_BITMAP, void (APIENTRY *)(GLsizei, GLsizei, GLfloat, GLfloat, GLfloat, GLfloat, const GLubyte *))(
            sz ? (GLsizei) gm.gmBlackBoxX : 0, sz ? (GLsizei) gm.gmBlackBoxY : 0,
            (GLfloat) -gm.gmptGlyphOrigin.x, (GLfloat) ((LONG) gm.gmBlackBoxY - gm.gmptGlyphOrigin.y),
            (GLfloat) gm.gmCellIncX, (GLfloat) gm.gmCellIncY, flip);
        GLF(I_ENDLIST, void (APIENTRY *)(void))();
        if (bits) LocalFree(bits);
    }
    GLF(I_PIXELSTOREI, void (APIENTRY *)(GLenum, GLint))(GL_UNPACK_ALIGNMENT, align);
    return ok;
}

BOOL WINAPI wglUseFontBitmapsA(HDC hdc, DWORD first, DWORD count, DWORD base)
{
    return use_font_bitmaps(hdc, first, count, base, FALSE);
}

BOOL WINAPI wglUseFontBitmapsW(HDC hdc, DWORD first, DWORD count, DWORD base)
{
    return use_font_bitmaps(hdc, first, count, base, TRUE);
}

BOOL WINAPI wglUseFontOutlinesA(HDC hdc, DWORD first, DWORD count, DWORD base, FLOAT dev, FLOAT ext, int fmt,
                                LPGLYPHMETRICSFLOAT gmf)
{
    (void) hdc; (void) first; (void) count; (void) base; (void) dev; (void) ext; (void) fmt; (void) gmf;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI wglUseFontOutlinesW(HDC hdc, DWORD first, DWORD count, DWORD base, FLOAT dev, FLOAT ext, int fmt,
                                LPGLYPHMETRICSFLOAT gmf)
{
    return wglUseFontOutlinesA(hdc, first, count, base, dev, ext, fmt, gmf);
}

BOOL WINAPI WvboEntry(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void) reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        /* DisableThreadLibraryCalls is not on NT 3.1's KERNEL32 (optimisation
           only): resolve it at run time so the DLL loads there too. */
        HMODULE k = GetModuleHandleA("KERNEL32.DLL");
        BOOL (WINAPI *dtlc)(HMODULE) = k ? (BOOL (WINAPI *)(HMODULE))
            GetProcAddress(k, "DisableThreadLibraryCalls") : NULL;
        if (dtlc)
            dtlc(h);
        InitializeCriticalSection(&lk);
        tls_rc = TlsAlloc();
        tls_dc = TlsAlloc();
        if (tls_rc == 0xFFFFFFFF || tls_dc == 0xFFFFFFFF) return FALSE;
    }
    return TRUE;
}
