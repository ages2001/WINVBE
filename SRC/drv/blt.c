/*
 * WINVBE - BitBlt and rectangle filling
 */
#include "drv.h"

/* build n pixels of pattern for destination row y starting at x */
void pattern_row(BYTE *dst, LPPBRUSH br, LPDRAWMODE dm, SURF *d, WORD y, WORD x, WORD n)
{
    WORD i;
    BYTE r = (BYTE)(y & 7);
    if (!br || br->style == BS_HOLLOW) {
        memset(dst, 0, n * d->Bpp);
        return;
    }
    if (d->mono) {
        BYTE bits = br->mono[r];
        if (br->flags & PBF_MONOPAT) {
            /* mono pattern brush on mono dest: 0 -> text color, 1 -> bk color */
            BYTE fg = dm ? mono_of(dm->TextColor) : 0x00;
            BYTE bg = dm ? mono_of(dm->bkColor) : 0xFF;
            BYTE m = br->mask[r];
            for (i = 0; i < n; i++)
                dst[i] = (m & (0x80 >> ((x + i) & 7))) ? bg : fg;
            return;
        }
        for (i = 0; i < n; i++)
            dst[i] = (bits & (0x80 >> ((x + i) & 7))) ? 0xFF : 0x00;
        return;
    }
    if (br->flags & PBF_SOLID) {
        pix_fill(dst, n, pix_get(br->pat, d->Bpp), d->Bpp);
        return;
    }
    if (br->flags & PBF_MONOPAT) {
        DWORD fg = pix_of(dm ? dm->TextColor : 0), bg = pix_of(dm ? dm->bkColor : phys_white());
        BYTE m = br->mask[r];
        BYTE Bpp = d->Bpp;
        for (i = 0; i < n; i++) {
            pix_put(dst, (m & (0x80 >> ((x + i) & 7))) ? bg : fg, Bpp);
            dst += Bpp;
        }
        return;
    }
    {
        const BYTE *row = br->pat + r * 32;
        BYTE Bpp = d->Bpp;
        WORD c = x & 7;
        WORD k;
        /* first partial period */
        while (n && c) {
            for (k = 0; k < Bpp; k++) *dst++ = row[c * Bpp + k];
            c = (c + 1) & 7; n--;
        }
        /* full periods */
        {
            WORD per = 8 * Bpp;
            while (n >= 8) { memcpy(dst, row, per); dst += per; n -= 8; }
            if (n) memcpy(dst, row, n * Bpp);
        }
    }
}

/* convert a source row into destination working format in bufS */
void load_src(SURF *s, SURF *d, WORD y, WORD x, WORD n, LPDRAWMODE dm)
{
    WORD i;
    if (s->planar) {
        static const DWORD vga16[16] = {
            0x000000UL, 0x000080UL, 0x008000UL, 0x008080UL, 0x800000UL, 0x800080UL, 0x808000UL, 0xC0C0C0UL,
            0x808080UL, 0x0000FFUL, 0x00FF00UL, 0x00FFFFUL, 0xFF0000UL, 0xFF00FFUL, 0xFFFF00UL, 0xFFFFFFUL };
        DWORD map[16];
        BYTE Bpp = d->Bpp;
        for (i = 0; i < 16; i++)
            map[i] = d->mono ? (rgb_is_white(vga16[i]) ? 0xFF : 0) : pix_of(rgb_to_phys(vga16[i]));
        row_read(s, y, x, n, bufS);
        for (i = n; i-- > 0;)
            pix_put(bufS + i * Bpp, map[bufS[i] & 15], Bpp);
        return;
    }
    if (s->mono == d->mono && (s->mono || s->Bpp == d->Bpp)) {
        row_read(s, y, x, n, bufS);
        return;
    }
    if (s->mono) {
        /* mono -> color: 0 = text color, 1 = background color */
        DWORD fg = dm ? pix_of(dm->TextColor) : 0;
        DWORD bg = dm ? pix_of(dm->bkColor) : pix_of(phys_white());
        BYTE Bpp = d->Bpp;
        row_read(s, y, x, n, bufS);
        for (i = n; i-- > 0;)
            pix_put(bufS + i * Bpp, bufS[i] ? bg : fg, Bpp);
        return;
    }
    /* color -> mono: pixels equal to background color become white */
    {
        DWORD bg = dm ? pix_of(dm->bkColor) : pix_of(phys_white());
        BYTE Bpp = s->Bpp;
        row_read(s, y, x, n, bufS);
        for (i = 0; i < n; i++)
            bufS[i] = (pix_get(bufS + i * Bpp, Bpp) == bg) ? 0xFF : 0x00;
    }
}

/* solid fill of a rectangle (already clipped) with a physical color */
void solid_rect(SURF *d, SHORT x, SHORT y, SHORT w, SHORT h, DWORD phys, BYTE rop3)
{
    WORD r;
    DWORD pix = d->mono ? (DWORD)mono_of(phys) : pix_of(phys);
    BYTE Bpp = d->Bpp;
    if (w <= 0 || h <= 0) return;
    if (rop3 == 0xF0 && d->screen) {
        if (g.svga && (DWORD)w * h >= 2048 && svga_fill(pix, x, y, w, h)) return;
        for (r = 0; r < (WORD)h; r++)
            scr_fill(y + r, x, w, pix);
        return;
    }
    if (rop3 == 0xF0 && !d->mono) {
        for (r = 0; r < (WORD)h; r++) {
            LPBYTE p = mem_row(d, y + r) + x * Bpp;
            if (Bpp == 1) far_memset(p, (BYTE)pix, w);
            else {
                WORD left = w, xx = 0;
                while (left) {
                    WORD c = left > CHUNKPX ? CHUNKPX : left;
                    pix_fill(bufP, c, pix, Bpp);
                    far_memcpy(p + xx * Bpp, bufP, c * Bpp);
                    xx += c; left -= c;
                }
            }
        }
        return;
    }
    {
        BYTE needD = ROP_NEEDS_D(rop3) ? 1 : 0;
        for (r = 0; r < (WORD)h; r++) {
            WORD left = w, xx = x;
            while (left) {
                WORD c = left > CHUNKPX ? CHUNKPX : left;
                pix_fill(bufP, c, pix, Bpp);
                if (needD) row_read(d, y + r, xx, c, bufD);
                rop_apply(rop3, bufD, NULL, bufP, c * Bpp);
                row_write(d, y + r, xx, c, bufD);
                xx += c; left -= c;
            }
        }
    }
}

/* pattern fill of a clipped rectangle */
void fill_rect(SURF *d, SHORT x, SHORT y, SHORT w, SHORT h, BYTE rop3, LPPBRUSH br, LPDRAWMODE dm)
{
    WORD r;
    BYTE needD, needP;
    if (w <= 0 || h <= 0) return;
    needP = ROP_NEEDS_P(rop3) ? 1 : 0;
    needD = ROP_NEEDS_D(rop3) ? 1 : 0;
    if (!needP || (br && (br->flags & PBF_SOLID) && !(br->flags & PBF_MONOPAT))) {
        DWORD c = 0;
        if (br && needP) c = d->mono ? (br->mono[0] ? phys_white() : 0) : br->fg;
        solid_rect(d, x, y, w, h, c, rop3);
        return;
    }
    if (br && br->style == BS_HOLLOW)
        return;
    for (r = 0; r < (WORD)h; r++) {
        WORD left = w, xx = x;
        while (left) {
            WORD c = left > CHUNKPX ? CHUNKPX : left;
            pattern_row(bufP, br, dm, d, y + r, xx, c);
            if (needD) row_read(d, y + r, xx, c, bufD);
            rop_apply(rop3, bufD, NULL, bufP, c * d->Bpp);
            row_write(d, y + r, xx, c, bufD);
            xx += c; left -= c;
        }
    }
}

WORD EXPORT BitBlt(LPVOID lpDst, SHORT dx, SHORT dy, LPVOID lpSrc, SHORT sx, SHORT sy,
                   SHORT w, SHORT h, DWORD rop, LPPBRUSH br, LPDRAWMODE dm)
{
    TRACE("BitBlt\n");
    SURF d, s;
    BYTE rop3 = (BYTE)(rop >> 16);
    BYTE needS = ROP_NEEDS_S(rop3) ? 1 : 0;
    BYTE needP = ROP_NEEDS_P(rop3) ? 1 : 0;
    BYTE needD = ROP_NEEDS_D(rop3) ? 1 : 0;
    SHORT r, rstart, rend, rstep;
    BYTE rtl = 0, same = 0;
    SHORT el, et, er, eb;

    if (!g.enabled && ((LPGBITMAP)lpDst)->bmType) return 1;
    surf_from_dev(&d, lpDst);
    if (g.debug > 1)
        dbg_printf("BitBlt d=%u/%u (%d,%d) s=%u (%d,%d) %dx%d rop=%lx br=%x/%x fg=%lx\n", d.screen, d.Bpp, dx, dy,
                   needS ? (((LPGBITMAP)lpSrc)->bmType ? 1 : ((LPGBITMAP)lpSrc)->bmBitsPixel) : 99, sx, sy, w, h, rop,
                   br ? br->style : 0xFFFF, br ? br->flags : 0, br ? br->fg : 0UL);
    if (g.debug > 1 && dm)
        dbg_printf("   dm text=%lx bk=%lx bkmode=%u\n", dm->TextColor, dm->bkColor, dm->bkMode);
    if (needS) {
        if (!lpSrc) return 0;
        surf_from_dev(&s, lpSrc);
    }
    /* clip destination */
    if (dx < 0) { w += dx; if (needS) sx -= dx; dx = 0; }
    if (dy < 0) { h += dy; if (needS) sy -= dy; dy = 0; }
    if (dx + w > (SHORT)d.width) w = d.width - dx;
    if (dy + h > (SHORT)d.height) h = d.height - dy;
    if (needS) {
        if (sx < 0) { w += sx; dx -= sx; sx = 0; }
        if (sy < 0) { h += sy; dy -= sy; sy = 0; }
        if (sx + w > (SHORT)s.width) w = s.width - sx;
        if (sy + h > (SHORT)s.height) h = s.height - sy;
    }
    if (w <= 0 || h <= 0) return 1;

    el = dx; et = dy; er = dx + w; eb = dy + h;
    if (needS && s.screen) {
        if (sx < el) el = sx;
        if (sy < et) et = sy;
        if (sx + w > er) er = sx + w;
        if (sy + h > eb) eb = sy + h;
    }
    if (d.screen || (needS && s.screen)) {
        screen_busy++;
        cursor_exclude(el, et, er, eb);
    }

    if (!needS) {
        if (needP && br && br->style == BS_HOLLOW && !needD) { /* nothing */ }
        else fill_rect(&d, dx, dy, w, h, rop3, needP ? br : NULL, dm);
        goto done;
    }

    same = (d.screen && s.screen) ||
           (!d.screen && !s.screen && d.sel == s.sel && d.off == s.off);
    /* VMware SVGA II: screen to screen copy by the host */
    if (d.screen && s.screen && rop3 == 0xCC && g.svga && svga_copy(sx, sy, dx, dy, w, h)) goto done;
    rstart = 0; rend = h; rstep = 1;
    if (same && sy < dy) { rstart = h - 1; rend = -1; rstep = -1; }
    if (same && sy == dy && sx < dx) rtl = 1;

    {
        BYTE simple = (rop3 == 0xCC) && !s.planar && (s.mono == d.mono) && (s.mono || s.Bpp == d.Bpp);
        WORD Bpp = d.Bpp;
        for (r = rstart; r != rend; r += rstep) {
            WORD left = w;
            SHORT off = 0;
            if (rtl) off = w;
            while (left) {
                WORD c = left > CHUNKPX ? CHUNKPX : left;
                SHORT xo;
                if (rtl) { off -= c; xo = off; } else { xo = off; off += c; }
                if (simple) {
                    row_read(&s, sy + r, sx + xo, c, bufD);
                } else {
                    if (needD) row_read(&d, dy + r, dx + xo, c, bufD);
                    load_src(&s, &d, sy + r, sx + xo, c, dm);
                    if (needP) pattern_row(bufP, br, dm, &d, dy + r, dx + xo, c);
                    rop_apply(rop3, bufD, bufS, bufP, c * Bpp);
                }
                row_write(&d, dy + r, dx + xo, c, bufD);
                left -= c;
            }
        }
    }
done:
    if (d.screen || (needS && s.screen)) {
        cursor_unexclude();
        screen_busy--;
    }
    return 1;
}

/* nearest-neighbour stretch of bufS (n src pixels) into out (dn dest pixels) */
void stretch_row(BYTE *out, const BYTE *src, WORD sw, WORD dw, WORD dfirst, WORD dn, BYTE flip, BYTE Bpp)
{
    WORD i;
    DWORD acc = (DWORD)dfirst * sw;
    for (i = 0; i < dn; i++, acc += sw) {
        WORD sxp = (WORD)(acc / dw);
        if (flip) sxp = sw - 1 - sxp;
        switch (Bpp) {
        case 1: out[i] = src[sxp]; break;
        case 2: ((WORD *)out)[i] = ((const WORD *)src)[sxp]; break;
        case 3: out[i * 3] = src[sxp * 3]; out[i * 3 + 1] = src[sxp * 3 + 1]; out[i * 3 + 2] = src[sxp * 3 + 2]; break;
        default: ((DWORD *)out)[i] = ((const DWORD *)src)[sxp]; break;
        }
    }
}

BYTE bufT[ROWBUF];

WORD EXPORT StretchBlt(LPVOID lpDst, SHORT dx, SHORT dy, SHORT dw, SHORT dh, LPVOID lpSrc,
                       SHORT sx, SHORT sy, SHORT sw, SHORT sh, DWORD rop, LPPBRUSH br,
                       LPDRAWMODE dm, LPGRECT clip)
{
    TRACE("StretchBlt\n");
    SURF d, s;
    BYTE rop3 = (BYTE)(rop >> 16);
    BYTE needS = ROP_NEEDS_S(rop3) ? 1 : 0;
    BYTE needP = ROP_NEEDS_P(rop3) ? 1 : 0;
    BYTE needD = ROP_NEEDS_D(rop3) ? 1 : 0;
    BYTE flipx = 0, flipy = 0;
    SHORT cl, ct, cr, cb, y;
    SHORT lastsy = -32768;

    if (!g.enabled && ((LPGBITMAP)lpDst)->bmType) return 1;
    if (!dw || !dh) return 1;
    if (dw < 0) { dx += dw + 1; dw = -dw; flipx ^= 1; }
    if (dh < 0) { dy += dh + 1; dh = -dh; flipy ^= 1; }
    if (needS) {
        if (!lpSrc || !sw || !sh) return 1;
        if (sw < 0) { sx += sw + 1; sw = -sw; flipx ^= 1; }
        if (sh < 0) { sy += sh + 1; sh = -sh; flipy ^= 1; }
        surf_from_dev(&s, lpSrc);
        if (sw > CHUNKPX) return (WORD)-1;
        if (sx < 0 || sy < 0 || sx + sw > (SHORT)s.width || sy + sh > (SHORT)s.height) return (WORD)-1;
        if (s.screen && ((LPGBITMAP)lpDst)->bmType) return (WORD)-1;   /* overlapping screen stretch */
    }
    surf_from_dev(&d, lpDst);
    cl = dx; ct = dy; cr = dx + dw; cb = dy + dh;
    if (clip) {
        if (clip->left > cl) cl = clip->left;
        if (clip->top > ct) ct = clip->top;
        if (clip->right < cr) cr = clip->right;
        if (clip->bottom < cb) cb = clip->bottom;
    }
    if (cl < 0) cl = 0;
    if (ct < 0) ct = 0;
    if (cr > (SHORT)d.width) cr = d.width;
    if (cb > (SHORT)d.height) cb = d.height;
    if (cr <= cl || cb <= ct) return 1;
    if (!needS) {
        BEGIN_SCREEN(&d, cl, ct, cr, cb);
        fill_rect(&d, cl, ct, cr - cl, cb - ct, rop3, needP ? br : NULL, dm);
        END_SCREEN(&d);
        return 1;
    }
    if (d.screen || s.screen) {
        screen_busy++;
        cursor_exclude(cl, ct, cr, cb);
        if (s.screen) cursor_exclude(sx, sy, sx + sw, sy + sh);
    }
    for (y = ct; y < cb; y++) {
        SHORT ry = (SHORT)(((DWORD)(y - dy) * sh) / dh);
        SHORT srow = flipy ? sy + sh - 1 - ry : sy + ry;
        SHORT x0 = cl;
        if (srow != lastsy) {
            load_src(&s, &d, srow, sx, sw, dm);
            memcpy(bufT, bufS, sw * d.Bpp);
            lastsy = srow;
        }
        while (x0 < cr) {
            WORD n = cr - x0 > CHUNKPX ? CHUNKPX : cr - x0;
            if (rop3 == 0xCC) {
                stretch_row(bufD, bufT, sw, dw, x0 - dx, n, flipx, d.Bpp);
            } else {
                stretch_row(bufS, bufT, sw, dw, x0 - dx, n, flipx, d.Bpp);
                if (needD) row_read(&d, y, x0, n, bufD);
                if (needP) pattern_row(bufP, br, dm, &d, y, x0, n);
                rop_apply(rop3, bufD, bufS, bufP, n * d.Bpp);
            }
            row_write(&d, y, x0, n, bufD);
            x0 += n;
        }
    }
    if (d.screen || s.screen) {
        cursor_unexclude();
        screen_busy--;
    }
    return 1;
}
