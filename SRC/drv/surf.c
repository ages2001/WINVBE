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

static void vram_put(DWORD off, WORD len, const BYTE *buf)
{
    while (len) {
        WORD av, c;
        LPBYTE p = scr_ptr(off, &av, 0);
        c = len < av ? len : av;
        far_memcpy(p, (LPVOID)buf, c);
        buf += c; off += c; len -= c;
    }
}

/* shadow -> video memory, len bytes at off */
static void sh_to_vram(DWORD off, DWORD len)
{
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

void scr_read(WORD y, WORD x, WORD n, BYTE *buf)
{
    DWORD off = (DWORD)y * g.pitch + (DWORD)x * g.Bpp;
    WORD len = n * g.Bpp;
    if (g.shadow) {
        while (len) {
            WORD av, c;
            LPBYTE p = sh_ptr(off, &av);
            c = len < av ? len : av;
            far_memcpy((LPVOID)buf, p, c);
            buf += c; off += c; len -= c;
        }
        return;
    }
    if (g.in_background) { memset(buf, 0, len); return; }
    vram_read(off, len, buf);
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
        DWORD o = off;
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
    vram_put(off, len, buf);
}

/* video memory only: the mouse cursor, which is never part of the shadow */
void vram_write(WORD y, WORD x, WORD n, const BYTE *buf)
{
    if (g.in_background) return;
    vram_put((DWORD)y * g.pitch + (DWORD)x * g.Bpp, n * g.Bpp, buf);
}

/* copy a rectangle of the shadow to the screen (DCI, switch back from a
   full-screen MS-DOS box) */
void shadow_flush(WORD l, WORD t, WORD r, WORD b)
{
    if (!g.shadow || g.in_background) return;
    if (r > g.xres) r = g.xres;
    if (b > g.yres) b = g.yres;
    if (l >= r || t >= b) return;
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
    if (g.Bpp != 3) {
        WORD len = n * g.Bpp;
        if (g.shadow) fill_bytes(off, len, pix, 1);
        if (!g.in_background) fill_bytes(off, len, pix, 0);
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
        s->screen = 1;
        s->Bpp = g.Bpp;
        s->width = g.xres;
        s->height = g.yres;
        s->pitch = g.pitch;
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
