/*
 * WINVBE - DIB engine mode (Windows 95/98/ME).
 *
 * With a linear frame buffer WINVBE runs as a DIB engine mini driver, like
 * the display drivers of Windows 9x itself: the drawing entry points jump
 * to DIBENG.DLL (entry.asm, dibe_tab), which draws straight into the frame
 * buffer, and the screen PDEVICE is a DIBENGINE structure describing it.
 * WINVBE keeps the mode setting, the palette, the screen switches and its
 * escapes.  This is what DirectDraw requires (a 4.0 driver with
 * C1_DIBENGINE and a DIBENGINE PDEVICE of type 'RP'), and DIB sections,
 * WinG and Video for Windows draw at DIB engine speed.
 *
 * Without a linear frame buffer (banked or VGA modes), below 8 bits per
 * pixel, on Windows 3.x or with DibEngine=0 in [WINVBE], WINVBE draws with
 * its own code as before.
 */
#include "drv.h"

int vbe_lfb_possible(void);

#ifndef OLDWIN

extern WORD  __far __pascal GetModuleHandle(LPSTR);
extern DWORD __far __pascal GetProcAddress(WORD, LPSTR);
typedef WORD (__far __pascal *PLOADLIBRARY)(LPSTR);

BYTE dibeng_on;                     /* the DIB engine draws */
#pragma aux dibeng_on "*"
WORD dib_palettized;                /* extra argument of DIB_DibBltExt */
#pragma aux dib_palettized "*"
DWORD dibe_tab[DT_COUNT];            /* DIBENG.DLL entry points (entry.asm) */
#pragma aux dibe_tab "*"

static const WORD dib_ord[DT_COUNT] = {
    1, 2, 6, 402, 8, 9, 400, 11, 12, 13, 14, 15, 16, 17, 18, 401, 20, 21,
    406, 407, 408, 404, 27, 28, 29, 30, 101, 102, 103, 104,
    5, 3, 405, 105, 106
};

typedef WORD  (__far __pascal *PF_ENABLE)(LPVOID, WORD, LPSTR, LPSTR, LPVOID);
typedef SHORT (__far __pascal *PF_CONTROL)(LPVOID, WORD, LPVOID, LPVOID);
typedef WORD  (__far __pascal *PF_SETPAL)(WORD, WORD, LPVOID, LPVOID);
typedef WORD  (__far __pascal *PF_SETPALXLAT)(LPWORD, LPVOID);
typedef void  (__far __pascal *PF_BEGIN)(LPVOID, WORD, WORD, WORD, WORD, WORD);
typedef void  (__far __pascal *PF_END)(LPVOID, WORD);

#define CURSOREXCLUDE 0x0008

static WORD dib_pdevsize;           /* DIBENGINE part of the PDEVICE */
static WORD de_sel;                 /* 16:32 selector over the frame buffer */
static DWORD de_base, de_limit;
static BYTE de_tried;
static BYTE de_access;              /* dibeng_begin nesting */

/* does the current mode work with the DIB engine */
int dibeng_mode_ok(void)
{
    return g.access == ACC_LFB && g.lfbsel && g.lfblin && g.bpp >= 8;
}

/* load DIBENG.DLL and decide (first Enable): the mode chosen at load time
   must have a linear frame buffer */
int dibeng_check(void)
{
    WORD hk, hd, i;
    if (de_tried) return dibeng_on;
    de_tried = 1;
    /* colour correction in high or true colour is applied to the driver's own
       screen writes: then without the DIB engine */
    if ((lut_on && g.mi.bpp > 8) || g.xf) {
        DBG("DIB engine: not used (colour/xf %u)\n", g.xf);
        return 0;
    }
    if (!g.win9x || !g.ini_dibeng || !g.cpu386 || !g.pmode || !vbe_lfb_possible()) {
        DBG("DIB engine: not used (win9x=%u ini=%u mode %x bpp=%u lfb=%u)\n", g.win9x, g.ini_dibeng, g.mi.mode,
            g.mi.bpp, g.lfb_state);
        return 0;
    }
    hd = GetModuleHandle((LPSTR)"DIBENG");
    if (!hd) {
        PLOADLIBRARY ll;
        hk = GetModuleHandle((LPSTR)"KERNEL");
        ll = (PLOADLIBRARY)GetProcAddress(hk, (LPSTR)MK_FP(0, 95));
        if (ll) {
            hd = ll((LPSTR)"DIBENG.DLL");
            if (hd < 32) hd = 0;
        }
    }
    if (!hd) { DBG("DIB engine: DIBENG.DLL not found\n"); return 0; }
    for (i = 0; i < DT_COUNT; i++) {
        dibe_tab[i] = GetProcAddress(hd, (LPSTR)MK_FP(0, dib_ord[i]));
        if (!dibe_tab[i]) { DBG("DIB engine: no ordinal %u\n", dib_ord[i]); return 0; }
    }
    /* VMware SVGA II: the host follows the frame buffer by itself (no
       command FIFO), as the DIB engine reports no drawn rectangles */
    if (g.svga) g.ini_vmfifo = 0;
    g.ini_shadow = 0;
    dibeng_on = 1;
    DBG("DIB engine: on\n");
    return 1;
}

/* GDIINFO: the DIB engine's capabilities (curves, lines, text, raster...)
   over WINVBE's own fields; a 4.0 driver with C1_DIBENGINE */
void dibeng_gdiinfo(LPGDIINFO gi)
{
    GDIINFO di;
    far_memset(&di, 0, sizeof(di));
    ((PF_ENABLE)dibe_tab[DT_ENABLE])((LPVOID)&di, 1, NULL, NULL, NULL);
    gi->dpVersion = 0x400;
    gi->dpCurves = di.dpCurves;
    gi->dpLines = di.dpLines;
    gi->dpPolygonals = di.dpPolygonals;
    gi->dpText = di.dpText | 0x2004;            /* TC_CP_STROKE | TC_RA_ABLE */
    gi->dpClip = di.dpClip;
    gi->dpRaster = di.dpRaster | 0x0200;        /* RC_DIBTODEV */
    gi->dpStyleLen = di.dpStyleLen;
    gi->dpDCManage = 4;                         /* DC_IGNOREDFNP */
    /* C1_DIBENGINE, C1_GAMMA_RAMP, C1_REINIT_ABLE, C1_GLYPH_INDEX, C1_BYTE_PACKED,
       C1_COLORCURSOR */
    gi->dpCaps1 = di.dpCaps1 | 0x0010 | 0x0020 | 0x0080 | 0x0100 | 0x0400 | 0x0800;
    dib_pdevsize = di.dpDEVICEsize;
    if (g.mi.Bpp == 1) {                        /* (g.palettized is set with the mode) */
        gi->dpRaster |= 0x0100 | 0x0040;        /* RC_PALETTE, RC_SAVEBITMAP */
        gi->dpNumPens = 16;
        gi->dpNumColors = 20;
        gi->dpPalColors = 256;
        gi->dpPalReserved = 20;
        gi->dpPalResolution = 18;
    } else {
        gi->dpNumPens = -1;
        gi->dpNumColors = -1;
        gi->dpPalColors = 0;
        gi->dpPalReserved = 0;
        gi->dpPalResolution = 0;
    }
    /* + BITMAPINFOHEADER and a 256 entry color table (any depth, so that a
       later ReEnable fits) */
    gi->dpDEVICEsize = dib_pdevsize + 40 + 256 * 4;
}

static LPBYTE dib_bmi(void)
{
    return (LPBYTE)scr_pdev + dib_pdevsize;
}

/* fill the DIBENGINE from the current mode (Enable, ReEnable, mode sets,
   screen switches); BUSY while a DOS box owns the screen */
void pdev_sync(void)
{
    LPDIBENGINE de = (LPDIBENGINE)scr_pdev;
    LPBYTE bmi;
    WORD fl = DE_MINIDRIVER | DE_VRAM;
    if (!dibeng_on || !de) return;
    if (g.lfblin && (g.lfblin != de_base || ((DWORD)g.lfbtiles << 16) - 1 != de_limit)) {
        if (!de_sel) de_sel = sel_alloc(1);
        if (de_sel && sel_setbase(de_sel, g.lfblin) && sel_setlimit(de_sel, ((DWORD)g.lfbtiles << 16) - 1)) {
            de_base = g.lfblin; de_limit = ((DWORD)g.lfbtiles << 16) - 1;
        }
    }
    de->deType = 0x5250;                        /* 'RP' */
    de->deWidth = g.xres;
    de->deHeight = g.yres;
    de->deWidthBytes = g.pitch;
    de->dePlanes = 1;
    de->deBitsPixel = g.bpp == 15 ? 16 : (BYTE)g.bpp;
    de->deReserved1 = 0;
    de->deDeltaScan = g.pitch;
    de->delpPDevice = 0;
    de->deBitsOffset = g.fbofs;
    de->deBitsSelector = de_sel;
    de->deVersion = 0x400;
    /* CTCHANGE (0x0400): the color table is new - the DIB engine rebuilds
       its color matching from it */
    if (g.palettized) fl |= DE_PALETTIZED | 0x0400;
    if (g.bpp == 16) fl |= DE_FIVE6FIVE;
    if (!g.enabled || g.in_background || !dibeng_mode_ok()) fl |= DE_BUSY;
    de->deFlags = fl;
    bmi = dib_bmi();
    {
        LPDWORD h = (LPDWORD)bmi;
        h[0] = 40;                              /* biSize */
        h[1] = g.xres;                          /* biWidth */
        h[2] = g.yres;                          /* biHeight */
        ((LPWORD)bmi)[6] = 1;                   /* biPlanes */
        ((LPWORD)bmi)[7] = de->deBitsPixel;     /* biBitCount */
        h[4] = 0;                               /* biCompression */
        h[5] = (DWORD)g.pitch * g.yres;         /* biSizeImage */
        h[6] = h[7] = 0;
        h[8] = g.palettized ? 256 : 0;          /* biClrUsed */
        h[9] = 0;
    }
    de->deBitmapInfo = (LPVOID)bmi;
    de->deBeginAccess = (LPVOID)dibe_tab[DT_BEGINACCESS];
    de->deEndAccess = (LPVOID)dibe_tab[DT_ENDACCESS];
    dib_palettized = g.palettized;
}

/* the color table of the BITMAPINFO from WINVBE's palette (r,g,b,x) */
static void bmi_colors_from_pal(WORD start, WORD count)
{
    LPBYTE c = dib_bmi() + 40;
    WORD i;
    for (i = start; i < start + count && i < 256; i++) {
        BYTE r = g.pal[i * 4 + 0], gg = g.pal[i * 4 + 1], b = g.pal[i * 4 + 2];
        BYTE fl = 0;
        c[i * 4 + 0] = b;
        c[i * 4 + 1] = gg;
        c[i * 4 + 2] = r;
        /* DIB engine color entry flags: NONSTATIC (0x80) for all but the 16
           VGA colors (the 4 extra static colors can change), MAPTOWHITE
           (0x01) for the light ones (white in monochrome conversions) */
        if ((i >= 8 && i < 248)) fl |= 0x80;
        if ((WORD)r * 30 + (WORD)gg * 59 + (WORD)b * 11 >= 128 * 100) fl |= 0x01;
        c[i * 4 + 3] = fl;
    }
}

/* Enable for the screen, after the mode is set */
WORD dibeng_enable(LPVOID lpDevice, LPSTR devtype, LPSTR outfile, LPVOID data)
{
    scr_pdev = (LPPDEVICE)lpDevice;
    if (!dibeng_mode_ok()) {
        /* the frame buffer failed its check at the mode set: nothing to give
           the DIB engine (Windows falls back to its VGA driver) */
        DBG("DIB engine: no linear frame buffer after the mode set\n");
        return 0;
    }
    ((PF_ENABLE)dibe_tab[DT_ENABLE])(lpDevice, 0, devtype, outfile, data);
    pdev_sync();
    if (g.palettized) {
        bmi_colors_from_pal(0, 256);
        ((PF_SETPALXLAT)dibe_tab[DT_SETPALXLAT])(NULL, lpDevice);
    }
    dd_mode_changed();
    DBG("DIB engine PDEVICE: sel %x ofs %lx pitch %u bpp %u flags %x size %u\n", de_sel, g.fbofs, g.pitch,
        ((LPDIBENGINE)lpDevice)->deBitsPixel, ((LPDIBENGINE)lpDevice)->deFlags, dib_pdevsize);
    return 1;
}

/* ReEnable: new resolution or color depth (the mode is set) */
void dibeng_reenable(LPVOID lpDevice)
{
    ((PF_ENABLE)dibe_tab[DT_ENABLE])(lpDevice, 0, NULL, NULL, NULL);
    pdev_sync();
    if (g.palettized) {
        bmi_colors_from_pal(0, 256);
        ((PF_SETPALXLAT)dibe_tab[DT_SETPALXLAT])(NULL, lpDevice);
    }
    dd_mode_changed();
    DBG("DIB engine ReEnable: %ux%ux%u flags %x\n", g.xres, g.yres, g.bpp, ((LPDIBENGINE)lpDevice)->deFlags);
}

WORD dibeng_setpalette(WORD start, WORD count, LPBYTE pal)
{
    LPBYTE c;
    WORD i;
    ((PF_SETPAL)dibe_tab[DT_SETPALETTE])(start, count, (LPVOID)pal, (LPVOID)scr_pdev);
    c = dib_bmi() + 40;
    for (i = start; i < start + count && i < 256; i++) {
        g.pal[i * 4 + 0] = c[i * 4 + 2];
        g.pal[i * 4 + 1] = c[i * 4 + 1];
        g.pal[i * 4 + 2] = c[i * 4 + 0];
        g.pal[i * 4 + 3] = 1;
    }
    if (g.enabled && !g.in_background && g.palettized) vbe_set_dac(start, count, g.pal + start * 4);
    return 0;
}

SHORT dibeng_control(LPVOID lpDev, WORD func, LPVOID lpIn, LPVOID lpOut)
{
    return ((PF_CONTROL)dibe_tab[DT_CONTROL])(lpDev, func, lpIn, lpOut);
}

/* frame buffer access of WINVBE's own (screen clear, DCI clients): the DIB
   engine takes its software cursor off the rectangle */
void dibeng_begin(SHORT l, SHORT t, SHORT r, SHORT b)
{
    if (!scr_pdev) return;
    if (de_access++) return;
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > (SHORT)g.xres) r = g.xres;
    if (b > (SHORT)g.yres) b = g.yres;
    ((PF_BEGIN)dibe_tab[DT_BEGINACCESS])((LPVOID)scr_pdev, l, t, r > l ? r - 1 : l, b > t ? b - 1 : t, CURSOREXCLUDE);
}

void dibeng_end(void)
{
    if (!scr_pdev || !de_access) return;
    if (--de_access) return;
    ((PF_END)dibe_tab[DT_ENDACCESS])((LPVOID)scr_pdev, CURSOREXCLUDE);
}

/* ---- the pointer: the DIB engine draws it; WINVBE Mouse's color and
   double size are made here, as a new shape (USER's shape is kept to
   apply new options) ---- */
extern BYTE cur_scale, cur_color;
extern const DWORD cur_rgb[8];
extern DWORD __far __pascal GlobalDosAlloc(DWORD);
typedef WORD (__far __pascal *PF_SETCURSOR)(LPVOID, LPVOID);
static LPBYTE cs_user;              /* copy of USER's shape (1 KB) */
static LPBYTE cs_made;              /* the shape for the DIB engine (17 KB) */
static BYTE cs_have;

static WORD cs_size(LPCURSORSHAPE cs)
{
    return sizeof(CURSORSHAPE) + 2 * cs->csWidthBytes * cs->csHeight;
}

static void dibeng_cursor_send(LPCURSORSHAPE cs)
{
    LPCURSORSHAPE o;
    /* double size: 3/2 here - the DIB engine draws pointers up to 32x32 */
    WORD num = cur_scale == 2 ? 3 : 1, den = cur_scale == 2 ? 2 : 1, w, h, wbm, Bpp, r, c, color;
    LPBYTE andm, xorm, oand, oxor;
    DWORD body = 0, white;
    if (!cs || (!cur_color && num == 1) || cs->csColor != 0x0101 || cs->csWidth > 32 || cs->csHeight > 32) {
        ((PF_SETCURSOR)dibe_tab[27])((LPVOID)cs, (LPVOID)scr_pdev);
        return;
    }
    if (!cs_made) {
        DWORD db = GlobalDosAlloc(sizeof(CURSORSHAPE) + 32UL * 4 + 32UL * 32 * 4);
        if (!db) { ((PF_SETCURSOR)dibe_tab[27])((LPVOID)cs, (LPVOID)scr_pdev); return; }
        cs_made = (LPBYTE)MK_FP((WORD)db, 0);
    }
    color = cur_color >= 2;                 /* a color shape at the screen depth */
    Bpp = (g.bpp + 7) / 8;
    w = cs->csWidth * num / den; h = cs->csHeight * num / den;
    if (w > 32) w = 32;
    if (h > 32) h = 32;
    wbm = (w + 15) / 16 * 2;
    o = (LPCURSORSHAPE)cs_made;
    o->csHotX = cs->csHotX * num / den; o->csHotY = cs->csHotY * num / den;
    o->csWidth = w; o->csHeight = h; o->csWidthBytes = wbm;
    o->csColor = color ? (WORD)(0x0001 | ((g.bpp == 15 ? 16 : g.bpp) << 8)) : 0x0101;
    andm = (LPBYTE)(cs + 1); xorm = andm + cs->csWidthBytes * cs->csHeight;
    oand = (LPBYTE)(o + 1); oxor = oand + wbm * h;
    white = pix_of(phys_white());
    if (color) body = pix_of(rgb_to_phys(cur_rgb[cur_color]));
    far_memset(oand, 0xFF, wbm * h);
    far_memset(oxor, 0, color ? w * Bpp * h : wbm * h);
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++) {
            WORD sr = r * den / num, scn = c * den / num;
            BYTE bit = (BYTE)(0x80 >> (scn & 7)), obit = (BYTE)(0x80 >> (c & 7));
            BYTE am = andm[sr * cs->csWidthBytes + (scn >> 3)] & bit;
            BYTE xm = xorm[sr * cs->csWidthBytes + (scn >> 3)] & bit;
            if (!am) oand[r * wbm + (c >> 3)] &= (BYTE)~obit;
            if (color) {
                DWORD v = 0;
                LPBYTE p = oxor + ((DWORD)r * w + c) * Bpp;
                if (!am) v = xm ? body : 0;         /* the white parts in the color */
                else if (xm) v = white;
                if (Bpp == 1) p[0] = (BYTE)v;
                else if (Bpp == 2) *(LPWORD)p = (WORD)v;
                else { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); if (Bpp == 4) p[3] = 0; }
            } else {
                BYTE ox = xm ? 1 : 0;
                if (cur_color == 1 && !am) ox ^= 1;   /* black pointer, white outline */
                if (ox) oxor[r * wbm + (c >> 3)] |= obit;
            }
        }
    ((PF_SETCURSOR)dibe_tab[27])((LPVOID)o, (LPVOID)scr_pdev);
}

void dibeng_setcursor(LPCURSORSHAPE cs)
{
    if (cs && cs->csColor == 0x0101 && cs->csWidth <= 32 && cs->csHeight <= 32) {
        if (!cs_user) {
            DWORD db = GlobalDosAlloc(sizeof(CURSORSHAPE) + 2 * 4 * 32);
            if (db) cs_user = (LPBYTE)MK_FP((WORD)db, 0);
        }
        if (cs_user && cs_size(cs) <= sizeof(CURSORSHAPE) + 2 * 4 * 32) {
            far_memcpy(cs_user, cs, cs_size(cs));
            cs_have = 1;
        }
    } else
        cs_have = 0;
    dibeng_cursor_send(cs);
}

void dibeng_cursor_again(void)
{
    if (cs_have) dibeng_cursor_send((LPCURSORSHAPE)cs_user);
}

/* GDI only calls these for device bitmaps (RC_DEVBITS), which only the DIB
   engine offers: never reached in WINVBE's own drawing mode */
WORD EXPORT SelectBitmap(LPVOID lpDevice, LPVOID lpPrev, LPVOID lpBitmap, DWORD flags)
{
    (void)lpDevice; (void)lpPrev; (void)lpBitmap; (void)flags;
    return 0;
}

DWORD EXPORT BitmapBits(LPVOID lpDevice, DWORD flags, DWORD count, LPVOID bits)
{
    (void)lpDevice; (void)flags; (void)count; (void)bits;
    return 0;
}

#endif
