/*
 * WINVBE - color conversion and palette management
 *
 * Physical color format (DWORD):
 *   bits 0..23  pixel value in the screen format
 *               (8bpp: palette index, 15/16bpp: packed, 24/32bpp: RGB)
 *   bit  24     PHYS_MONO: color maps to white on monochrome bitmaps
 */
#include "drv.h"

/* Windows static colors: first 10 and last 10 palette entries (R,G,B,flags) */
BYTE static_pal[20 * 4] = {
    0x00,0x00,0x00,0,  0x80,0x00,0x00,0,  0x00,0x80,0x00,0,  0x80,0x80,0x00,0,
    0x00,0x00,0x80,0,  0x80,0x00,0x80,0,  0x00,0x80,0x80,0,  0xC0,0xC0,0xC0,0,
    0xC0,0xDC,0xC0,0,  0xA6,0xCA,0xF0,0,
    0xFF,0xFB,0xF0,0,  0xA0,0xA0,0xA4,0,  0x80,0x80,0x80,0,  0xFF,0x00,0x00,0,
    0x00,0xFF,0x00,0,  0xFF,0xFF,0x00,0,  0x00,0x00,0xFF,0,  0xFF,0x00,0xFF,0,
    0x00,0xFF,0xFF,0,  0xFF,0xFF,0xFF,0
};

static BYTE scale_from(WORD v, BYTE size);
#define R_(c) ((BYTE)(c))
#define G_(c) ((BYTE)((c) >> 8))
#define B_(c) ((BYTE)((c) >> 16))

BYTE rgb_is_white(DWORD rgb)
{
    return (WORD)R_(rgb) + G_(rgb) + B_(rgb) >= 0x180 ? 1 : 0;
}

BYTE mono_of(DWORD phys)
{
    /* Windows 95 GDI hands monochrome DCs 0xFF000000 (black) and 0xFFFFFFFF
       (white) instead of colours from ColorInfo */
    if ((BYTE)(phys >> 24) == 0xFF) return (phys & 0x00FFFFFFUL) ? 0xFF : 0x00;
    return (phys & PHYS_MONO) ? 0xFF : 0x00;
}

/* physical color -> raw pixel value */
DWORD pix_of(DWORD phys)
{
    if (g.pf == PF_24 || g.pf == PF_32) {
        WORD lo = (WORD)phys;
        BYTE hi = (BYTE)(phys >> 16);
        BYTE r = (BYTE)(((lo >> 11) << 3) | (hi & 7));
        BYTE gg = (BYTE)((((lo >> 5) & 0x3F) << 2) | ((hi >> 3) & 3));
        BYTE b = (BYTE)(((lo & 0x1F) << 3) | (hi >> 5));
        return ((DWORD)r << g.mi.rpos) | ((DWORD)gg << g.mi.gpos) | ((DWORD)b << g.mi.bpos);
    }
    return phys & PIXMASK;
}

/* raw pixel -> logical RGB */
DWORD pixel_to_rgb(DWORD v)
{
    BYTE r, gg, b;
    if (g.pf == PF_8) {
        WORD i = (WORD)v & 0xFF;
        r = g.pal[i * 4]; gg = g.pal[i * 4 + 1]; b = g.pal[i * 4 + 2];
    } else {
        r  = scale_from((WORD)(v >> g.mi.rpos) & ((1 << g.mi.rsize) - 1), g.mi.rsize);
        gg = scale_from((WORD)(v >> g.mi.gpos) & ((1 << g.mi.gsize) - 1), g.mi.gsize);
        b  = scale_from((WORD)(v >> g.mi.bpos) & ((1 << g.mi.bsize) - 1), g.mi.bsize);
    }
    return (DWORD)r | ((DWORD)gg << 8) | ((DWORD)b << 16);
}

/* raw pixel -> physical color */
DWORD pixel_to_phys(DWORD v)
{
    return rgb_to_phys(pixel_to_rgb(v));
}

/* 16-bit physical color as stored by Windows 3.0 GDI in DIB color tables */
DWORD word_to_phys(WORD w)
{
    if (g.pf == PF_24 || g.pf == PF_32) {
        BYTE r = (BYTE)((w >> 11) << 3), gg = (BYTE)(((w >> 5) & 0x3F) << 2), b = (BYTE)((w & 0x1F) << 3);
        r |= r >> 5; gg |= gg >> 6; b |= b >> 5;
        return rgb_to_phys((DWORD)r | ((DWORD)gg << 8) | ((DWORD)b << 16));
    }
    return pixel_to_phys(w);
}

DWORD phys_white(void)
{
    switch (g.pf) {
    case PF_8:   return 0xFFUL | PHYS_MONO;
    case PF_555: return 0x7FFFUL | PHYS_MONO;
    default:     return 0xFFFFFFUL | PHYS_MONO;
    }
}

void palette_init(void)
{
    WORD i;
    BYTE *p;
    /* static colors */
    for (i = 0; i < 10; i++) {
        g.pal[i * 4 + 0] = static_pal[i * 4 + 0];
        g.pal[i * 4 + 1] = static_pal[i * 4 + 1];
        g.pal[i * 4 + 2] = static_pal[i * 4 + 2];
        g.pal[i * 4 + 3] = 0;
        g.pal[(246 + i) * 4 + 0] = static_pal[(10 + i) * 4 + 0];
        g.pal[(246 + i) * 4 + 1] = static_pal[(10 + i) * 4 + 1];
        g.pal[(246 + i) * 4 + 2] = static_pal[(10 + i) * 4 + 2];
        g.pal[(246 + i) * 4 + 3] = 0;
    }
    /* entries 10..245 belong to applications: black and unused until
       GDI realizes a palette into them (keeps GDI's view consistent) */
    p = g.pal + 10 * 4;
    for (i = 0; i < 236; i++) {
        p[0] = p[1] = p[2] = 0; p[3] = 0;
        p += 4;
    }
    for (i = 0; i < 10; i++) { g.pal[i * 4 + 3] = 1; g.pal[(246 + i) * 4 + 3] = 1; }
#ifdef OLDWIN
    /* no palette manager before Windows 3.0: all 256 entries are fixed.
       The layout keeps index i and 255-i complementary (like the static
       colours) so that inverting a pixel (DSTINVERT, XOR selections, menu
       highlights) gives the inverse colour:
         20..235  6x6x6 colour cube, (r,g,b) at 20 + r*36 + g*6 + b
         10..19 and 245..236  grey ramp and its inverse */
    {
        WORD r, gg, b, n = 20;
        for (r = 0; r < 6; r++)
            for (gg = 0; gg < 6; gg++)
                for (b = 0; b < 6; b++) {
                    g.pal[n * 4 + 0] = (BYTE)(r * 51);
                    g.pal[n * 4 + 1] = (BYTE)(gg * 51);
                    g.pal[n * 4 + 2] = (BYTE)(b * 51);
                    g.pal[n * 4 + 3] = 1;
                    n++;
                }
        for (i = 0; i < 10; i++) {
            BYTE v = (BYTE)(17 + i * 10);           /* 17..107 */
            n = 10 + i;
            g.pal[n * 4 + 0] = g.pal[n * 4 + 1] = g.pal[n * 4 + 2] = v;
            g.pal[n * 4 + 3] = 1;
            n = 245 - i;
            g.pal[n * 4 + 0] = g.pal[n * 4 + 1] = g.pal[n * 4 + 2] = (BYTE)(255 - v);
            g.pal[n * 4 + 3] = 1;
        }
    }
#endif
    for (i = 0; i < 256; i++)
        g.xlat[i] = i;
    g.xlat_identity = 1;
}

/* nearest static color index (0..9, 246..255) */
static BYTE nearest_static(BYTE r, BYTE gg, BYTE b)
{
    WORD i, best = 0;
    DWORD bd = 0xFFFFFFFFUL;
    for (i = 0; i < 20; i++) {
        SHORT dr = (SHORT)r - static_pal[i * 4 + 0];
        SHORT dg = (SHORT)gg - static_pal[i * 4 + 1];
        SHORT db = (SHORT)b - static_pal[i * 4 + 2];
        DWORD d = (DWORD)((LONG)dr * dr) * 3 + (DWORD)((LONG)dg * dg) * 4 + (DWORD)((LONG)db * db) * 2;
        if (d < bd) { bd = d; best = i; }
    }
    return (BYTE)(best < 10 ? best : best + 236);
}

static WORD scale_to(BYTE v, BYTE size)
{
    return v >> (8 - size);
}

static BYTE scale_from(WORD v, BYTE size)
{
    WORD x;
    if (size >= 8) return (BYTE)(v >> (size - 8));
    x = v << (8 - size);
    x |= x >> size;
    if (size < 4) x |= x >> (size * 2);
    return (BYTE)x;
}

/* logical RGB -> physical color for the screen format */
DWORD rgb_to_phys(DWORD rgb)
{
    BYTE r, gg, b;
    DWORD p;
    BYTE hi = (BYTE)(rgb >> 24);
    if ((hi == 0xFF || hi == 0x01) && g.palettized) {
        /* palette index (already translated by GDI for 0xFF) */
        WORD idx = (WORD)rgb & 0xFF;
        if (hi == 0x01) idx = g.xlat[idx] & 0xFF;
        p = idx;
        if (rgb_is_white(MAKELONG(g.pal[idx * 4] | (g.pal[idx * 4 + 1] << 8), g.pal[idx * 4 + 2])))
            p |= PHYS_MONO;
        return p;
    }
    if (hi == 0xFF || hi == 0x01) {
        WORD idx = (WORD)rgb & 0xFF;
        rgb = (DWORD)g.pal[idx * 4] | ((DWORD)g.pal[idx * 4 + 1] << 8) | ((DWORD)g.pal[idx * 4 + 2] << 16);
    }
    r = R_(rgb); gg = G_(rgb); b = B_(rgb);
    switch (g.pf) {
    case PF_8:
#ifdef OLDWIN
        p = nearest_index(r, gg, b);       /* fixed palette: all 256 usable */
#else
        p = nearest_static(r, gg, b);
#endif
        break;
    case PF_24:
    case PF_32:
        /* low word = RGB565 (GDI 3.0 keeps only 16 bits in DIB color tables),
           bits 16..23 carry the remaining precision */
        p = ((DWORD)(r >> 3) << 11) | ((DWORD)(gg >> 2) << 5) | (b >> 3) |
            ((DWORD)(r & 7) << 16) | ((DWORD)(gg & 3) << 19) | ((DWORD)(b & 7) << 21);
        break;
    default:
        p = ((DWORD)scale_to(r, g.mi.rsize) << g.mi.rpos) |
            ((DWORD)scale_to(gg, g.mi.gsize) << g.mi.gpos) |
            ((DWORD)scale_to(b, g.mi.bsize) << g.mi.bpos);
        break;
    }
    if (rgb_is_white(rgb)) p |= PHYS_MONO;
    return p;
}

/* physical -> logical RGB */
DWORD phys_to_rgb(DWORD phys)
{
    return pixel_to_rgb(pix_of(phys));
}

/* device dependent: mono bitmaps get pure black/white */
DWORD color_for_dev(LPVOID lpDev, DWORD rgb)
{
    LPGBITMAP bm = (LPGBITMAP)lpDev;
    DWORD p = rgb_to_phys(rgb);
    if (bm && bm->bmType == 0 && bm->bmBitsPixel == 1 && bm->bmPlanes == 1) {
        if (p & PHYS_MONO) return phys_white();
        return 0;
    }
    return p;
}

/* inverse palette cache 4:4:4 -> index over the full hardware palette */
static BYTE inv_idx[4096];
static BYTE inv_ok[512];

void invcache_reset(void)
{
    memset(inv_ok, 0, sizeof(inv_ok));
}

BYTE nearest_index(BYTE r, BYTE gg, BYTE b)
{
    WORD key = ((WORD)(r >> 4) << 8) | ((gg >> 4) << 4) | (b >> 4);
    WORD i, j, best = 0;
    DWORD bd = 0xFFFFFFFFUL;
    if (inv_ok[key >> 3] & (1 << (key & 7)))
        return inv_idx[key];
    /* static colours first (0..9, 246..255; see below for 1.x/2.x): an exact match then always
       gets the static index, so white is 255 and black 0 and the bitwise
       raster operations GDI relies on keep working */
    for (j = 0; j < 256; j++) {
        SHORT dr, dg, db;
        DWORD d;
#ifdef OLDWIN
        /* fixed palette: black 0 and white 255 first, then the colour cube,
           so that pure colours invert like on the EGA/VGA (cyan -> red) */
        i = j < 2 ? (j ? 255 : 0) : (j < 218 ? j + 18 : (j < 237 ? j - 217 : j - 1));
#else
        i = j < 20 ? (j < 10 ? j : j + 236) : j - 10;
#endif
        if (!g.pal[i * 4 + 3]) continue;           /* unused entry */
        dr = (SHORT)r - g.pal[i * 4 + 0];
        dg = (SHORT)gg - g.pal[i * 4 + 1];
        db = (SHORT)b - g.pal[i * 4 + 2];
        d = (DWORD)((LONG)dr * dr) * 3 + (DWORD)((LONG)dg * dg) * 4 + (DWORD)((LONG)db * db) * 2;
        if (d < bd) { bd = d; best = i; if (!d) break; }
    }
    inv_idx[key] = (BYTE)best;
    inv_ok[key >> 3] |= (BYTE)(1 << (key & 7));
    return (BYTE)best;
}

/* 4x4 ordered dither of an RGB color to a palette index (8bpp) */
static const SHORT bayer4[16] = { -60, 4, -44, 20, 36, -28, 52, -12, -36, 28, -52, 12, 60, -4, 44, -20 };

BYTE dither_index(DWORD rgb, WORD x, WORD y)
{
    BYTE r = (BYTE)rgb, gg = (BYTE)(rgb >> 8), b = (BYTE)(rgb >> 16);
    BYTE idx = nearest_index(r, gg, b);
    SHORT t, rr, g2, bb;
    if (g.pal[idx * 4] == r && g.pal[idx * 4 + 1] == gg && g.pal[idx * 4 + 2] == b)
        return idx;
    t = bayer4[((y & 3) << 2) | (x & 3)];
    rr = r + t; g2 = gg + t; bb = b + t;
    if (rr < 0) rr = 0; if (rr > 255) rr = 255;
    if (g2 < 0) g2 = 0; if (g2 > 255) g2 = 255;
    if (bb < 0) bb = 0; if (bb > 255) bb = 255;
    return nearest_index((BYTE)rr, (BYTE)g2, (BYTE)bb);
}
