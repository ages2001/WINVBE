/*
 * WINVBE - text output with GDI raster fonts (2.0 and 3.0 layouts)
 */
#include "drv.h"

#define MAXCH 256

typedef struct {
    WORD   seg, base;        /* font segment and offset of dfVersion */
    WORD   ver;
    WORD   height;
    WORD   v1;               /* Windows 1.x font: row-major strike */
    WORD   wbytes;           /* v1: bytes per strike row */
    WORD   bits;             /* v1: offset of the strike in the segment */
    WORD   tab;              /* v1: offset of the bit offset table */
    WORD   pixw;             /* v1: width of a fixed pitch font (0 = variable) */
    BYTE   first, last, defch, brkch;
} FONTCTX;

static SHORT ch_x[MAXCH];        /* start x of each char */
static WORD  ch_w[MAXCH];        /* glyph width */
static DWORD ch_off[MAXCH];      /* glyph offset */

static void font_ctx(FONTCTX *f, LPFONTINFO fi)
{
    LPBYTE b;
    BYTE prefix = 0;
    f->seg = FP_SEG(fi);
    f->base = FP_OFF(fi) >= 0x42 ? FP_OFF(fi) - 0x42 : 0;
    b = (LPBYTE)MK_FP(f->seg, f->base);
    f->ver = *(LPWORD)b;
    if (FP_OFF(fi) >= 0x42 && (f->ver == 0x100 || f->ver == 0x200 || f->ver == 0x300))
        prefix = 1;             /* whole font file in memory */
    else if ((g.winver & 0xFF) < 2)
        f->ver = 0x100;         /* GDI 1.x: header without version/copyright,
                                   offsets relative to the FONTINFO */
    f->height = fi->dfPixHeight;
    f->first = fi->dfFirstChar;
    f->last = fi->dfLastChar;
    f->defch = fi->dfDefaultChar;
    f->brkch = fi->dfBreakChar;
    f->v1 = f->ver < 0x200;
    f->wbytes = fi->dfWidthBytes;
    f->bits = (WORD)fi->dfBitsOffset + (prefix ? f->base : FP_OFF(fi));
    f->tab = FP_OFF(fi) + (0x75 - 0x42);
    f->pixw = (fi->dfPitchAndFamily & 1) ? 0 : fi->dfPixWidth;
    if (g.debug > 1) {
        LPBYTE p = (LPBYTE)fi;
        WORD i;
        DBG("font %x:%x ver=%x h=%u first=%u ", FP_SEG(fi), FP_OFF(fi), f->ver, f->height, f->first);
        for (i = 0; i < 0x40; i++) DBG("%02x", p[i]);
        DBG("\n");
    }
}

/* width and glyph offset of character index idx (already biased) */
static WORD glyph(FONTCTX *f, WORD idx, DWORD *off)
{
    LPBYTE b = (LPBYTE)MK_FP(f->seg, f->base);
    if (f->v1) {
        /* 1.x: glyphs side by side in one bitmap; offsets are bit
           positions, a table of them (0x75) only for variable pitch */
        if (f->pixw) {
            *off = (DWORD)idx * f->pixw;
            return f->pixw;
        } else {
            LPWORD t = (LPWORD)MK_FP(f->seg, f->tab);
            *off = t[idx];
            return t[idx + 1] - t[idx];
        }
    }
    if (f->ver >= 0x300) {
        LPBYTE e = b + 0x94 + idx * 6;
        *off = *(LPDWORD)(e + 2);
        return *(LPWORD)e;
    } else {
        LPBYTE e = b + 0x76 + idx * 4;
        *off = *(LPWORD)(e + 2);
        return *(LPWORD)e;
    }
}

static WORD char_index(FONTCTX *f, BYTE c)
{
    if (c < f->first || c > f->last)
        return f->defch;
    return c - f->first;
}

/* lay out characters; returns total advance */
static SHORT layout(FONTCTX *f, LPSTR str, WORD count, SHORT x, LPDRAWMODE dm, LPWORD dx, BYTE update)
{
    WORD i;
    SHORT cx = x;
    SHORT extra = dm ? dm->CharacterExtra : 0;
    SHORT berr = dm ? dm->BreakErr : 0;
    for (i = 0; i < count; i++) {
        WORD idx = char_index(f, (BYTE)str[i]);
        WORD w = glyph(f, idx, &ch_off[i]);
        ch_x[i] = cx;
        ch_w[i] = w;
        if (dx) {
            cx += (SHORT)dx[i];
        } else {
            cx += w + extra;
            if (dm && dm->BreakExtra + dm->BreakRem && idx == f->brkch) {
                cx += dm->BreakExtra;
                berr -= dm->BreakRem;
                if (berr <= 0) { cx++; berr += dm->BreakCount; }
            }
        }
    }
    if (update && dm && !dx) dm->BreakErr = berr;
    return cx - x;
}

static DWORD do_text(LPVOID lpDev, SHORT x, SHORT y, LPGRECT clip, LPSTR str, SHORT count,
                     LPFONTINFO font, LPDRAWMODE dm, LPWORD dx, LPGRECT opq, WORD opts)
{
    FONTCTX f;
    SURF s;
    SHORT cl, ct, cr, cb, total, h, r;
    BYTE extent_only = 0;
    WORD n, i;

    if (!font) return 0x80000000UL;
    font_ctx(&f, font);
    h = f.height;
    if (count < 0) { extent_only = 1; count = -count; }
    n = count > MAXCH ? MAXCH : count;
    total = n ? layout(&f, str, n, x, dm, dx, (BYTE)!extent_only) : 0;
    if (extent_only)
        return MAKELONG(total, h);

    if (!g.enabled && ((LPGBITMAP)lpDev)->bmType) return MAKELONG(total, h);
    surf_from_dev(&s, lpDev);
    cl = 0; ct = 0; cr = s.width; cb = s.height;
    if (clip) {
        if (clip->left > cl) cl = clip->left;
        if (clip->top > ct) ct = clip->top;
        if (clip->right < cr) cr = clip->right;
        if (clip->bottom < cb) cb = clip->bottom;
    }

    if (s.screen) {
        SHORT el = x, et = y, er = x + total, eb = y + h;
        if (opq) {
            if (opq->left < el) el = opq->left;
            if (opq->top < et) et = opq->top;
            if (opq->right > er) er = opq->right;
            if (opq->bottom > eb) eb = opq->bottom;
        }
        if (total < 0) { el = x + total; er = x; }
        screen_busy++;
        cursor_exclude_l(el, et, (LONG)er + 8, eb);
    }

    /* opaque rectangle */
    if (opq && (opts & ETO_OPAQUE)) {
        SHORT l = opq->left > cl ? opq->left : cl, t = opq->top > ct ? opq->top : ct;
        SHORT rr = opq->right < cr ? opq->right : cr, bb = opq->bottom < cb ? opq->bottom : cb;
        if (rr > l && bb > t && dm)
            solid_rect(&s, l, t, rr - l, bb - t, dm->bkColor, 0xF0);
    }
    if (opq && (opts & ETO_CLIPPED)) {
        if (opq->left > cl) cl = opq->left;
        if (opq->top > ct) ct = opq->top;
        if (opq->right < cr) cr = opq->right;
        if (opq->bottom < cb) cb = opq->bottom;
    }
    if (cr <= cl || cb <= ct || !n) goto done;

    /* background of the character cells */
    if (dm && dm->bkMode == OPAQUE) {
        SHORT l = x, rr = x + total;
        if (total < 0) { l = x + total; rr = x; }
        if (l < cl) l = cl;
        if (rr > cr) rr = cr;
        {
            SHORT t = y > ct ? y : ct, bb = y + h < cb ? y + h : cb;
            if (rr > l && bb > t)
                solid_rect(&s, l, t, rr - l, bb - t, dm->bkColor, 0xF0);
        }
    }

    /* glyph foreground */
    {
        SHORT minx = 32767, maxx = -32768;
        DWORD fg = dm ? dm->TextColor : 0;
        BYTE Bpp = s.Bpp;
        DWORD fgpix = s.mono ? (DWORD)mono_of(fg) : pix_of(fg);
        for (i = 0; i < n; i++) {
            if (ch_x[i] < minx) minx = ch_x[i];
            if (ch_x[i] + (SHORT)ch_w[i] > maxx) maxx = ch_x[i] + ch_w[i];
        }
        if (minx < cl) minx = cl;
        if (maxx > cr) maxx = cr;
        for (r = (y > ct ? y : ct); r < y + h && r < cb; r++) {
            SHORT gr = r - y;
            SHORT sx0 = minx;
            while (sx0 < maxx) {
                SHORT sx1 = sx0 + CHUNKPX < maxx ? sx0 + CHUNKPX : maxx;
                WORD len = sx1 - sx0;
                BYTE any = 0;
                memset(bufS, 0, len);
                for (i = 0; i < n; i++) {
                    SHORT c0 = ch_x[i], c1 = ch_x[i] + ch_w[i];
                    SHORT j0, j1, j;
                    LPBYTE gb;
                    if (c1 <= sx0 || c0 >= sx1) continue;
                    j0 = c0 < sx0 ? sx0 - c0 : 0;
                    j1 = c1 > sx1 ? sx1 - c0 : (SHORT)ch_w[i];
                    if (f.v1) {
                        WORD bit = (WORD)ch_off[i];
                        gb = (LPBYTE)MK_FP(f.seg, f.bits + gr * f.wbytes);
                        for (j = j0; j < j1; j++) {
                            WORD p = bit + j;
                            if (gb[p >> 3] & (0x80 >> (p & 7))) { bufS[c0 + j - sx0] = 1; any = 1; }
                        }
                        continue;
                    }
                    gb = (LPBYTE)MK_FP(f.seg, f.base + (WORD)ch_off[i] + gr);
                    for (j = j0; j < j1; j++) {
                        BYTE bits = gb[(j >> 3) * h];
                        if (bits & (0x80 >> (j & 7))) { bufS[c0 + j - sx0] = 1; any = 1; }
                    }
                }
                if (any) {
                    WORD k;
                    row_read(&s, r, sx0, len, bufD);
                    for (k = 0; k < len; k++)
                        if (bufS[k]) pix_put(bufD + k * Bpp, fgpix, Bpp);
                    row_write(&s, r, sx0, len, bufD);
                }
                sx0 = sx1;
            }
        }
    }
done:
    if (s.screen) { cursor_unexclude(); screen_busy--; }
    return MAKELONG(total, h);
}

DWORD EXPORT ExtTextOut(LPVOID lpDev, SHORT x, SHORT y, LPGRECT clip, LPSTR str, SHORT count,
                        LPFONTINFO font, LPDRAWMODE dm, LPTEXTXFORM xf, LPWORD dx,
                        LPGRECT opq, WORD opts)
{
    TRACE("ExtTextOut\n");
    return do_text(lpDev, x, y, clip, str, count, font, dm, dx, opq, opts);
}

DWORD EXPORT StrBlt(LPVOID lpDev, SHORT x, SHORT y, LPGRECT clip, LPSTR str, SHORT count,
                    LPFONTINFO font, LPDRAWMODE dm, LPTEXTXFORM xf)
{
    TRACE("StrBlt\n");
    return do_text(lpDev, x, y, clip, str, count, font, dm, NULL, NULL, 0);
}

WORD EXPORT GetCharWidth(LPVOID lpDev, LPWORD buf, WORD first, WORD last, LPFONTINFO font,
                         LPDRAWMODE dm, LPTEXTXFORM xf)
{
    TRACE("GetCharWidth\n");
    FONTCTX f;
    WORD c;
    DWORD off;
    font_ctx(&f, font);
    for (c = first; c <= last; c++) {
        *buf++ = glyph(&f, char_index(&f, (BYTE)c), &off);
        if (c == 0xFFFF) break;
    }
    return 1;
}
