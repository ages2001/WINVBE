/*
 * WINVBE - device independent bitmaps:
 *   DeviceBitmapBits (Get/SetDIBits), DIBToDevice (SetDIBitsToDevice),
 *   CreateBitmap (not used).
 * Supports 1/4/8/16/24/32 bpp DIBs (16/32: BI_RGB and BI_BITFIELDS), BI_RLE8
 * and BI_RLE4 on input.  Windows 9x GDI hands a DIB in the format of the
 * screen (16 bpp on a 16 bpp screen, 32 on 32) to the driver unconverted;
 * SetDIBits of a 32 bpp DIB failed on a 32 bpp screen, which made Delphi
 * programs (TImageList data) fail at start-up.
 */
#include "drv.h"

#define BI_RGB  0
#define BI_RLE8 1
#define BI_RLE4 2
#define BI_BITFIELDS 3

typedef struct {
    WORD  width, height;
    WORD  bits;
    WORD  comp;
    WORD  ncolors;
    BYTE  core;          /* BITMAPCOREHEADER */
    LPBYTE colors;
    DWORD stride;
    BYTE  topdown;       /* negative biHeight: scan 0 is the top row in memory */
    /* 16/32 bpp: red, green, blue masks as shift and width */
    BYTE  sh[3], wd[3];
    DWORD mask[3];
} DIBINFO;

/* byte offset of DIB scan 'scan' (0 = bottom row, as GDI counts them) in
   a buffer holding the whole DIB (StretchDIBits) */
#define SCANOFF(di, scan) ((DWORD)((di)->topdown ? (di)->height - 1 - (scan) : (scan)) * (di)->stride)
/* DeviceBitmapBits and DIBToDevice (Get/SetDIBits, SetDIBitsToDevice) get
   a buffer with just the scans [start, start + count): scan start + k is
   at k (top-down: count - 1 - k). Explorer reads its image lists in bands
   of 256 scans - offsets from scan 0 wrote past the end of its buffer */
#define BANDOFF(di, k, count) ((DWORD)((di)->topdown ? (count) - 1 - (k) : (k)) * (di)->stride)

static DWORD cmap[256];          /* DIB color index -> physical */
#define rlebuf bufT                /* decoded RLE scan (indices) */

static int dib_parse(DIBINFO *di, LPBYTE bmi)
{
    DWORD hs = *(LPDWORD)bmi;
    di->topdown = 0;
    if (hs == 12) {
        BMCHEADER FAR *h = (BMCHEADER FAR *)bmi;
        di->width = h->bcWidth; di->height = h->bcHeight; di->bits = h->bcBitCount;
        di->comp = BI_RGB; di->core = 1;
        di->ncolors = di->bits <= 8 ? 1 << di->bits : 0;
    } else {
        BMIHEADER FAR *h = (BMIHEADER FAR *)bmi;
        LONG ht = h->biHeight;
        if (h->biWidth <= 0 || h->biWidth > 32767 || ht == 0 || ht > 32767 || ht < -32767) return 0;
        di->topdown = ht < 0;
        di->width = (WORD)h->biWidth; di->height = (WORD)(ht < 0 ? -ht : ht); di->bits = h->biBitCount;
        di->comp = (WORD)h->biCompression; di->core = 0;
        di->ncolors = di->bits <= 8 ? 1 << di->bits : 0;
        if (h->biClrUsed && h->biClrUsed < di->ncolors) di->ncolors = (WORD)h->biClrUsed;
    }
    if (di->bits != 1 && di->bits != 4 && di->bits != 8 && di->bits != 16 && di->bits != 24 && di->bits != 32)
        return 0;
    if (di->comp == BI_BITFIELDS && di->bits != 16 && di->bits != 32) return 0;
    if (di->topdown && di->comp != BI_RGB && di->comp != BI_BITFIELDS) return 0;  /* RLE: bottom-up */
    di->colors = bmi + (WORD)hs;
    if (di->bits == 16 || di->bits == 32) {
        WORD i;
        if (di->comp == BI_BITFIELDS) {
            /* the masks follow a BITMAPINFOHEADER; a V4/V5 header holds
               them at the same offset */
            LPDWORD m = (LPDWORD)(bmi + 40);
            di->mask[0] = m[0]; di->mask[1] = m[1]; di->mask[2] = m[2];
        } else if (di->comp == BI_RGB) {
            if (di->bits == 16) { di->mask[0] = 0x7C00; di->mask[1] = 0x03E0; di->mask[2] = 0x001F; }
            else { di->mask[0] = 0xFF0000UL; di->mask[1] = 0xFF00; di->mask[2] = 0xFF; }
        } else
            return 0;
        for (i = 0; i < 3; i++) {
            DWORD m = di->mask[i];
            BYTE sh = 0, wd = 0;
            if (!m) return 0;
            while (!(m & 1)) { m >>= 1; sh++; }
            while (m & 1) { m >>= 1; wd++; }
            di->sh[i] = sh; di->wd[i] = wd;
        }
        di->ncolors = 0;
    }
    di->stride = (((DWORD)di->width * di->bits + 31) >> 5) << 2;
    return 1;
}

static DWORD dib_rgb(DIBINFO *di, WORD i)
{
    LPBYTE c = di->colors + i * (di->core ? 3 : 4);
    return (DWORD)c[2] | ((DWORD)c[1] << 8) | ((DWORD)c[0] << 16);
}

/* 16/32 bpp DIB pixel -> RGB (COLORREF order) and back */
static BYTE chan_get(DWORD v, BYTE sh, BYTE wd)
{
    DWORD c = (v >> sh) & (wd >= 32 ? 0xFFFFFFFFUL : (1UL << wd) - 1);
    if (wd >= 8) return (BYTE)(c >> (wd - 8));
    c <<= 8 - wd;                       /* replicate the top bits */
    return (BYTE)(c | (c >> wd) | (wd < 4 ? c >> (2 * wd) : 0));
}

static DWORD dib_hi_rgb(DIBINFO *di, const BYTE *p)
{
    DWORD v = di->bits == 16 ? (DWORD)p[0] | ((DWORD)p[1] << 8) : *(const DWORD *)p;
    if (di->comp == BI_RGB && di->bits == 32) return (DWORD)p[2] | ((DWORD)p[1] << 8) | ((DWORD)p[0] << 16);
    return (DWORD)chan_get(v, di->sh[0], di->wd[0]) | ((DWORD)chan_get(v, di->sh[1], di->wd[1]) << 8) |
           ((DWORD)chan_get(v, di->sh[2], di->wd[2]) << 16);
}

static DWORD chan_put(BYTE c, BYTE sh, BYTE wd)
{
    DWORD v = wd >= 8 ? (DWORD)c << (wd - 8) : (DWORD)(c >> (8 - wd));
    return v << sh;
}

static void dib_hi_put(DIBINFO *di, BYTE *o, DWORD rgb)
{
    DWORD v;
    if (di->comp == BI_RGB && di->bits == 32) {
        o[0] = (BYTE)(rgb >> 16); o[1] = (BYTE)(rgb >> 8); o[2] = (BYTE)rgb; o[3] = 0;
        return;
    }
    v = chan_put((BYTE)rgb, di->sh[0], di->wd[0]) | chan_put((BYTE)(rgb >> 8), di->sh[1], di->wd[1]) |
        chan_put((BYTE)(rgb >> 16), di->sh[2], di->wd[2]);
    o[0] = (BYTE)v; o[1] = (BYTE)(v >> 8);
    if (di->bits == 32) { o[2] = (BYTE)(v >> 16); o[3] = (BYTE)(v >> 24); }
}

/* physical color of an RGB for DIB conversion (palette: full match) */
/* GDI DeviceColorMatch (GDI.449): maps an RGB to the DC's palette */
static LPVOID cur_cinfo;
static BYTE dcm_idx[4096];
static BYTE dcm_ok[512];

static void dcm_begin(LPVOID cinfo)
{
    cur_cinfo = (g.palettized && g.devcolormatch) ? cinfo : NULL;
    memset(dcm_ok, 0, sizeof(dcm_ok));
}

static BYTE match8(BYTE r, BYTE gg, BYTE b)
{
    WORD key;
    if (!cur_cinfo) return nearest_index(r, gg, b);
    key = ((WORD)(r >> 4) << 8) | ((gg >> 4) << 4) | (b >> 4);
    if (!(dcm_ok[key >> 3] & (1 << (key & 7)))) {
        typedef DWORD (__far __pascal *DCMPROC)(DWORD, LPVOID);
        DWORD bgr = ((DWORD)r << 16) | ((DWORD)gg << 8) | b;
        dcm_idx[key] = (BYTE)((DCMPROC)g.devcolormatch)(bgr, cur_cinfo);
        dcm_ok[key >> 3] |= (BYTE)(1 << (key & 7));
    }
    return dcm_idx[key];
}

static const SHORT bayer4d[16] = { -60, 4, -44, 20, 36, -28, 52, -12, -36, 28, -52, 12, 60, -4, 44, -20 };

static BYTE dither8(DWORD rgb, WORD x, WORD y)
{
    BYTE r = (BYTE)rgb, gg = (BYTE)(rgb >> 8), b = (BYTE)(rgb >> 16);
    BYTE idx = match8(r, gg, b);
    SHORT t, rr, g2, bb;
    if (g.pal[idx * 4] == r && g.pal[idx * 4 + 1] == gg && g.pal[idx * 4 + 2] == b)
        return idx;
    t = bayer4d[((y & 3) << 2) | (x & 3)];
    rr = r + t; g2 = gg + t; bb = b + t;
    if (rr < 0) rr = 0; if (rr > 255) rr = 255;
    if (g2 < 0) g2 = 0; if (g2 > 255) g2 = 255;
    if (bb < 0) bb = 0; if (bb > 255) bb = 255;
    return match8((BYTE)rr, (BYTE)g2, (BYTE)bb);
}

static DWORD dib_phys(DWORD rgb, BYTE mono)
{
    if (mono) return rgb_is_white(rgb) ? phys_white() : 0;
    if (g.pf == PF_8)
        return match8((BYTE)rgb, (BYTE)(rgb >> 8), (BYTE)(rgb >> 16));
    return rgb_to_phys(rgb);
}

static void build_cmap(DIBINFO *di, BYTE mono, LPWORD xlat)
{
    WORD i;
    for (i = 0; i < 256; i++) cmap[i] = 0;
    for (i = 0; i < di->ncolors; i++) {
        if (g.dibwords) {
            /* Windows 3.0 GDI: color table already holds 16-bit physical colors */
            DWORD p = word_to_phys(((LPWORD)di->colors)[i]);
            cmap[i] = mono ? (rgb_is_white(phys_to_rgb(p)) ? phys_white() : 0) : p;
        } else
            cmap[i] = dib_phys(dib_rgb(di, i), mono);
    }
}

/* huge pointer helpers: GDI's tiled huge pointers (selector + n * __AHINCR
   per 64 KB); a selector with a limit above 64 KB (Win32 DIB bits passed
   through a 32-bit segment) is addressed through a selector of our own */
DWORD seg_limit(WORD sel);
#pragma aux seg_limit = \
    ".386" \
    "movzx eax,ax" \
    "lsl eax,eax" \
    "jz ok" \
    "xor eax,eax" \
    "ok:" \
    "mov edx,eax" \
    "shr edx,16" \
    parm [ax] value [dx ax];

static WORD big_sel;

static LPBYTE hptr(LPBYTE base, DWORD off, WORD *avail)
{
    DWORD lin = (DWORD)FP_OFF(base) + off;
    WORD o = (WORD)lin;
#ifndef OLDWIN
    if (g.pmode && lin > 0xFFFFUL && seg_limit(FP_SEG(base)) > 0xFFFFUL) {
        if (!big_sel) {
            big_sel = sel_alloc(1);
            if (big_sel && !sel_setlimit(big_sel, 0xFFFFUL)) { sel_free(big_sel, 1); big_sel = 0; }
        }
        if (big_sel && sel_setbase(big_sel, sel_getbase(FP_SEG(base)) + lin)) {
            *avail = 0xFFFF;
            return (LPBYTE)MK_FP(big_sel, 0);
        }
    }
#endif
    *avail = o ? (WORD)(0x10000UL - o) : 0xFFFF;
    return (LPBYTE)MK_FP(FP_SEG(base) + (WORD)(lin >> 16) * g.ahincr, o);
}

static void huge_read(LPBYTE base, DWORD off, WORD len, BYTE FAR *dst)
{
    while (len) {
        WORD av, c;
        LPBYTE p = hptr(base, off, &av);
        c = len < av ? len : av;
        far_memcpy(dst, p, c);
        dst += c; off += c; len -= c;
    }
}

static void huge_write(LPBYTE base, DWORD off, WORD len, const BYTE FAR *src)
{
    while (len) {
        WORD av, c;
        LPBYTE p = hptr(base, off, &av);
        c = len < av ? len : av;
        far_memcpy(p, src, c);
        src += c; off += c; len -= c;
    }
}

/* convert n DIB pixels starting at x0 of a raw scan (in bufS) to working format in bufD */
static void dib_to_work(DIBINFO *di, const BYTE *raw, WORD x0, WORD n, BYTE mono, BYTE Bpp, WORD dx, WORD dy)
{
    WORD i;
    BYTE *o = bufD;
    if (di->bits >= 16) {
        BYTE step = (BYTE)(di->bits >> 3);
        const BYTE *p = raw + x0 * step;
        for (i = 0; i < n; i++, p += step) {
            DWORD rgb = di->bits == 24 ? (DWORD)p[2] | ((DWORD)p[1] << 8) | ((DWORD)p[0] << 16) : dib_hi_rgb(di, p);
            if (mono) *o++ = rgb_is_white(rgb) ? 0xFF : 0;
            else if (g.pf == PF_8) *o++ = dither8(rgb, dx + i, dy);
            else { pix_put(o, pix_of(dib_phys(rgb, 0)), Bpp); o += Bpp; }
        }
        return;
    }
    for (i = 0; i < n; i++) {
        WORD x = x0 + i, idx;
        if (di->bits == 8) idx = raw[x];
        else if (di->bits == 4) idx = (raw[x >> 1] >> ((x & 1) ? 0 : 4)) & 0x0F;
        else idx = (raw[x >> 3] >> (7 - (x & 7))) & 1;
        if (mono) *o++ = mono_of(cmap[idx]);
        else if (g.pf == PF_8 && !g.dibwords) *o++ = dither8(dib_rgb(di, idx), dx + i, dy);
        else { pix_put(o, pix_of(cmap[idx]), Bpp); o += Bpp; }
    }
}

/* ---------------- RLE decoding --------------------------------------- */
typedef struct {
    LPBYTE base;
    DWORD pos;
    WORD  x, y;          /* next pixel position in DIB coordinates */
    BYTE  eob;
} RLESTATE;

static BYTE rle_byte(RLESTATE *r)
{
    WORD av;
    BYTE b = *hptr(r->base, r->pos, &av);
    r->pos++;
    return b;
}

static void rle_put(DIBINFO *di, RLESTATE *r, BYTE v, WORD scan)
{
    if (r->y == scan && r->x < di->width && r->x < ROWBUF)
        rlebuf[r->x] = v;
    r->x++;
}

/* decode until scan 'scan' is complete; rlebuf receives its indices */
static void rle_decode_scan(DIBINFO *di, RLESTATE *r, WORD scan)
{
    BYTE four = (di->comp == BI_RLE4);
    memset(rlebuf, 0, ROWBUF);
    while (!r->eob && r->y <= scan) {
        BYTE cnt = rle_byte(r);
        BYTE val = rle_byte(r);
        if (cnt) {
            WORD k;
            for (k = 0; k < cnt; k++) {
                BYTE v = four ? ((k & 1) ? (val & 0x0F) : (val >> 4)) : val;
                rle_put(di, r, v, scan);
            }
            continue;
        }
        switch (val) {
        case 0: r->x = 0; r->y++; break;                       /* end of line */
        case 1: r->eob = 1; break;                             /* end of bitmap */
        case 2: r->x += rle_byte(r); r->y += rle_byte(r); break; /* delta */
        default: {                                             /* absolute run */
            WORD k, nbytes;
            BYTE b = 0;
            for (k = 0; k < val; k++) {
                BYTE v;
                if (four) {
                    if (!(k & 1)) b = rle_byte(r);
                    v = (k & 1) ? (b & 0x0F) : (b >> 4);
                } else
                    v = rle_byte(r);
                rle_put(di, r, v, scan);
            }
            nbytes = four ? (val + 1) / 2 : val;
            if (nbytes & 1) rle_byte(r);
        }
        }
        if (r->y > scan) break;
    }
}

/* ---------------- set: DIB -> surface ---------------------------------- */
/*
 * draws DIB scans [start, start+count) onto surface rows; DIB scan k
 * goes to row ytop + (start + count - 1 - k), columns dx.. ; clipped
 */
static WORD dib_set(SURF *s, SHORT dx, SHORT ytop, WORD start, WORD count, LPBYTE bits,
                    DIBINFO *di, LPGRECT clip, LPWORD xlat)
{
    WORD k;
    SHORT cl = 0, ct = 0, cr = s->width, cb = s->height;
    RLESTATE rs;
    BYTE rle = (di->comp == BI_RLE8 || di->comp == BI_RLE4);
    if (di->comp != BI_RGB && di->comp != BI_BITFIELDS && !rle) return 0;
    if (clip) {
        if (clip->left > cl) cl = clip->left;
        if (clip->top > ct) ct = clip->top;
        if (clip->right < cr) cr = clip->right;
        if (clip->bottom < cb) cb = clip->bottom;
    }
    build_cmap(di, s->mono, xlat);
    if (rle) {
        memset(&rs, 0, sizeof(rs));
        rs.base = bits;
        rs.y = 0;
        /* RLE data always starts at scan 0 of the bitmap */
    }
    for (k = 0; k < count; k++) {
        WORD scan = start + k;
        SHORT row = ytop + (SHORT)(count - 1 - k);
        SHORT x0 = dx, x1 = dx + di->width;
        WORD sx0;
        if (rle) rle_decode_scan(di, &rs, scan);
        else if (scan >= di->height) continue;
        if (row < ct || row >= cb) continue;
        if (x0 < cl) x0 = cl;
        if (x1 > cr) x1 = cr;
        if (x1 <= x0) continue;
        sx0 = x0 - dx;
        while (x0 < x1) {
            WORD n = x1 - x0 > CHUNKPX ? CHUNKPX : x1 - x0;
            const BYTE *raw;
            WORD rawx;
            if (rle) {
                raw = rlebuf; rawx = sx0;
                /* rlebuf holds indices one per byte: emulate 8-bit */
                {
                    WORD bits_save = di->bits, i;
                    BYTE *o = bufD;
                    for (i = 0; i < n; i++) {
                        WORD idx = (sx0 + i) < ROWBUF ? rlebuf[sx0 + i] : 0;
                        if (s->mono) *o++ = mono_of(cmap[idx]);
                        else { pix_put(o, pix_of(cmap[idx]), s->Bpp); o += s->Bpp; }
                    }
                    (void)bits_save; (void)raw; (void)rawx;
                }
            } else {
                DWORD rowoff = BANDOFF(di, k, count);
                WORD bx0 = (WORD)(((DWORD)sx0 * di->bits) >> 3);
                WORD bx1 = (WORD)((((DWORD)(sx0 + n) * di->bits) + 7) >> 3);
                huge_read(bits, rowoff + bx0, bx1 - bx0, bufS);
                /* re-base x so that bufS[0] corresponds to byte bx0 */
                dib_to_work(di, bufS, (WORD)(sx0 - (((DWORD)bx0 << 3) / di->bits)), n, s->mono, s->Bpp, x0, row);
            }
            row_write(s, row, x0, n, bufD);
            x0 += n; sx0 += n;
        }
    }
    return count;
}

/* ---------------- get: surface -> DIB ---------------------------------- */
static const BYTE vga16[16 * 3] = {
    0,0,0, 0x80,0,0, 0,0x80,0, 0x80,0x80,0, 0,0,0x80, 0x80,0,0x80, 0,0x80,0x80, 0x80,0x80,0x80,
    0xC0,0xC0,0xC0, 0xFF,0,0, 0,0xFF,0, 0xFF,0xFF,0, 0,0,0xFF, 0xFF,0,0xFF, 0,0xFF,0xFF, 0xFF,0xFF,0xFF
};

static BYTE dib_tab[256 * 3];
static WORD dib_tabn;

static void make_table(DIBINFO *di)
{
    WORD i, n = 1 << di->bits;
    if (di->bits == 1) {
        dib_tab[0] = dib_tab[1] = dib_tab[2] = 0;
        dib_tab[3] = dib_tab[4] = dib_tab[5] = 0xFF;
    } else if (di->bits == 4) {
        memcpy(dib_tab, vga16, 48);
    } else {
        if (g.palettized) {
            for (i = 0; i < 256; i++) {
                dib_tab[i * 3] = g.pal[i * 4]; dib_tab[i * 3 + 1] = g.pal[i * 4 + 1];
                dib_tab[i * 3 + 2] = g.pal[i * 4 + 2];
            }
        } else {
            for (i = 0; i < 256; i++) {
                BYTE r, gg, b;
                if (i < 216) { r = (BYTE)((i / 36) * 51); gg = (BYTE)(((i / 6) % 6) * 51); b = (BYTE)((i % 6) * 51); }
                else r = gg = b = (BYTE)((i - 216) * 6 + 8);
                dib_tab[i * 3] = r; dib_tab[i * 3 + 1] = gg; dib_tab[i * 3 + 2] = b;
            }
        }
    }
    dib_tabn = n;
    for (i = 0; i < n; i++) {
        LPBYTE c = di->colors + i * (di->core ? 3 : 4);
        c[0] = dib_tab[i * 3 + 2]; c[1] = dib_tab[i * 3 + 1]; c[2] = dib_tab[i * 3];
        if (!di->core) c[3] = 0;
    }
}

static BYTE table_match(DWORD rgb)
{
    BYTE r = (BYTE)rgb, gg = (BYTE)(rgb >> 8), b = (BYTE)(rgb >> 16);
    WORD i, best = 0;
    DWORD bd = 0xFFFFFFFFUL;
    if (dib_tabn == 2 && !g.dibwords) return rgb_is_white(rgb);
    for (i = 0; i < dib_tabn; i++) {
        SHORT dr = (SHORT)r - dib_tab[i * 3], dg = (SHORT)gg - dib_tab[i * 3 + 1], db = (SHORT)b - dib_tab[i * 3 + 2];
        DWORD d = (DWORD)((LONG)dr * dr) + (DWORD)((LONG)dg * dg) + (DWORD)((LONG)db * db);
        if (d < bd) { bd = d; best = i; if (!d) break; }
    }
    return (BYTE)best;
}

static WORD dib_get(SURF *s, WORD start, WORD count, LPBYTE bits, DIBINFO *di, LPBYTE xlat)
{
    WORD k;
    BYTE Bpp = s->Bpp;
    DWORD lastpix = 0xFFFFFFFFUL;
    BYTE lastidx = 0;
    if (di->comp != BI_RGB && di->comp != BI_BITFIELDS) return 0;
    if (g.dibwords && di->bits <= 8 && xlat && g.palettized && !s->mono) {
        /* GDI supplies a byte table physical index -> DIB index; in pieces
           of CHUNKPX pixels (whole bytes) */
        for (k = 0; k < count; k++) {
            WORD scan = start + k, n = di->width < s->width ? di->width : s->width, x0;
            SHORT row = s->height - 1 - scan;
            DWORD rowoff = BANDOFF(di, k, count);
            if (row < 0 || scan >= di->height) continue;
            for (x0 = 0; x0 < di->width; x0 += CHUNKPX) {
                WORD i, m = di->width - x0 > CHUNKPX ? CHUNKPX : di->width - x0;
                WORD nb = (WORD)(((DWORD)m * di->bits + 7) >> 3);
                WORD have = x0 < n ? (n - x0 < m ? n - x0 : m) : 0;
                if (x0 + m == di->width) nb = (WORD)(di->stride - (((DWORD)x0 * di->bits) >> 3));
                memset(bufS, 0, nb);
                if (have) row_read(s, row, x0, have, bufD);
                for (i = 0; i < have; i++) {
                    BYTE idx = xlat[bufD[i]];
                    if (di->bits == 8) bufS[i] = idx;
                    else if (di->bits == 4) bufS[i >> 1] |= (i & 1) ? (idx & 15) : (idx << 4);
                    else if (idx & 1) bufS[i >> 3] |= (BYTE)(0x80 >> (i & 7));
                }
                huge_write(bits, rowoff + (((DWORD)x0 * di->bits) >> 3), nb, bufS);
            }
        }
        return count;
    }
    if (g.dibwords && di->bits <= 8) {
        WORD i, n = di->ncolors;
        BYTE rgbq = 1;
        for (i = 0; i < n; i++) if (di->colors[i * 4 + 3]) rgbq = 0;
        for (i = 0; i < n; i++) {
            DWORD rgb = rgbq ? dib_rgb(di, i) : phys_to_rgb(word_to_phys(((LPWORD)di->colors)[i]));
            dib_tab[i * 3] = (BYTE)rgb; dib_tab[i * 3 + 1] = (BYTE)(rgb >> 8); dib_tab[i * 3 + 2] = (BYTE)(rgb >> 16);
        }
        dib_tabn = n;
    } else if (di->bits <= 8)
        make_table(di);
    for (k = 0; k < count; k++) {
        WORD scan = start + k;
        SHORT row = s->height - 1 - scan;
        WORD n = di->width < s->width ? di->width : s->width, x0;
        DWORD rowoff = BANDOFF(di, k, count);
        if (row < 0 || scan >= di->height) continue;
        /* in pieces of CHUNKPX pixels (a multiple of 8: whole bytes) */
        for (x0 = 0; x0 < di->width; x0 += CHUNKPX) {
            WORD i, m = di->width - x0 > CHUNKPX ? CHUNKPX : di->width - x0;
            WORD nb = (WORD)(((DWORD)m * di->bits + 7) >> 3);
            WORD have = x0 < n ? (n - x0 < m ? n - x0 : m) : 0;
            BYTE *o = bufS;
            if (x0 + m == di->width)        /* the last piece: the scan's padding too */
                nb = (WORD)(di->stride - (((DWORD)x0 * di->bits) >> 3));
            memset(bufS, 0, nb < ROWBUF ? nb : ROWBUF);
            if (have) row_read(s, row, x0, have, bufD);
            for (i = 0; i < have; i++) {
                DWORD rgb;
                if (s->mono) rgb = bufD[i] ? 0xFFFFFFUL : 0;
                else rgb = pixel_to_rgb(pix_get(bufD + i * Bpp, Bpp));
                if (di->bits == 24) {
                    o[0] = (BYTE)(rgb >> 16); o[1] = (BYTE)(rgb >> 8); o[2] = (BYTE)rgb; o += 3;
                } else if (di->bits >= 16) {
                    dib_hi_put(di, o, rgb); o += di->bits >> 3;
                } else {
                    BYTE idx;
                    if (rgb == lastpix) idx = lastidx;
                    else { idx = table_match(rgb); lastpix = rgb; lastidx = idx; }
                    if (di->bits == 8) bufS[i] = idx;
                    else if (di->bits == 4) bufS[i >> 1] |= (i & 1) ? idx : (idx << 4);
                    else if (idx) bufS[i >> 3] |= (BYTE)(0x80 >> (i & 7));
                }
            }
            huge_write(bits, rowoff + (((DWORD)x0 * di->bits) >> 3), nb, bufS);
        }
    }
    return count;
}

WORD EXPORT DeviceBitmapBits(LPGBITMAP bm, WORD fGet, WORD start, WORD count, LPBYTE bits,
                             LPBYTE bmi, LPDRAWMODE dm, LPWORD xlat)
{
    TRACE("DeviceBitmapBits\n");
    DIBINFO di;
    SURF s;
    if (!dib_parse(&di, bmi)) return 0;
    surf_from_dev(&s, bm);
    DBG("DBB get=%u bm=%ux%ux%u st=%u n=%u dib=%ux%ux%u comp=%u nc=%u core=%u xlat=%p\n", fGet, bm->bmWidth, bm->bmHeight,
        bm->bmBitsPixel, start, count, di.width, di.height, di.bits, di.comp, di.ncolors, di.core, xlat);
    if (fGet) {
        if (start >= s.height) return 0;
        if (start + count > s.height) count = s.height - start;
        if (g.debug > 1) {
            WORD k;
            dbg_printf(" get: xlat=%x:%x tab:", FP_SEG(xlat), FP_OFF(xlat));
            for (k = 0; k < 48; k++) dbg_printf(" %02x", di.colors[k]);
            if (xlat) { dbg_printf(" xl:"); for (k = 0; k < 256; k += 8) dbg_printf(" %02x", ((LPBYTE)xlat)[k]); for (k = 240; k < 256; k++) dbg_printf(" %02x", ((LPBYTE)xlat)[k]); }
            dbg_printf("\n");
        }
        if (!bits) return count;
        {
            WORD r = dib_get(&s, start, count, bits, &di, (LPBYTE)xlat);
            DBG(" get -> %u\n", r);
            return r;
        }
    }
    dcm_begin(xlat);
    /* the DIB is placed at the top left of the bitmap: DIB scan n goes to
       row (DIB height - 1 - n). A bitmap can be taller than the DIB - GDI
       converts big Win32 DIBs in bands of 60 scans through a bitmap of 63
       rows, which it then copies from its top (rows counted from the
       bitmap's bottom left a black line at every band) */
    if (start >= di.height) return 0;
    if (start + count > di.height) count = di.height - start;
    if (g.debug > 1) {
        WORD k, r;
        r = dib_set(&s, 0, (SHORT)(di.height - start - count), start, count, bits, &di, NULL, xlat);
        dbg_printf(" xlat=%x:%x dib:", FP_SEG(xlat), FP_OFF(xlat));
        for (k = 0; k < (WORD)(di.stride * di.height) && k < 64; k++) dbg_printf(" %02x", bits[k]);
        dbg_printf("\n bmp(%x:%x):", FP_SEG(bm->bmBits), FP_OFF(bm->bmBits));
        for (k = 0; k < bm->bmWidthBytes * bm->bmHeight && k < 64; k++) dbg_printf(" %02x", bm->bmBits[k]);
        dbg_printf("\n cmap:");
        for (k = 0; k < 16; k++) dbg_printf(" %lx/%lx", cmap[k], dib_rgb(&di, k));
        dbg_printf(" ni=%u bufD:", (WORD)nearest_index(0xC0,0xC0,0xC0));
        for (k = 0; k < 16; k++) dbg_printf(" %02x", bufD[k]);
        dbg_printf("\n");
        return r;
    }
    return dib_set(&s, 0, (SHORT)(di.height - start - count), start, count, bits, &di, NULL, xlat);
}

WORD EXPORT DIBToDevice(LPVOID lpDev, SHORT x, SHORT y, WORD start, WORD count, LPGRECT clip,
                        LPDRAWMODE dm, LPBYTE bits, LPBYTE bmi, LPWORD xlat)
{
    TRACE("DIBToDevice\n");
    DIBINFO di;
    SURF s;
    SHORT ytop;
    WORD r;
    if (!dib_parse(&di, bmi)) return 0;
    surf_from_dev(&s, lpDev);
    if (s.screen && !g.enabled) return count;
    ytop = y + (SHORT)di.height - (SHORT)(start + count);
#ifdef DIBDBG
    DBG("D2D x=%d y=%d start=%u count=%u %ux%u bpp=%u td=%u bits=%x:%x lim=%lx\n", x, y, start, count, di.width,
        di.height, di.bits, di.topdown, FP_SEG(bits), FP_OFF(bits), seg_limit(FP_SEG(bits)));
#endif
    dcm_begin(xlat);
    if (s.screen) {
        screen_busy++;
        cursor_exclude_l(x, ytop, (LONG)x + di.width, (LONG)ytop + count);
    }
    r = dib_set(&s, x, ytop, start, count, bits, &di, clip, xlat);
    if (s.screen) { cursor_unexclude(); screen_busy--; }
    return r;
}

WORD EXPORT CreateBitmap(void)
{
    TRACE("CreateBitmap\n");
    return 0;
}

/* StretchDIBits: nearest neighbour, any rop */
WORD EXPORT StretchDIBits(LPVOID lpDev, WORD fGet, SHORT dx, SHORT dy, SHORT dw, SHORT dh,
                          SHORT sx, SHORT sy, SHORT sw, SHORT sh, LPBYTE bits, LPBYTE bmi,
                          LPVOID cinfo, DWORD rop, LPPBRUSH br, LPDRAWMODE dm, LPGRECT clip)
{
    TRACE("StretchDIBits\n");
    DIBINFO di;
    SURF s;
    BYTE rop3 = (BYTE)(rop >> 16);
    BYTE needP = ROP_NEEDS_P(rop3) ? 1 : 0;
    BYTE needD = ROP_NEEDS_D(rop3) ? 1 : 0;
    BYTE flipx = 0, flipy = 0;
    SHORT cl, ct, cr, cb, y, lastscan = -32768;
    if (fGet) return (WORD)-1;
    if (!dib_parse(&di, bmi) || (di.comp != BI_RGB && di.comp != BI_BITFIELDS)) return (WORD)-1;
    if (!dw || !dh || !sw || !sh) return 1;
    if (dw < 0) { dx += dw + 1; dw = -dw; flipx ^= 1; }
    if (dh < 0) { dy += dh + 1; dh = -dh; flipy ^= 1; }
    if (sw < 0) { sx += sw + 1; sw = -sw; flipx ^= 1; }
    if (sh < 0) { sy += sh + 1; sh = -sh; flipy ^= 1; }
    if (sw > CHUNKPX || sx < 0 || sy < 0 || sx + sw > (SHORT)di.width || sy + sh > (SHORT)di.height)
        return (WORD)-1;
    surf_from_dev(&s, lpDev);
    if (s.screen && !g.enabled) return 1;
    cl = dx; ct = dy; cr = dx + dw; cb = dy + dh;
    if (clip) {
        if (clip->left > cl) cl = clip->left;
        if (clip->top > ct) ct = clip->top;
        if (clip->right < cr) cr = clip->right;
        if (clip->bottom < cb) cb = clip->bottom;
    }
    if (cl < 0) cl = 0;
    if (ct < 0) ct = 0;
    if (cr > (SHORT)s.width) cr = s.width;
    if (cb > (SHORT)s.height) cb = s.height;
    if (cr <= cl || cb <= ct) return 1;
    dcm_begin(cinfo);
    build_cmap(&di, s.mono, NULL);
    if (s.screen) { screen_busy++; cursor_exclude(cl, ct, cr, cb); }
    for (y = ct; y < cb; y++) {
        SHORT ry = (SHORT)(((DWORD)(y - dy) * sh) / dh);
        /* source rect origin is the lower-left corner of the DIB */
        SHORT scan = flipy ? sy + ry : sy + sh - 1 - ry;
        SHORT x0 = cl;
        if (scan != lastscan) {
            WORD bx0 = (WORD)(((DWORD)sx * di.bits) >> 3);
            WORD bx1 = (WORD)((((DWORD)(sx + sw) * di.bits) + 7) >> 3);
            huge_read(bits, SCANOFF(&di, scan) + bx0, bx1 - bx0, bufS);
            dib_to_work(&di, bufS, (WORD)(sx - (((DWORD)bx0 << 3) / di.bits)), sw, s.mono, s.Bpp, 0, y);
            memcpy(bufT, bufD, sw * s.Bpp);
            lastscan = scan;
        }
        while (x0 < cr) {
            WORD n = cr - x0 > CHUNKPX ? CHUNKPX : cr - x0;
            if (rop3 == 0xCC) {
                stretch_row(bufD, bufT, sw, dw, x0 - dx, n, flipx, s.Bpp);
            } else {
                stretch_row(bufS, bufT, sw, dw, x0 - dx, n, flipx, s.Bpp);
                if (needD) row_read(&s, y, x0, n, bufD);
                if (needP) pattern_row(bufP, br, dm, &s, y, x0, n);
                rop_apply(rop3, bufD, bufS, bufP, n * s.Bpp);
            }
            row_write(&s, y, x0, n, bufD);
            x0 += n;
        }
    }
    if (s.screen) { cursor_unexclude(); screen_busy--; }
    return 1;
}
