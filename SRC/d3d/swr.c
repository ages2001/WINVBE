/*
 * SWR - WINVBE's software rasterizer for its Direct3D HAL (see swr.h).
 */
#include "swr.h"

void *memset(void *, int, __SIZE_TYPE__);

/* render states (D3DRENDERSTATETYPE) */
#define RS_TEXTUREHANDLE   1
#define RS_ZENABLE         7
#define RS_FILLMODE        8
#define RS_SHADEMODE       9
#define RS_ZWRITEENABLE    14
#define RS_ALPHATESTENABLE 15
#define RS_SRCBLEND        19
#define RS_DESTBLEND       20
#define RS_CULLMODE        22
#define RS_ZFUNC           23
#define RS_ALPHAREF        24
#define RS_ALPHAFUNC       25
#define RS_ALPHABLENDENABLE 27
#define RS_FOGENABLE       28
#define RS_SPECULARENABLE  29
#define RS_FOGCOLOR        34
#define RS_FOGTABLEMODE    35
#define RS_FOGSTART        36
#define RS_FOGEND          37
#define RS_FOGDENSITY      38
#define RS_COLORKEYENABLE  41
#define RS_ZBIAS           47
#define RS_STENCILENABLE   52
#define RS_STENCILFAIL     53
#define RS_STENCILZFAIL    54
#define RS_STENCILPASS     55
#define RS_STENCILFUNC     56
#define RS_STENCILREF      57
#define RS_STENCILMASK     58
#define RS_STENCILWRITEMASK 59
#define RS_TEXTUREFACTOR   60

/* texture stage states */
#define TS_COLOROP   1
#define TS_COLORARG1 2
#define TS_COLORARG2 3
#define TS_ALPHAOP   4
#define TS_ALPHAARG1 5
#define TS_ALPHAARG2 6
#define TS_TEXCOORDINDEX 11
#define TS_ADDRESS   12
#define TS_ADDRESSU  13
#define TS_ADDRESSV  14
#define TS_BORDERCOLOR 15
#define TS_MAGFILTER 16
#define TS_MINFILTER 17
#define TS_MIPFILTER 18
#define TS_MIPMAPLODBIAS 19
#define TS_MAXMIPLEVEL 20

static float fbits(swu32 v) { union { swu32 u; float f; } x; x.u = v; return x.f; }
static swu32 bitsf(float f) { union { swu32 u; float f; } x; x.f = f; return x.u; }

/* float -> int without changing the x87 rounding mode (a C cast changes it
   twice, which is very slow): round to nearest, |f| < 2^22 */
static sws32 f2i(float f)
{
    union { float f; sws32 i; } u;
    u.f = f + 12582912.0f;
    return u.i - 0x4B400000;
}

static sws32 ifloor(float f)
{
    sws32 i;
    if (f > 4000000.0f || f < -4000000.0f) return (sws32)f;
    i = f2i(f);
    return (float)i > f ? i - 1 : i;
}

static sws32 iceil(float f)
{
    sws32 i;
    if (f > 4000000.0f || f < -4000000.0f) return (sws32)f;
    i = f2i(f);
    return (float)i < f ? i + 1 : i;
}

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* log2 of a positive float (approximate, for the mipmap level) */
static float flog2(float f)
{
    swu32 b = bitsf(f);
    float e = (float)((sws32)((b >> 23) & 255) - 127);
    float m = fbits((b & 0x007FFFFF) | 0x3F800000);     /* 1 .. 2 */
    return e + (m - 1.0f) * (1.3466f - 0.3466f * (m - 1.0f));
}

/* e^-x, x >= 0 (fog) */
static float fexpn(float x)
{
    float r = 1.0f, t = 1.0f;
    int k, n = 0;
    if (x <= 0) return 1.0f;
    if (x > 30) return 0;
    while (x > 0.5f) { x *= 0.5f; n++; }            /* e^-x = (e^-x/2^n)^(2^n) */
    for (k = 1; k < 8; k++) { t *= -x / (float)k; r += t; }
    while (n--) r *= r;
    return r;
}

int sw_bpp(int fmt)
{
    switch (fmt) {
    case SWF_P8: case SWF_R3G3B2: case SWF_L8: case SWF_A8: case SWF_A4L4: return 8;
    case SWF_R5G6B5: case SWF_X1R5G5B5: case SWF_A1R5G5B5: case SWF_A4R4G4B4: case SWF_X4R4G4B4:
    case SWF_A8L8: case SWF_Z16: case SWF_Z15S1: case SWF_UYVY: case SWF_YUY2: return 16;
    case SWF_R8G8B8: return 24;
    case SWF_DXT1: return 4;
    case SWF_DXT2: case SWF_DXT3: case SWF_DXT4: case SWF_DXT5: return 8;
    case SWF_NONE: return 0;
    }
    return 32;
}

/* ---------------- texel and pixel formats ---------------- */

static swu32 x565(swu32 v)
{
    swu32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}

static swu32 x555(swu32 v, int alpha)
{
    swu32 r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
    swu32 a = alpha ? ((v & 0x8000) ? 0xFF000000u : 0) : 0xFF000000u;
    return a | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
}

/* one texel of a DXT block */
static swu32 dxt_texel(const SWSURF *s, sws32 x, sws32 y)
{
    sws32 bx = x >> 2, by = y >> 2, i = (y & 3) * 4 + (x & 3);
    int bsize = s->fmt == SWF_DXT1 ? 8 : 16;
    const swu8 *b = s->bits + by * s->pitch + bx * bsize;
    const swu8 *cb = bsize == 16 ? b + 8 : b;
    swu32 c0 = cb[0] | (cb[1] << 8), c1 = cb[2] | (cb[3] << 8);
    swu32 sel = (cb[4 + (i >> 2)] >> ((i & 3) * 2)) & 3;
    swu32 a = 255, col, p0 = x565(c0), p1 = x565(c1);
    int k;
    if (sel == 0) col = p0;
    else if (sel == 1) col = p1;
    else {
        swu32 r = 0;
        for (k = 0; k < 24; k += 8) {
            swu32 v0 = (p0 >> k) & 255, v1 = (p1 >> k) & 255, v;
            if (c0 > c1 || bsize == 16) v = sel == 2 ? (2 * v0 + v1) / 3 : (v0 + 2 * v1) / 3;
            else v = sel == 2 ? (v0 + v1) / 2 : 0;
            r |= v << k;
        }
        col = r;
        if (bsize == 8 && c0 <= c1 && sel == 3) a = 0;
    }
    if (s->fmt == SWF_DXT2 || s->fmt == SWF_DXT3) {
        a = (b[i >> 1] >> ((i & 1) * 4)) & 15;
        a |= a << 4;
    } else if (s->fmt == SWF_DXT4 || s->fmt == SWF_DXT5) {
        swu32 a0 = b[0], a1 = b[1], bit = i * 3, idx;
        swu32 lo = b[2] | (b[3] << 8) | (b[4] << 16), hi = b[5] | (b[6] << 8) | (b[7] << 16);
        idx = bit < 24 ? (bit <= 21 ? (lo >> bit) & 7 : ((lo >> bit) | (hi << (24 - bit))) & 7) : (hi >> (bit - 24)) & 7;
        if (idx == 0) a = a0;
        else if (idx == 1) a = a1;
        else if (a0 > a1) a = ((8 - idx) * a0 + (idx - 1) * a1) / 7;
        else if (idx == 6) a = 0;
        else if (idx == 7) a = 255;
        else a = ((6 - idx) * a0 + (idx - 1) * a1) / 5;
    }
    return (a << 24) | (col & 0xFFFFFF);
}

/* the raw value of a texel (colour keys), bits of the format */
static swu32 sw_raw(const SWSURF *s, sws32 x, sws32 y)
{
    const swu8 *p = s->bits + y * s->pitch;
    switch (sw_bpp(s->fmt)) {
    case 8: return p[x];
    case 16: return ((const swu16 *)p)[x];
    case 24: p += x * 3; return p[0] | (p[1] << 8) | (p[2] << 16);
    case 32: return ((const swu32 *)p)[x];
    }
    return 0;
}

swu32 sw_fetch(const SWSURF *s, const swu32 *pal, sws32 x, sws32 y)
{
    swu32 v;
    switch (s->fmt) {
    case SWF_DXT1: case SWF_DXT2: case SWF_DXT3: case SWF_DXT4: case SWF_DXT5:
        return dxt_texel(s, x, y);
    case SWF_UYVY: case SWF_YUY2: {
        const swu8 *p = s->bits + y * s->pitch + (x & ~1) * 2;
        int Y, U, V, c, d, e;
        if (s->fmt == SWF_YUY2) { Y = p[(x & 1) * 2]; U = p[1]; V = p[3]; }
        else { Y = p[1 + (x & 1) * 2]; U = p[0]; V = p[2]; }
        c = 298 * (Y - 16); d = U - 128; e = V - 128;
        return 0xFF000000u | ((swu32)clamp255((c + 409 * e + 128) >> 8) << 16) |
               ((swu32)clamp255((c - 100 * d - 208 * e + 128) >> 8) << 8) | (swu32)clamp255((c + 516 * d + 128) >> 8);
    }
    }
    v = sw_raw(s, x, y);
    switch (s->fmt) {
    case SWF_P8: return pal ? pal[v & 255] : 0xFF000000u | (v * 0x010101u);
    case SWF_R3G3B2: {
        swu32 r = (v >> 5) & 7, g = (v >> 2) & 7, b = v & 3;
        return 0xFF000000u | ((r * 255 / 7) << 16) | ((g * 255 / 7) << 8) | (b * 255 / 3);
    }
    case SWF_R5G6B5: return x565(v);
    case SWF_X1R5G5B5: return x555(v, 0);
    case SWF_A1R5G5B5: return x555(v, 1);
    case SWF_A4R4G4B4: case SWF_X4R4G4B4: {
        swu32 a = (v >> 12) & 15, r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
        if (s->fmt == SWF_X4R4G4B4) a = 15;
        return (a * 0x11u << 24) | (r * 0x11u << 16) | (g * 0x11u << 8) | (b * 0x11u);
    }
    case SWF_R8G8B8: case SWF_X8R8G8B8: return 0xFF000000u | (v & 0xFFFFFF);
    case SWF_L8: return 0xFF000000u | (v * 0x010101u);
    case SWF_A8: return v << 24 | 0xFFFFFF;
    case SWF_A8L8: return ((v >> 8) << 24) | ((v & 255) * 0x010101u);
    case SWF_A4L4: return ((v >> 4) * 0x11u << 24) | ((v & 15) * 0x111111u);
    }
    return v;
}

static swu32 rt_get(const SWSURF *s, sws32 x, sws32 y)
{
    return sw_fetch(s, 0, x, y);
}

/* ARGB into a render target format */
static swu32 rt_pack(int fmt, swu32 c)
{
    swu32 a = c >> 24, r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    switch (fmt) {
    case SWF_R5G6B5: return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    case SWF_X1R5G5B5: return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    case SWF_A1R5G5B5: return (a >= 128 ? 0x8000 : 0) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    case SWF_A4R4G4B4: case SWF_X4R4G4B4: return ((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
    case SWF_R3G3B2: return (r & 0xE0) | ((g >> 3) & 0x1C) | (b >> 6);
    case SWF_L8: return (r * 77 + g * 151 + b * 28) >> 8;
    case SWF_A8: return a;
    }
    return c;
}

static void rt_put(const SWSURF *s, sws32 x, sws32 y, swu32 v)
{
    swu8 *p = s->bits + y * s->pitch;
    switch (sw_bpp(s->fmt)) {
    case 8: p[x] = (swu8)v; break;
    case 16: ((swu16 *)p)[x] = (swu16)v; break;
    case 24: p += x * 3; p[0] = (swu8)v; p[1] = (swu8)(v >> 8); p[2] = (swu8)(v >> 16); break;
    case 32: ((swu32 *)p)[x] = v; break;
    }
}

/* depth buffer: depth as 0 .. max, stencil */
static swu32 z_max(int fmt)
{
    switch (fmt) {
    case SWF_Z16: return 0xFFFF;
    case SWF_Z15S1: return 0x7FFF;
    case SWF_Z32: return 0xFFFFFFFFu;
    }
    return 0xFFFFFF;
}

static swu32 z_get(const SWSURF *s, sws32 x, sws32 y, swu32 *st)
{
    const swu8 *p = s->bits + y * s->pitch;
    swu32 v;
    *st = 0;
    switch (s->fmt) {
    case SWF_Z16: return ((const swu16 *)p)[x];
    case SWF_Z15S1: v = ((const swu16 *)p)[x]; *st = v >> 15; return v & 0x7FFF;
    case SWF_Z32: return ((const swu32 *)p)[x];
    case SWF_Z24S8: v = ((const swu32 *)p)[x]; *st = v & 255; return v >> 8;
    case SWF_Z24X8: v = ((const swu32 *)p)[x]; return v >> 8;
    case SWF_S8Z24: v = ((const swu32 *)p)[x]; *st = v >> 24; return v & 0xFFFFFF;
    }
    return 0;
}

static void z_put(const SWSURF *s, sws32 x, sws32 y, swu32 z, swu32 st)
{
    swu8 *p = s->bits + y * s->pitch;
    switch (s->fmt) {
    case SWF_Z16: ((swu16 *)p)[x] = (swu16)z; break;
    case SWF_Z15S1: ((swu16 *)p)[x] = (swu16)((z & 0x7FFF) | ((st & 1) << 15)); break;
    case SWF_Z32: ((swu32 *)p)[x] = z; break;
    case SWF_Z24S8: case SWF_Z24X8: ((swu32 *)p)[x] = (z << 8) | (st & 255); break;
    case SWF_S8Z24: ((swu32 *)p)[x] = (z & 0xFFFFFF) | (st << 24); break;
    }
}

static int has_stencil(int fmt) { return fmt == SWF_Z24S8 || fmt == SWF_S8Z24 || fmt == SWF_Z15S1; }

static int cmp_ok(swu32 func, swu32 a, swu32 b)       /* a <func> b */
{
    switch (func) {
    case 1: return 0;
    case 2: return a < b;
    case 3: return a == b;
    case 4: return a <= b;
    case 5: return a > b;
    case 6: return a != b;
    case 7: return a >= b;
    }
    return 1;
}

static swu32 stencil_op(swu32 op, swu32 v, swu32 ref, swu32 max)
{
    switch (op) {
    case 2: return 0;                           /* ZERO */
    case 3: return ref;                         /* REPLACE */
    case 4: return v < max ? v + 1 : max;       /* INCRSAT */
    case 5: return v ? v - 1 : 0;               /* DECRSAT */
    case 6: return ~v & max;                    /* INVERT */
    case 7: return (v + 1) & max;               /* INCR */
    case 8: return (v - 1) & max;               /* DECR */
    }
    return v;                                   /* KEEP */
}

/* ---------------- textures ---------------- */

static sws32 tc_addr(sws32 i, sws32 n, swu32 mode, int *border)
{
    switch (mode) {
    case 2: {                                   /* MIRROR */
        sws32 p = n * 2, k = i % p;
        if (k < 0) k += p;
        return k < n ? k : p - 1 - k;
    }
    case 3:                                     /* CLAMP */
        return i < 0 ? 0 : i >= n ? n - 1 : i;
    case 4:                                     /* BORDER */
        if (i < 0 || i >= n) { *border = 1; return 0; }
        return i;
    }
    i %= n;                                     /* WRAP */
    return i < 0 ? i + n : i;
}

static swu32 texel(const SWTEX *t, const SWSURF *s, sws32 x, sws32 y, swu32 am_u, swu32 am_v, swu32 bc)
{
    int border = 0;
    x = tc_addr(x, s->w, am_u, &border);
    y = tc_addr(y, s->h, am_v, &border);
    if (border) return bc;
    return sw_fetch(s, t->pal, x, y);
}

static swu32 lerp4(swu32 a, swu32 b, swu32 c, swu32 d, swu32 fx, swu32 fy)     /* fx, fy 0..256 */
{
    swu32 r = 0;
    int k;
    for (k = 0; k < 32; k += 8) {
        swu32 va = (a >> k) & 255, vb = (b >> k) & 255, vc = (c >> k) & 255, vd = (d >> k) & 255;
        swu32 top = va * (256 - fx) + vb * fx, bot = vc * (256 - fx) + vd * fx;
        r |= (((top * (256 - fy) + bot * fy) >> 16) & 255) << k;
    }
    return r;
}

/* sample stage s at (u, v), mipmap level lod (log2 of the footprint);
   *kill = colour-keyed texel */
static swu32 tex_sample(const SWCTX *c, int st, float u, float v, float lod, int *kill)
{
    const SWTEX *t = c->tex[st];
    const swu32 *ts = c->tss[st];
    const SWSURF *s;
    swu32 am_u = ts[TS_ADDRESSU], am_v = ts[TS_ADDRESSV], bc = ts[TS_BORDERCOLOR], filt;
    int level = t->lod;
    sws32 x, y;
    lod += fbits(ts[TS_MIPMAPLODBIAS]);
    if (lod > 0.0f && ts[TS_MIPFILTER] >= 2 && t->nlvl > 1) {
        level += f2i(lod);
        if (level < (int)ts[TS_MAXMIPLEVEL]) level = (int)ts[TS_MAXMIPLEVEL];
    }
    if (level >= t->nlvl) level = t->nlvl - 1;
    if (level < 0) level = 0;
    s = &t->lvl[level];
    filt = lod > 0.0f ? ts[TS_MINFILTER] : ts[TS_MAGFILTER];
    if (filt >= 2) {                            /* bilinear */
        float fu = u * (float)s->w - 0.5f, fv = v * (float)s->h - 0.5f;
        sws32 x0 = ifloor(fu), y0 = ifloor(fv);
        swu32 fx = (swu32)f2i((fu - (float)x0) * 256.0f), fy = (swu32)f2i((fv - (float)y0) * 256.0f);
        swu32 a, b, d, e;
        if (fx > 256) fx = 256;
        if (fy > 256) fy = 256;
        if (t->has_ckey && c->rs[RS_COLORKEYENABLE]) {
            int border = 0;
            sws32 nx = tc_addr(ifloor(u * (float)s->w), s->w, am_u, &border);
            sws32 ny = tc_addr(ifloor(v * (float)s->h), s->h, am_v, &border);
            swu32 raw = border ? 0xFFFFFFFFu : sw_raw(s, nx, ny);
            if (!border && raw >= t->ckey_lo && raw <= t->ckey_hi) { *kill = 1; return 0; }
        }
        a = texel(t, s, x0, y0, am_u, am_v, bc);
        b = texel(t, s, x0 + 1, y0, am_u, am_v, bc);
        d = texel(t, s, x0, y0 + 1, am_u, am_v, bc);
        e = texel(t, s, x0 + 1, y0 + 1, am_u, am_v, bc);
        return lerp4(a, b, d, e, fx, fy);
    }
    x = ifloor(u * (float)s->w);
    y = ifloor(v * (float)s->h);
    if (t->has_ckey && c->rs[RS_COLORKEYENABLE]) {
        int border = 0;
        sws32 nx = tc_addr(x, s->w, am_u, &border), ny = tc_addr(y, s->h, am_v, &border);
        if (!border) {
            swu32 raw = sw_raw(s, nx, ny);
            if (raw >= t->ckey_lo && raw <= t->ckey_hi) { *kill = 1; return 0; }
        }
    }
    return texel(t, s, x, y, am_u, am_v, bc);
}

/* ---------------- texture stage operations ---------------- */

typedef struct { int r, g, b, a; } C4;

static C4 c4(swu32 v)
{
    C4 c;
    c.a = (int)(v >> 24); c.r = (int)((v >> 16) & 255); c.g = (int)((v >> 8) & 255); c.b = (int)(v & 255);
    return c;
}

static C4 stage_arg(swu32 arg, const C4 *dif, const C4 *cur, const C4 *tex, const C4 *tf, const C4 *spec, const C4 *tmp)
{
    C4 v;
    switch (arg & 15) {
    case 0: v = *dif; break;
    case 1: v = *cur; break;
    case 2: v = *tex; break;
    case 3: v = *tf; break;
    case 4: v = *spec; break;
    default: v = *tmp; break;
    }
    if (arg & 0x10) { v.r = 255 - v.r; v.g = 255 - v.g; v.b = 255 - v.b; v.a = 255 - v.a; }
    if (arg & 0x20) v.r = v.g = v.b = v.a;
    return v;
}

#define MUL(a, b) (((a) * (b) + 127) / 255)

static int op1(swu32 op, int a, int b, int da, int ta, int fa, int ca)
{
    switch (op) {
    case 2: return a;
    case 3: return b;
    case 4: return MUL(a, b);
    case 5: return clamp255(MUL(a, b) * 2);
    case 6: return clamp255(MUL(a, b) * 4);
    case 7: return clamp255(a + b);
    case 8: return clamp255(a + b - 128);
    case 9: return clamp255((a + b - 128) * 2);
    case 10: return clamp255(a - b);
    case 11: return clamp255(a + b - MUL(a, b));
    case 12: return MUL(a, da) + MUL(b, 255 - da);
    case 13: return MUL(a, ta) + MUL(b, 255 - ta);
    case 14: return MUL(a, fa) + MUL(b, 255 - fa);
    case 15: return clamp255(a + MUL(b, 255 - ta));
    case 16: return MUL(a, ca) + MUL(b, 255 - ca);
    }
    return a;
}

/* ---------------- triangle setup ---------------- */

#define NA_FIXED 10                         /* z, q, diffuse rgba, specular rgba */
#define NA_MAX (NA_FIXED + SW_STAGES * 2)

typedef struct {
    float v[NA_MAX];
    float dx[NA_MAX];
    float dy[NA_MAX];
    int   n;
    int   flat;
    float flatc[8];
} ATTR;

void sw_init(SWCTX *c)
{
    int i, k;
    for (i = 0; i < SWRS_MAX; i++) c->rs[i] = 0;
    c->rs[RS_ZENABLE] = 0;
    c->rs[RS_FILLMODE] = 3;
    c->rs[RS_SHADEMODE] = 2;
    c->rs[RS_ZWRITEENABLE] = 1;
    c->rs[RS_SRCBLEND] = 2;
    c->rs[RS_DESTBLEND] = 1;
    c->rs[RS_CULLMODE] = 3;
    c->rs[RS_ZFUNC] = 4;
    c->rs[RS_ALPHAFUNC] = 8;
    c->rs[RS_FOGEND] = bitsf(1.0f);
    c->rs[RS_FOGDENSITY] = bitsf(1.0f);
    c->rs[RS_STENCILFAIL] = c->rs[RS_STENCILZFAIL] = c->rs[RS_STENCILPASS] = 1;
    c->rs[RS_STENCILFUNC] = 8;
    c->rs[RS_STENCILMASK] = c->rs[RS_STENCILWRITEMASK] = 0xFFFFFFFFu;
    c->rs[RS_TEXTUREFACTOR] = 0xFFFFFFFFu;
    for (i = 0; i < SW_MAXTEX; i++) {
        for (k = 0; k < 32; k++) c->tss[i][k] = 0;
        c->tss[i][TS_COLOROP] = i ? 1 : 4;
        c->tss[i][TS_COLORARG1] = 2;
        c->tss[i][TS_COLORARG2] = 1;
        c->tss[i][TS_ALPHAOP] = i ? 1 : 2;
        c->tss[i][TS_ALPHAARG1] = 2;
        c->tss[i][TS_ALPHAARG2] = 1;
        c->tss[i][TS_TEXCOORDINDEX] = (swu32)i;
        c->tss[i][TS_ADDRESS] = c->tss[i][TS_ADDRESSU] = c->tss[i][TS_ADDRESSV] = 1;
        c->tss[i][TS_MAGFILTER] = c->tss[i][TS_MINFILTER] = 1;
        c->tss[i][TS_MIPFILTER] = 1;
        c->tex[i] = 0;
    }
    c->clip[0] = c->clip[1] = 0;
    c->clip[2] = c->clip[3] = 0;
    c->wnear = 0; c->wfar = 1;
    c->nstages = 1;
}

void sw_prepare(SWCTX *c)
{
    int i;
    c->nstages = 0;
    for (i = 0; i < SW_STAGES; i++) {
        if (c->tss[i][TS_COLOROP] <= 1) break;
        c->stage_tc[i] = (int)(c->tss[i][TS_TEXCOORDINDEX] & 0xFFFF) & (SW_MAXTEX - 1);
        c->nstages = i + 1;
    }
    /* trailing stages without a texture that only pass the colour on (the
       default MODULATE of texture and diffuse: a missing texture is white) */
    while (c->nstages > 0) {
        const swu32 *ts = c->tss[c->nstages - 1];
        swu32 a2 = ts[TS_COLORARG2] & 0x3F, ao = ts[TS_ALPHAOP];
        int pass_c = (ts[TS_COLOROP] == 4 || ts[TS_COLOROP] == 2) && (ts[TS_COLORARG1] & 0x3F) == 2 &&
                     (a2 == 0 || a2 == 1);
        int alpha_white = ao == 2 && (ts[TS_ALPHAARG1] & 0x3F) == 2;
        int pass_a = ao <= 1 || (ao == 4 && (ts[TS_ALPHAARG1] & 0x3F) == 2) ||
                     (ao == 3 && ((ts[TS_ALPHAARG2] & 0x3F) <= 1)) ||
                     (alpha_white && !c->rs[RS_ALPHABLENDENABLE] && !c->rs[RS_ALPHATESTENABLE]);
        if (c->tex[c->nstages - 1] || !pass_c || !pass_a || (c->nstages > 1 && a2 == 0)) break;
        c->nstages--;
    }
    if (c->clip[2] <= 0 || c->clip[2] > c->rt.w) c->clip[2] = c->rt.w;
    if (c->clip[3] <= 0 || c->clip[3] > c->rt.h) c->clip[3] = c->rt.h;
    if (c->clip[0] < 0) c->clip[0] = 0;
    if (c->clip[1] < 0) c->clip[1] = 0;
}

/* the value of every attribute of a vertex */
static void vtx_attr(const SWCTX *c, const SWVTX *p, float *a)
{
    int i, n = NA_FIXED;
    float q = p->rhw;
    a[0] = p->z;
    a[1] = q;
    for (i = 0; i < 4; i++) { a[2 + i] = p->c[i]; a[6 + i] = p->s[i]; }
    for (i = 0; i < c->nstages; i++) {
        a[n++] = p->t[c->stage_tc[i]][0] * q;
        a[n++] = p->t[c->stage_tc[i]][1] * q;
    }
}

/* ---------------- pixels ---------------- */

static void shade_pixel(const SWCTX *c, sws32 x, sws32 y, const float *a, const ATTR *at)
{
    const swu32 *rs = c->rs;
    float q = a[1], w, cf[8];
    C4 dif, spec, cur, tmp, tf, tex;
    int i, kill = 0, zen = (int)rs[RS_ZENABLE] && c->zb.fmt != SWF_NONE;
    swu32 zv = 0, zold = 0, st = 0, out, zmax = 0;

    if (q <= 0) q = 1e-12f;
    w = 1.0f / q;

    /* depth first value (written only when everything passes) */
    if (zen) {
        float z = rs[RS_ZENABLE] == 2 && c->wfar > c->wnear ? (w - c->wnear) / (c->wfar - c->wnear) : a[0];
        zmax = z_max(c->zb.fmt);
        if (z < 0) z = 0;
        if (z > 1) z = 1;
        if (zmax <= 0xFFFFFF) zv = (swu32)f2i(z * (float)zmax);
        else zv = (swu32)((double)z * 4294967295.0);
        zold = z_get(&c->zb, x, y, &st);
    }

    for (i = 0; i < 8; i++) cf[i] = at->flat ? at->flatc[i] : a[2 + i];
    dif.r = clamp255(f2i(cf[0] * 255.0f)); dif.g = clamp255(f2i(cf[1] * 255.0f));
    dif.b = clamp255(f2i(cf[2] * 255.0f)); dif.a = clamp255(f2i(cf[3] * 255.0f));
    spec.r = clamp255(f2i(cf[4] * 255.0f)); spec.g = clamp255(f2i(cf[5] * 255.0f));
    spec.b = clamp255(f2i(cf[6] * 255.0f)); spec.a = clamp255(f2i(a[9] * 255.0f));
    tf = c4(rs[RS_TEXTUREFACTOR]);
    cur = dif;
    tmp.r = tmp.g = tmp.b = tmp.a = 0;

    for (i = 0; i < c->nstages; i++) {
        const swu32 *ts = c->tss[i];
        C4 a1, a2, r;
        if (c->tex[i] && c->tex[i]->nlvl > 0) {
            float u = a[NA_FIXED + i * 2] * w, v = a[NA_FIXED + i * 2 + 1] * w, lod = 0;
            const SWSURF *s0 = &c->tex[i]->lvl[0];
            if (ts[TS_MINFILTER] != ts[TS_MAGFILTER] || (ts[TS_MIPFILTER] >= 2 && c->tex[i]->nlvl > 1)) {
                /* footprint: derivatives of u/q, v/q */
                float dqx = at->dx[1], dqy = at->dy[1];
                float U = a[NA_FIXED + i * 2], V = a[NA_FIXED + i * 2 + 1];
                float dudx = (at->dx[NA_FIXED + i * 2] - u * dqx) * w * (float)s0->w;
                float dvdx = (at->dx[NA_FIXED + i * 2 + 1] - v * dqx) * w * (float)s0->h;
                float dudy = (at->dy[NA_FIXED + i * 2] - u * dqy) * w * (float)s0->w;
                float dvdy = (at->dy[NA_FIXED + i * 2 + 1] - v * dqy) * w * (float)s0->h;
                float rx = dudx * dudx + dvdx * dvdx, ry = dudy * dudy + dvdy * dvdy, rho = rx > ry ? rx : ry;
                (void)U; (void)V;
                lod = rho > 0 ? 0.5f * flog2(rho) : -16.0f;
            }
            tex = c4(tex_sample(c, i, u, v, lod, &kill));
            if (kill) return;
        } else
            tex.r = tex.g = tex.b = tex.a = 255;
        /* colour */
        a1 = stage_arg(ts[TS_COLORARG1], &dif, &cur, &tex, &tf, &spec, &tmp);
        a2 = stage_arg(ts[TS_COLORARG2], &dif, &cur, &tex, &tf, &spec, &tmp);
        switch (ts[TS_COLOROP]) {
        case 18:                            /* MODULATEALPHA_ADDCOLOR */
            r.r = clamp255(a1.r + MUL(a1.a, a2.r)); r.g = clamp255(a1.g + MUL(a1.a, a2.g));
            r.b = clamp255(a1.b + MUL(a1.a, a2.b));
            break;
        case 19:                            /* MODULATECOLOR_ADDALPHA */
            r.r = clamp255(MUL(a1.r, a2.r) + a1.a); r.g = clamp255(MUL(a1.g, a2.g) + a1.a);
            r.b = clamp255(MUL(a1.b, a2.b) + a1.a);
            break;
        case 20:                            /* MODULATEINVALPHA_ADDCOLOR */
            r.r = clamp255(a1.r + MUL(255 - a1.a, a2.r)); r.g = clamp255(a1.g + MUL(255 - a1.a, a2.g));
            r.b = clamp255(a1.b + MUL(255 - a1.a, a2.b));
            break;
        case 21:                            /* MODULATEINVCOLOR_ADDALPHA */
            r.r = clamp255(MUL(255 - a1.r, a2.r) + a1.a); r.g = clamp255(MUL(255 - a1.g, a2.g) + a1.a);
            r.b = clamp255(MUL(255 - a1.b, a2.b) + a1.a);
            break;
        case 24: {                          /* DOTPRODUCT3 */
            int d = ((a1.r - 128) * (a2.r - 128) + (a1.g - 128) * (a2.g - 128) + (a1.b - 128) * (a2.b - 128)) / 32;
            r.r = r.g = r.b = r.a = clamp255(d);
            break;
        }
        default:
            r.r = op1(ts[TS_COLOROP], a1.r, a2.r, dif.a, tex.a, tf.a, cur.a);
            r.g = op1(ts[TS_COLOROP], a1.g, a2.g, dif.a, tex.a, tf.a, cur.a);
            r.b = op1(ts[TS_COLOROP], a1.b, a2.b, dif.a, tex.a, tf.a, cur.a);
            break;
        }
        /* alpha */
        if (ts[TS_COLOROP] != 24) {
            if (ts[TS_ALPHAOP] <= 1) r.a = cur.a;
            else {
                a1 = stage_arg(ts[TS_ALPHAARG1], &dif, &cur, &tex, &tf, &spec, &tmp);
                a2 = stage_arg(ts[TS_ALPHAARG2], &dif, &cur, &tex, &tf, &spec, &tmp);
                r.a = op1(ts[TS_ALPHAOP] >= 18 ? 2 : ts[TS_ALPHAOP], a1.a, a2.a, dif.a, tex.a, tf.a, cur.a);
            }
        }
        cur = r;
    }

    if (rs[RS_SPECULARENABLE]) {
        cur.r = clamp255(cur.r + spec.r); cur.g = clamp255(cur.g + spec.g); cur.b = clamp255(cur.b + spec.b);
    }
    if (rs[RS_FOGENABLE]) {
        float f;
        C4 fc = c4(rs[RS_FOGCOLOR]);
        int fi;
        if (rs[RS_FOGTABLEMODE]) {
            float d = c->wfar > c->wnear && w > 0 && w < 1e30f ? w : a[0];
            float fs = fbits(rs[RS_FOGSTART]), fe = fbits(rs[RS_FOGEND]), fd = fbits(rs[RS_FOGDENSITY]);
            if (rs[RS_FOGTABLEMODE] == 3) f = fe > fs ? (fe - d) / (fe - fs) : 1.0f;
            else if (rs[RS_FOGTABLEMODE] == 1) f = fexpn(fd * d);
            else f = fexpn(fd * d * fd * d);
        } else
            f = a[9];
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        fi = f2i(f * 255.0f);
        cur.r = MUL(cur.r, fi) + MUL(fc.r, 255 - fi);
        cur.g = MUL(cur.g, fi) + MUL(fc.g, 255 - fi);
        cur.b = MUL(cur.b, fi) + MUL(fc.b, 255 - fi);
    }
    if (rs[RS_ALPHATESTENABLE] && !cmp_ok(rs[RS_ALPHAFUNC], (swu32)cur.a, rs[RS_ALPHAREF] & 255)) return;

    /* stencil and depth */
    if (c->zb.fmt != SWF_NONE && (zen || (rs[RS_STENCILENABLE] && has_stencil(c->zb.fmt)))) {
        int zpass = 1;
        swu32 nst = st, smax = c->zb.fmt == SWF_Z15S1 ? 1 : 255;
        if (!zen) zold = z_get(&c->zb, x, y, &st), nst = st;
        if (rs[RS_STENCILENABLE] && has_stencil(c->zb.fmt)) {
            swu32 m = rs[RS_STENCILMASK], ref = rs[RS_STENCILREF] & smax;
            if (!cmp_ok(rs[RS_STENCILFUNC], ref & m, st & m)) {
                nst = stencil_op(rs[RS_STENCILFAIL], st, ref, smax);
                nst = (st & ~rs[RS_STENCILWRITEMASK]) | (nst & rs[RS_STENCILWRITEMASK]);
                z_put(&c->zb, x, y, zold, nst & smax);
                return;
            }
            if (zen && !cmp_ok(rs[RS_ZFUNC], zv, zold)) zpass = 0;
            nst = stencil_op(zpass ? rs[RS_STENCILPASS] : rs[RS_STENCILZFAIL], st, ref, smax);
            nst = (st & ~rs[RS_STENCILWRITEMASK]) | (nst & rs[RS_STENCILWRITEMASK]);
        } else if (zen && !cmp_ok(rs[RS_ZFUNC], zv, zold))
            zpass = 0;
        if (!zpass) {
            if (nst != st) z_put(&c->zb, x, y, zold, nst & smax);
            return;
        }
        if ((zen && rs[RS_ZWRITEENABLE]) || nst != st)
            z_put(&c->zb, x, y, zen && rs[RS_ZWRITEENABLE] ? zv : zold, nst & smax);
    }

    out = ((swu32)cur.a << 24) | ((swu32)cur.r << 16) | ((swu32)cur.g << 8) | (swu32)cur.b;
    if (rs[RS_ALPHABLENDENABLE]) {
        C4 d = c4(rt_get(&c->rt, x, y)), s = cur, fs, fd, r;
        swu32 sb = rs[RS_SRCBLEND], db = rs[RS_DESTBLEND];
        int k;
        if (sb == 12) { sb = 5; db = 6; }           /* BOTHSRCALPHA */
        else if (sb == 13) { sb = 6; db = 5; }      /* BOTHINVSRCALPHA */
        for (k = 0; k < 2; k++) {
            swu32 f = k ? db : sb;
            C4 *o = k ? &fd : &fs;
            switch (f) {
            case 1: o->r = o->g = o->b = o->a = 0; break;
            case 3: *o = s; break;
            case 4: o->r = 255 - s.r; o->g = 255 - s.g; o->b = 255 - s.b; o->a = 255 - s.a; break;
            case 5: o->r = o->g = o->b = o->a = s.a; break;
            case 6: o->r = o->g = o->b = o->a = 255 - s.a; break;
            case 7: o->r = o->g = o->b = o->a = d.a; break;
            case 8: o->r = o->g = o->b = o->a = 255 - d.a; break;
            case 9: *o = d; break;
            case 10: o->r = 255 - d.r; o->g = 255 - d.g; o->b = 255 - d.b; o->a = 255 - d.a; break;
            case 11: { int m = s.a < 255 - d.a ? s.a : 255 - d.a; o->r = o->g = o->b = m; o->a = 255; break; }
            default: o->r = o->g = o->b = o->a = 255; break;
            }
        }
        r.r = clamp255(MUL(s.r, fs.r) + MUL(d.r, fd.r));
        r.g = clamp255(MUL(s.g, fs.g) + MUL(d.g, fd.g));
        r.b = clamp255(MUL(s.b, fs.b) + MUL(d.b, fd.b));
        r.a = clamp255(MUL(s.a, fs.a) + MUL(d.a, fd.a));
        out = ((swu32)r.a << 24) | ((swu32)r.r << 16) | ((swu32)r.g << 8) | (swu32)r.b;
    }
    rt_put(&c->rt, x, y, rt_pack(c->rt.fmt, out));
}


/* ---------------- the fast path ----------------
 * Spans of the common states in integer arithmetic: at most one texture
 * stage that selects or modulates texture and diffuse colour, any filter
 * and address mode, Z (not W) buffer without stencil, vertex fog,
 * specular, alpha test, blending. Texture coordinates are corrected for
 * perspective every 16 pixels and interpolated linearly in between.
 * Everything else goes through shade_pixel(). */

/* float -> 16.16 fixed point (|f| < 32768) with f2i only: Direct3D runs the
   FPU in single precision, where the usual double trick fails */
static sws32 fx16(float f)
{
    sws32 i = ifloor(f);
    return (sws32)((swu32)i << 16) + f2i((f - (float)i) * 65536.0f);
}

/* the usual texture formats without calls */
static swu32 fetch_q(const SWSURF *s, const swu32 *pal, sws32 x, sws32 y)
{
    const swu8 *p = s->bits + y * s->pitch;
    swu32 v;
    switch (s->fmt) {
    case SWF_R5G6B5:
        v = ((const swu16 *)p)[x];
        return 0xFF000000u | ((v & 0xF800) << 8) | ((v & 0xE000) << 3) | ((v & 0x07E0) << 5) | ((v & 0x0600) >> 1) |
               ((v & 0x1F) << 3) | ((v & 0x1C) >> 2);
    case SWF_X1R5G5B5: case SWF_A1R5G5B5:
        v = ((const swu16 *)p)[x];
        return ((s->fmt == SWF_X1R5G5B5 || (v & 0x8000)) ? 0xFF000000u : 0) | ((v & 0x7C00) << 9) | ((v & 0x7000) << 4) |
               ((v & 0x03E0) << 6) | ((v & 0x0380) << 1) | ((v & 0x1F) << 3) | ((v & 0x1C) >> 2);
    case SWF_A4R4G4B4:
        v = ((const swu16 *)p)[x];
        return ((v & 0xF000) << 16) | ((v & 0xF000) << 12) | ((v & 0x0F00) << 12) | ((v & 0x0F00) << 8) |
               ((v & 0x00F0) << 8) | ((v & 0x00F0) << 4) | ((v & 0x000F) << 4) | (v & 0x000F);
    case SWF_A8R8G8B8: return ((const swu32 *)p)[x];
    case SWF_X8R8G8B8: return ((const swu32 *)p)[x] | 0xFF000000u;
    }
    return sw_fetch(s, pal, x, y);
}

typedef struct {
    int on;
    int cm, am;                     /* 0 diffuse, 1 texture, 2 modulate */
    int tex, bilin, mip, ckey;
    int zen, zwr, zfmt;
    float zscale;
    int bpp;
    swu32 zfunc;
} FAST;

static void fast_setup(const SWCTX *c, const ATTR *at, FAST *f)
{
    const swu32 *rs = c->rs;
    memset(f, 0, sizeof(*f));
    if (c->nstages > 1 || at->flat) return;
    if (rs[RS_STENCILENABLE] || rs[RS_ZENABLE] == 2 || (rs[RS_FOGENABLE] && rs[RS_FOGTABLEMODE])) return;
    f->bpp = sw_bpp(c->rt.fmt);
    if (f->bpp != 16 && f->bpp != 32) return;
    f->tex = 0; f->cm = f->am = 0;
    if (c->nstages == 1) {
        const swu32 *ts = c->tss[0];
        swu32 co = ts[TS_COLOROP], a1 = ts[TS_COLORARG1], a2 = ts[TS_COLORARG2];
        swu32 ao = ts[TS_ALPHAOP], b1 = ts[TS_ALPHAARG1], b2 = ts[TS_ALPHAARG2];
        int t1 = a1 == 2, d1 = a1 <= 1, t2 = a2 == 2, d2 = a2 <= 1;
        int u1 = b1 == 2, e1 = b1 <= 1, u2 = b2 == 2, e2 = b2 <= 1;
        if (!c->tex[0] || c->tex[0]->nlvl < 1) return;
        if (co == 2 && (t1 || d1)) f->cm = t1 ? 1 : 0;
        else if (co == 3 && (t2 || d2)) f->cm = t2 ? 1 : 0;
        else if (co == 4 && ((t1 && d2) || (d1 && t2))) f->cm = 2;
        else return;
        if (ao <= 1) f->am = 0;
        else if (ao == 2 && (u1 || e1)) f->am = u1 ? 1 : 0;
        else if (ao == 3 && (u2 || e2)) f->am = u2 ? 1 : 0;
        else if (ao == 4 && ((u1 && e2) || (e1 && u2))) f->am = 2;
        else return;
        f->tex = f->cm || f->am;
        f->bilin = ts[TS_MAGFILTER] >= 2 || ts[TS_MINFILTER] >= 2;
        f->mip = ts[TS_MIPFILTER] >= 2 && c->tex[0]->nlvl > 1;
        f->ckey = c->tex[0]->has_ckey && rs[RS_COLORKEYENABLE];
    }
    f->zen = rs[RS_ZENABLE] && c->zb.fmt != SWF_NONE;
    f->zwr = f->zen && rs[RS_ZWRITEENABLE];
    f->zfmt = c->zb.fmt;
    f->zfunc = rs[RS_ZFUNC];
    f->zscale = f->zen ? (float)(z_max(c->zb.fmt) > 0xFFFFFF ? 0xFFFFFF : z_max(c->zb.fmt)) : 0;
    f->on = 1;
}

/* one span [xl, xr] of row y; a: the attributes at xl */
static void span_fast(const SWCTX *c, const FAST *f, sws32 y, sws32 xl, sws32 xr, const float *a, const ATTR *at)
{
    const swu32 *rs = c->rs;
    swu8 *rp = c->rt.bits + y * c->rt.pitch;
    swu8 *zp = f->zen ? c->zb.bits + y * c->zb.pitch : 0;
    const SWTEX *t = c->tex[0];
    const swu32 *ts = c->tss[0];
    swu32 am_u = ts[TS_ADDRESSU], am_v = ts[TS_ADDRESSV], bc = ts[TS_BORDERCOLOR];
    sws32 r = f2i(a[2] * 1044480.0f), g = f2i(a[3] * 1044480.0f), b = f2i(a[4] * 1044480.0f), al = f2i(a[5] * 1044480.0f);
    sws32 dr = f2i(at->dx[2] * 1044480.0f), dg = f2i(at->dx[3] * 1044480.0f), db = f2i(at->dx[4] * 1044480.0f);
    sws32 dal = f2i(at->dx[5] * 1044480.0f);
    sws32 sr = f2i(a[6] * 1044480.0f), sg = f2i(a[7] * 1044480.0f), sb = f2i(a[8] * 1044480.0f), fo = f2i(a[9] * 1044480.0f);
    sws32 dsr = f2i(at->dx[6] * 1044480.0f), dsg = f2i(at->dx[7] * 1044480.0f), dsb = f2i(at->dx[8] * 1044480.0f);
    sws32 dfo = f2i(at->dx[9] * 1044480.0f);
    float zf = a[0], dz = at->dx[0];
    float q = a[1], U = a[NA_FIXED], V = a[NA_FIXED + 1];
    float dq = at->dx[1], dU = at->dx[NA_FIXED], dV = at->dx[NA_FIXED + 1];
    int spec = (int)rs[RS_SPECULARENABLE], fog = (int)rs[RS_FOGENABLE], atest = (int)rs[RS_ALPHATESTENABLE];
    int blend = (int)rs[RS_ALPHABLENDENABLE];
    swu32 aref = rs[RS_ALPHAREF] & 255, afunc = rs[RS_ALPHAFUNC];
    C4 fc = c4(rs[RS_FOGCOLOR]);
    const SWSURF *ls = t ? &t->lvl[t->lod < t->nlvl ? t->lod : 0] : 0;
    sws32 u = 0, v = 0, du = 0, dv = 0, wmask = 0, hmask = 0, seg = 0;
    int pow2 = 0;
    sws32 x;

    for (x = xl; x <= xr; x++) {
        swu32 tc = 0xFFFFFFFFu, out, zi = 0;
        int cr, cg, cb, ca;

        if (f->tex && seg == 0) {
            /* a new 16-pixel segment: exact coordinates at both ends */
            sws32 n = xr - x + 1 < 16 ? xr - x + 1 : 16;
            float q0 = q > 1e-20f ? q : 1e-20f, q1 = q + dq * (float)n;
            float u0 = U / q0, v0 = V / q0, u1, v1;
            double W, H, fu, fv;
            if (q1 < 1e-20f) q1 = 1e-20f;
            u1 = (U + dU * (float)n) / q1; v1 = (V + dV * (float)n) / q1;
            if (f->mip) {
                /* the footprint: texels per pixel along x and y */
                const SWSURF *s0 = &t->lvl[0];
                float iw = 1.0f / q0;
                float dudy = (at->dy[NA_FIXED] - u0 * at->dy[1]) * iw * (float)s0->w;
                float dvdy = (at->dy[NA_FIXED + 1] - v0 * at->dy[1]) * iw * (float)s0->h;
                float dudx = (u1 - u0) / (float)n * (float)s0->w, dvdx = (v1 - v0) / (float)n * (float)s0->h;
                float rx = dudx * dudx + dvdx * dvdx, ry = dudy * dudy + dvdy * dvdy, rho = rx > ry ? rx : ry;
                float lod = (rho > 0 ? 0.5f * flog2(rho) : -16.0f) + fbits(ts[TS_MIPMAPLODBIAS]);
                int level = t->lod;
                if (lod > 0) level += f2i(lod);
                if (level < (int)ts[TS_MAXMIPLEVEL]) level = (int)ts[TS_MAXMIPLEVEL];
                if (level >= t->nlvl) level = t->nlvl - 1;
                ls = &t->lvl[level];
            }
            W = (double)ls->w; H = (double)ls->h;
            fu = (double)u0 * W; fv = (double)v0 * H;
            if (f->bilin) { fu -= 0.5; fv -= 0.5; }
            /* keep the 16.16 numbers small: whole periods off */
            {
                double pu = am_u == 2 ? 2 * W : W, pv = am_v == 2 ? 2 * H : H;
                double ku = (double)ifloor((float)(fu / pu)), kv = (double)ifloor((float)(fv / pv));
                double eu = ((double)u1 - (double)u0) * W, ev = ((double)v1 - (double)v0) * H;
                if (am_u == 1 || am_u == 2) fu -= ku * pu;
                else if (fu < -4 * W) fu = -4 * W; else if (fu > 4 * W) fu = 4 * W;
                if (am_v == 1 || am_v == 2) fv -= kv * pv;
                else if (fv < -4 * H) fv = -4 * H; else if (fv > 4 * H) fv = 4 * H;
                if (eu > 16000.0) eu = 16000.0; else if (eu < -16000.0) eu = -16000.0;
                if (ev > 16000.0) ev = 16000.0; else if (ev < -16000.0) ev = -16000.0;
                u = fx16((float)fu); v = fx16((float)fv);
                du = fx16((float)(eu / (double)n)); dv = fx16((float)(ev / (double)n));
            }
            pow2 = !(ls->w & (ls->w - 1)) && !(ls->h & (ls->h - 1)) && am_u == 1 && am_v == 1;
            wmask = ls->w - 1; hmask = ls->h - 1;
            seg = n;
        }

        if (f->zen) {
            swu32 zold;
            float zc = zf < 0 ? 0 : zf > 1 ? 1 : zf;
            zi = (swu32)f2i(zc * f->zscale);
            switch (f->zfmt) {
            case SWF_Z16: zold = ((swu16 *)zp)[x]; break;
            case SWF_Z32: zold = ((swu32 *)zp)[x] >> 8; break;
            case SWF_Z24S8: case SWF_Z24X8: zold = ((swu32 *)zp)[x] >> 8; break;
            case SWF_S8Z24: zold = ((swu32 *)zp)[x] & 0xFFFFFF; break;
            default: zold = ((swu16 *)zp)[x] & 0x7FFF; zi >>= 1; break;
            }
            if (!cmp_ok(f->zfunc, zi, zold)) goto next;
        }

        if (f->tex) {
            sws32 tu = u >> 16, tv = v >> 16;
            if (f->ckey) {
                int border = 0;
                sws32 nx = tc_addr(f->bilin ? (u + 0x8000) >> 16 : tu, ls->w, am_u, &border);
                sws32 ny = tc_addr(f->bilin ? (v + 0x8000) >> 16 : tv, ls->h, am_v, &border);
                if (!border) {
                    swu32 raw = sw_raw(ls, nx, ny);
                    if (raw >= t->ckey_lo && raw <= t->ckey_hi) goto next;
                }
            }
            if (!f->bilin) {
                if (pow2) tc = fetch_q(ls, t->pal, tu & wmask, tv & hmask);
                else tc = texel(t, ls, tu, tv, am_u, am_v, bc);
            } else {
                swu32 fx = (swu32)(u & 0xFFFF) >> 8, fy = (swu32)(v & 0xFFFF) >> 8, t0, t1, t2, t3;
                if (pow2) {
                    sws32 x0 = tu & wmask, x1 = (tu + 1) & wmask, y0 = tv & hmask, y1 = (tv + 1) & hmask;
                    t0 = fetch_q(ls, t->pal, x0, y0); t1 = fetch_q(ls, t->pal, x1, y0);
                    t2 = fetch_q(ls, t->pal, x0, y1); t3 = fetch_q(ls, t->pal, x1, y1);
                } else {
                    t0 = texel(t, ls, tu, tv, am_u, am_v, bc); t1 = texel(t, ls, tu + 1, tv, am_u, am_v, bc);
                    t2 = texel(t, ls, tu, tv + 1, am_u, am_v, bc); t3 = texel(t, ls, tu + 1, tv + 1, am_u, am_v, bc);
                }
                tc = lerp4(t0, t1, t2, t3, fx, fy);
            }
        }

        /* colour */
        cr = clamp255(r >> 12); cg = clamp255(g >> 12); cb = clamp255(b >> 12);
        if (f->cm == 1) { cr = (int)((tc >> 16) & 255); cg = (int)((tc >> 8) & 255); cb = (int)(tc & 255); }
        else if (f->cm == 2) {
            cr = MUL(cr, (int)((tc >> 16) & 255)); cg = MUL(cg, (int)((tc >> 8) & 255)); cb = MUL(cb, (int)(tc & 255));
        }
        ca = clamp255(al >> 12);
        if (f->am == 1) ca = (int)(tc >> 24);
        else if (f->am == 2) ca = MUL(ca, (int)(tc >> 24));
        else if (f->tex && ts[TS_ALPHAOP] <= 1) ca = clamp255(al >> 12);
        if (spec) {
            cr = clamp255(cr + (sr >> 12)); cg = clamp255(cg + (sg >> 12)); cb = clamp255(cb + (sb >> 12));
        }
        if (fog) {
            int fi = clamp255(fo >> 12);
            cr = MUL(cr, fi) + MUL(fc.r, 255 - fi);
            cg = MUL(cg, fi) + MUL(fc.g, 255 - fi);
            cb = MUL(cb, fi) + MUL(fc.b, 255 - fi);
        }
        if (atest && !cmp_ok(afunc, (swu32)ca, aref)) goto next;
        out = ((swu32)ca << 24) | ((swu32)cr << 16) | ((swu32)cg << 8) | (swu32)cb;
        if (blend) {
            C4 d = c4(f->bpp == 16 ? sw_fetch(&c->rt, 0, x, y) : (c->rt.fmt == SWF_A8R8G8B8 ? ((swu32 *)rp)[x] : ((swu32 *)rp)[x] | 0xFF000000u));
            swu32 sbl = rs[RS_SRCBLEND], dbl = rs[RS_DESTBLEND];
            int k;
            C4 fs, fd, s;
            s.r = cr; s.g = cg; s.b = cb; s.a = ca;
            if (sbl == 12) { sbl = 5; dbl = 6; }
            else if (sbl == 13) { sbl = 6; dbl = 5; }
            for (k = 0; k < 2; k++) {
                swu32 bf = k ? dbl : sbl;
                C4 *o = k ? &fd : &fs;
                switch (bf) {
                case 1: o->r = o->g = o->b = o->a = 0; break;
                case 3: *o = s; break;
                case 4: o->r = 255 - s.r; o->g = 255 - s.g; o->b = 255 - s.b; o->a = 255 - s.a; break;
                case 5: o->r = o->g = o->b = o->a = s.a; break;
                case 6: o->r = o->g = o->b = o->a = 255 - s.a; break;
                case 7: o->r = o->g = o->b = o->a = d.a; break;
                case 8: o->r = o->g = o->b = o->a = 255 - d.a; break;
                case 9: *o = d; break;
                case 10: o->r = 255 - d.r; o->g = 255 - d.g; o->b = 255 - d.b; o->a = 255 - d.a; break;
                case 11: { int m = s.a < 255 - d.a ? s.a : 255 - d.a; o->r = o->g = o->b = m; o->a = 255; break; }
                default: o->r = o->g = o->b = o->a = 255; break;
                }
            }
            out = ((swu32)clamp255(MUL(s.a, fs.a) + MUL(d.a, fd.a)) << 24) |
                  ((swu32)clamp255(MUL(s.r, fs.r) + MUL(d.r, fd.r)) << 16) |
                  ((swu32)clamp255(MUL(s.g, fs.g) + MUL(d.g, fd.g)) << 8) |
                  (swu32)clamp255(MUL(s.b, fs.b) + MUL(d.b, fd.b));
        }
        if (c->rt.fmt == SWF_R5G6B5)
            ((swu16 *)rp)[x] = (swu16)(((out >> 8) & 0xF800) | ((out >> 5) & 0x07E0) | ((out >> 3) & 0x1F));
        else if (f->bpp == 16) ((swu16 *)rp)[x] = (swu16)rt_pack(c->rt.fmt, out);
        else ((swu32 *)rp)[x] = out;
        if (f->zwr) {
            switch (f->zfmt) {
            case SWF_Z16: ((swu16 *)zp)[x] = (swu16)zi; break;
            case SWF_Z32: ((swu32 *)zp)[x] = zi << 8 | zi >> 16; break;
            case SWF_Z24S8: case SWF_Z24X8: ((swu32 *)zp)[x] = (zi << 8) | (((swu32 *)zp)[x] & 255); break;
            case SWF_S8Z24: ((swu32 *)zp)[x] = (zi & 0xFFFFFF) | (((swu32 *)zp)[x] & 0xFF000000u); break;
            default: ((swu16 *)zp)[x] = (swu16)((zi & 0x7FFF) | (((swu16 *)zp)[x] & 0x8000)); break;
            }
        }
    next:
        r += dr; g += dg; b += db; al += dal;
        sr += dsr; sg += dsg; sb += dsb; fo += dfo;
        zf += dz;
        if (f->tex) { u += du; v += dv; q += dq; U += dU; V += dV; seg--; }
    }
}

/* ---------------- primitives ---------------- */

static float fclampc(float v)
{
    if (v != v) return 0;                       /* NaN */
    if (v > 1e6f) return 1e6f;
    if (v < -1e6f) return -1e6f;
    return v;
}

static void tri_fill(SWCTX *c, const SWVTX *p0, const SWVTX *p1, const SWVTX *p2, const SWVTX *flatv)
{
    float x0 = fclampc(p0->x), y0 = fclampc(p0->y), x1 = fclampc(p1->x), y1 = fclampc(p1->y);
    float x2 = fclampc(p2->x), y2 = fclampc(p2->y);
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    float a0[NA_MAX], a1[NA_MAX], a2[NA_MAX], row[NA_MAX], cur[NA_MAX];
    sws64 X0, Y0, X1, Y1, X2, Y2, e0r, e1r, e2r, e0dx, e1dx, e2dx, e0dy, e1dy, e2dy;
    int b0, b1, b2, i, plain;
    sws32 minx, maxx, miny, maxy, x, y;
    ATTR at;
    FAST fast;

    if (area == 0 || area != area) return;
    if (area < 0) {                             /* make it clockwise */
        const SWVTX *t = p1; float tx = x1, ty = y1;
        p1 = p2; p2 = t;
        x1 = x2; y1 = y2; x2 = tx; y2 = ty;
        area = -area;
    }
    /* bounds */
    minx = ifloor(x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2));
    maxx = iceil(x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2));
    miny = ifloor(y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2));
    maxy = iceil(y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2));
    if (minx < c->clip[0]) minx = c->clip[0];
    if (miny < c->clip[1]) miny = c->clip[1];
    if (maxx > c->clip[2] - 1) maxx = c->clip[2] - 1;
    if (maxy > c->clip[3] - 1) maxy = c->clip[3] - 1;
    if (minx > maxx || miny > maxy) return;

    /* attribute planes */
    vtx_attr(c, p0, a0); vtx_attr(c, p1, a1); vtx_attr(c, p2, a2);
    at.n = NA_FIXED + c->nstages * 2;
    for (i = 0; i < at.n; i++) {
        float d1 = a1[i] - a0[i], d2 = a2[i] - a0[i];
        at.dx[i] = (d1 * (y2 - y0) - d2 * (y1 - y0)) / area;
        at.dy[i] = (d2 * (x1 - x0) - d1 * (x2 - x0)) / area;
        at.v[i] = a0[i];
    }
    at.flat = c->rs[RS_SHADEMODE] == 1;
    if (at.flat)
        for (i = 0; i < 4; i++) { at.flatc[i] = flatv->c[i]; at.flatc[4 + i] = flatv->s[i]; }

    /* edges in 28.4 fixed point; inside > 0 (or = 0 on top/left edges) */
    X0 = (sws64)(x0 * 16.0f + (x0 < 0 ? -0.5f : 0.5f)); Y0 = (sws64)(y0 * 16.0f + (y0 < 0 ? -0.5f : 0.5f));
    X1 = (sws64)(x1 * 16.0f + (x1 < 0 ? -0.5f : 0.5f)); Y1 = (sws64)(y1 * 16.0f + (y1 < 0 ? -0.5f : 0.5f));
    X2 = (sws64)(x2 * 16.0f + (x2 < 0 ? -0.5f : 0.5f)); Y2 = (sws64)(y2 * 16.0f + (y2 < 0 ? -0.5f : 0.5f));
    /* E(v_a -> v_b)(P) = (Xb - Xa)(Py - Ya) - (Yb - Ya)(Px - Xa) */
#define EDGE(XA, YA, XB, YB, PX, PY) (((XB) - (XA)) * ((PY) - (YA)) - ((YB) - (YA)) * ((PX) - (XA)))
    e0r = EDGE(X0, Y0, X1, Y1, (sws64)minx * 16, (sws64)miny * 16);
    e1r = EDGE(X1, Y1, X2, Y2, (sws64)minx * 16, (sws64)miny * 16);
    e2r = EDGE(X2, Y2, X0, Y0, (sws64)minx * 16, (sws64)miny * 16);
    e0dx = -(Y1 - Y0) * 16; e1dx = -(Y2 - Y1) * 16; e2dx = -(Y0 - Y2) * 16;
    e0dy = (X1 - X0) * 16; e1dy = (X2 - X1) * 16; e2dy = (X0 - X2) * 16;
    /* top-left rule: a top edge goes right (dy = 0, dx > 0), a left edge up */
    b0 = (Y1 - Y0 < 0 || (Y1 == Y0 && X1 > X0)) ? 0 : 1;
    b1 = (Y2 - Y1 < 0 || (Y2 == Y1 && X2 > X1)) ? 0 : 1;
    b2 = (Y0 - Y2 < 0 || (Y0 == Y2 && X0 > X2)) ? 0 : 1;
    e0r -= b0; e1r -= b1; e2r -= b2;

    for (i = 0; i < at.n; i++) row[i] = at.v[i] + at.dx[i] * ((float)minx - x0) + at.dy[i] * ((float)miny - y0);
    /* the plain case (2D games, user interfaces): Gouraud colour only */
    plain = !c->nstages && !(c->rs[RS_ZENABLE] && c->zb.fmt != SWF_NONE) && !c->rs[RS_ALPHABLENDENABLE] &&
            !c->rs[RS_ALPHATESTENABLE] && !c->rs[RS_FOGENABLE] && !c->rs[RS_SPECULARENABLE] &&
            !c->rs[RS_STENCILENABLE] && !at.flat && (sw_bpp(c->rt.fmt) == 16 || sw_bpp(c->rt.fmt) == 32);
    fast_setup(c, &at, &fast);
    for (y = miny; y <= maxy; y++) {
        /* the span of the row: where all three edge functions are >= 0 */
        sws32 xl = minx, xr = maxx;
        sws64 E[3], D[3];
        int k;
        E[0] = e0r; E[1] = e1r; E[2] = e2r;
        D[0] = e0dx; D[1] = e1dx; D[2] = e2dx;
        for (k = 0; k < 3 && xl <= xr; k++) {
            if (D[k] > 0) {
                if (E[k] < 0) {
                    sws64 st = (-E[k] + D[k] - 1) / D[k];
                    if (st > (sws64)(xr - minx)) xl = xr + 1;
                    else if (minx + (sws32)st > xl) xl = minx + (sws32)st;
                }
            } else if (D[k] < 0) {
                if (E[k] < 0) xl = xr + 1;
                else {
                    sws64 st = E[k] / -D[k];
                    if (st < (sws64)(xr - minx)) xr = minx + (sws32)st;
                }
            } else if (E[k] < 0)
                xl = xr + 1;
        }
        if (xl <= xr) {
            for (i = 0; i < at.n; i++) cur[i] = row[i] + at.dx[i] * (float)(xl - minx);
            if (plain) {
                swu8 *rp = c->rt.bits + y * c->rt.pitch;
                float r = cur[2] * 255.0f, g = cur[3] * 255.0f, b = cur[4] * 255.0f;
                float dr = at.dx[2] * 255.0f, dg = at.dx[3] * 255.0f, db = at.dx[4] * 255.0f;
                int is16 = sw_bpp(c->rt.fmt) == 16;
                for (x = xl; x <= xr; x++, r += dr, g += dg, b += db) {
                    swu32 v = ((swu32)clamp255(f2i(r)) << 16) | ((swu32)clamp255(f2i(g)) << 8) | (swu32)clamp255(f2i(b)) |
                              0xFF000000u;
                    if (is16) ((swu16 *)rp)[x] = (swu16)rt_pack(c->rt.fmt, v);
                    else ((swu32 *)rp)[x] = v;
                }
            } else if (fast.on)
                span_fast(c, &fast, y, xl, xr, cur, &at);
            else
                for (x = xl; x <= xr; x++) {
                    shade_pixel(c, x, y, cur, &at);
                    for (i = 0; i < at.n; i++) cur[i] += at.dx[i];
                }
        }
        e0r += e0dy; e1r += e1dy; e2r += e2dy;
        for (i = 0; i < at.n; i++) row[i] += at.dy[i];
    }
#undef EDGE
}

void sw_triangle(SWCTX *c, const SWVTX *a, const SWVTX *b, const SWVTX *d)
{
    float area = (b->x - a->x) * (d->y - a->y) - (d->x - a->x) * (b->y - a->y);
    swu32 cull = c->rs[RS_CULLMODE];
    if ((cull == 2 && area > 0) || (cull == 3 && area < 0)) return;
    if (c->rt.fmt == SWF_NONE || !c->rt.bits) return;
    switch (c->rs[RS_FILLMODE]) {
    case 1: sw_point(c, a); sw_point(c, b); sw_point(c, d); return;
    case 2: sw_line(c, a, b); sw_line(c, b, d); sw_line(c, d, a); return;
    }
    tri_fill(c, a, b, d, a);
}

void sw_line(SWCTX *c, const SWVTX *p, const SWVTX *q)
{
    float a0[NA_MAX], a1[NA_MAX], cur[NA_MAX];
    float dx = q->x - p->x, dy = q->y - p->y, adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy, n;
    int steps, k, i;
    ATTR at;
    if (c->rt.fmt == SWF_NONE || !c->rt.bits) return;
    vtx_attr(c, p, a0); vtx_attr(c, q, a1);
    at.n = NA_FIXED + c->nstages * 2;
    at.flat = c->rs[RS_SHADEMODE] == 1;
    if (at.flat)
        for (i = 0; i < 4; i++) { at.flatc[i] = p->c[i]; at.flatc[4 + i] = p->s[i]; }
    n = adx > ady ? adx : ady;
    if (n > 1e5f) return;
    steps = (int)n;
    if (steps < 1) steps = 1;
    for (i = 0; i < at.n; i++) { at.dx[i] = (a1[i] - a0[i]) / (float)steps; at.dy[i] = 0; }
    for (k = 0; k < steps; k++) {             /* the last pixel is not drawn */
        float t = (float)k / (float)steps;
        sws32 x = ifloor(p->x + dx * t + 0.5f), y = ifloor(p->y + dy * t + 0.5f);
        if (x < c->clip[0] || y < c->clip[1] || x >= c->clip[2] || y >= c->clip[3]) continue;
        for (i = 0; i < at.n; i++) cur[i] = a0[i] + (a1[i] - a0[i]) * t;
        shade_pixel(c, x, y, cur, &at);
    }
}

void sw_point(SWCTX *c, const SWVTX *p)
{
    float a[NA_MAX];
    sws32 x = ifloor(p->x + 0.5f), y = ifloor(p->y + 0.5f);
    int i;
    ATTR at;
    if (c->rt.fmt == SWF_NONE || !c->rt.bits) return;
    if (x < c->clip[0] || y < c->clip[1] || x >= c->clip[2] || y >= c->clip[3]) return;
    vtx_attr(c, p, a);
    at.n = NA_FIXED + c->nstages * 2;
    for (i = 0; i < at.n; i++) at.dx[i] = at.dy[i] = 0;
    at.flat = 0;
    shade_pixel(c, x, y, a, &at);
}

void sw_clear(SWCTX *c, swu32 flags, swu32 argb, float z, swu32 stencil, sws32 x0, sws32 y0, sws32 x1, sws32 y1)
{
    sws32 x, y;
    if ((flags & 1) && c->rt.bits && c->rt.fmt != SWF_NONE) {
        swu32 v = rt_pack(c->rt.fmt, argb);
        sws32 X0 = x0 < 0 ? 0 : x0, Y0 = y0 < 0 ? 0 : y0;
        sws32 X1 = x1 > c->rt.w ? c->rt.w : x1, Y1 = y1 > c->rt.h ? c->rt.h : y1;
        int bpp = sw_bpp(c->rt.fmt);
        for (y = Y0; y < Y1; y++) {
            swu8 *row = c->rt.bits + y * c->rt.pitch;
            if (bpp == 32) { swu32 *q = (swu32 *)row + X0; for (x = X0; x < X1; x++) *q++ = v; }
            else if (bpp == 16) { swu16 *q = (swu16 *)row + X0; for (x = X0; x < X1; x++) *q++ = (swu16)v; }
            else for (x = X0; x < X1; x++) rt_put(&c->rt, x, y, v);
        }
    }
    if ((flags & 6) && c->zb.bits && c->zb.fmt != SWF_NONE) {
        swu32 zm = z_max(c->zb.fmt), zv;
        sws32 X0 = x0 < 0 ? 0 : x0, Y0 = y0 < 0 ? 0 : y0;
        sws32 X1 = x1 > c->zb.w ? c->zb.w : x1, Y1 = y1 > c->zb.h ? c->zb.h : y1;
        if (z < 0) z = 0;
        if (z > 1) z = 1;
        zv = (swu32)((double)z * (double)zm);
        if ((flags & 2) && (c->zb.fmt == SWF_Z16 || ((flags & 4) || !has_stencil(c->zb.fmt)) ||
                            c->zb.fmt == SWF_Z32)) {
            /* whole values: no stencil to keep */
            swu32 v;
            z_put(&c->zb, X0, Y0 < Y1 ? Y0 : 0, zv, stencil);
            v = sw_bpp(c->zb.fmt) == 16 ? ((const swu16 *)(c->zb.bits + (Y0 < Y1 ? Y0 : 0) * c->zb.pitch))[X0]
                                        : ((const swu32 *)(c->zb.bits + (Y0 < Y1 ? Y0 : 0) * c->zb.pitch))[X0];
            for (y = Y0; y < Y1; y++) {
                swu8 *row = c->zb.bits + y * c->zb.pitch;
                if (sw_bpp(c->zb.fmt) == 16) { swu16 *q = (swu16 *)row + X0; for (x = X0; x < X1; x++) *q++ = (swu16)v; }
                else { swu32 *q = (swu32 *)row + X0; for (x = X0; x < X1; x++) *q++ = v; }
            }
        } else
        for (y = Y0; y < Y1; y++)
            for (x = X0; x < X1; x++) {
                swu32 st, old = z_get(&c->zb, x, y, &st);
                z_put(&c->zb, x, y, (flags & 2) ? zv : old, (flags & 4) ? stencil : st);
            }
    }
}
