/*
 * WINVBE.DLL - emulated overlay (Windows NT 4.0 and later), included by
 * winvbe.c after ddblt.c.
 *
 * Video players use an overlay when the card has one: a YUV surface that
 * the card shows over the desktop where the screen has the colour key,
 * without the player drawing into the screen. VBE cards have no overlay
 * hardware, so the driver draws it: whenever the overlay changes (update,
 * move, flip of the overlay chain, unlock of the overlay surface) the
 * picture is converted into the primary surface, into the pixels that
 * have the key colour or still hold what the overlay drew there last
 * time. One overlay at a time.
 */

typedef struct {
    int    shown;
    PDEV  *pd;
    PDD_SURFACE_LOCAL src;              /* the overlay surface shown (front) */
    PDD_SURFACE_LOCAL dst;              /* the primary surface */
    RECTL  rs, rd;                      /* source and destination */
    int    keyed;
    ULONG  key;
    BYTE  *last;                        /* what was drawn into rd (rd size, Bpp) */
    ULONG  lastsz;
    LONG   lastw, lasth;
    RECTL  lastrd;
} OVL;

/* (Windows 9x: shared by all processes, like the surfaces) */
static OVL ovl DD_SHARED = { 0 };
static BYTE *ovl_row DD_SHARED = NULL;  /* one converted source row */
static ULONG ovl_rowsz DD_SHARED = 0;

static void ovl_free_last(void)
{
    mem_free(ovl.last);
    ovl.last = NULL;
    ovl.lastsz = 0;
}

/* gives the key colour back to the pixels the overlay still covers */
static void ovl_erase(void)
{
    PDEV *pd = ovl.pd;
    DSURF d;
    LONG x, y;
    if (!pd || !ovl.last || !ovl.dst || !dsurf(pd, ovl.dst, &d)) return;
    for (y = ovl.lastrd.top; y < ovl.lastrd.bottom; y++) {
        BYTE *p = d.base + y * d.pitch + ovl.lastrd.left * pd->Bpp;
        const BYTE *l = ovl.last + ((y - ovl.lastrd.top) * ovl.lastw) * pd->Bpp;
        for (x = ovl.lastrd.left; x < ovl.lastrd.right; x++, p += pd->Bpp, l += pd->Bpp)
            if (get_px(p, pd->Bpp) == get_px(l, pd->Bpp))
                put_px(p, pd->Bpp, ovl.keyed ? ovl.key : 0);
    }
    shadow_dd_changed(pd, ovl.dst, &ovl.lastrd);
    ovl_free_last();
}

/* draws the overlay, from surface s (the one to show) */
static void ovl_draw(PDD_SURFACE_LOCAL s)
{
    PDEV *pd = ovl.pd;
    DSURF d, src;
    PIXFMT pf;
    RECTL r = ovl.rd, whole;
    LONG dw = ovl.rd.right - ovl.rd.left, dh = ovl.rd.bottom - ovl.rd.top;
    LONG sw = ovl.rs.right - ovl.rs.left, sh = ovl.rs.bottom - ovl.rs.top;
    LONG x, y, w, h;
    ULONG step, Bpp;
    if (!pd || !ovl.shown || !s || !dsurf(pd, ovl.dst, &d) || !dsurf(pd, s, &src)) return;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0 || dw > 32767 || dh > 32767 || sw > 32767 || sh > 32767) return;
    whole.left = whole.top = 0;
    whole.right = (LONG)d.w; whole.bottom = (LONG)d.h;
    if (!rect_and(&r, &whole)) return;
    Bpp = pd->Bpp;
    w = r.right - r.left; h = r.bottom - r.top;
    /* the memory of what is drawn: a new destination starts clean */
    if (!ovl.last || ovl.lastrd.left != r.left || ovl.lastrd.top != r.top ||
        ovl.lastrd.right != r.right || ovl.lastrd.bottom != r.bottom) {
        if (ovl.last) ovl_erase();
        ovl.lastsz = (ULONG)w * (ULONG)h * Bpp;
        ovl.last = mem_alloc(ovl.lastsz);
        if (!ovl.last) return;
        ovl.lastw = w; ovl.lasth = h;
        ovl.lastrd = r;
        /* nothing drawn yet: an impossible value would be better, but the
           key check below covers the first frame */
        memset(ovl.last, 0xA5, ovl.lastsz);
    }
    pixfmt_screen(pd, &pf);
    step = ((ULONG)sw << 16) / (ULONG)dw;
    if (!ovl_row || ovl_rowsz < (ULONG)w * 4) {
        mem_free(ovl_row);
        ovl_rowsz = (ULONG)w * 4;
        ovl_row = mem_alloc(ovl_rowsz);
        if (!ovl_row) { ovl_rowsz = 0; return; }
    }
    for (y = r.top; y < r.bottom; y++) {
        ULONG sy = (ULONG)(ovl.rs.top + (LONG)((ULONG)(y - ovl.rd.top) * (ULONG)sh / (ULONG)dh));
        ULONG sx16 = ((ULONG)ovl.rs.left << 16) + (ULONG)(r.left - ovl.rd.left) * step;
        BYTE *p = d.base + y * d.pitch + r.left * Bpp;
        BYTE *l = ovl.last + (ULONG)(y - r.top) * (ULONG)w * Bpp;
        const BYTE *c = ovl_row;
        if (src.fourcc) yuv_row(pd, &src, sy, sx16, step, (ULONG)w, ovl_row);
        else {
            ULONG i;
            for (i = 0; i < (ULONG)w; i++, sx16 += step)
                put_px(ovl_row + i * Bpp, Bpp, get_px(src.base + sy * src.pitch + (sx16 >> 16) * Bpp, Bpp));
        }
        for (x = r.left; x < r.right; x++, p += Bpp, l += Bpp, c += Bpp) {
            ULONG cur = get_px(p, Bpp), v;
            if (ovl.keyed && cur != ovl.key && cur != get_px(l, Bpp)) {
                put_px(l, Bpp, ~cur);       /* covered by a window: not ours */
                continue;
            }
            v = get_px(c, Bpp);
            /* never the key colour itself, or the area would look free */
            if (ovl.keyed && v == ovl.key) v ^= 1;
            put_px(p, Bpp, v);
            put_px(l, Bpp, v);
        }
    }
    shadow_dd_changed(pd, ovl.dst, &r);
}

static DWORD APIENTRY DdUpdateOverlay(PDD_UPDATEOVERLAYDATA u)
{
    PDEV *pd = DD_PDEV(u->lpDD);
    if (u->dwFlags & DDOVER_HIDE) {
        if (ovl.src == u->lpDDSrcSurface) {
            ovl_erase();
            ovl.shown = 0;
            ovl.src = NULL;
        }
        u->ddRVal = DD_OK;
        return DDHAL_DRIVER_HANDLED;
    }
    if (ovl.shown && ovl.src != u->lpDDSrcSurface) {
        u->ddRVal = DDERR_OUTOFCAPS;        /* one overlay at a time */
        return DDHAL_DRIVER_HANDLED;
    }
    if (u->dwFlags & DDOVER_SHOW || ovl.shown) {
        if (ovl.shown) ovl_erase();
        ovl.pd = pd;
        ovl.src = u->lpDDSrcSurface;
        ovl.dst = u->lpDDDestSurface;
        ovl.rs = u->rSrc;
        ovl.rd = u->rDest;
        ovl.keyed = 0;
        if (u->dwFlags & DDOVER_KEYDESTOVERRIDE) {
            ovl.keyed = 1;
            ovl.key = u->overlayFX.dckDestColorkey.dwColorSpaceLowValue;
        } else if (u->dwFlags & DDOVER_KEYDEST) {
            ovl.keyed = 1;
            ovl.key = u->lpDDDestSurface->ddckCKDestOverlay.dwColorSpaceLowValue;
        }
        if (pd->Bpp < 4) ovl.key &= (1UL << pd->bpp) - 1;
        ovl.shown = 1;
        ovl_draw(ovl.src);
    }
    u->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY DdSetOverlayPosition(PDD_SETOVERLAYPOSITIONDATA s)
{
    if (ovl.shown && ovl.src == s->lpDDSrcSurface) {
        LONG w = ovl.rd.right - ovl.rd.left, h = ovl.rd.bottom - ovl.rd.top;
        ovl_erase();
        ovl.rd.left = s->lXPos; ovl.rd.top = s->lYPos;
        ovl.rd.right = s->lXPos + w; ovl.rd.bottom = s->lYPos + h;
        ovl_draw(ovl.src);
    }
    s->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* the overlay surface or the screen was written to */
static DWORD APIENTRY DdUnlock(PDD_UNLOCKDATA u)
{
    PDEV *pd = DD_PDEV(u->lpDD);
    if (pd->ddlocked && dd_is_screen(pd, u->lpDDSurface)) {
        pd->ddlocked = 0;
        shadow_pull(pd, &pd->ddlock);
    }
    if (ovl.shown && u->lpDDSurface == ovl.src) ovl_draw(ovl.src);
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD APIENTRY DdDestroySurface(PDD_DESTROYSURFACEDATA d)
{
#ifdef DD_DESTROY_HOOK
    DD_DESTROY_HOOK(d->lpDDSurface);    /* the Direct3D HAL's surface handles */
#endif
    if (ovl.src == d->lpDDSurface) {
        ovl_erase();
        ovl.shown = 0;
        ovl.src = NULL;
    }
    return DDHAL_DRIVER_NOTHANDLED;     /* DirectDraw frees the memory */
}

/* a flip of the overlay chain: show the new front buffer */
static int ovl_flip(PDD_FLIPDATA f)
{
    if (!(f->lpSurfCurr->ddsCaps.dwCaps & DDSCAPS_OVERLAY)) return 0;
    if (ovl.shown && ovl.src == f->lpSurfCurr) ovl_draw(f->lpSurfTarg);
    f->ddRVal = DD_OK;
    return 1;
}

static void ovl_reset(void)
{
    ovl_free_last();
    mem_free(ovl_row);
    ovl_row = NULL;
    ovl_rowsz = 0;
    memset(&ovl, 0, sizeof(ovl));
}
