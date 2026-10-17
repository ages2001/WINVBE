/*
 * WINVBE - Output (polylines, scanlines), Pixel, ScanLR
 */
#include "drv.h"

typedef struct {
    SURF *s;
    BYTE rop3;
    BYTE needD;
    SHORT cl, ct, cr, cb;     /* clip rect, exclusive right/bottom */
} PLOT;

static void plot_init(PLOT *p, SURF *s, LPGRECT clip, WORD rop2)
{
    p->s = s;
    p->rop3 = rop2_to_rop3(rop2 ? rop2 : 13);
    p->needD = ROP_NEEDS_D(p->rop3) ? 1 : 0;
    p->cl = 0; p->ct = 0; p->cr = s->width; p->cb = s->height;
    if (clip) {
        if (clip->left > p->cl) p->cl = clip->left;
        if (clip->top > p->ct) p->ct = clip->top;
        if (clip->right < p->cr) p->cr = clip->right;
        if (clip->bottom < p->cb) p->cb = clip->bottom;
    }
}

static void plot_pix(PLOT *p, SHORT x, SHORT y, DWORD phys)
{
    BYTE px[4], pd[4];
    SURF *s = p->s;
    if (x < p->cl || x >= p->cr || y < p->ct || y >= p->cb) return;
    if (s->mono) px[0] = mono_of(phys); else pix_put(px, pix_of(phys), s->Bpp);
    if (p->needD) {
        row_read(s, y, x, 1, pd);
        rop_apply(p->rop3, pd, NULL, px, s->Bpp);
        row_write(s, y, x, 1, pd);
    } else {
        rop_apply(p->rop3, pd, NULL, px, s->Bpp);
        row_write(s, y, x, 1, pd);
    }
}

/* horizontal span of solid color, clipped */
static void plot_hspan(PLOT *p, SHORT x0, SHORT x1, SHORT y, DWORD phys)
{
    if (y < p->ct || y >= p->cb) return;
    if (x0 < p->cl) x0 = p->cl;
    if (x1 > p->cr) x1 = p->cr;
    if (x1 <= x0) return;
    solid_rect(p->s, x0, y, x1 - x0, 1, phys, p->rop3);
}

/* style masks: run lengths in pixels, alternating on/off */
static const BYTE style_runs[5][6] = {
    { 0 },
    { 18, 6, 0 },                    /* dash */
    { 3, 3, 0 },                     /* dot */
    { 9, 6, 3, 6, 0 },               /* dash dot */
    { 9, 3, 3, 3, 3, 3 }             /* dash dot dot */
};

static void draw_line(PLOT *p, SHORT x0, SHORT y0, SHORT x1, SHORT y1, LPPPEN pen, LPDRAWMODE dm,
                      WORD *stylepos)
{
    SHORT dx = x1 - x0, dy = y1 - y0, sx = 1, sy = 1, err, n, i;
    WORD style = pen->style;
    DWORD fg = pen->color, bg = dm ? dm->bkColor : 0;
    BYTE opaque = dm && dm->bkMode == OPAQUE;
    if (dx < 0) { dx = -dx; sx = -1; }
    if (dy < 0) { dy = -dy; sy = -1; }
    if (style == PS_SOLID && dy == 0 && dx > 0) {
        if (sx > 0) plot_hspan(p, x0, x1, y0, fg);
        else plot_hspan(p, x1 + 1, x0 + 1, y0, fg);
        return;
    }
    n = dx > dy ? dx : dy;
    err = (dx > dy ? dx : -dy) / 2;
    for (i = 0; i < n; i++) {
        if (style == PS_SOLID) {
            plot_pix(p, x0, y0, fg);
        } else {
            const BYTE *runs = style_runs[style];
            WORD total = 0, k, pos;
            BYTE on = 1;
            for (k = 0; k < 6 && runs[k]; k++) total += runs[k];
            pos = *stylepos % total;
            for (k = 0; k < 6 && runs[k]; k++) {
                if (pos < runs[k]) { on = !(k & 1); break; }
                pos -= runs[k];
            }
            if (on) plot_pix(p, x0, y0, fg);
            else if (opaque) plot_pix(p, x0, y0, bg);
            (*stylepos)++;
        }
        {
            SHORT e2 = err;
            if (e2 > -dx) { err -= dy; x0 += sx; }
            if (e2 < dy) { err += dx; y0 += sy; }
        }
    }
}

/* fill a scanline segment [x0,x1) on row y with a brush */
static void fill_span(PLOT *p, SHORT x0, SHORT x1, SHORT y, LPPBRUSH br, LPDRAWMODE dm)
{
    SURF *s = p->s;
    if (y < p->ct || y >= p->cb) return;
    if (x0 < p->cl) x0 = p->cl;
    if (x1 > p->cr) x1 = p->cr;
    if (x1 <= x0) return;
    if (br && (br->flags & PBF_HATCH) && dm && dm->bkMode == TRANSPARENT) {
        /* transparent hatch: draw only the hatch lines */
        BYTE m = br->mask[y & 7];
        SHORT x;
        for (x = x0; x < x1; x++)
            if (m & (0x80 >> (x & 7)))
                plot_pix(p, x, y, br->fg);
        return;
    }
    fill_rect(s, x0, y, x1 - x0, 1, p->rop3, br, dm);
}

WORD EXPORT Output(LPVOID lpDev, WORD style, WORD count, LPGPOINT pts, LPPPEN pen,
                   LPPBRUSH br, LPDRAWMODE dm, LPGRECT clip)
{
    TRACE("Output\n");
    SURF s;
    PLOT p;
    WORD i;
    if (!g.enabled && ((LPGBITMAP)lpDev)->bmType) return 1;
    surf_from_dev(&s, lpDev);
    plot_init(&p, &s, clip, dm ? dm->Rop2 : 13);

    switch (style) {
    case OS_SCANLINES: {
        SHORT y = pts[0].y;
        LPPBRUSH b = br;
        PBRUSH tmp;
        if (count < 2) return 1;
        if (b && b->style == BS_HOLLOW) return 1;
        if (!b) {
            if (!pen || pen->style == PS_NULL) return 1;
            far_memset(&tmp, 0, sizeof(tmp));
            tmp.style = BS_SOLID; tmp.flags = PBF_SOLID; tmp.fg = pen->color;
            far_memset(tmp.mono, mono_of(pen->color), 8);
            pix_fill(tmp.pat, 8, pix_of(pen->color), g.Bpp);
            b = &tmp;
        }
        if (s.screen) {
            SHORT l = 32767, r = -32768;
            for (i = 1; i < count; i++) {
                if (pts[i].x < l) l = pts[i].x;
                if (pts[i].y > r) r = pts[i].y;
            }
            screen_busy++;
            cursor_exclude_l(l, y, r, (LONG)y + 1);
        }
        for (i = 1; i < count; i++)
            fill_span(&p, pts[i].x, pts[i].y, y, b, dm);
        if (s.screen) { cursor_unexclude(); screen_busy--; }
        return 1;
    }
    case OS_POLYLINE: {
        WORD stylepos = 0;
        SHORT l, t, r, b2;
        if (!pen || pen->style == PS_NULL || count < 2) return 1;
        if (s.screen) {
            l = t = 32767; r = b2 = -32768;
            for (i = 0; i < count; i++) {
                if (pts[i].x < l) l = pts[i].x;
                if (pts[i].x > r) r = pts[i].x;
                if (pts[i].y < t) t = pts[i].y;
                if (pts[i].y > b2) b2 = pts[i].y;
            }
            screen_busy++;
            cursor_exclude_l(l, t, (LONG)r + 1, (LONG)b2 + 1);
        }
        for (i = 0; i + 1 < count; i++)
            draw_line(&p, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, pen, dm, &stylepos);
        if (s.screen) { cursor_unexclude(); screen_busy--; }
        return 1;
    }
    }
    /* not supported: GDI simulates */
    return (WORD)-1;
}

DWORD EXPORT Pixel(LPVOID lpDev, SHORT x, SHORT y, DWORD color, LPDRAWMODE dm)
{
    TRACE("Pixel\n");
    SURF s;
    BYTE px[4];
    DWORD r;
    surf_from_dev(&s, lpDev);
    if (x < 0 || y < 0 || x >= (SHORT)s.width || y >= (SHORT)s.height)
        return 0x80000000UL;
    if (s.screen && !g.enabled) return 0x80000000UL;
    if (s.screen) { screen_busy++; cursor_exclude(x, y, x + 1, y + 1); }
    if (!dm) {
        row_read(&s, y, x, 1, px);
        if (s.mono) r = px[0] ? phys_white() : 0;
        else {
            r = pixel_to_phys(pix_get(px, s.Bpp));
        }
    } else {
        PLOT p;
        plot_init(&p, &s, NULL, dm->Rop2);
        plot_pix(&p, x, y, color);
        r = 1;
    }
    if (s.screen) { cursor_unexclude(); screen_busy--; }
    return r;
}

SHORT EXPORT ScanLR(LPVOID lpDev, SHORT x, SHORT y, DWORD color, WORD style)
{
    TRACE("ScanLR\n");
    SURF s;
    SHORT res = -1;
    BYTE left = (style & 2) ? 1 : 0;
    BYTE ne = (style & 1) ? 0 : 1;      /* FIND_COLOR = 1, FIND_NOT_COLOR = 0 */
    DWORD want;
    surf_from_dev(&s, lpDev);
    if (y < 0 || y >= (SHORT)s.height || x < 0 || x >= (SHORT)s.width) return (SHORT)0x8000;
    if (s.screen && !g.enabled) return (SHORT)0x8000;
    want = s.mono ? (DWORD)mono_of(color) : pix_of(color);
    if (s.screen) { screen_busy++; cursor_exclude(0, y, s.width, y + 1); }
    while (x >= 0 && x < (SHORT)s.width) {
        WORD n, i;
        SHORT start;
        if (left) { n = x + 1 > CHUNKPX ? CHUNKPX : x + 1; start = x - n + 1; }
        else { n = s.width - x > CHUNKPX ? CHUNKPX : s.width - x; start = x; }
        row_read(&s, y, start, n, bufD);
        for (i = 0; i < n; i++) {
            WORD k = left ? n - 1 - i : i;
            DWORD v = s.mono ? bufD[k] : pix_get(bufD + k * s.Bpp, s.Bpp);
            BYTE eq = (v == want);
            if (eq != ne) { res = start + k; goto out; }
        }
        x = left ? start - 1 : start + n;
    }
out:
    if (s.screen) { cursor_unexclude(); screen_busy--; }
    return res;
}
