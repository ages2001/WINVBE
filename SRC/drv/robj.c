/*
 * WINVBE - RealizeObject: pens and brushes (fonts are left to GDI)
 */
#include "drv.h"

static const BYTE hatches[6][8] = {
    { 0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00 },   /* HS_HORIZONTAL */
    { 0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08 },   /* HS_VERTICAL */
    { 0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x01 },   /* HS_FDIAGONAL */
    { 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80 },   /* HS_BDIAGONAL */
    { 0x08,0x08,0x08,0xFF,0x08,0x08,0x08,0x08 },   /* HS_CROSS */
    { 0x81,0x42,0x24,0x18,0x18,0x24,0x42,0x81 }    /* HS_DIAGCROSS */
};

static BYTE rotbyte(BYTE b, WORD n)
{
    n &= 7;
    return n ? (BYTE)((b >> n) | (b << (8 - n))) : b;
}

/* rotate pattern so that pattern origin lands on (ox,oy) */
static void rotate_brush(LPPBRUSH b, WORD ox, WORD oy)
{
    BYTE tm[8], tk[8];
    BYTE tp[PB_COLORBYTES];
    WORD r, c, k;
    BYTE Bpp = g.Bpp;
    ox &= 7; oy &= 7;
    if (!ox && !oy) return;
    for (r = 0; r < 8; r++) {
        WORD sr = (r - oy) & 7;
        tm[r] = rotbyte(b->mono[sr], ox);
        tk[r] = rotbyte(b->mask[sr], ox);
        for (c = 0; c < 8; c++) {
            WORD sc = (c - ox) & 7;
            for (k = 0; k < Bpp; k++)
                tp[r * 32 + c * Bpp + k] = b->pat[sr * 32 + sc * Bpp + k];
        }
    }
    for (r = 0; r < 8; r++) { b->mono[r] = tm[r]; b->mask[r] = tk[r]; }
    far_memcpy(b->pat, tp, PB_COLORBYTES);
}

static const BYTE bayer8[64] = {
     0,32, 8,40, 2,34,10,42, 48,16,56,24,50,18,58,26, 12,44, 4,36,14,46, 6,38, 60,28,52,20,62,30,54,22,
     3,35,11,43, 1,33, 9,41, 51,19,59,27,49,17,57,25, 15,47, 7,39,13,45, 5,37, 63,31,55,23,61,29,53,21 };

static BYTE clampb(SHORT v) { return (BYTE)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* ordered dither of a logical color into the brush (8bpp color pattern + mono pattern) */
/* best two-colour mix of static colours approximating rgb; returns k/64 of colour b */
static WORD pair_cache_rgb[8];
static BYTE pair_cache_a[8], pair_cache_b[8], pair_cache_k[8], pair_cache_hi[8], pair_cache_n;

static void best_pair(DWORD rgb, BYTE *pa, BYTE *pb, BYTE *pk)
{
    LONG R = (BYTE)rgb, G = (BYTE)(rgb >> 8), B = (BYTE)(rgb >> 16);
    WORD i, j, n;
    DWORD best = 0xFFFFFFFFUL;
    WORD key = (WORD)rgb;
    BYTE hi = (BYTE)(rgb >> 16);
    for (n = 0; n < 8; n++)
        if (pair_cache_rgb[n] == key && pair_cache_hi[n] == hi && pair_cache_k[n] != 0xFF) {
            *pa = pair_cache_a[n]; *pb = pair_cache_b[n]; *pk = pair_cache_k[n];
            return;
        }
    *pa = *pb = 0; *pk = 0;
    for (i = 0; i < 20; i++) {
        LONG ar = static_pal[i * 4], ag = static_pal[i * 4 + 1], ab = static_pal[i * 4 + 2];
        for (j = i; j < 20; j++) {
            LONG dr = (LONG)static_pal[j * 4] - ar, dg = (LONG)static_pal[j * 4 + 1] - ag, db = (LONG)static_pal[j * 4 + 2] - ab;
            LONG len = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            LONG k = 0, er, eg, eb;
            DWORD err;
            if (len) {
                LONG dot = (R - ar) * dr * 3 + (G - ag) * dg * 4 + (B - ab) * db * 2;
                k = (dot * 64 + len / 2) / len;
                if (k < 0) k = 0;
                if (k > 64) k = 64;
            }
            er = ar + dr * k / 64 - R; eg = ag + dg * k / 64 - G; eb = ab + db * k / 64 - B;
            err = (DWORD)(er * er * 3 + eg * eg * 4 + eb * eb * 2);
            /* penalise high contrast pairs (visible noise) */
            if (k && k < 64) err += (DWORD)(len / 48);
            if (err < best) { best = err; *pa = (BYTE)i; *pb = (BYTE)j; *pk = (BYTE)k; }
        }
    }
    n = pair_cache_n++ & 7;
    pair_cache_rgb[n] = key; pair_cache_hi[n] = hi;
    pair_cache_a[n] = *pa; pair_cache_b[n] = *pb; pair_cache_k[n] = *pk;
}

/* ordered dither of a logical color into the brush (8bpp color pattern + mono pattern) */
static void dither_brush(LPPBRUSH b, DWORD rgb, BYTE color)
{
    WORD r, c;
    BYTE R = (BYTE)rgb, G = (BYTE)(rgb >> 8), B = (BYTE)(rgb >> 16);
    WORD lum = ((WORD)R * 30 + (WORD)G * 59 + (WORD)B * 11) / 100;
    BYTE pa = 0, pb = 0, pk = 0, ia, ib;
    if (color) {
        best_pair(rgb, &pa, &pb, &pk);
        ia = pa < 10 ? pa : pa + 236;
        ib = pb < 10 ? pb : pb + 236;
    }
    for (r = 0; r < 8; r++) {
        BYTE mv = 0;
        for (c = 0; c < 8; c++) {
            if (color)
                b->pat[r * 32 + c] = (bayer8[r * 8 + c] < pk) ? ib : ia;
            if ((WORD)lum * 64 / 256 > bayer8[r * 8 + c]) mv |= (BYTE)(0x80 >> c);
        }
        b->mono[r] = mv;
    }
}

static BYTE is_static_exact(DWORD rgb)
{
    WORD i;
    for (i = 0; i < 20; i++)
        if (static_pal[i * 4] == (BYTE)rgb && static_pal[i * 4 + 1] == (BYTE)(rgb >> 8) &&
            static_pal[i * 4 + 2] == (BYTE)(rgb >> 16))
            return 1;
    return 0;
}

static void fill_pat_solid(LPPBRUSH b, DWORD phys)
{
    BYTE row[32];
    WORD r;
    pix_fill(row, 8, pix_of(phys), g.Bpp);
    for (r = 0; r < 8; r++) far_memcpy(b->pat + r * 32, row, 32);
}

static void realize_brush(LPVOID lpDev, LPLOGBRUSH lb, LPPBRUSH b, WORD ox, WORD oy)
{
    WORD r, c;
    BYTE Bpp = g.Bpp;
    far_memset(b, 0, sizeof(PBRUSH));
    b->style = lb->lbStyle;
    switch (lb->lbStyle) {
    case BS_HOLLOW:
        return;
    case BS_HATCHED: {
        WORD h = lb->lbHatch;
        DWORD fg, bg;
        if (h > 5) goto solid;
        fg = color_for_dev(lpDev, lb->lbColor);
        bg = color_for_dev(lpDev, lb->lbBkColor);
        b->fg = fg; b->bg = bg;
        b->flags = PBF_HATCH;
        b->hatch = (BYTE)h;
        for (r = 0; r < 8; r++) {
            BYTE m = hatches[h][r];
            b->mask[r] = m;
            b->mono[r] = (m & mono_of(fg)) | (~m & mono_of(bg));
            for (c = 0; c < 8; c++)
                pix_put(b->pat + r * 32 + c * Bpp, pix_of((m & (0x80 >> c)) ? fg : bg), Bpp);
        }
        break;
    }
    case BS_PATTERN: {
        LPGBITMAP bm = (LPGBITMAP)MK_FP(HIWORD(lb->lbColor), LOWORD(lb->lbColor));
        SURF s;
        if (!bm) goto solid;
        surf_from_dev(&s, bm);
        if (s.mono) {
            b->flags = PBF_MONOPAT;
            for (r = 0; r < 8; r++) {
                BYTE v = r < s.height ? mem_row(&s, r)[0] : 0xFF;
                b->mask[r] = v;
                b->mono[r] = v;
            }
        } else {
            BYTE tmp[32];
            for (r = 0; r < 8; r++) {
                BYTE mv = 0;
                LPBYTE p = mem_row(&s, r < s.height ? r : 0);
                far_memcpy(tmp, p, 8 * Bpp);
                far_memcpy(b->pat + r * 32, tmp, 8 * Bpp);
                for (c = 0; c < 8; c++)
                    if (rgb_is_white(pixel_to_rgb(pix_get(tmp + c * Bpp, Bpp))))
                        mv |= (BYTE)(0x80 >> c);
                b->mono[r] = mv;
                b->mask[r] = mv;
            }
        }
        break;
    }
    default:
    solid:
        b->style = BS_SOLID;
        b->fg = color_for_dev(lpDev, lb->lbColor);
        b->flags = PBF_SOLID;
        far_memset(b->mono, mono_of(b->fg), 8);
        far_memset(b->mask, 0xFF, 8);
        fill_pat_solid(b, b->fg);
        {
            DWORD rgb = lb->lbColor;
            BYTE hi = (BYTE)(rgb >> 24);
            LPGBITMAP dv = (LPGBITMAP)lpDev;
            BYTE monodev = (dv->bmType == 0 && dv->bmBitsPixel == 1);
            if (hi == 0 && !is_static_exact(rgb)) {
#ifdef OLDWIN
                /* Windows 1.x/2.x: all 256 colours are fixed and the colour
                   cube is close enough - solid, like the EGA/VGA drivers */
                if (g.pf == PF_8 && !monodev) {
                } else {
#else
                if (g.pf == PF_8 && !monodev) {
                    dither_brush(b, rgb, 1);
                    b->flags = PBF_DITHER;
                } else {
#endif
                    BYTE save[8], k, same = 1;
                    dither_brush(b, rgb, 0);
                    for (k = 1; k < 8; k++) if (b->mono[k] != b->mono[0]) same = 0;
                    if (b->mono[0] != 0 && b->mono[0] != 0xFF) same = 0;
                    (void)save;
                    if (monodev) {
                        if (!same) b->flags = PBF_DITHER;
                        for (k = 0; k < 8; k++) {
                            WORD c2;
                            for (c2 = 0; c2 < 8; c2++)
                                pix_put(b->pat + k * 32 + c2 * g.Bpp,
                                        (b->mono[k] & (0x80 >> c2)) ? pix_of(phys_white()) : 0, g.Bpp);
                        }
                    }
                }
            }
        }
        rotate_brush(b, ox, oy);
        return;
    }
    rotate_brush(b, ox, oy);
}

SHORT EXPORT RealizeObject(LPVOID lpDev, SHORT style, LPVOID lpIn, LPVOID lpOut, LPTEXTXFORM lpXform)
{
    TRACE("RealizeObject\n");
    if (style < 0)
        return 1;           /* deleting: nothing kept */
    switch (style) {
    case OBJ_PEN:
        if (lpOut) {
            LPLOGPEN lp = (LPLOGPEN)lpIn;
            LPPPEN pp = (LPPPEN)lpOut;
            WORD st = lp->lopnStyle;
            pp->color = color_for_dev(lpDev, lp->lopnColor);
            if (st == PS_INSIDEFRAME) st = PS_SOLID;
            if (st > PS_NULL) st = PS_SOLID;
            if (lp->lopnWidth.x > 1 && st != PS_NULL) st = PS_SOLID;
            pp->style = st;
            pp->width = lp->lopnWidth.x;
        }
        return sizeof(PPEN);
    case OBJ_BRUSH:
        if (lpOut)
            realize_brush(lpDev, (LPLOGBRUSH)lpIn, (LPPBRUSH)lpOut, FP_OFF(lpXform), FP_SEG(lpXform));
        return sizeof(PBRUSH);
    case OBJ_FONT:
        return 0;
    }
    return 0;
}
