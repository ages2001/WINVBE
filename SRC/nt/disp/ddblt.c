/*
 * WINVBE.DLL - DirectDraw blits (Windows NT 4.0 and later), included by
 * winvbe.c.
 *
 * VBE cards have no blitter, so these blits run on the CPU, but in the
 * driver: DirectDraw's own emulation (HEL) cannot read YUV surfaces. The
 * driver offers
 *   - off-screen surfaces in the YUV formats of video codecs (YUY2, UYVY,
 *     YV12) in video memory,
 *   - blits from them to RGB surfaces (the primary surface) with colour
 *     conversion (ITU-R BT.601) and stretching, the path video players
 *     take when there is no overlay,
 *   - RGB copies and stretches, colour fills and source colour keys
 *     between video memory surfaces,
 *   - emulated overlays (ddovl.c).
 * Integer arithmetic only (no FPU state in the kernel).
 *
 * The Windows 9x HAL (dd9x/wvbdd32.c) includes this file and ddovl.c as
 * well, with its own PDEV and the DirectDraw 9x structures: DD_PDEV gives
 * the PDEV of a DirectDraw object, DD_FPBASE what fpVidMem counts from
 * (NT: the start of video memory; 9x: linear addresses).
 */
#ifndef DD_PDEV
#define DD_PDEV(dd) ((PDEV *)(dd)->dhpdev)
#define DD_FPBASE(pd) 0
#define DD_SHARED
#endif

#define FOURCC_YUY2 0x32595559UL        /* 'YUY2' */
#define FOURCC_UYVY 0x59565955UL        /* 'UYVY' */
#define FOURCC_YV12 0x32315659UL        /* 'YV12' */

static const DWORD dd_fourcc[] = { FOURCC_YUY2, FOURCC_UYVY, FOURCC_YV12 };
#define DD_NFOURCC (sizeof(dd_fourcc) / sizeof(dd_fourcc[0]))

/* BT.601, video range: R = 1.164 (Y-16) + 1.596 (V-128) ... in 1/256 */
static LONG yt[256], rv[256], gu[256], gv[256], bu[256];
static BYTE clip_tab[1024];             /* clip_tab[x + 384] = x in 0..255 */
static int yuv_ready;

static void yuv_init(void)
{
    LONG i;
    if (yuv_ready) return;
    for (i = 0; i < 256; i++) {
        yt[i] = 298 * (i - 16) + 128;
        rv[i] = 409 * (i - 128);
        gu[i] = -100 * (i - 128);
        gv[i] = -208 * (i - 128);
        bu[i] = 516 * (i - 128);
    }
    for (i = 0; i < 1024; i++) {
        LONG v = i - 384;
        clip_tab[i] = (BYTE)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    yuv_ready = 1;
}

#define CLIP(x) clip_tab[((x) >> 8) + 384]

static int is_fourcc_ok(DWORD f)
{
    ULONG i;
    for (i = 0; i < DD_NFOURCC; i++)
        if (dd_fourcc[i] == f) return 1;
    return 0;
}

static DWORD surf_fourcc(PDD_SURFACE_LOCAL s)
{
    DDPIXELFORMAT *pf = &s->lpGbl->ddpfSurface;
    if (!(s->dwFlags & DDRAWISURF_HASPIXELFORMAT)) return 0;
    return (pf->dwFlags & DDPF_FOURCC) ? pf->dwFourCC : 0;
}

/* an RGB pixel in the format of the screen */
typedef struct {
    ULONG bpp, Bpp;
    int   rs, gs, bs;                   /* shift of the top bit of each channel - 7 */
    ULONG rm, gm, bm;
} PIXFMT;

static int top_bit(ULONG m)
{
    int b = -1;
    while (m) { b++; m >>= 1; }
    return b;
}

static void pixfmt_screen(PDEV *pd, PIXFMT *f)
{
    f->bpp = pd->bpp; f->Bpp = pd->Bpp;
    f->rm = pd->rmask; f->gm = pd->gmask; f->bm = pd->bmask;
    f->rs = top_bit(f->rm) - 7; f->gs = top_bit(f->gm) - 7; f->bs = top_bit(f->bm) - 7;
}

static ULONG pack_rgb(const PIXFMT *f, ULONG r, ULONG g, ULONG b)
{
    ULONG v;
    v  = (f->rs >= 0 ? r << f->rs : r >> -f->rs) & f->rm;
    v |= (f->gs >= 0 ? g << f->gs : g >> -f->gs) & f->gm;
    v |= (f->bs >= 0 ? b << f->bs : b >> -f->bs) & f->bm;
    return v;
}

/* a surface in video memory, as the driver sees it */
typedef struct {
    BYTE  *base;                        /* first byte */
    LONG   pitch;
    ULONG  w, h;
    DWORD  fourcc;                      /* 0: RGB (or a Z buffer) */
    ULONG  Bpp;                         /* bytes per pixel */
    int    screen;                      /* RGB in the screen's format */
} DSURF;

static int dsurf(PDEV *pd, PDD_SURFACE_LOCAL s, DSURF *d)
{
    PDD_SURFACE_GLOBAL g = s->lpGbl;
    ULONG off = (ULONG)g->fpVidMem - (ULONG)DD_FPBASE(pd);
    if (s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) return 0;
    if (off >= pd->vram) return 0;
    d->base = (BYTE *)pd->fb + off;
    d->pitch = g->lPitch;
    d->w = g->wWidth;
    d->h = g->wHeight;
    d->fourcc = surf_fourcc(s);
    d->Bpp = pd->Bpp;
    d->screen = 1;
    /* textures and Z buffers of the Direct3D HAL: formats of their own */
    if ((s->dwFlags & DDRAWISURF_HASPIXELFORMAT) && !d->fourcc) {
        const DDPIXELFORMAT *pf = &g->ddpfSurface;
        ULONG bits = (pf->dwFlags & DDPF_ZBUFFER) ? pf->dwZBufferBitDepth : pf->dwRGBBitCount;
        if (bits == 15) bits = 16;
        if (bits < 8 || bits > 32 || (bits & 7)) return 0;
        d->Bpp = bits / 8;
        d->screen = !(pf->dwFlags & DDPF_ZBUFFER) && d->Bpp == pd->Bpp &&
                    (pd->Bpp == 1 || (pf->dwRBitMask == pd->rmask && pf->dwGBitMask == pd->gmask &&
                                      pf->dwBBitMask == pd->bmask && !(pf->dwFlags & DDPF_ALPHAPIXELS)));
    }
    return d->fourcc == 0 || is_fourcc_ok(d->fourcc);
}

/* the YUV of source pixel x of row y */
static void yuv_at(const DSURF *s, ULONG x, ULONG y, ULONG *Y, ULONG *U, ULONG *V)
{
    const BYTE *p;
    switch (s->fourcc) {
    case FOURCC_YUY2:
        p = s->base + y * s->pitch + (x & ~1UL) * 2;
        *Y = p[(x & 1) * 2]; *U = p[1]; *V = p[3];
        break;
    case FOURCC_UYVY:
        p = s->base + y * s->pitch + (x & ~1UL) * 2;
        *Y = p[1 + (x & 1) * 2]; *U = p[0]; *V = p[2];
        break;
    default: {                          /* YV12: Y plane, V plane, U plane */
        LONG cp = s->pitch / 2;
        const BYTE *vp = s->base + s->pitch * s->h;
        const BYTE *up = vp + cp * (s->h / 2);
        *Y = s->base[y * s->pitch + x];
        *V = vp[(y / 2) * cp + x / 2];
        *U = up[(y / 2) * cp + x / 2];
        break;
    }
    }
}

/* one row of YUV source pixels, converted to the screen format: n pixels
   from source row sy, column sx16 (16.16) on, step apart, into out */
static void yuv_row(PDEV *pd, const DSURF *s, ULONG sy, ULONG sx16, ULONG step, ULONG n, BYTE *out)
{
    PIXFMT pf;
    const BYTE *yr, *ur, *vr;
    ULONG yo, uo, vo, i;
    int kind;
    pixfmt_screen(pd, &pf);
    kind = pf.Bpp == 4 && pf.rm == 0xFF0000 && pf.gm == 0xFF00 && pf.bm == 0xFF ? 1 :
           pf.Bpp == 2 && pf.rm == 0xF800 && pf.gm == 0x7E0 && pf.bm == 0x1F ? 2 :
           pf.Bpp == 2 && pf.rm == 0x7C00 && pf.gm == 0x3E0 && pf.bm == 0x1F ? 3 : 0;
    if (s->fourcc == FOURCC_YV12) {
        LONG cp = s->pitch / 2;
        const BYTE *vp = s->base + s->pitch * s->h;
        yr = s->base + sy * s->pitch;
        vr = vp + (sy / 2) * cp;
        ur = vp + cp * (s->h / 2) + (sy / 2) * cp;
    } else
        yr = ur = vr = s->base + sy * s->pitch;
    /* byte offsets in a 4-byte pair of pixels */
    if (s->fourcc == FOURCC_YUY2) { yo = 0; uo = 1; vo = 3; }
    else { yo = 1; uo = 0; vo = 2; }
    for (i = 0; i < n; i++, sx16 += step) {
        ULONG x = sx16 >> 16, Y, U, V, r, g, b;
        LONG yy;
        if (s->fourcc == FOURCC_YV12) { Y = yr[x]; U = ur[x / 2]; V = vr[x / 2]; }
        else {
            const BYTE *q = yr + (x & ~1UL) * 2;
            Y = q[yo + (x & 1) * 2]; U = q[uo]; V = q[vo];
        }
        yy = yt[Y];
        r = CLIP(yy + rv[V]); g = CLIP(yy + gu[U] + gv[V]); b = CLIP(yy + bu[U]);
        switch (kind) {
        case 1: ((ULONG *)out)[i] = (r << 16) | (g << 8) | b; break;
        case 2: ((USHORT *)out)[i] = (USHORT)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); break;
        case 3: ((USHORT *)out)[i] = (USHORT)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)); break;
        default: put_px(out + i * pf.Bpp, pf.Bpp, pack_rgb(&pf, r, g, b)); break;
        }
    }
}

/* the part of the destination rectangle d inside clip c */
static int rect_and(RECTL *d, const RECTL *c)
{
    if (d->left < c->left) d->left = c->left;
    if (d->top < c->top) d->top = c->top;
    if (d->right > c->right) d->right = c->right;
    if (d->bottom > c->bottom) d->bottom = c->bottom;
    return d->left < d->right && d->top < d->bottom;
}

/* one clipped piece: dr (destination, inside the surface) of the whole
   blit od <- os (od/os: unclipped destination and source) */
static void blt_piece(PDEV *pd, const DSURF *dst, const DSURF *src, const RECTL *dr,
                      const RECTL *od, const RECTL *os, DWORD flags, const DDBLTFX *fx, ULONG key)
{
    PIXFMT pf;
    LONG dw = od->right - od->left, dh = od->bottom - od->top;
    LONG sw = os ? os->right - os->left : 0, sh = os ? os->bottom - os->top : 0;
    LONG x, y;
    ULONG Bpp = dst->Bpp;
    pixfmt_screen(pd, &pf);
    if (dw <= 0 || dh <= 0 || dw > 32767 || dh > 32767 || sw > 32767 || sh > 32767) return;

    if (flags & (DDBLT_COLORFILL | DDBLT_DEPTHFILL)) {
        /* colour fills; depth fills of Z buffers (Direct3D's Clear) */
        ULONG c = (flags & DDBLT_COLORFILL) ? fx->dwFillColor : fx->dwFillDepth;
        for (y = dr->top; y < dr->bottom; y++) {
            BYTE *d = dst->base + y * dst->pitch + dr->left * Bpp;
            if (Bpp == 4) { ULONG *q = (ULONG *)d; for (x = dr->left; x < dr->right; x++) *q++ = c; }
            else if (Bpp == 2) { USHORT *q = (USHORT *)d; for (x = dr->left; x < dr->right; x++) *q++ = (USHORT)c; }
            else for (x = dr->left; x < dr->right; x++, d += Bpp) put_px(d, Bpp, c);
        }
        return;
    }
    if (!src || sw <= 0 || sh <= 0) return;

    for (y = dr->top; y < dr->bottom; y++) {
        ULONG sy = (ULONG)(os->top + (LONG)((ULONG)(y - od->top) * (ULONG)sh / (ULONG)dh));
        /* 16.16 step through the source row */
        ULONG step = ((ULONG)sw << 16) / (ULONG)dw;  /* sizes up to 32767 */
        ULONG sx16 = (ULONG)os->left << 16;
        BYTE *d = dst->base + y * dst->pitch + dr->left * Bpp;
        sx16 += (ULONG)(dr->left - od->left) * step;
        if (src->fourcc) {
            yuv_row(pd, src, sy, sx16, step, (ULONG)(dr->right - dr->left), d);
        } else if (step == 0x10000 && !(flags & (DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE))) {
            const BYTE *s = src->base + sy * src->pitch + (sx16 >> 16) * Bpp;
            ULONG n = (ULONG)(dr->right - dr->left) * Bpp;
            /* overlapping blits inside one surface: rows are walked in the
               right order below; inside a row move backwards when needed */
            if (s < d && s + n > d) { ULONG i = n; while (i--) d[i] = s[i]; }
            else memcpy(d, s, n);
        } else {
            for (x = dr->left; x < dr->right; x++, d += Bpp, sx16 += step) {
                ULONG v = get_px(src->base + sy * src->pitch + (sx16 >> 16) * Bpp, Bpp);
                if ((flags & (DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE)) && v == key) continue;
                put_px(d, Bpp, v);
            }
        }
    }
}

/* overlapping copies inside one surface going down: bottom row first */
static void blt_rect(PDEV *pd, const DSURF *dst, const DSURF *src, const RECTL *dr,
                     const RECTL *od, const RECTL *os, DWORD flags, const DDBLTFX *fx, ULONG key)
{
    if (src && src->base == dst->base && os && od->top > os->top &&
        os->bottom - os->top == od->bottom - od->top && os->right - os->left == od->right - od->left) {
        LONG y;
        for (y = dr->bottom - 1; y >= dr->top; y--) {
            RECTL r = *dr;
            r.top = y; r.bottom = y + 1;
            blt_piece(pd, dst, src, &r, od, os, flags, fx, key);
        }
        return;
    }
    blt_piece(pd, dst, src, dr, od, os, flags, fx, key);
}

static DWORD APIENTRY DdBlt(PDD_BLTDATA b)
{
    PDEV *pd = DD_PDEV(b->lpDD);
    DSURF dst, src;
    int have_src = 0;
    ULONG key = 0, i;
    RECTL od = b->rDest, os = b->rSrc, whole;
    DWORD fl = b->dwFlags;

    TRACE(("Blt fl %x rop %x clip %u n %u\n", fl, b->bltFX.dwROP, b->IsClipped, b->dwRectCnt));

    if (!dsurf(pd, b->lpDDDestSurface, &dst) || dst.fourcc) {
        b->ddRVal = DDERR_UNSUPPORTED;
        return DDHAL_DRIVER_HANDLED;
    }
    if (fl & (DDBLT_COLORFILL | DDBLT_DEPTHFILL)) {
        /* nothing else is needed */
    } else if ((fl & DDBLT_ROP) && (b->bltFX.dwROP > 0xFF ? (b->bltFX.dwROP >> 16) & 0xFF : b->bltFX.dwROP) != 0xCC) {
        /* SRCCOPY only (the ROP comes as 0x00CC0020 or as its index CCh) */
        b->ddRVal = DDERR_NORASTEROPHW;
        return DDHAL_DRIVER_HANDLED;
    } else {
        if (!b->lpDDSrcSurface || !dsurf(pd, b->lpDDSrcSurface, &src)) {
            b->ddRVal = DDERR_UNSUPPORTED;
            return DDHAL_DRIVER_HANDLED;
        }
        have_src = 1;
        if (fl & DDBLT_KEYSRCOVERRIDE) key = b->bltFX.ddckSrcColorkey.dwColorSpaceLowValue;
        else if (fl & DDBLT_KEYSRC) key = b->lpDDSrcSurface->ddckCKSrcBlt.dwColorSpaceLowValue;
        if (src.fourcc && (fl & (DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE))) {
            b->ddRVal = DDERR_UNSUPPORTED;
            return DDHAL_DRIVER_HANDLED;
        }
        /* copies between the same formats; YUV to the screen's format.
           Anything else (a texture of another format) DirectDraw does
           itself through Lock */
        if (src.fourcc ? !dst.screen : (src.Bpp != dst.Bpp || src.screen != dst.screen))
            return DDHAL_DRIVER_NOTHANDLED;
        if (src.Bpp < 4) key &= (1UL << (src.screen ? pd->bpp : src.Bpp * 8)) - 1;
    }
    if (b->IsClipped) {
        od = b->rOrigDest;
        os = b->rOrigSrc;
    }
    whole.left = whole.top = 0;
    whole.right = (LONG)dst.w; whole.bottom = (LONG)dst.h;
    if (b->IsClipped && b->prDestRects) {
        for (i = 0; i < b->dwRectCnt; i++) {
            RECTL r;
            r.left = b->prDestRects[i].left; r.top = b->prDestRects[i].top;
            r.right = b->prDestRects[i].right; r.bottom = b->prDestRects[i].bottom;
            if (rect_and(&r, &od) && rect_and(&r, &whole))
                blt_rect(pd, &dst, have_src ? &src : NULL, &r, &od, have_src ? &os : NULL, fl, &b->bltFX, key);
        }
    } else {
        RECTL r = od;
        if (rect_and(&r, &whole))
            blt_rect(pd, &dst, have_src ? &src : NULL, &r, &od, have_src ? &os : NULL, fl, &b->bltFX, key);
    }
    shadow_dd_changed(pd, b->lpDDDestSurface, &od);
    b->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* YUV surfaces: the size of the block in video memory. On NT the heap is
   rectangular with the pitch of the screen, so every surface has that
   pitch; YV12 takes whole rows, because its chroma planes follow the Y
   plane with half the pitch */
static DWORD APIENTRY DdCreateSurface(PDD_CREATESURFACEDATA c)
{
    PDEV *pd = DD_PDEV(c->lpDD);
    ULONG i;
    for (i = 0; i < c->dwSCnt; i++) {
        PDD_SURFACE_LOCAL s = c->lplpSList[i];
        PDD_SURFACE_GLOBAL g = s->lpGbl;
        DWORD f = surf_fourcc(s);
        ULONG w = g->wWidth, h = g->wHeight;
        if (!f) continue;
#ifdef DD_CREATE_FOURCC
        if (DD_CREATE_FOURCC(s, g, f)) continue;    /* other FourCCs of the platform (DXT textures) */
#endif
        if (!is_fourcc_ok(f) || !w || !h || pd->bpp < 15) {
            c->ddRVal = DDERR_INVALIDPIXELFORMAT;
            return DDHAL_DRIVER_HANDLED;
        }
#ifdef DD_LINEAR_HEAP
        /* a linear heap (Windows 9x): a pitch of the surface's own, the
           block size in bytes */
        if (f == FOURCC_YV12) {
            ULONG pitch = (w + 15) & ~15UL;     /* the chroma planes: pitch / 2 */
            h = (h + 1) & ~1UL;
            g->lPitch = (LONG)pitch;
            g->dwBlockSizeX = pitch * h + pitch * h / 2;
            g->ddpfSurface.dwYUVBitCount = 12;
        } else {
            ULONG pitch = ((w + 1) / 2 * 4 + 7) & ~7UL;
            g->lPitch = (LONG)pitch;
            g->dwBlockSizeX = pitch * h;
            g->ddpfSurface.dwYUVBitCount = 16;
        }
        g->dwBlockSizeY = 1;
#else
        g->lPitch = pd->stride;
        if (f == FOURCC_YV12) {
            h = (h + 1) & ~1UL;
            if (w > (ULONG)pd->stride) goto toobig;
            g->dwBlockSizeX = (DWORD)pd->stride;
            g->dwBlockSizeY = h + (h + 1) / 2;
            g->ddpfSurface.dwYUVBitCount = 12;
        } else {
            if ((w + 1) / 2 * 4 > (ULONG)pd->stride) goto toobig;
            g->dwBlockSizeX = (w + 1) / 2 * 4;
            g->dwBlockSizeY = h;
            g->ddpfSurface.dwYUVBitCount = 16;
        }
#endif
        g->fpVidMem = DDHAL_PLEASEALLOC_BLOCKSIZE;
    }
    return DDHAL_DRIVER_NOTHANDLED;     /* DirectDraw allocates the blocks */
#ifndef DD_LINEAR_HEAP
toobig:
    c->ddRVal = DDERR_OUTOFVIDEOMEMORY;
    return DDHAL_DRIVER_HANDLED;
#endif
}
