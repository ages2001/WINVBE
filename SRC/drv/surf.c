/*
 * WINVBE - surface access: screen (LFB tiles / banked window / VGA 13h)
 * and GDI memory bitmaps (mono and device-format color, huge).
 *
 * "Working format": a row of pixels as Bpp bytes per pixel in screen
 * pixel layout; monochrome surfaces use one byte per pixel (00/FF).
 */
#include "drv.h"

BYTE bufD[ROWBUF], bufS[ROWBUF], bufP[ROWBUF];

/* far pointer into video memory for a screen offset (without fbofs) and
   the number of bytes reachable through it */
static LPBYTE scr_ptr(DWORD off, WORD *avail, BYTE rd)
{
    WORD o, hi;
    off += g.fbofs;
    o = (WORD)off;
    hi = (WORD)(off >> 16);
    switch (g.access) {
    case ACC_LFB:
        *avail = o ? (WORD)(0x10000UL - o) : 0xFFFF;
        return (LPBYTE)MK_FP(g.lfbsel + hi * g.lfbinc, o);
    case ACC_BANKED: {
        /* windows of 64 KB or less (power of two) */
        WORD wmask = (WORD)((1UL << g.wshift) - 1);
        vbe_set_bank((WORD)(off >> g.wshift));
        o &= wmask;
        *avail = (WORD)(wmask - o) + 1;
        if (!*avail) *avail = 0xFFFF;
        return (LPBYTE)MK_FP(rd ? g.winrsel : g.winsel, o);
    }
    default:
        *avail = 0xFFFF - o;
        return (LPBYTE)MK_FP(g.a000sel, o);
    }
}

/* the shadow copy: offset y * pitch + x * Bpp */
static LPBYTE sh_ptr(DWORD off, WORD *avail)
{
    WORD o = (WORD)off;
    *avail = o ? (WORD)(0x10000UL - o) : 0xFFFF;
    return (LPBYTE)MK_FP(g.shsel + (WORD)(off >> 16) * g.lfbinc, o);
}

static void vram_read(DWORD off, WORD len, BYTE *buf)
{
    while (len) {
        WORD av, c;
        LPBYTE p = scr_ptr(off, &av, 1);
        c = len < av ? len : av;
        far_memcpy((LPVOID)buf, p, c);
        buf += c; off += c; len -= c;
    }
}

static void vram_put_raw(DWORD off, WORD len, const BYTE *buf)
{
    while (len) {
        WORD av, c;
        LPBYTE p = scr_ptr(off, &av, 0);
        c = len < av ? len : av;
        far_memcpy(p, (LPVOID)buf, c);
        buf += c; off += c; len -= c;
    }
}

/* ---------------- colour correction --------------------------------- */
/* Gamma, brightness and contrast ([WINVBE] Gamma, Brightness, Contrast,
   WINVBECF > Colors), followed by the gamma ramp of SetDeviceGammaRamp /
   IDirectDrawGammaControl (Windows 9x): a table per channel. In 256
   colours it goes into the palette (DAC); in high and true colour it is
   applied to every pixel written to video memory - with the shadow frame
   buffer only (screen reads come from it, unconverted) and without the
   DIB engine. */
BYTE lut_on;                    /* the tables are not the identity */
BYTE lut_hc;                    /* ... and applied to screen writes */
BYTE lutc[3][256];              /* R, G, B */
static WORD lut_gm = 100, lut_ct = 100;         /* the WINVBE curve */
static SHORT lut_br;
static BYTE gramp_on;                           /* a gamma ramp is set */
static BYTE __far gramp[3][256];                /* its high bytes */
static WORD lut_r[64], lut_g[64], lut_b[64];    /* 15/16 bpp: channel value -> bits */
static BYTE __far lbuf[240];        /* converted pixels (240: whole pixels of 2, 3, 4 bytes) */

/* log2 of v (16.16, v > 0), 16.16 */
static long lg2(DWORD v)
{
    long r = 0;
    int k;
    while (v >= 0x20000UL) { v >>= 1; r += 0x10000L; }
    while (v < 0x10000UL) { v <<= 1; r -= 0x10000L; }
    v >>= 1;                                /* 1.15 in [1, 2) */
    for (k = 15; k >= 0; k--) {
        v = (v * v) >> 15;
        if (v >= 0x10000UL) { v >>= 1; r += 1L << k; }
    }
    return r;
}

/* 2^e (16.16, e <= 0), 16.16 */
static DWORD ex2(long e)
{
    /* 2^(2^-k) in 1.15, k = 1 .. 15 */
    static const WORD t[15] = { 46341, 38968, 35734, 34219, 33486, 33125, 32946, 32857,
                                32813, 32790, 32779, 32774, 32771, 32769, 32769 };
    long ip = e >> 16;                      /* floor */
    WORD f = (WORD)(e & 0xFFFF);
    DWORD r = 0x8000UL;                     /* 1.0 in 1.15 */
    int k;
    for (k = 0; k < 15; k++)
        if (f & (0x8000U >> k)) r = (r * t[k]) >> 15;
    r <<= 1;                                /* 16.16 */
    if (ip < -31) return 0;
    return ip < 0 ? r >> (int)-ip : r;
}

/* gamma in 1/100 (100 = none), brightness -100 .. 100, contrast in percent;
   the same curve as WINVBECF for Windows NT */
static void lut_compose(void)
{
    WORD i, gamma = lut_gm, contr = lut_ct;
    SHORT bright = lut_br;
    lut_on = 0;
    for (i = 0; i < 256; i++) {
        long y;
        if (!i) y = 0;
        else if (gamma == 100) y = ((DWORD)i << 16) / 255;
        else y = (long)ex2(lg2(((DWORD)i << 16) / 255) * 100 / (long)gamma);
        y = (y - 32768L) * (long)contr / 100 + 32768L + (long)bright * 65536L / 200;
        if (y < 0) y = 0;
        if (y > 65536L) y = 65536L;
        y = (y * 255 + 32768L) >> 16;
        if (gramp_on) {
            lutc[0][i] = gramp[0][(BYTE)y]; lutc[1][i] = gramp[1][(BYTE)y]; lutc[2][i] = gramp[2][(BYTE)y];
        } else
            lutc[0][i] = lutc[1][i] = lutc[2][i] = (BYTE)y;
        if (lutc[0][i] != i || lutc[1][i] != i || lutc[2][i] != i) lut_on = 1;
    }
}

void lut_build(WORD gamma, SHORT bright, WORD contr)
{
    if (gamma < 30 || gamma > 300) gamma = 100;
    if (bright < -100 || bright > 100) bright = 0;
    if (contr < 20 || contr > 300) contr = 100;
    lut_gm = gamma; lut_br = bright; lut_ct = contr;
    lut_compose();
}

/* the gamma ramp: 3 x 256 WORDs (R, G, B; 0 .. 65535); NULL = identity */
void lut_ramp(const WORD __far *r)
{
    WORD c, i;
    gramp_on = 0;
    for (c = 0; c < 3; c++)
        for (i = 0; i < 256; i++) {
            BYTE v = r ? (BYTE)(r[c * 256 + i] >> 8) : (BYTE)i;
            gramp[c][i] = v;
            if (v != i) gramp_on = 1;
        }
    lut_compose();
}

void lut_get_ramp(WORD __far *r)
{
    WORD c, i;
    for (c = 0; c < 3; c++)
        for (i = 0; i < 256; i++) r[c * 256 + i] = (WORD)(gramp_on ? gramp[c][i] : i) * 257;
}

/* after a mode set or a new table */
void lut_update(void)
{
    WORD i;
#ifndef OLDWIN
    lut_hc = lut_on && g.Bpp > 1 && g.shadow && !dibeng_on;
#else
    lut_hc = lut_on && g.Bpp > 1 && g.shadow;
#endif
    DBG("colour table %u %u: %u %u %u\n", lut_on, lut_hc, lutc[0][128], lutc[1][128], lutc[2][128]);
    if (!lut_hc || g.Bpp != 2) return;
    for (i = 0; i < 64; i++) {
        BYTE c5 = (BYTE)(i & 31), c6 = (BYTE)i, x5 = (BYTE)((c5 << 3) | (c5 >> 2));
        BYTE r5 = lutc[0][x5], b5 = lutc[2][x5];
        if (g.bpp == 15) {
            lut_r[i] = (WORD)(r5 >> 3) << 10; lut_g[i] = (WORD)(lutc[1][x5] >> 3) << 5; lut_b[i] = b5 >> 3;
        } else {
            lut_r[i] = (WORD)(r5 >> 3) << 11; lut_g[i] = (WORD)(lutc[1][(c6 << 2) | (c6 >> 4)] >> 2) << 5;
            lut_b[i] = b5 >> 3;
        }
    }
}

/* a screen pixel through the table */
DWORD lut_px(DWORD p)
{
    switch (g.Bpp) {
    case 2:
        if (g.bpp == 15) return lut_r[(p >> 10) & 31] | lut_g[(p >> 5) & 31] | lut_b[p & 31];
        return lut_r[(p >> 11) & 31] | lut_g[(p >> 5) & 63] | lut_b[p & 31];
    case 3: case 4:
        return (DWORD)lutc[2][(BYTE)p] | ((DWORD)lutc[1][(BYTE)(p >> 8)] << 8) | ((DWORD)lutc[0][(BYTE)(p >> 16)] << 16);
    }
    return p;
}

static void lut_row(BYTE *b, WORD len)
{
    WORD i;
    switch (g.Bpp) {
    case 2:
        for (i = 0; i < len; i += 2) *(WORD *)(b + i) = (WORD)lut_px(*(WORD *)(b + i));
        break;
    default:
        for (i = 0; i < len; i += g.Bpp) { b[i] = lutc[2][b[i]]; b[i + 1] = lutc[1][b[i + 1]]; b[i + 2] = lutc[0][b[i + 2]]; }
        break;
    }
}

static void vram_put(DWORD off, WORD len, const BYTE *buf)
{
    if (!lut_hc) { vram_put_raw(off, len, buf); return; }
    while (len) {
        WORD c = len < sizeof(lbuf) ? len : sizeof(lbuf);
        memcpy(lbuf, buf, c);
        lut_row(lbuf, c);
        vram_put_raw(off, c, lbuf);
        buf += c; off += c; len -= c;
    }
}

static void sh_read(DWORD off, WORD len, BYTE *buf)
{
    while (len) {
        WORD av, c;
        LPBYTE p = sh_ptr(off, &av);
        c = len < av ? len : av;
        far_memcpy((LPVOID)buf, p, c);
        buf += c; off += c; len -= c;
    }
}

/* shadow -> video memory, len bytes at off */
static void sh_to_vram(DWORD off, DWORD len)
{
    if (lut_hc) {                           /* in whole pixels, through the table */
        while (len) {
            WORD c = len < sizeof(lbuf) ? (WORD)len : sizeof(lbuf);
            sh_read(off, c, lbuf);
            lut_row(lbuf, c);
            vram_put_raw(off, c, lbuf);
            off += c; len -= c;
        }
        return;
    }
    while (len) {
        WORD a1, a2, c;
        LPBYTE s = sh_ptr(off, &a1);
        LPBYTE d = scr_ptr(off, &a2, 0);
        c = a1 < a2 ? a1 : a2;
        if ((DWORD)c > len) c = (WORD)len;
        far_memcpy(d, s, c);
        off += c; len -= c;
    }
}

/* ---------------- turned and scaled desktops ------------------------ */
/* GDI draws on g.lw x g.lh; desktop pixel (x, y) is shown at
     turned  90: (xres-1-y, x)   180: (xres-1-x, yres-1-y)   270: (y, yres-1-x)
     scaled: the screen pixels sdx + X, sdy + Y with x = X * lw / sdw,
             y = Y * lh / sdh (nearest, the same as Windows NT) */
static BYTE __far xbuf[240];        /* screen pixels of a scaled row */
static void fill_bytes(DWORD off, WORD len, DWORD pix, BYTE shadow);

static DWORD px_off(WORD X, WORD Y)
{
    return (DWORD)Y * g.pitch + (DWORD)X * g.Bpp;
}

static void px_wr(DWORD off, DWORD v)
{
    WORD av, i;
    LPBYTE p = scr_ptr(off, &av, 0);
    if (lut_hc) v = lut_px(v);
    switch (g.Bpp) {
    case 1: *p = (BYTE)v; break;
    case 2: *(WORD FAR *)p = (WORD)v; break;
    case 4: *(DWORD FAR *)p = v; break;
    default:                            /* 24 bpp: may cross a 64 KB tile */
        for (i = 0; i < 3; i++, v >>= 8) {
            if (i) p = scr_ptr(off + i, &av, 0);
            *p = (BYTE)v;
        }
    }
}

static DWORD px_rd(DWORD off)
{
    WORD av, i;
    LPBYTE p = scr_ptr(off, &av, 1);
    DWORD v = 0;
    switch (g.Bpp) {
    case 1: return *p;
    case 2: return *(WORD FAR *)p;
    case 4: return *(DWORD FAR *)p & PIXMASK;
    }
    for (i = 0; i < 3; i++) {
        if (i) p = scr_ptr(off + i, &av, 1);
        v |= (DWORD)*p << (8 * i);
    }
    return v;
}

/* the screen pixel that shows desktop pixel (x, y) (scaled: the first one) */
static DWORD xf_off(WORD x, WORD y)
{
    if (g.xf == 2)
        return px_off(g.sdx + (WORD)(((DWORD)x * g.sdw + g.lw - 1) / g.lw),
                      g.sdy + (WORD)(((DWORD)y * g.sdh + g.lh - 1) / g.lh));
    switch (g.rot) {
    case 1: return px_off(g.xres - 1 - y, x);
    case 2: return px_off(g.xres - 1 - x, g.yres - 1 - y);
    default: return px_off(y, g.yres - 1 - x);
    }
}

/* n desktop pixels of row y from x on to the screen */
static void xf_put(WORD y, WORD x, WORD n, const BYTE *buf)
{
    BYTE Bpp = g.Bpp;
    if (y >= g.lh || x >= g.lw) return;
    if (n > g.lw - x) n = g.lw - x;
    if (g.xf == 1) {
        /* turned: consecutive pixels go down, up or to the left */
        DWORD off = xf_off(x, y);
        long step = g.rot == 1 ? (long)g.pitch : g.rot == 2 ? -(long)Bpp : -(long)g.pitch;
        WORD i;
        for (i = 0; i < n; i++, buf += Bpp, off += step)
            px_wr(off, pix_get(buf, Bpp));
        return;
    }
    {
        /* scaled: screen columns X0..X1 and rows Y0..Y1 show these pixels */
        WORD X0 = (WORD)(((DWORD)x * g.sdw + g.lw - 1) / g.lw);
        WORD X1 = (WORD)(((DWORD)(x + n) * g.sdw + g.lw - 1) / g.lw);
        WORD Y0 = (WORD)(((DWORD)y * g.sdh + g.lh - 1) / g.lh);
        WORD Y1 = (WORD)(((DWORD)(y + 1) * g.sdh + g.lh - 1) / g.lh);
        DWORD num = (DWORD)X0 * g.lw;
        WORD sx = (WORD)(num / g.sdw), rem = (WORD)(num % g.sdw);
        WORD per = sizeof(xbuf) / Bpp;
        while (X0 < X1) {
            WORD c = X1 - X0 < per ? X1 - X0 : per, i, Y;
            BYTE *d = xbuf;
            for (i = 0; i < c; i++, d += Bpp) {
                DWORD v = pix_get(buf + (sx - x) * Bpp, Bpp);
                pix_put(d, lut_hc ? lut_px(v) : v, Bpp);
                rem += g.lw;
                while (rem >= g.sdw) { rem -= g.sdw; sx++; }
            }
            for (Y = Y0; Y < Y1; Y++)
                vram_put_raw(px_off(g.sdx + X0, g.sdy + Y), c * Bpp, xbuf);
            X0 += c;
        }
    }
}

/* the same backwards (no shadow buffer): n desktop pixels from the screen */
static void xf_get(WORD y, WORD x, WORD n, BYTE *buf)
{
    WORD i;
    for (i = 0; i < n; i++, buf += g.Bpp)
        pix_put(buf, (y < g.lh && x + i < g.lw) ? px_rd(xf_off(x + i, y)) : 0, g.Bpp);
}

/* the whole mode black (the borders of a scaled desktop) */
void vram_clear(void)
{
    WORD y;
    if (g.in_background) return;
    for (y = 0; y < g.yres; y++)
        fill_bytes(px_off(0, y), g.xres * g.Bpp, 0, 0);
}

void scr_read(WORD y, WORD x, WORD n, BYTE *buf)
{
    WORD len = n * g.Bpp;
    if (g.shadow) { sh_read((DWORD)y * g.spitch + (DWORD)x * g.Bpp, len, buf); return; }
    if (g.in_background) { memset(buf, 0, len); return; }
    if (g.xf) { xf_get(y, x, n, buf); return; }
    vram_read((DWORD)y * g.pitch + (DWORD)x * g.Bpp, len, buf);
}

extern const char *trace_last;
int cursor_hit(WORD y, WORD x, WORD n);
#define CHK_CURSOR(y, x, n) if (g.debug && cursor_hit(y, x, n)) dbg_printf("cursor hit: %s %u,%u %u\n", trace_last, x, y, n)

void scr_write(WORD y, WORD x, WORD n, const BYTE *buf)
{
    DWORD off = (DWORD)y * g.pitch + (DWORD)x * g.Bpp;
    WORD len = n * g.Bpp;
    CHK_CURSOR(y, x, n);
    if (g.shadow) {
        DWORD o = (DWORD)y * g.spitch + (DWORD)x * g.Bpp;
        WORD l = len;
        const BYTE *b = buf;
        while (l) {
            WORD av, c;
            LPBYTE p = sh_ptr(o, &av);
            c = l < av ? l : av;
            far_memcpy(p, (LPVOID)b, c);
            b += c; o += c; l -= c;
        }
    }
    if (g.in_background) return;
    if (g.xf) { xf_put(y, x, n, buf); return; }
    vram_put(off, len, buf);
}

/* video memory only: the mouse cursor, which is never part of the shadow */
void vram_write(WORD y, WORD x, WORD n, const BYTE *buf)
{
    if (g.in_background) return;
    if (g.xf) { xf_put(y, x, n, buf); return; }
    vram_put((DWORD)y * g.pitch + (DWORD)x * g.Bpp, n * g.Bpp, buf);
}

/* copy a rectangle of the shadow to the screen (DCI, switch back from a
   full-screen MS-DOS box) */
void shadow_flush(WORD l, WORD t, WORD r, WORD b)
{
    if (!g.shadow || g.in_background) return;
    if (r > g.lw) r = g.lw;
    if (b > g.lh) b = g.lh;
    if (l >= r || t >= b) return;
    if (g.xf) {                         /* through lbuf, which only vram_put uses */
        WORD per = sizeof(lbuf) / g.Bpp;
        for (; t < b; t++) {
            WORD x;
            for (x = l; x < r; x += per) {
                WORD c = r - x < per ? r - x : per;
                sh_read((DWORD)t * g.spitch + (DWORD)x * g.Bpp, c * g.Bpp, lbuf);
                xf_put(t, x, c, lbuf);
            }
        }
        return;
    }
    for (; t < b; t++)
        sh_to_vram((DWORD)t * g.pitch + (DWORD)l * g.Bpp, (DWORD)(r - l) * g.Bpp);
}

void pix_fill(BYTE *buf, WORD n, DWORD pix, BYTE Bpp)
{
    WORD i;
    switch (Bpp) {
    case 1: memset(buf, (BYTE)pix, n); break;
    case 2: { WORD *w = (WORD *)buf; WORD v = (WORD)pix; for (i = 0; i < n; i++) w[i] = v; } break;
    case 3: {
        BYTE a = (BYTE)pix, b = (BYTE)(pix >> 8), c = (BYTE)(pix >> 16);
        for (i = 0; i < n; i++) { buf[0] = a; buf[1] = b; buf[2] = c; buf += 3; }
        } break;
    default: { DWORD *d = (DWORD *)buf; DWORD v = pix & PIXMASK; for (i = 0; i < n; i++) d[i] = v; } break;
    }
}

DWORD pix_get(const BYTE *p, BYTE Bpp)
{
    switch (Bpp) {
    case 1: return p[0];
    case 2: return *(const WORD *)p;
    case 3: return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16);
    default: return *(const DWORD *)p & PIXMASK;
    }
}

void pix_put(BYTE *p, DWORD v, BYTE Bpp)
{
    switch (Bpp) {
    case 1: p[0] = (BYTE)v; break;
    case 2: *(WORD *)p = (WORD)v; break;
    case 3: p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); break;
    default: *(DWORD *)p = v & PIXMASK; break;
    }
}

/* fill len bytes of far memory with pixels (8, 16, 32 bpp; whole pixels,
   a 16/32-bit pixel never crosses a 64 KB tile) */
static void span_fill(LPBYTE p, WORD len, DWORD pix)
{
    switch (g.Bpp) {
    case 1: far_memset(p, (BYTE)pix, len); break;
    case 2: {
        DWORD v = (pix & 0xFFFF) | (pix << 16);
        if (FP_OFF(p) & 2) { *(WORD FAR *)p = (WORD)pix; p += 2; len -= 2; }
        far_memset32(p, v, len >> 2);
        if (len & 2) *(WORD FAR *)(p + (len & ~3)) = (WORD)pix;
        } break;
    default: far_memset32(p, pix & PIXMASK, len >> 2); break;
    }
}

/* fill a span: through the given pointer function, in tile-sized pieces */
static void fill_bytes(DWORD off, WORD len, DWORD pix, BYTE shadow)
{
    while (len) {
        WORD av, c;
        LPBYTE p = shadow ? sh_ptr(off, &av) : scr_ptr(off, &av, 0);
        c = len < av ? len : av;
        span_fill(p, c, pix);
        off += c; len -= c;
    }
}

void scr_fill(WORD y, WORD x, WORD n, DWORD pix)
{
    DWORD off = (DWORD)y * g.pitch + (DWORD)x * g.Bpp;
    CHK_CURSOR(y, x, n);
    if (g.Bpp != 3 && !g.xf) {
        WORD len = n * g.Bpp;
        if (g.shadow) fill_bytes(off, len, pix, 1);
        if (!g.in_background) fill_bytes(off, len, lut_hc ? lut_px(pix) : pix, 0);
        return;
    }
    /* 24 bpp: a buffer of pixels, written in chunks */
    while (n) {
        WORD c = n > CHUNKPX ? CHUNKPX : n;
        pix_fill(bufP, c, pix, g.Bpp);
        scr_write(y, x, c, bufP);
        x += c; n -= c;
    }
}

/* ---------------- memory bitmaps ------------------------------------ */

/* a memory bitmap of more than 64 KB without the tiled layout of GDI
   (bmSegmentIndex = 0): one segment with a 32-bit limit - its rows are
   reached through a selector of their own (a few in turn: source,
   destination and pattern can be such bitmaps at the same time) */
static WORD flat_sel[4];
static BYTE flat_next;

static void surf_flat(SURF *s, DWORD size)
{
#ifndef OLDWIN
    WORD *ps;
    if (!g.pmode || s->scanseg || size <= 0x10000UL) return;
    ps = &flat_sel[flat_next++ & 3];
    if (!*ps) {
        *ps = sel_alloc(1);
        if (!*ps || !sel_setlimit(*ps, 0xFFFFUL)) { *ps = 0; return; }
    }
    s->flat = sel_getbase(s->sel) + s->off;
    s->rsel = *ps;
#endif
}

void surf_from_dev(SURF *s, LPVOID lpDev)
{
    LPGBITMAP bm = (LPGBITMAP)lpDev;
    memset(s, 0, sizeof(*s));
    if (bm->bmType != 0) {
#ifdef DEDBG
        {
            extern LPVOID scr_pdev;
            static WORD nlog;
            if ((LPVOID)bm != scr_pdev && nlog < 40) {
                LPDIBENGINE de = (LPDIBENGINE)bm;
                nlog++;
                dbg_printf("foreign PDEVICE %lx type %x %ux%ux%u fl %x bits %x:%lx delta %ld\n", (DWORD)bm, de->deType,
                           de->deWidth, de->deHeight, de->deBitsPixel, de->deFlags, de->deBitsSelector, de->deBitsOffset, de->deDeltaScan);
            }
        }
#endif
        s->screen = 1;
        s->Bpp = g.Bpp;
        s->width = g.lw;                /* the desktop (turned or scaled: not the mode) */
        s->height = g.lh;
        s->pitch = g.spitch;
        return;
    }
    s->mono = (bm->bmBitsPixel == 1 && bm->bmPlanes == 1);
    if (bm->bmPlanes > 1 && bm->bmBitsPixel == 1) {
        s->planar = bm->bmPlanes;
        s->Bpp = 1;
        s->width = bm->bmWidth;
        s->height = bm->bmHeight;
        s->pitch = bm->bmWidthBytes;
        s->sel = FP_SEG(bm->bmBits);
        s->off = FP_OFF(bm->bmBits);
        s->scanseg = bm->bmSegmentIndex ? bm->bmScanSegment : 0;
        s->seginc = bm->bmSegmentIndex;
        surf_flat(s, (DWORD)s->pitch * s->planar * s->height);
        return;
    }
    s->Bpp = s->mono ? 1 : (BYTE)((bm->bmBitsPixel + 7) >> 3);
    s->width = bm->bmWidth;
    s->height = bm->bmHeight;
    s->pitch = bm->bmWidthBytes;
    s->sel = FP_SEG(bm->bmBits);
    s->off = FP_OFF(bm->bmBits);
    s->scanseg = bm->bmSegmentIndex ? bm->bmScanSegment : 0;
    s->seginc = bm->bmSegmentIndex;
    surf_flat(s, (DWORD)s->pitch * s->height);
}

LPBYTE mem_row(SURF *s, WORD y)
{
    WORD pitch = s->planar ? s->pitch * s->planar : s->pitch;
    if (s->flat) {
        sel_setbase(s->rsel, s->flat + (DWORD)y * pitch);
        return (LPBYTE)MK_FP(s->rsel, 0);
    }
    if (s->planar) {
        if (s->scanseg) {
            WORD seg = y / s->scanseg;
            WORD r = y - seg * s->scanseg;
            return (LPBYTE)MK_FP(s->sel + seg * s->seginc, s->off + r * pitch);
        }
        return (LPBYTE)MK_FP(s->sel, s->off + (WORD)((DWORD)y * pitch));
    }
    if (s->scanseg) {
        WORD seg = y / s->scanseg;
        WORD r = y - seg * s->scanseg;
        return (LPBYTE)MK_FP(s->sel + seg * s->seginc, s->off + r * s->pitch);
    }
    return (LPBYTE)MK_FP(s->sel, s->off + (WORD)((DWORD)y * s->pitch));
}

/* read n pixels from (x,y) into working format */
void row_read(SURF *s, WORD y, WORD x, WORD n, BYTE *buf)
{
    LPBYTE p;
    if (s->screen) { scr_read(y, x, n, buf); return; }
    p = mem_row(s, y);
    if (s->planar) {
        /* EGA/VGA interleaved planes -> 4-bit color index per byte */
        WORD i;
        for (i = 0; i < n; i++) {
            WORD xx = x + i, pl;
            BYTE bit = (BYTE)(0x80 >> (xx & 7)), v = 0;
            for (pl = 0; pl < s->planar && pl < 4; pl++)
                if (p[pl * s->pitch + (xx >> 3)] & bit) v |= (BYTE)(1 << pl);
            buf[i] = v;
        }
        return;
    }
    if (!s->mono) {
        far_memcpy((LPVOID)buf, p + x * s->Bpp, n * s->Bpp);
        return;
    }
    {
        LPBYTE q = p + (x >> 3);
        BYTE bit = (BYTE)(0x80 >> (x & 7));
        BYTE v = *q++;
        while (n--) {
            *buf++ = (v & bit) ? 0xFF : 0x00;
            bit >>= 1;
            if (!bit) { bit = 0x80; if (n) v = *q++; }
        }
    }
}

void row_write(SURF *s, WORD y, WORD x, WORD n, const BYTE *buf)
{
    LPBYTE p;
    if (s->screen) { scr_write(y, x, n, buf); return; }
    if (s->planar) return;
    p = mem_row(s, y);
    if (!s->mono) {
        far_memcpy(p + x * s->Bpp, (LPVOID)buf, n * s->Bpp);
        return;
    }
    {
        LPBYTE q = p + (x >> 3);
        BYTE bit = (BYTE)(0x80 >> (x & 7));
        BYTE v = *q;
        while (n--) {
            if (*buf++ & 0x80) v |= bit; else v &= ~bit;
            bit >>= 1;
            if (!bit) { *q++ = v; bit = 0x80; if (n) v = *q; }
        }
        if (bit != 0x80) *q = v;
    }
}
