/*
 * glnt.c - the display driver's part of the WINVBE OpenGL ICD on Windows NT
 * 3.5, 3.51 and 4.0 (included by winvbe.c).
 *
 * OPENGL32.DLL of these versions does not ask the ICD for pixel formats and
 * buffer swaps but GDI, which asks the display driver:
 *   DrvDescribePixelFormat  the ICD's formats (gl/wvbpf.h, the same list
 *                           the ICD gives Windows 2000 and later);
 *   DrvSetPixelFormat       a WNDOBJ for the window, so that GDI tells the
 *                           driver where the window is and what of it shows;
 *   DrvSwapBuffers          (after glFinish) copies the window's back buffer
 *                           to the screen, clipped to the visible part.
 * The ICD renders into a 32-bit bottom-up DIB section and names it with
 * ExtEscape(WVBGL_ESC_BUFFER) whenever it (re)allocates it. The DIB is
 * locked as an engine surface; on a 32 bpp 8:8:8 desktop it is copied as it
 * is, else the visible part is converted to the screen format first (8 bpp:
 * nearest palette colour). The copy goes through DrvCopyBits, so the
 * shadow buffer, rotation, scaling and the colour tables work as for any
 * other drawing. Windows 2000 and later use the ICD's own functions and
 * get none of this (DrvEnableDriver).
 */

#include "../../gl/wvbpf.h"

#ifdef NT3X
/* not exported by NT 3.1's WINSRV.DLL: looked up at run time */
typedef WNDOBJ *(APIENTRY *PFN_EngCreateWnd)(SURFOBJ *, HWND, WNDOBJCHANGEPROC, FLONG, int);
typedef VOID (APIENTRY *PFN_WNDOBJ_vSetConsumer)(WNDOBJ *, PVOID);
typedef HANDLE (APIENTRY *PFN_EngGetProcessHandle)(VOID);
static PFN_EngCreateWnd p_EngCreateWnd;
static PFN_WNDOBJ_vSetConsumer p_WNDOBJ_vSetConsumer;
static PFN_EngGetProcessHandle p_EngGetProcessHandle;
static RTL_CRITICAL_SECTION gl_cs;
#define gl_lock()   EnterCriticalSection(&gl_cs)
#define gl_unlock() LeaveCriticalSection(&gl_cs)
#else
#define p_EngCreateWnd EngCreateWnd
#define p_WNDOBJ_vSetConsumer WNDOBJ_vSetConsumer
static HSEMAPHORE gl_sem;
#define gl_lock()   EngAcquireSemaphore(gl_sem)
#define gl_unlock() EngReleaseSemaphore(gl_sem)
#endif

#define GLW_MAX 64
typedef struct {
    HWND     hwnd;              /* 0 = free */
    WNDOBJ  *pwo;
    HSURF    hbm;               /* the ICD's buffer (its DIB section) */
    HSURF    htmp;              /* the screen-format copy */
    SURFOBJ *tmp;
    ULONG    tfmt;
    LONG     tw, th;
#ifdef NT3X
    /* NT 3.x: the program's DIB section is not a surface of CSRSS's GDI
       (its handles are the client's): its memory is read with
       ReadProcessMemory into a copy here */
    ULONG    bits;
    LONG     bw, bh, pitch;
    BYTE    *rbits;
    HSURF    hrd;
    SURFOBJ *rd;
    LONG     rw, rh, rp;
#endif
} GLWIN;
static GLWIN glw[GLW_MAX];
static ULONG gl_ready;
static ULONG gl_on;                 /* OpenGL functions given to GDI */
/* counters for WVBGL_ESC_STATUS (diagnostics: ICDTEST, GLTEST) */
static ULONG gl_stat[8];
#define GLS_SETPF 0
#define GLS_WND   1
#define GLS_BUF   2
#define GLS_SWAP  3
#define GLS_SHOWN 4
#define GLS_NOBUF 5
#define GLS_NOLOCK 6
#define GLS_DESC  7

/* 8 bpp: nearest palette entry for every 15-bit colour */
static BYTE *gl_pal15;
static ULONG gl_pal15gen = (ULONG)-1;

/* the bpp the formats report: 15 and 16 bpp both give 16 */
static int gl_bpp(PDEV *pd)
{
    return pd->bpp == 15 ? 16 : (int)pd->bpp;
}

static void gl_init(void)
{
    if (gl_ready) return;
#ifdef NT3X
    {
        HMODULE m = GetModuleHandleA("winsrv.dll");
        if (!m) return;
        p_EngCreateWnd = (PFN_EngCreateWnd)GetProcAddress(m, "EngCreateWnd");
        p_WNDOBJ_vSetConsumer = (PFN_WNDOBJ_vSetConsumer)GetProcAddress(m, "WNDOBJ_vSetConsumer");
        p_EngGetProcessHandle = (PFN_EngGetProcessHandle)GetProcAddress(m, "EngGetProcessHandle");
        if (!p_EngCreateWnd || !p_WNDOBJ_vSetConsumer) return;
        InitializeCriticalSection(&gl_cs);
    }
#else
    gl_sem = EngCreateSemaphore();
    if (!gl_sem) return;
#endif
    gl_ready = 1;
}

static void gl_free_tmp(GLWIN *e)
{
    if (e->tmp) EngUnlockSurface(e->tmp);
    if (e->htmp) EngDeleteSurface(e->htmp);
    e->tmp = NULL;
    e->htmp = NULL;
    e->tw = e->th = 0;
#ifdef NT3X
    if (e->rd) EngUnlockSurface(e->rd);
    if (e->hrd) EngDeleteSurface(e->hrd);
    mem_free(e->rbits);
    e->rd = NULL; e->hrd = NULL; e->rbits = NULL;
    e->rw = e->rh = e->rp = 0;
#endif
}

#ifdef NT3X
/* the program's buffer, read into CSRSS (a bottom-up engine bitmap) */
static SURFOBJ *gl_read_client(GLWIN *e)
{
    DWORD got = 0;
    HANDLE ph;
    if (!e->bits || e->bw <= 0 || e->bh <= 0 || e->pitch < e->bw * 4 || !p_EngGetProcessHandle) return NULL;
    if (!e->rd || e->rw != e->bw || e->rh != e->bh || e->rp != e->pitch) {
        SIZEL sz;
        if (e->rd) EngUnlockSurface(e->rd);
        if (e->hrd) EngDeleteSurface(e->hrd);
        mem_free(e->rbits);
        e->rd = NULL; e->hrd = NULL;
        e->rbits = mem_alloc((ULONG)e->pitch * (ULONG)e->bh);
        if (!e->rbits) return NULL;
        sz.cx = e->bw; sz.cy = e->bh;
        e->hrd = (HSURF)EngCreateBitmap(sz, e->pitch, BMF_32BPP, 0, e->rbits);
        if (e->hrd) e->rd = EngLockSurface(e->hrd);
        if (!e->rd) return NULL;
        e->rw = e->bw; e->rh = e->bh; e->rp = e->pitch;
    }
    ph = p_EngGetProcessHandle();
    if (!ph || !ReadProcessMemory(ph, (LPCVOID)(ULONG_PTR)e->bits, e->rbits, (DWORD)e->pitch * (DWORD)e->bh, &got))
        return NULL;
    return e->rd;
}
#endif

static GLWIN *gl_find(HWND hwnd)
{
    int i;
    for (i = 0; i < GLW_MAX; i++)
        if (glw[i].hwnd && glw[i].hwnd == hwnd) return &glw[i];
    return NULL;
}

/* GDI: the window changed or is gone */
static VOID CALLBACK gl_wndchange(WNDOBJ *pwo, FLONG fl)
{
    GLWIN *e;
    if (!pwo || !(fl & WOC_DELETE)) return;
    gl_lock();
    e = (GLWIN *)pwo->pvConsumer;
    if (e >= glw && e < glw + GLW_MAX && e->pwo == pwo) {
        gl_free_tmp(e);
        memset(e, 0, sizeof(*e));
    }
    gl_unlock();
}

LONG APIENTRY DrvDescribePixelFormat(DHPDEV dhpdev, LONG iPixelFormat, ULONG cjpfd, PIXELFORMATDESCRIPTOR *ppfd)
{
    PDEV *pd = (PDEV *)dhpdev;
    gl_stat[GLS_DESC]++;
    if (!ppfd || cjpfd < sizeof(PIXELFORMATDESCRIPTOR)) return WVB_NUM_PF;
    return wvb_pf_describe((int)iPixelFormat, gl_bpp(pd), ppfd);
}

BOOL APIENTRY DrvSetPixelFormat(SURFOBJ *pso, LONG iPixelFormat, HWND hwnd)
{
    GLWIN *e;
    int i;
    gl_stat[GLS_SETPF]++;
    if (iPixelFormat < 1 || iPixelFormat > WVB_NUM_PF || !hwnd) return FALSE;
    gl_init();
    if (!gl_ready) return FALSE;
    gl_lock();
    e = gl_find(hwnd);
    if (!e)
        for (i = 0; i < GLW_MAX && !e; i++)
            if (!glw[i].hwnd) e = &glw[i];
    if (e && !e->pwo) {
        WNDOBJ *pwo = p_EngCreateWnd(pso, hwnd, gl_wndchange, WO_RGN_CLIENT, (int)iPixelFormat);
        if (pwo && pwo != (WNDOBJ *)-1) {
            memset(e, 0, sizeof(*e));
            e->hwnd = hwnd;
            e->pwo = pwo;
            p_WNDOBJ_vSetConsumer(pwo, e);
            gl_stat[GLS_WND]++;
        } else
            e = NULL;
    }
    gl_unlock();
    TRACE(("SetPixelFormat %u hwnd %x -> %x\n", iPixelFormat, (ULONG)(ULONG_PTR)hwnd, (ULONG)(ULONG_PTR)e));
    return e != NULL;
}

/* ExtEscape(WVBGL_ESC_BUFFER) from the ICD */
static ULONG gl_escape_buffer(ULONG cjIn, PVOID pvIn)
{
    const WVBGL_BUFFER *b = (const WVBGL_BUFFER *)pvIn;
    GLWIN *e;
    if (!b || cjIn < sizeof(*b) || b->magic != WVBGL_ESC_MAGIC || !gl_ready) return 0;
    gl_lock();
    e = gl_find((HWND)(ULONG_PTR)b->hwnd);
    if (e) {
        e->hbm = (HSURF)(ULONG_PTR)b->hbm;
#ifdef NT3X
        if (cjIn >= sizeof(*b)) { e->bits = b->hbm ? b->bits : 0; e->bw = b->width; e->bh = b->height; e->pitch = b->pitch; }
#endif
        gl_stat[GLS_BUF]++;
    }
    gl_unlock();
    return e != NULL;
}

/* bits of a colour mask: lowest bit and count */
static void mask_info(ULONG m, int *shift, int *bits)
{
    int s = 0, n = 0;
    if (m) { while (!(m & 1)) { m >>= 1; s++; } while (m & 1) { m >>= 1; n++; } }
    *shift = s;
    *bits = n;
}

static void gl_make_pal15(PDEV *pd)
{
    ULONG c;
    if (!gl_pal15) gl_pal15 = mem_alloc(32768);
    if (!gl_pal15 || !pd->pal) return;
    for (c = 0; c < 32768; c++) {
        LONG r = (LONG)((c >> 10) << 3 | (c >> 12)), g = (LONG)(((c >> 5) & 31) << 3 | ((c >> 7) & 7)),
             b = (LONG)((c & 31) << 3 | ((c >> 2) & 7));
        ULONG best = 0, bd = 0xFFFFFFFFUL, i;
        for (i = 0; i < 256; i++) {
            LONG dr = r - pd->pal[i].peRed, dg = g - pd->pal[i].peGreen, db = b - pd->pal[i].peBlue;
            ULONG d = (ULONG)(dr * dr * 3 + dg * dg * 4 + db * db * 2);
            if (d < bd) { bd = d; best = i; if (!d) break; }
        }
        gl_pal15[c] = (BYTE)best;
    }
    gl_pal15gen = pd->palgen;
}

/* the part (l,t)-(r,b) of src (32 bpp) into tmp at the same place, in the
   format of the screen */
static void gl_convert(PDEV *pd, SURFOBJ *src, SURFOBJ *tmp, LONG l, LONG t, LONG r, LONG b)
{
    LONG x, y;
    int rs, rb, gs, gb, bs, bb;
    mask_info(pd->rmask, &rs, &rb);
    mask_info(pd->gmask, &gs, &gb);
    mask_info(pd->bmask, &bs, &bb);
    if (tmp->iBitmapFormat == BMF_8BPP && gl_pal15gen != pd->palgen) gl_make_pal15(pd);
    for (y = t; y < b; y++) {
        const ULONG *s = (const ULONG *)((BYTE *)src->pvScan0 + y * src->lDelta) + l;
        BYTE *d = (BYTE *)tmp->pvScan0 + y * tmp->lDelta;
        switch (tmp->iBitmapFormat) {
        case BMF_32BPP: {
            ULONG *o = (ULONG *)d + l;
            for (x = l; x < r; x++) {
                ULONG p = *s++;
                *o++ = ((p >> (24 - rb)) & ((1UL << rb) - 1)) << rs | ((p >> (16 - gb)) & ((1UL << gb) - 1)) << gs |
                       ((p >> (8 - bb)) & ((1UL << bb) - 1)) << bs;
            }
            break; }
        case BMF_24BPP: {
            BYTE *o = d + l * 3;
            for (x = l; x < r; x++) {
                ULONG p = *s++;
                o[0] = (BYTE)p; o[1] = (BYTE)(p >> 8); o[2] = (BYTE)(p >> 16);
                o += 3;
            }
            break; }
        case BMF_16BPP: {
            USHORT *o = (USHORT *)d + l;
            if (rs == 11 && gb == 6 && bs == 0)         /* 5:6:5 */
                for (x = l; x < r; x++) {
                    ULONG p = *s++;
                    *o++ = (USHORT)((p >> 8 & 0xF800) | (p >> 5 & 0x07E0) | (p >> 3 & 0x001F));
                }
            else if (rs == 10 && gb == 5 && bs == 0)    /* 5:5:5 */
                for (x = l; x < r; x++) {
                    ULONG p = *s++;
                    *o++ = (USHORT)((p >> 9 & 0x7C00) | (p >> 6 & 0x03E0) | (p >> 3 & 0x001F));
                }
            else
                for (x = l; x < r; x++) {
                    ULONG p = *s++;
                    *o++ = (USHORT)(((p >> (24 - rb)) & ((1UL << rb) - 1)) << rs |
                                    ((p >> (16 - gb)) & ((1UL << gb) - 1)) << gs |
                                    ((p >> (8 - bb)) & ((1UL << bb) - 1)) << bs);
                }
            break; }
        case BMF_8BPP: {
            BYTE *o = d + l;
            if (!gl_pal15) break;
            for (x = l; x < r; x++) {
                ULONG p = *s++;
                *o++ = gl_pal15[(p >> 9 & 0x7C00) | (p >> 6 & 0x03E0) | (p >> 3 & 0x001F)];
            }
            break; }
        }
    }
}

BOOL APIENTRY DrvSwapBuffers(SURFOBJ *pso, WNDOBJ *pwo)
{
    PDEV *pd = pso ? (PDEV *)pso->dhpdev : NULL;
    GLWIN *e;
    SURFOBJ *src = NULL;
    BOOL locked = FALSE;
    RECTL rc, bd;
    POINTL p0;
    LONG w, h;

    gl_stat[GLS_SWAP]++;
    if (!pd || !pwo || !gl_ready) return TRUE;
    gl_lock();
    e = (GLWIN *)pwo->pvConsumer;
#ifdef NT3X
    if (!(e >= glw && e < glw + GLW_MAX && e->pwo == pwo && e->bits)) { gl_stat[GLS_NOBUF]++; gl_unlock(); return TRUE; }
#else
    if (!(e >= glw && e < glw + GLW_MAX && e->pwo == pwo && e->hbm)) { gl_stat[GLS_NOBUF]++; gl_unlock(); return TRUE; }
#endif
#ifdef NT3X
    src = gl_read_client(e);
#else
    src = EngLockSurface(e->hbm);
    locked = src != NULL;
#endif
    if (!src) { gl_stat[GLS_NOLOCK]++; gl_unlock(); return TRUE; }
    gl_stat[GLS_SHOWN]++;
    if (src->iBitmapFormat != BMF_32BPP) goto done;
    rc = pwo->rclClient;
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    if (w > src->sizlBitmap.cx) w = src->sizlBitmap.cx;
    if (h > src->sizlBitmap.cy) h = src->sizlBitmap.cy;
    if (w <= 0 || h <= 0) goto done;
    rc.right = rc.left + w;
    rc.bottom = rc.top + h;
    p0.x = p0.y = 0;
    if (pd->bpp == 32 && pd->rmask == 0xFF0000 && pd->gmask == 0xFF00 && pd->bmask == 0xFF) {
        DrvCopyBits(pso, src, &pwo->coClient, NULL, &rc, &p0);   /* as it is */
    } else {
        ULONG fmt = pd->bpp == 8 ? BMF_8BPP : pd->bpp == 24 ? BMF_24BPP : pd->bpp == 32 ? BMF_32BPP : BMF_16BPP;
        if (!e->tmp || e->tfmt != fmt || e->tw < w || e->th < h) {
            SIZEL sz;
            gl_free_tmp(e);
            sz.cx = w; sz.cy = h;
            e->htmp = (HSURF)EngCreateBitmap(sz, 0, fmt, BMF_TOPDOWN, NULL);
            if (e->htmp) e->tmp = EngLockSurface(e->htmp);
            if (!e->tmp) { gl_free_tmp(e); goto done; }
            e->tfmt = fmt; e->tw = w; e->th = h;
        }
        /* only what shows of the window */
        bd = rc;
        if (pwo->coClient.iDComplexity != DC_TRIVIAL) {
            RECTL cb = pwo->coClient.rclBounds;
            if (cb.left > bd.left) bd.left = cb.left;
            if (cb.top > bd.top) bd.top = cb.top;
            if (cb.right < bd.right) bd.right = cb.right;
            if (cb.bottom < bd.bottom) bd.bottom = cb.bottom;
        }
        if (bd.right > bd.left && bd.bottom > bd.top) {
            gl_convert(pd, src, e->tmp, bd.left - rc.left, bd.top - rc.top, bd.right - rc.left, bd.bottom - rc.top);
            DrvCopyBits(pso, e->tmp, &pwo->coClient, NULL, &rc, &p0);
        }
    }
done:
    if (locked) EngUnlockSurface(src);
    gl_unlock();
    return TRUE;
}
