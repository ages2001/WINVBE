/*
 * WVBDD32.DLL - the 32-bit part of WINVBE's DirectDraw HAL for Windows
 * 95/98/ME.
 *
 * DirectDraw loads it by the name the display driver gives (ddhal.c) and
 * calls DriverInit with the address of the block the two share
 * (drv/wvbdd.h). The callbacks are those of the Windows NT driver -
 * ddblt.c and ddovl.c are included from nt/disp - running on the CPU:
 *   - blits between video memory surfaces: copies, stretches, colour fills,
 *     source colour keys,
 *   - YUV surfaces (YUY2, UYVY, YV12) and blits from them with colour
 *     conversion, which DirectDraw's emulation cannot do,
 *   - one emulated overlay with a destination colour key.
 * DirectDraw calls them in the processes that use it, so what they keep
 * between calls is in a shared section and a shared heap.
 */
#include <windows.h>
#include <ddraw.h>
#include <ddrawi.h>
#include <stddef.h>
#include "../drv/wvbdd.h"

/* callback data missing from the mingw-w64 headers (DDK ddrawi.h) */
typedef struct {
    LPDDRAWI_DIRECTDRAW_GBL lpDD;
    DWORD dwFlags;
    DWORD bIsInVB;
    ULONG_PTR hEvent;
    HRESULT ddRVal;
    LPVOID WaitForVerticalBlank;
} WAITVBDATA;

typedef struct {
    LPDDRAWI_DIRECTDRAW_GBL lpDD;
    LPDDRAWI_DDRAWSURFACE_LCL lpDDSurface;
    DWORD dwFlags;
    HRESULT ddRVal;
    LPVOID fn;
} STATUSDATA;

struct _DDHAL_SETOVERLAYPOSITIONDATA {
    LPDDRAWI_DIRECTDRAW_GBL lpDD;
    LPDDRAWI_DDRAWSURFACE_LCL lpDDSrcSurface;
    LPDDRAWI_DDRAWSURFACE_LCL lpDDDestSurface;
    LONG lXPos;
    LONG lYPos;
    HRESULT ddRVal;
    LPVOID SetOverlayPosition;
};

struct _DDHAL_FLIPTOGDISURFACEDATA {
    LPDDRAWI_DIRECTDRAW_GBL lpDD;
    DWORD dwToGDI;
    DWORD dwReserved;
    HRESULT ddRVal;
    LPVOID FlipToGDISurface;
};

#define DDWAITVB_I_TESTVB 0x80000006UL
#ifndef DDHAL_PLEASEALLOC_BLOCKSIZE
#define DDHAL_PLEASEALLOC_BLOCKSIZE 0x00000002UL
#endif

/* the layout of drv/wvbdd.h is that of DirectDraw's structures */
#define SAME(a, b, f) _Static_assert(offsetof(a, f) == offsetof(b, f), #f)
_Static_assert(sizeof(WD_HALINFO) == sizeof(DDHALINFO), "DDHALINFO");
_Static_assert(sizeof(WD_CORECAPS) == sizeof(DDCORECAPS), "DDCORECAPS");
_Static_assert(sizeof(WD_VIDMEMINFO) == sizeof(VIDMEMINFO), "VIDMEMINFO");
_Static_assert(sizeof(WD_VIDMEM) == sizeof(VIDMEM), "VIDMEM");
_Static_assert(sizeof(WD_DDCB) == sizeof(DDHAL_DDCALLBACKS), "DDHAL_DDCALLBACKS");
_Static_assert(sizeof(WD_SURFCB) == sizeof(DDHAL_DDSURFACECALLBACKS), "DDHAL_DDSURFACECALLBACKS");
_Static_assert(sizeof(WD_PALCB) == sizeof(DDHAL_DDPALETTECALLBACKS), "DDHAL_DDPALETTECALLBACKS");
_Static_assert(sizeof(WD_MODE) == sizeof(DDHALMODEINFO), "DDHALMODEINFO");
_Static_assert(sizeof(WD_EXEBUFCB) == sizeof(DDHAL_DDEXEBUFCALLBACKS), "DDHAL_DDEXEBUFCALLBACKS");
_Static_assert(sizeof(WD_PIXFMT) == sizeof(DDPIXELFORMAT), "DDPIXELFORMAT");
SAME(WD_HALINFO, DDHALINFO, vmiData);
SAME(WD_HALINFO, DDHALINFO, ddCaps);
SAME(WD_HALINFO, DDHALINFO, dwModeIndex);
SAME(WD_HALINFO, DDHALINFO, lpModeInfo);
SAME(WD_HALINFO, DDHALINFO, hInstance);
SAME(WD_HALINFO, DDHALINFO, lpDDExeBufCallbacks);
SAME(WD_CORECAPS, DDCORECAPS, dwRops);
SAME(WD_CORECAPS, DDCORECAPS, ddsCaps);
SAME(WD_CORECAPS, DDCORECAPS, dwSVBCaps);
SAME(WD_CORECAPS, DDCORECAPS, dwSVBCaps2);
SAME(WD_VIDMEMINFO, VIDMEMINFO, ddpfDisplay);
SAME(WD_VIDMEMINFO, VIDMEMINFO, pvmList);
SAME(WD_DDCB, DDHAL_DDCALLBACKS, CanCreateSurface);
SAME(WD_SURFCB, DDHAL_DDSURFACECALLBACKS, Blt);
SAME(WD_SURFCB, DDHAL_DDSURFACECALLBACKS, SetOverlayPosition);
SAME(WD_MODE, DDHALMODEINFO, dwRBitMask);

/* ------------------------------------------------------------------ */
/* what ddblt.c and ddovl.c expect                                     */

#define DD_SHARED __attribute__((section(".shr"), shared))

typedef LPDDRAWI_DDRAWSURFACE_LCL PDD_SURFACE_LOCAL;
typedef LPDDRAWI_DDRAWSURFACE_GBL PDD_SURFACE_GLOBAL;
typedef LPDDHAL_BLTDATA PDD_BLTDATA;
typedef LPDDHAL_CREATESURFACEDATA PDD_CREATESURFACEDATA;
typedef LPDDHAL_UPDATEOVERLAYDATA PDD_UPDATEOVERLAYDATA;
typedef LPDDHAL_SETOVERLAYPOSITIONDATA PDD_SETOVERLAYPOSITIONDATA;
typedef LPDDHAL_UNLOCKDATA PDD_UNLOCKDATA;
typedef LPDDHAL_DESTROYSURFACEDATA PDD_DESTROYSURFACEDATA;
typedef LPDDHAL_FLIPDATA PDD_FLIPDATA;

typedef struct {
    ULONG bpp, Bpp;
    ULONG rmask, gmask, bmask;
    BYTE *fb;                           /* the video memory mapped for DirectDraw */
    ULONG vram;
    LONG  stride;
    ULONG w, h;
    int   shadowed, ddlocked;
    RECTL ddlock;
} PDEV;

static WVBDD *g_dd DD_SHARED = NULL;
static PDEV g_pd DD_SHARED = { 0 };
static HANDLE g_heap DD_SHARED = NULL;
static HINSTANCE g_inst;

/* the mode the display driver describes in the shared block */
static PDEV *dd_pdev(void)
{
    WVBDD *d = g_dd;
    PDEV *pd = &g_pd;
    if (!d) return pd;
    pd->bpp = d->bpp; pd->Bpp = d->Bpp;
    pd->rmask = d->rmask; pd->gmask = d->gmask; pd->bmask = d->bmask;
    pd->fb = (BYTE *)d->vramlin;
    pd->vram = d->vramsize;
    pd->stride = (LONG)d->pitch;
    pd->w = d->width; pd->h = d->height;
    return pd;
}

#define DD_PDEV(dd) dd_pdev()
#define DD_FPBASE(pd) ((ULONG)(pd)->fb)
#define DD_LINEAR_HEAP
#define DD_CREATE_FOURCC(s, g, f) dxt_create(s, g, f)
static int dxt_create(LPDDRAWI_DDRAWSURFACE_LCL s, LPDDRAWI_DDRAWSURFACE_GBL g, DWORD f);
#define TRACE(x)

static void *mem_cpy(void *d, const void *s, size_t n)
{
    BYTE *p = d;
    const BYTE *q = s;
    if (!((ULONG)p & 3) && !((ULONG)q & 3)) {
        while (n >= 4) { *(ULONG *)p = *(const ULONG *)q; p += 4; q += 4; n -= 4; }
    }
    while (n--) *p++ = *q++;
    return d;
}

static void *mem_set(void *d, int v, size_t n)
{
    BYTE *p = d;
    while (n--) *p++ = (BYTE)v;
    return d;
}

/* gcc may call these itself */
void *memcpy(void *d, const void *s, size_t n) { return mem_cpy(d, s, n); }
void *memset(void *d, int v, size_t n) { return mem_set(d, v, n); }
int memcmp(const void *a, const void *b, size_t n)
{
    const BYTE *p = a, *q = b;
    for (; n; n--, p++, q++) if (*p != *q) return *p < *q ? -1 : 1;
    return 0;
}

/* shared memory (every process DirectDraw runs in sees the overlay) */
static void *mem_alloc(ULONG n)
{
    if (!g_heap) g_heap = HeapCreate(0x04000000 /* HEAP_SHARED (Windows 9x) */, 0x10000, 0);
    if (!g_heap) return NULL;
    return HeapAlloc(g_heap, HEAP_ZERO_MEMORY, n);
}

static void mem_free(void *p)
{
    if (p && g_heap) HeapFree(g_heap, 0, p);
}

static void put_px(BYTE *p, ULONG Bpp, ULONG v)
{
    switch (Bpp) {
    case 4: *(ULONG *)p = v; break;
    case 2: *(USHORT *)p = (USHORT)v; break;
    case 3: p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); break;
    default: *p = (BYTE)v; break;
    }
}

static ULONG get_px(const BYTE *p, ULONG Bpp)
{
    switch (Bpp) {
    case 4: return *(const ULONG *)p;
    case 2: return *(const USHORT *)p;
    case 3: return p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16);
    default: return *p;
    }
}

/* no shadow frame buffer with the HAL (the display driver checks) */
static int dd_is_screen(PDEV *pd, PDD_SURFACE_LOCAL s) { (void)pd; (void)s; return 0; }
static void shadow_pull(PDEV *pd, const RECTL *r) { (void)pd; (void)r; }
static void shadow_dd_changed(PDEV *pd, PDD_SURFACE_LOCAL s, const RECTL *r) { (void)pd; (void)s; (void)r; }

#include "../nt/disp/ddblt.c"
#include "../nt/disp/ddovl.c"

/* ------------------------------------------------------------------ */
/* callbacks                                                           */

static UCHAR in_port(USHORT p)
{
    UCHAR v;
    __asm__ __volatile__("inb %w1, %0" : "=a"(v) : "Nd"(p));
    return v;
}

/* [WINVBE] Debug=1: a line to the debug port (Bochs/QEMU port E9h) */
static void dbg(const char *s, ULONG a, ULONG b)
{
    static const char hx[] = "0123456789ABCDEF";
    char buf[96];
    int n = 0, i;
    if (!g_dd || !g_dd->debug) return;
    while (*s && n < 60) buf[n++] = *s++;
    buf[n++] = ' ';
    for (i = 28; i >= 0; i -= 4) buf[n++] = hx[(a >> i) & 15];
    buf[n++] = ' ';
    for (i = 28; i >= 0; i -= 4) buf[n++] = hx[(b >> i) & 15];
    buf[n++] = '\n';
    for (i = 0; i < n; i++) __asm__ __volatile__("outb %b0, %w1" : : "a"(buf[i]), "Nd"((USHORT)0xE9));
}

static int dd_ok(void)
{
    return g_dd && g_dd->valid;
}

static DWORD WINAPI Dd9xBlt(LPDDHAL_BLTDATA b)
{
    if (!dd_ok()) return DDHAL_DRIVER_NOTHANDLED;
    yuv_init();
    return DdBlt(b);
}

static DWORD WINAPI Dd9xCreateSurface(LPDDHAL_CREATESURFACEDATA c)
{
    dbg("Dd9xCreateSurface", c->dwSCnt, c->lplpSList[0]->ddsCaps.dwCaps);
    if (!dd_ok()) return DDHAL_DRIVER_NOTHANDLED;
    return DdCreateSurface(c);
}

static int d3d_can_create(const DDSURFACEDESC *sd);

/* surfaces in the format of the screen, the YUV formats (high and true
   colour screens), Z buffers and textures (Direct3D, wvbd3d.c) */
static DWORD WINAPI Dd9xCanCreateSurface(LPDDHAL_CANCREATESURFACEDATA d)
{
    DDPIXELFORMAT *pf = &d->lpDDSurfaceDesc->ddpfPixelFormat;
    d->ddRVal = DD_OK;
    if (d->bIsDifferentPixelFormat &&
        !((d->lpDDSurfaceDesc->dwFlags & DDSD_PIXELFORMAT) && (pf->dwFlags & DDPF_FOURCC) &&
          is_fourcc_ok(pf->dwFourCC) && dd_ok() && g_dd->fourcc_ok) &&
        !(dd_ok() && d3d_can_create(d->lpDDSurfaceDesc)))
        d->ddRVal = DDERR_INVALIDPIXELFORMAT;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI Dd9xUpdateOverlay(LPDDHAL_UPDATEOVERLAYDATA u)
{
    if (!dd_ok()) return DDHAL_DRIVER_NOTHANDLED;
    yuv_init();
    return DdUpdateOverlay(u);
}

static DWORD WINAPI Dd9xSetOverlayPosition(LPDDHAL_SETOVERLAYPOSITIONDATA s)
{
    if (!dd_ok()) return DDHAL_DRIVER_NOTHANDLED;
    yuv_init();
    return DdSetOverlayPosition(s);
}

/* nothing to do (no blitter to wait for), but it has to be there: without
   a Lock callback DirectDraw 6 and later hand out pointers into "aliases"
   of the heap that DDRAW.VXD makes, which fault when written */
static DWORD WINAPI Dd9xLock(LPDDHAL_LOCKDATA l)
{
    (void)l;
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD WINAPI Dd9xUnlock(LPDDHAL_UNLOCKDATA u)
{
    if (!dd_ok()) return DDHAL_DRIVER_NOTHANDLED;
    yuv_init();
    return DdUnlock(u);
}

static void d3d_forget(LPDDRAWI_DDRAWSURFACE_LCL s);   /* wvbd3d.c */

static DWORD WINAPI Dd9xDestroySurface(LPDDHAL_DESTROYSURFACEDATA d)
{
    d3d_forget(d->lpDDSurface);         /* the Direct3D HAL's surface handles */
    return DdDestroySurface(d);
}

static void out_w(USHORT p, USHORT v)
{
    __asm__ __volatile__("outw %w0, %w1" : : "a"(v), "Nd"(p));
}

/* show the surface at linear address a: the DISPI display start (Bochs,
   QEMU, VirtualBox; the virtual width is the pitch of the screen) */
static int set_start(ULONG a)
{
    WVBDD *d = g_dd;
    ULONG off = a - d->vramlin, x, y;
    if (d->flip != 1 || a < d->vramlin || off >= d->vramsize || !d->Bpp) return 0;
    y = off / d->pitch;
    x = off % d->pitch;
    if (x % d->Bpp || y > 0xFFFF) return 0;
    out_w(0x1CE, 8); out_w(0x1CF, (USHORT)(x / d->Bpp));
    out_w(0x1CE, 9); out_w(0x1CF, (USHORT)y);
    d->shown = a;
    return 1;
}

/* the surface rows of src into dst (both the size of the screen) */
static void copy_screen(ULONG dst, LONG dpitch, ULONG src, LONG spitch)
{
    WVBDD *d = g_dd;
    ULONG y, n = d->width * d->Bpp;
    for (y = 0; y < d->height; y++)
        memcpy((BYTE *)dst + y * dpitch, (const BYTE *)src + y * spitch, n);
}

/* flips: of the overlay chain (ddovl.c), or of the screen - the display
   start moves to the target (DISPI); without that the target is copied to
   the screen, and the memory of the two surfaces is exchanged here as well,
   so that DirectDraw's own exchange after the flip leaves the front buffer
   where the screen is */
static DWORD WINAPI Dd9xFlip(LPDDHAL_FLIPDATA f)
{
    LPDDRAWI_DDRAWSURFACE_GBL cur = f->lpSurfCurr->lpGbl, targ = f->lpSurfTarg->lpGbl;
    yuv_init();
    if (dd_ok() && ovl_flip(f)) return DDHAL_DRIVER_HANDLED;
    if (!dd_ok() || !(f->lpSurfCurr->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE)) {
        f->ddRVal = DDERR_UNSUPPORTED;
        return DDHAL_DRIVER_HANDLED;
    }
    if (set_start((ULONG)targ->fpVidMem)) {
        f->ddRVal = DD_OK;
        return DDHAL_DRIVER_HANDLED;
    }
    if ((ULONG)cur->fpVidMem == g_dd->fblin && cur->wWidth == targ->wWidth && cur->wHeight == targ->wHeight) {
        FLATPTR t = cur->fpVidMem;
        copy_screen((ULONG)cur->fpVidMem, cur->lPitch, (ULONG)targ->fpVidMem, targ->lPitch);
        cur->fpVidMem = targ->fpVidMem;
        targ->fpVidMem = t;
        f->ddRVal = DD_OK;
        return DDHAL_DRIVER_HANDLED;
    }
    f->ddRVal = DDERR_UNSUPPORTED;
    return DDHAL_DRIVER_HANDLED;
}

/* back to the screen GDI draws on */
static DWORD WINAPI Dd9xFlipToGDISurface(LPDDHAL_FLIPTOGDISURFACEDATA f)
{
    dbg("Dd9xFlipToGDISurface", g_dd ? g_dd->shown : 0, 0);
    if (dd_ok() && g_dd->flip == 1 && g_dd->shown != g_dd->fblin) set_start(g_dd->fblin);
    f->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI Dd9xStatus(STATUSDATA *s)
{
    s->ddRVal = DD_OK;                  /* blits are done when they return */
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI Dd9xWaitForVerticalBlank(WAITVBDATA *d)
{
    ULONG n;
    switch (d->dwFlags) {
    case DDWAITVB_I_TESTVB:
        d->bIsInVB = (in_port(0x3DA) & 8) != 0;
        break;
    case DDWAITVB_BLOCKBEGIN:
        for (n = 0; n < 2000000 && (in_port(0x3DA) & 8); n++) ;
        for (n = 0; n < 2000000 && !(in_port(0x3DA) & 8); n++) ;
        break;
    case DDWAITVB_BLOCKEND:
        for (n = 0; n < 2000000 && !(in_port(0x3DA) & 8); n++) ;
        for (n = 0; n < 2000000 && (in_port(0x3DA) & 8); n++) ;
        break;
    default:
        d->ddRVal = DDERR_UNSUPPORTED;
        return DDHAL_DRIVER_HANDLED;
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI Dd9xDestroyDriver(LPDDHAL_DESTROYDRIVERDATA d)
{
    if (dd_ok() && g_dd->flip == 1 && g_dd->shown != g_dd->fblin) set_start(g_dd->fblin);
    dbg("Dd9xDestroyDriver", 0, 0);
    ovl_reset();
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

#include "../d3d/swr.c"
#include "wvbd3d.c"

/* DirectDraw: the context is the linear address of the shared block */
__declspec(dllexport) DWORD WINAPI DriverInit(DWORD ctx)
{
    WVBDD *d = (WVBDD *)ctx;
    if (!d) return 1;
    if (d->size != sizeof(WVBDD) || d->version != WD_VERSION) return 0;
    g_dd = d;
    dbg("DriverInit", ctx, (ULONG)g_inst);
    yuv_init();
    d->hinst = (WD32)g_inst;
    d3d_init(d);
    memset(&d->cb, 0, sizeof(d->cb));
    memset(&d->scb, 0, sizeof(d->scb));
    d->cb.dwSize = sizeof(d->cb);
    d->cb.dwFlags = WD_CB_DESTROYDRIVER | WD_CB_CREATESURFACE | WD_CB_WAITFORVBLANK | WD_CB_CANCREATESURFACE |
                    WD_CB_FLIPTOGDISURFACE;
    d->cb.FlipToGDISurface = (WD32)Dd9xFlipToGDISurface;
    d->cb.DestroyDriver = (WD32)Dd9xDestroyDriver;
    d->cb.CreateSurface = (WD32)Dd9xCreateSurface;
    d->cb.WaitForVerticalBlank = (WD32)Dd9xWaitForVerticalBlank;
    d->cb.CanCreateSurface = (WD32)Dd9xCanCreateSurface;
    d->scb.dwSize = sizeof(d->scb);
    d->scb.dwFlags = WD_SCB_DESTROYSURFACE | WD_SCB_FLIP | WD_SCB_LOCK | WD_SCB_UNLOCK | WD_SCB_BLT | WD_SCB_GETBLTSTATUS |
                     WD_SCB_GETFLIPSTATUS | WD_SCB_UPDATEOVERLAY | WD_SCB_SETOVERLAYPOS;
    d->scb.DestroySurface = (WD32)Dd9xDestroySurface;
    d->scb.Flip = (WD32)Dd9xFlip;
    d->scb.Unlock = (WD32)Dd9xUnlock;
    d->scb.Lock = (WD32)Dd9xLock;
    d->scb.Blt = (WD32)Dd9xBlt;
    d->scb.GetBltStatus = (WD32)Dd9xStatus;
    d->scb.GetFlipStatus = (WD32)Dd9xStatus;
    d->scb.UpdateOverlay = (WD32)Dd9xUpdateOverlay;
    d->scb.SetOverlayPosition = (WD32)Dd9xSetOverlayPosition;
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}
