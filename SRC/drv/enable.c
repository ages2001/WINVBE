/*
 * WINVBE - Enable/Disable, GDIINFO, ColorInfo, palette management,
 *         Control and misc. entry points, load-time init, screen switching.
 */
#include "drv.h"
#include <conio.h>

extern WORD  __far __pascal GetModuleHandle(LPSTR);
#ifndef OLDWIN
extern int   __far __pascal GetPrivateProfileString(LPSTR, LPSTR, LPSTR, LPSTR, int, LPSTR);
extern WORD  __far __pascal FindResource(WORD hInst, LPSTR name, LPSTR type);
extern WORD  __far __pascal LoadResource(WORD hInst, WORD hRes);
extern LPBYTE __far __pascal LockResource(WORD hMem);
#endif

#ifndef OLDWIN
/* default system colours (R,G,B,0 x 19): scrollbar, desktop, active caption,
   inactive caption, menu, window, frame, menu text, window text, caption
   text, active border, inactive border, app workspace, highlight, highlight
   text, button face, button shadow, gray text, button text */
static BYTE colors31[19 * 4] = {        /* Windows 3.1: sky blue, cream workspace */
    192,192,192,0,  160,160,164,0,  166,202,240,0,  255,255,255,0,  255,255,255,0,
    255,255,255,0,  0,0,0,0,        0,0,0,0,        0,0,0,0,        0,0,0,0,
    192,192,192,0,  192,192,192,0,  255,251,240,0,  166,202,240,0,  0,0,0,0,
    192,192,192,0,  128,128,128,0,  128,128,128,0,  0,0,0,0
};
static BYTE colorsnavy[19 * 4] = {      /* classic Windows 3.1 VGA driver scheme */
    192,192,192,0,  192,192,192,0,  0,0,128,0,      255,255,255,0,  255,255,255,0,
    255,255,255,0,  0,0,0,0,        0,0,0,0,        0,0,0,0,        255,255,255,0,
    192,192,192,0,  192,192,192,0,  255,255,255,0,  0,0,128,0,      255,255,255,0,
    192,192,192,0,  128,128,128,0,  192,192,192,0,  0,0,0,0
};

/*
 * The default system colours (used when WIN.INI has no [colors] entries) come
 * from config resource 1, which holds the Windows 3.0 scheme. For the other
 * schemes the preloaded fixed copy is patched before USER reads it.
 * SYSTEM.INI [WINVBE] Scheme=0 automatic (3.0 or 3.1 by version),
 * 1 Windows 3.0, 2 Windows 3.1 (both sky blue), 3 classic navy.
 */
static void select_scheme(void)
{
    WORD scheme = get_ini_int("Scheme", 0), hr, hm, i;
    LPBYTE p, c;
    if (scheme == 0) scheme = ((g.winver >> 8) & 0xFF) >= 10 ? 2 : 1;   /* GetVersion: minor in high byte */
    if (scheme == 2) c = colors31;
    else if (scheme == 3) c = colorsnavy;
    else return;
    hr = FindResource(g.hInst, (LPSTR)MK_FP(0, 1), (LPSTR)MK_FP(0, 28678));
    if (!hr) return;
    hm = LoadResource(g.hInst, hr);
    if (!hm) return;
    p = LockResource(hm);
    if (!p) return;
    for (i = 0; i < 19 * 4; i++) p[18 + i] = c[i];
    DBG("default colours: scheme %u\n", scheme);
}
#endif
extern DWORD __far __pascal GetProcAddress(WORD, LPSTR);


DRVSTATE g;
#ifndef TRACEMAX
#define TRACEMAX 400
#endif
WORD trace_n;
const char *trace_last;
WORD trace_fn(const char *s)
{
    trace_last = s;
    if (g.debug > 2 && trace_n < TRACEMAX) { trace_n++; dbg_printf(s); }
    return 0;
}
DWORD old_int2f;
#pragma aux old_int2f "*"
DWORD rm_winfunc_ptr;
#pragma aux rm_winfunc_ptr "*"
BYTE rm_winfunc_both;
#pragma aux rm_winfunc_both "*"

extern WORD dbg_port;
void env_init(void);
int  vbe_prepare(void);
void cursor_invalidate(void);
static BYTE hooked;

#ifndef OLDWIN
/* Windows 95/98/ME: the display settings of the Display control panel are
   in HKEY_CURRENT_CONFIG\Display\Settings (Resolution "800,600",
   BitsPerPixel "16"). KERNEL has the registry functions only there. */
typedef LONG (__far __pascal *PREGOPENKEY)(DWORD, LPSTR, DWORD __far *);
typedef LONG (__far __pascal *PREGQUERYVALUEEX)(DWORD, LPSTR, DWORD __far *, DWORD __far *, LPBYTE, DWORD __far *);
typedef LONG (__far __pascal *PREGCLOSEKEY)(DWORD);
#define HKEY_CURRENT_CONFIG 0x80000005UL

static WORD parse_num(char __far **pp)
{
    WORD v = 0;
    char __far *p = *pp;
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    *pp = p;
    return v;
}

static void win9x_settings(void)
{
    WORD hk = GetModuleHandle((LPSTR)"KERNEL");
    PREGOPENKEY ro = (PREGOPENKEY)GetProcAddress(hk, (LPSTR)"REGOPENKEY");
    PREGQUERYVALUEEX rq = (PREGQUERYVALUEEX)GetProcAddress(hk, (LPSTR)"REGQUERYVALUEEX");
    PREGCLOSEKEY rc = (PREGCLOSEKEY)GetProcAddress(hk, (LPSTR)"REGCLOSEKEY");
    DWORD key = 0, type, len;
    char buf[32];
    char __far *p;
    WORD w, h, bpp;
    if (!ro || !rq || !rc) return;
    g.win9x = 1;
    if (ro(HKEY_CURRENT_CONFIG, (LPSTR)"Display\\Settings", &key) != 0) return;
    len = sizeof(buf) - 1;
    if (rq(key, (LPSTR)"Resolution", 0, &type, (LPBYTE)buf, &len) == 0) {
        buf[len < sizeof(buf) ? (WORD)len : sizeof(buf) - 1] = 0;
        p = buf;
        w = parse_num(&p);
        if (*p == ',') p++;
        h = parse_num(&p);
        if (w >= 320 && h >= 200) { g.req_w = w; g.req_h = h; g.req_mode = 0; }
    }
    len = sizeof(buf) - 1;
    if (rq(key, (LPSTR)"BitsPerPixel", 0, &type, (LPBYTE)buf, &len) == 0) {
        buf[len < sizeof(buf) ? (WORD)len : sizeof(buf) - 1] = 0;
        p = buf;
        bpp = parse_num(&p);
        /* 1/4 bpp (VGA) are not WINVBE modes: keep the [WINVBE] setting */
        if (bpp == 8 || bpp == 15 || bpp == 16 || bpp == 24 || bpp == 32) g.req_bpp = bpp;
    }
    rc(key);
    DBG("Windows 9x settings: %ux%ux%u\n", g.req_w, g.req_h, g.req_bpp);
}
#endif

/* ------------------------------------------------------------------ */
static void driver_init(void);
static BYTE inited;

/* load time: Windows 1.x boots its drivers before the profile (WIN.INI)
   functions work, so 1.x/2.x initialise on the first Enable call */
void __far __cdecl driver_load(WORD hInst)
{
    g.hInst = hInst;
#ifndef OLDWIN
    inited = 1;
    driver_init();
#endif
}

static void ensure_init(void)
{
    if (!inited) {
        inited = 1;
        driver_init();
    }
}

static void driver_init(void)
{
    char buf[16];
#ifdef MARK
    outp(0xE9, 'I');
    g.debug = 2;
#else
    g.debug = (BYTE)get_ini_int("Debug", 0);
    g.noleave = (BYTE)get_ini_int("NoLeave", 0);
    g.vddmode = (BYTE)get_ini_int("VddMode", 0xFF);
#endif
    dbg_port = get_ini_int("DebugPort", 0xE9);
#ifdef MARK
    DBG("dbgport=%x\n", dbg_port);
    dbg_port = 0xE9;
    DBG("debug read: %u\n", get_ini_int("Debug", 77));
#endif
    env_init();
    palette_init();
#ifndef OLDWIN
    select_scheme();
#endif
    g.req_w = get_ini_int("Width", 0);
    g.req_h = get_ini_int("Height", 0);
    g.req_bpp = get_ini_int("Depth", 0);
    g.req_mode = get_ini_int("Mode", 0);
    g.req_hz = get_ini_int("Refresh", 0);
    g.req_access = (BYTE)get_ini_int("Access", 0);
    g.lfb_check = (BYTE)get_ini_int("LfbCheck", 1);
    g.use_dispi = (BYTE)get_ini_int("Dispi", 1);
    g.dpi = get_ini_int("DPI", 0);
    g.ini_timing = (BYTE)get_ini_int("Timing", 2);
    g.ini_vgareserve = (BYTE)get_ini_int("VgaReserve", 1);
    g.ini_dac8 = (BYTE)get_ini_int("Dac8", 1);
    g.ini_repost = (BYTE)get_ini_int("Repost", 1);
    g.ini_vmaccel = (BYTE)get_ini_int("VMwareAccel", 1);
    g.ini_vmfifo = (BYTE)get_ini_int("VMwareFifo", 1);
    g.ini_shadow = (BYTE)get_ini_int("ShadowBuffer", 1);
    g.ini_wc = (BYTE)get_ini_int("WriteCombining", 1);
#ifndef OLDWIN
    win9x_settings();
    if (g.win9x) g.dpi = 0;     /* Windows 9x: follow the font size of the control panel */
#endif
#ifdef OLDWIN
    if (!g.dpi) g.dpi = 96;
#else
    if (!g.dpi) {
        /* no DPI setting (e.g. installed by Windows 3.0 Setup): follow the
           system font that Setup chose - 8514SYS.FON / WVBSYS.FON mean large fonts */
        char f[20];
        WORD i;
        GetPrivateProfileString("boot", "fonts.fon", "", f, sizeof(f), "system.ini");
        g.dpi = 96;
        for (i = 0; f[i] && f[i + 1] && f[i + 2] && f[i + 3]; i++) {
            if (f[i] == '8' && f[i + 1] == '5' && f[i + 2] == '1' && f[i + 3] == '4') { g.dpi = 120; break; }
            if ((f[i] | 0x20) == 'w' && (f[i + 1] | 0x20) == 'v' && (f[i + 2] | 0x20) == 'b') { g.dpi = 120; break; }
        }
    }
#endif
    if (g.dpi < 48 || g.dpi > 240) g.dpi = 96;
    if (!g.pmode && g.req_access == 1) g.req_access = 0;
    get_ini_str("Forcevga", "0", buf, sizeof(buf));
    if (buf[0] == '1' || !vbe_probe()) {
        g.req_mode = 0x13;
    }
    vbe_select_mode();
    if (!vbe_prepare()) {
        g.req_access = 2;
        vbe_select_mode();
        vbe_prepare();
    }
    rm_winfunc_ptr = g.mi.winfunc;
    rm_winfunc_both = (g.mi.winBattr & 1) && !((g.mi.winAattr & 7) == 7);
    DBG("WINVBE loaded: %ux%ux%u mode %x\n", g.mi.xres, g.mi.yres, g.mi.bpp, g.mi.mode);
}

/* ------------------------------------------------------------------ */
static void set_ratio(GPOINT FAR *win, GPOINT FAR *vpt, DWORD wx, DWORD vx, DWORD wy, DWORD vy)
{
    while (wx > 32767UL || vx > 32767UL || wy > 32767UL || vy > 32767UL) {
        wx = (wx + 1) >> 1; vx = (vx + 1) >> 1; wy = (wy + 1) >> 1; vy = (vy + 1) >> 1;
    }
    if (!vx) vx = 1;
    if (!vy) vy = 1;
    win->x = (SHORT)wx; win->y = (SHORT)wy;
    vpt->x = (SHORT)vx; vpt->y = -(SHORT)vy;
}

/* GDI 1.x/2.x know GDIINFO only up to the five reserved words after
   dpDCManage (104 bytes); the dpPal* fields were added in 3.0 */
#ifdef OLDWIN
#define GDIINFO_SIZE 104
#else
#define GDIINFO_SIZE sizeof(GDIINFO)
#endif

static void fill_gdiinfo(LPGDIINFO gi)
{
    WORD hs, vs;
    far_memset(gi, 0, GDIINFO_SIZE);
    hs = (WORD)((DWORD)g.mi.xres * 254UL / ((DWORD)g.dpi * 10UL));
    vs = (WORD)((DWORD)g.mi.yres * 254UL / ((DWORD)g.dpi * 10UL));
    if (!hs) hs = 1;
    if (!vs) vs = 1;
#ifdef OLDWIN
    gi->dpVersion = (g.winver & 0xFF) >= 2 ? 0x200 : 0x100;
#else
    /* GDI of Windows 95/98/ME only accepts Windows 3.1 (and later) drivers */
    gi->dpVersion = g.win9x ? 0x30A : 0x300;
#endif
    gi->dpTechnology = 1;           /* DT_RASDISPLAY */
    gi->dpHorzSize = hs;
    gi->dpVertSize = vs;
    gi->dpHorzRes = g.mi.xres;
    gi->dpVertRes = g.mi.yres;
    gi->dpBitsPixel = g.mi.Bpp == 2 ? 16 : g.mi.Bpp * 8;
    gi->dpPlanes = 1;
    gi->dpNumBrushes = -1;
    gi->dpNumPens = 100;
    gi->dpNumFonts = 0;
    gi->dpNumColors = 20;
    gi->dpDEVICEsize = sizeof(PDEVICE);
    gi->dpCurves = 0;
    gi->dpLines = 0x22;             /* LC_POLYLINE | LC_STYLED */
    gi->dpPolygonals = 0x08;        /* PC_SCANLINE */
    gi->dpText = 0x2004;            /* TC_CP_STROKE | TC_RA_ABLE */
    gi->dpClip = 1;                 /* CP_RECTANGLE */
    gi->dpRaster = 0x0001 | 0x0008 | 0x0010 | 0x0080 | 0x0200 | 0x0800 | 0x2000;  /* +STRETCHBLT, STRETCHDIB */
#ifdef OLDWIN
    /* GDI 2.x: RC_BITBLT | RC_BITMAP64 | RC_GDI20_OUTPUT; GDI 1.x: RC_BITBLT */
    gi->dpRaster = (g.winver & 0xFF) >= 2 ? 0x0019 : 0x0001;
    gi->dpNumColors = g.mi.Bpp == 1 ? 256 : 20;
#else
    if (g.mi.Bpp == 1) gi->dpRaster |= 0x0100;   /* RC_PALETTE */
    /* Windows 95/98/ME: GDI only has 3.0 format fonts - RC_BIGFONT */
    if (g.win9x) gi->dpRaster |= 0x0400;
#endif
    gi->dpAspectX = 36;
    gi->dpAspectY = 36;
    gi->dpAspectXY = 51;
    gi->dpStyleLen = 72;
    set_ratio(&gi->dpMLoWin, &gi->dpMLoVpt, (DWORD)hs * 10, g.mi.xres, (DWORD)vs * 10, g.mi.yres);
    set_ratio(&gi->dpMHiWin, &gi->dpMHiVpt, (DWORD)hs * 100, g.mi.xres, (DWORD)vs * 100, g.mi.yres);
    set_ratio(&gi->dpELoWin, &gi->dpELoVpt, (DWORD)g.dpi, 100, (DWORD)g.dpi, 100);
    {
        /* english: units of 1/100 inch -> dpi/100 pixels per unit */
        gi->dpELoWin.x = 100; gi->dpELoWin.y = 100; gi->dpELoVpt.x = g.dpi; gi->dpELoVpt.y = -(SHORT)g.dpi;
        gi->dpEHiWin.x = 1000; gi->dpEHiWin.y = 1000; gi->dpEHiVpt.x = g.dpi; gi->dpEHiVpt.y = -(SHORT)g.dpi;
        gi->dpTwpWin.x = 1440; gi->dpTwpWin.y = 1440; gi->dpTwpVpt.x = g.dpi; gi->dpTwpVpt.y = -(SHORT)g.dpi;
    }
    gi->dpLogPixelsX = g.dpi;
    gi->dpLogPixelsY = g.dpi;
    gi->dpDCManage = 4;
#ifndef OLDWIN
    if (g.mi.Bpp == 1) {
        gi->dpPalColors = 256;
        gi->dpPalReserved = 20;
        gi->dpPalResolution = 18;
    }
#endif
}

/* ------------------------------------------------------------------ */
/* Windows 9x: see vbe_dispi_repair() */
void win9x_check_screen(void)
{
    if (!g.win9x || !g.enabled || g.in_background) return;
    if (vbe_dispi_repair()) {
        DBG("DISPI was switched off (MS-DOS box): mode restored\n");
        /* the screen may hold text mode data: repaint at the next
           UserRepaintDisable(FALSE) (task time) */
        g.repaint_pending = 1;
    }
}

static void load_palette_hw(void)
{
    if (g.palettized)
        vbe_set_dac(0, 256, g.pal);
}

/* after a mode set: palette, snapshots of the hardware state */
static void after_mode_set(void)
{
    if (g.palettized && g.dacbits == 8 && (g.mi.mode == 0x13 || g.mi.mode == 0xFFFF))
        g.dacbits = 6;
    load_palette_hw();
    /* Windows 3.0 GDI hands palette devices DIB color tables as 16-bit
       physical colors; non-palette devices get plain RGBQUADs */
    /* GDI (3.0, and 3.1 for a dpVersion 0x300 driver) passes DIB colour
       tables to palette devices as WORD physical colours */
    g.dibwords = g.palettized;
    g.enabled = 1;
    vbe_dispi_snapshot();
    if (!g.win9x) vga_snapshot();
    g.in_background = 0;
    cursor_invalidate();
}

/* shadow frame buffer for the current mode (see drv.h); not with VMware
   SVGA II (its host does fills and copies in video memory, which is fast
   to read in a virtual machine) */
static void setup_shadow(void)
{
    g.shadow = 0;
    if (g.ini_shadow && !g.svga && g.pmode && g.cpu386 && (g.enhanced || g.win9x) &&
        shadow_alloc((DWORD)g.pitch * g.yres))
        g.shadow = 1;
    else
        shadow_free();
}

/* set the mode; restore = picture from the shadow (returns 1 when done,
   else the screen is cleared) */
int enable_hw(BYTE restore)
{
    WORD opitch = g.pitch, oy = g.yres;
    BYTE had = g.shadow;
    g.shadow = 0;
    if (!vbe_set_mode()) {
        DBG("Enable: mode set failed, falling back to VGA 13h\n");
        g.req_mode = 0x13;
        vbe_select_mode();
        vbe_set_mode();
    }
    after_mode_set();
    setup_shadow();
    if (restore && had && g.shadow && opitch == g.pitch && oy == g.yres) {
        shadow_flush(0, 0, g.xres, g.yres);
        return 1;
    }
    /* clear screen */
    {
        WORD y;
        for (y = 0; y < g.yres; y++)
            scr_fill(y, 0, g.xres, 0);
    }
    return 0;
}

WORD EXPORT Enable(LPVOID lpDevice, WORD style, LPSTR lpDevType, LPSTR lpOutFile, LPVOID lpData)
{
    TRACE("Enable\n");
#ifdef MARK
    outp(0xE9, 'E');
#endif
    ensure_init();
    DBG("Enable(%p, %x)\n", lpDevice, style);
    if (style & 1) {
        fill_gdiinfo((LPGDIINFO)lpDevice);
#ifdef OLDWIN
        {   /* test hook: GdiPatch=idx:val,idx:val (word index into GDIINFO) */
            char pb[400];
            WORD i = 0;
            get_ini_str("GdiPatch", "", pb, sizeof(pb));
            while (pb[i]) {
                WORD idx = 0, neg = 0, v = 0;
                while (pb[i] >= '0' && pb[i] <= '9') idx = idx * 10 + pb[i++] - '0';
                if (pb[i] == ':') i++;
                if (pb[i] == '-') { neg = 1; i++; }
                while (pb[i] >= '0' && pb[i] <= '9') v = v * 10 + pb[i++] - '0';
                if (idx < GDIINFO_SIZE / 2) ((LPWORD)lpDevice)[idx] = neg ? (WORD)(-(SHORT)v) : v;
                if (pb[i] == ',') i++; else break;
            }
        }
#endif
        if (g.debug > 1) {
            LPWORD w = (LPWORD)lpDevice;
            WORD i;
            DBG("GDIINFO:");
            for (i = 0; i < GDIINFO_SIZE / 2; i++) DBG(" %d", (SHORT)w[i]);
            DBG("\n");
        }
        return GDIINFO_SIZE;
    }
    {
        LPPDEVICE pd = (LPPDEVICE)lpDevice;
        far_memset(pd, 0, sizeof(PDEVICE));
        pd->bm.bmType = PDEV_TYPE;
        pd->bm.bmWidth = g.mi.xres;
        pd->bm.bmHeight = g.mi.yres;
        pd->bm.bmWidthBytes = g.mi.xres * g.mi.Bpp;
        pd->bm.bmPlanes = 1;
        pd->bm.bmBitsPixel = g.mi.Bpp == 2 ? 16 : g.mi.Bpp * 8;
        pd->magic = PDEV_MAGIC;
    }
    if (style & 0x8000)          /* information context only */
        return 1;
#ifndef OLDWIN
    /* INT 2Fh 4001h/4002h screen switch notifications exist from 3.0 on */
    if (!hooked) {
        hook_int2f(); hooked = 1;
        /* Windows 3.x: real video cards need the VDD's help (4001h/4002h)
           for full-screen DOS boxes, their VBE mode is unknown to it */
        if (g.vddmode == 0xFF) g.vddmode = vbe_virtual_hw() ? 0 : 1;
        if (!g.win9x && g.vddmode) g.vdd_assist = (BYTE)vdd_assist(g.vddmode);
    }
#endif
#ifndef OLDWIN
    /* 3.x only exports (asking 1.x for a missing ordinal is fatal) */
    if (!g.devcolormatch) {
        WORD hgdi = GetModuleHandle((LPSTR)"GDI");
        if (hgdi) g.devcolormatch = GetProcAddress(hgdi, (LPSTR)MK_FP(0, 449));
        DBG("GDI=%x DeviceColorMatch=%lx\n", hgdi, g.devcolormatch);
    }
    if (!g.repaint_proc) {
        WORD hu = GetModuleHandle((LPSTR)"USER");
        if (hu) g.repaint_proc = GetProcAddress(hu, (LPSTR)MK_FP(0, 275));
    }
#endif
    screen_busy++;
    enable_hw(0);
    screen_busy--;
    cursor_show();
    return 1;
}

WORD EXPORT Disable(LPVOID lpDevice)
{
    TRACE("Disable\n");
    DBG("Disable\n");
    cursor_hide_all();
#ifndef OLDWIN
    if (g.dpms) vbe_dpms(0);
#endif
    g.enabled = 0;
    shadow_free();
    vbe_text_mode();
    if (hooked) { if (g.vdd_assist) { vdd_assist(0); g.vdd_assist = 0; } unhook_int2f(); hooked = 0; }
    return 1;
}

static void redraw_from_shadow(void);

/* called by USER: TRUE = don't call RepaintScreen now */
void EXPORT UserRepaintDisable(WORD disable)
{
    g.repaint_disable = (BYTE)disable;
    if (!disable) win9x_check_screen();
    /* Windows 3.x enhanced mode, back from a full-screen DOS box: the VDD
       has reloaded its old VGA state - set our mode again and repaint */
    /* SVGA II: the VGA registers do not matter, only whether the DOS box
       switched the SVGA mode off */
    if (!disable && g.enabled && !g.in_background && g.svga) {
        /* (WINVBE.386 normally did it already); the text mode of the box
           has used the start of video memory: repaint everything */
        screen_busy++;
        if (!g.win9x && svga_repair()) DBG("SVGA II mode switched on again\n");
        svga_reverify();
        screen_busy--;
        cursor_show();
        g.repaint_pending = 1;
    } else
    if (!disable && !g.win9x && g.enabled && !g.in_background && g.vdd_assist != 1 && vga_changed()) {
        screen_busy++;
        g.curbank = 0xFFFF;
        /* usually only the DISPI interface was switched off (the BIOS of
           the DOS box) - the VDD has put the VGA registers back itself */
        if (vbe_dispi_repair() && !vga_changed()) {
            DBG("VGA state: DISPI restored\n");
        } else if (vga_restore()) {
            DBG("VGA state changed by the VDD: registers restored\n");
        } else {
            DBG("VGA state changed by the VDD: mode set again\n");
            enable_hw(1);
        }
        screen_busy--;
        cursor_show();
        g.repaint_pending = 1;
    }
    if (!disable && g.repaint_pending && (g.repaint_proc || g.shadow)) {
        g.repaint_pending = 0;
        if (g.shadow) redraw_from_shadow();
        else ((void (__far __pascal *)(void))g.repaint_proc)();
    }
}

/* the whole picture from the shadow copy (instead of a repaint by USER) */
static void redraw_from_shadow(void)
{
    if (!g.enabled || g.in_background) return;
    cursor_hide_all();
    screen_busy++;
    shadow_flush(0, 0, g.xres, g.yres);
    screen_busy--;
    cursor_show();
}

/* repaint the whole screen now, or at the next UserRepaintDisable(FALSE) */
static void repaint_now(void)
{
    if (g.shadow) { redraw_from_shadow(); return; }
    if (!g.repaint_proc) return;
    if (g.repaint_disable) g.repaint_pending = 1;
    else ((void (__far __pascal *)(void))g.repaint_proc)();
}

/* WINVBE.386, the system has woken up from suspend (APM or ACPI): the
   adapter may have lost the mode, or with its power the whole video BIOS
   state - set the mode again (initialising the video BIOS once more when
   that fails) and repaint */
#ifndef OLDWIN
/* set our mode again through the BIOS, keeping the BIOS data area that
   DOS boxes share with Windows; check = only when the mode was lost */
static void mode_again(BYTE check, BYTE repost)
{
    DBG("mode again (check %u)\n", check);
    screen_busy++;
    g.curbank = 0xFFFF;
    g.dpms = 0;
    if (check && vbe_mode_intact()) {
        /* the BIOS has restored the mode (APM BIOSes usually do): only
           the palette and the picture */
        DBG("mode kept\n");
        load_palette_hw();
    } else if (vbe_dispi_repair()) {
        DBG("DISPI registers written again\n");
    } else {
        vbe_bda_save();
        if (!vbe_set_mode()) {
            DBG("mode set failed\n");
            if (!repost || !vbe_repost() || !vbe_set_mode()) {
                g.req_mode = 0x13;
                vbe_select_mode();
                vbe_set_mode();
            }
        }
        vbe_bda_restore();
        after_mode_set();
    }
    screen_busy--;
    cursor_invalidate();
    repaint_now();
    cursor_show();
}

static void power_resume(void)
{
    DBG("power: resume (enabled %u, background %u)\n", g.enabled, g.in_background);
    if (!g.enabled || g.in_background) return;      /* INT 2Fh 4002h sets it later */
    mode_again(1, 1);
    DBG("power: resume done\n");
}
#endif

/* screen switch notifications from the VDD / task switcher; WVB_NOTIFY
   from WINVBE.386 (BX = event) */
void __far __cdecl on_int2f(WORD ax, WORD bx)
{
#ifndef OLDWIN
    if (ax == WVB_NOTIFY) {
        DBG("WINVBE.386 notification %u (enabled %u, background %u)\n", bx, g.enabled, g.in_background);
        if (bx == WVBN_RESUME) power_resume();
        else if (bx == WVBN_SUSPEND) DBG("power: suspend\n");
        else if ((bx == WVBN_MODECHECK || bx == WVBN_FOREGROUND) && g.enabled && !g.in_background) {
            /* real hardware: a DOS box has used the video BIOS, which left
               the VBE mode (full screen, or a windowed box setting its mode
               on the hardware); after a 4002h this was done already */
            if (bx == WVBN_FOREGROUND && g.fg_4002) DBG("foreground: done by 4002h\n");
            else mode_again(0, 0);
        }
        g.fg_4002 = 0;
        return;
    }
#endif
    if (ax == 0x4001) {
        DBG("INT2F 4001 (background)\n");
        screen_busy++;
        g.in_background = 1;
        cursor_invalidate();
        /* the VDD of Windows 9x only reloads the standard VGA registers for a
           full-screen DOS box: leave the VBE mode first (3.x: the VDD does
           its own mode change, an INT 10h from the system VM here upsets it) */
        if (g.win9x) vbe_text_mode();
        else if (g.svga) svga_off();        /* SVGA II: back to the VGA */
        else if (g.vdd_assist && !g.noleave) vbe_leave_bg();
        screen_busy--;
    } else if (ax == 0x4002) {
        DBG("INT2F 4002 (foreground)\n");
        g.fg_4002 = 1;
        if (!g.enabled) { g.in_background = 0; return; }
        screen_busy++;
        g.curbank = 0xFFFF;
        if (enable_hw(1)) {
            DBG("picture restored from the shadow\n");
            screen_busy--;
        } else {
            screen_busy--;
            repaint_now();
        }
        cursor_show();
    }
}

/* ------------------------------------------------------------------ */
DWORD EXPORT ColorInfo(LPVOID lpDev, DWORD color, LPDWORD lpPhys)
{
    TRACE("ColorInfo\n");
    if (lpPhys) {
        DWORD p = color_for_dev(lpDev, color);
        *lpPhys = p;
        if (((LPGBITMAP)lpDev)->bmType == 0 && ((LPGBITMAP)lpDev)->bmBitsPixel == 1)
            return (p & PHYS_MONO) ? 0x00FFFFFFUL : 0;
        return phys_to_rgb(p);
    }
    if (((LPGBITMAP)lpDev)->bmType == 0 && ((LPGBITMAP)lpDev)->bmBitsPixel == 1)
        return (color & PHYS_MONO) ? 0x00FFFFFFUL : 0;
    return phys_to_rgb(color);
}

/* ------------------------------------------------------------------ */
WORD EXPORT SetPalette(WORD start, WORD count, LPBYTE pal)
{
    TRACE("SetPalette\n");
    WORD i;
    if (!pal || start > 255) return 0;
    if (start + count > 256) count = 256 - start;
    DBG("SetPalette(%u,%u) %02x%02x%02x\n", start, count, pal[0], pal[1], pal[2]);
    for (i = 0; i < count; i++) {
        g.pal[(start + i) * 4 + 0] = pal[i * 4 + 0];
        g.pal[(start + i) * 4 + 1] = pal[i * 4 + 1];
        g.pal[(start + i) * 4 + 2] = pal[i * 4 + 2];
        g.pal[(start + i) * 4 + 3] = 1;
    }
    invcache_reset();
    if (g.enabled && !g.in_background && g.palettized) {
        screen_busy++;
        vbe_set_dac(start, count, g.pal + start * 4);
        screen_busy--;
    }
    return 0;
}

WORD EXPORT GetPalette(WORD start, WORD count, LPBYTE pal)
{
    TRACE("GetPalette\n");
    WORD i;
    if (!pal || start > 255) return 0;
    if (start + count > 256) count = 256 - start;
    for (i = 0; i < count; i++) {
        pal[i * 4 + 0] = g.pal[(start + i) * 4 + 0];
        pal[i * 4 + 1] = g.pal[(start + i) * 4 + 1];
        pal[i * 4 + 2] = g.pal[(start + i) * 4 + 2];
        pal[i * 4 + 3] = 0;
    }
    return 0;
}

WORD EXPORT SetPaletteTranslate(LPWORD xlat)
{
    TRACE("SetPaletteTranslate\n");
    WORD i;
    g.xlat_identity = 1;
    for (i = 0; i < 256; i++) {
        g.xlat[i] = xlat ? (xlat[i] & 0xFF) : i;
        if (g.xlat[i] != i) g.xlat_identity = 0;
    }
    return 0;
}

WORD EXPORT GetPaletteTranslate(LPWORD xlat)
{
    TRACE("GetPaletteTranslate\n");
    WORD i;
    if (!xlat) return 0;
    for (i = 0; i < 256; i++) xlat[i] = g.xlat[i];
    return 0;
}

void EXPORT UpdateColors(WORD x, WORD y, WORD w, WORD h, LPWORD xlat)
{
    WORD r, c;
    if (!xlat || !g.palettized || !g.enabled) return;
    if (x >= g.xres || y >= g.yres) return;
    if (x + w > g.xres) w = g.xres - x;
    if (y + h > g.yres) h = g.yres - y;
    screen_busy++;
    cursor_exclude(x, y, x + w, y + h);
    for (r = 0; r < h; r++) {
        WORD left = w, xx = x;
        while (left) {
            WORD n = left > CHUNKPX ? CHUNKPX : left;
            scr_read(y + r, xx, n, bufD);
            for (c = 0; c < n; c++) bufD[c] = (BYTE)xlat[bufD[c]];
            scr_write(y + r, xx, n, bufD);
            xx += n; left -= n;
        }
    }
    cursor_unexclude();
    screen_busy--;
}

/* ------------------------------------------------------------------ */
#define QUERYESCSUPPORT 8
#define GETCOLORTABLE   5
#define WINVBE_GETINFO   0x7701
/* WINVBE_DPMS: IN = WORD power state of the monitor (0 on, 1 stand by,
   2 suspend, 4 off; FFFFh = query).  Query returns 100h | VBE/PM states
   (bit 7 = VBE/PM present), a set 1 */
#define WINVBE_DPMS      0x7702

typedef struct {
    WORD  size;         /* sizeof this struct */
    WORD  version;      /* driver version (BCD 0x0100 = 1.00) */
    WORD  mode;         /* VBE mode, 0x13 = VGA, 0xFFFF = DISPI */
    WORD  width, height, bpp;
    WORD  pitch;
    WORD  access;       /* 1 LFB, 2 banked, 3 VGA */
    WORD  vbever;
    WORD  refresh;      /* 0.01 Hz units, 0 = BIOS default */
    WORD  dpi;
    WORD  memkb;
    char  oem[64];
} WINVBEINFO;

/* ------------------------------------------------------------------ */
/* DCI provider (primary surface): direct frame buffer access for Video
   for Windows, DirectDraw's emulation layer and other DCI clients
   (Windows 3.1 with DCI runtime, Windows 95/98/ME).  Linear frame buffer
   in 386 enhanced mode only: the surface is a 16:32 selector over it.    */
#define DCICOMMAND              3075
#define DCI_VERSION             0x0100
#define DCICREATEPRIMARYSURFACE 1
#define DCI_OK                  0
#define DCI_FAIL_GENERIC        (-1)
#define DCI_FAIL_UNSUPPORTEDVERSION (-2)
#define DCI_FAIL_UNSUPPORTED    (-4)
#define DCI_ERR_CURRENTLYNOTAVAIL (-5)
#define DCI_STATUS_POINTERCHANGED 1
#define DCI_STATUS_STRIDECHANGED  2
#define DCI_STATUS_FORMATCHANGED  4
#define DCI_STATUS_SURFACEINFOCHANGED 8
#define DCI_PRIMARY             0x00
#define DCI_VISIBLE             0x10
#define DCI_1632_ACCESS         0x40

typedef struct {
    DWORD dwCommand, dwParam1, dwParam2, dwVersion, dwReserved;
} DCICMD;

typedef struct {
    DWORD dwSize;
    DWORD dwDCICaps;
    DWORD dwCompression;
    DWORD dwMask[3];
    DWORD dwWidth;
    DWORD dwHeight;
    LONG  lStride;
    DWORD dwBitCount;
    DWORD dwOffSurface;
    WORD  wSelSurface;
    WORD  wReserved;
    DWORD dwReserved1, dwReserved2, dwReserved3;
    SHORT (__far __pascal *BeginAccess)(LPVOID, LPGRECT);
    void  (__far __pascal *EndAccess)(LPVOID);
    void  (__far __pascal *DestroySurface)(LPVOID);
} DCISURF;

static DCISURF dci;
static WORD dci_sel;            /* 16:32 selector over the frame buffer */
static DWORD dci_base, dci_limit;
static BYTE dci_in;             /* BeginAccess .. EndAccess */

static GRECT dci_rc;            /* rectangle of the access (shadow) */

/* with a shadow frame buffer DCI clients get the shadow (fast to read as
   well), and EndAccess copies their rectangle to the screen */
static int dci_possible(void)
{
    if (!g.enabled || !g.cpu386 || !(g.enhanced || g.win9x)) return 0;
    if (g.shadow) return 1;
    return g.access == ACC_LFB && g.lfbsel && g.lfblin;
}

/* fill the surface info from the current mode; DCI_STATUS_x of what changed */
static SHORT dci_fill(void)
{
    SHORT st = 0;
    DWORD base = g.lfblin, limit = ((DWORD)g.lfbtiles << 16) - 1;
    DWORD ofs = g.fbofs;
    DWORD comp = 0, m0 = 0, m1 = 0, m2 = 0;
    if (g.shadow) { base = g.shlin; limit = g.shsize - 1; ofs = 0; }
    if (!dci_sel) {
        dci_sel = sel_alloc(1);
        if (!dci_sel) return DCI_FAIL_GENERIC;
        dci_base = dci_limit = 0;
    }
    if (base != dci_base || limit != dci_limit) {
        if (!sel_setbase(dci_sel, base) || !sel_setlimit(dci_sel, limit)) return DCI_FAIL_GENERIC;
        dci_base = base; dci_limit = limit;
        st |= DCI_STATUS_POINTERCHANGED;
    }
    if (g.bpp == 16) {
        comp = 3;                           /* BI_BITFIELDS, 5-6-5 */
        m0 = 0xF800; m1 = 0x07E0; m2 = 0x001F;
    } else if (g.bpp == 15) {
        m0 = 0x7C00; m1 = 0x03E0; m2 = 0x001F;
    } else if (g.bpp >= 24) {
        m0 = 0xFF0000UL; m1 = 0xFF00; m2 = 0xFF;
    }
    if (dci.dwOffSurface != ofs || dci.wSelSurface != dci_sel) st |= DCI_STATUS_POINTERCHANGED;
    if (dci.lStride != (LONG)g.pitch) st |= DCI_STATUS_STRIDECHANGED;
    if (dci.dwBitCount != (g.bpp == 15 ? 16UL : g.bpp) || dci.dwCompression != comp || dci.dwMask[0] != m0)
        st |= DCI_STATUS_FORMATCHANGED;
    if (dci.dwWidth != g.xres || dci.dwHeight != g.yres) st |= DCI_STATUS_SURFACEINFOCHANGED;
    dci.dwSize = sizeof(dci);
    dci.dwDCICaps = DCI_PRIMARY | DCI_VISIBLE | DCI_1632_ACCESS;
    dci.dwCompression = comp;
    dci.dwMask[0] = m0; dci.dwMask[1] = m1; dci.dwMask[2] = m2;
    dci.dwWidth = g.xres;
    dci.dwHeight = g.yres;
    dci.lStride = g.pitch;
    dci.dwBitCount = g.bpp == 15 ? 16 : g.bpp;
    dci.dwOffSurface = ofs;
    dci.wSelSurface = dci_sel;
    return st;
}

static SHORT __far __pascal __loadds dci_begin(LPVOID surf, LPGRECT rc)
{
    SHORT st;
    if (!dci_possible() || g.in_background) return DCI_ERR_CURRENTLYNOTAVAIL;
    st = dci_fill();
    if (st < 0) return st;
    screen_busy++;
    dci_in = 1;
    if (rc) dci_rc = *rc;
    else { dci_rc.left = dci_rc.top = 0; dci_rc.right = g.xres; dci_rc.bottom = g.yres; }
    if (dci_rc.left < 0) dci_rc.left = 0;
    if (dci_rc.top < 0) dci_rc.top = 0;
    cursor_exclude(dci_rc.left, dci_rc.top, dci_rc.right, dci_rc.bottom);
    return st;
}

static void __far __pascal __loadds dci_end(LPVOID surf)
{
    if (!dci_in) return;
    dci_in = 0;
    if (g.shadow && dci_rc.right > dci_rc.left && dci_rc.bottom > dci_rc.top)
        shadow_flush(dci_rc.left, dci_rc.top, dci_rc.right, dci_rc.bottom);
    cursor_unexclude();
    screen_busy--;
}

static void __far __pascal __loadds dci_destroy(LPVOID surf)
{
    dci_end(surf);
}

static SHORT dci_command(DCICMD FAR *cmd, LPVOID lpOut)
{
    SHORT st;
    if (cmd->dwVersion != DCI_VERSION) return DCI_FAIL_UNSUPPORTEDVERSION;
    if (cmd->dwCommand != DCICREATEPRIMARYSURFACE) return DCI_FAIL_UNSUPPORTED;
    if (!dci_possible()) return DCI_FAIL_UNSUPPORTED;
    st = dci_fill();
    if (st < 0) return st;
    dci.BeginAccess = dci_begin;
    dci.EndAccess = dci_end;
    dci.DestroySurface = dci_destroy;
    *(DCISURF FAR * FAR *)lpOut = (DCISURF FAR *)&dci;
    DBG("DCI primary: sel %x ofs %lx %lux%lu stride %ld bits %lu\n", dci_sel, dci.dwOffSurface,
        dci.dwWidth, dci.dwHeight, dci.lStride, dci.dwBitCount);
    return DCI_OK;
}

SHORT EXPORT Control(LPVOID lpDev, WORD func, LPVOID lpIn, LPVOID lpOut)
{
    TRACE("Control\n");
    switch (func) {
    case QUERYESCSUPPORT: {
        WORD esc = *(LPWORD)lpIn;
        if (esc == DCICOMMAND) return dci_possible() ? DCI_VERSION : 0;
        return (esc == QUERYESCSUPPORT || esc == GETCOLORTABLE || esc == WINVBE_GETINFO ||
                esc == WINVBE_DPMS) ? 1 : 0;
    }
#ifndef OLDWIN
    case WINVBE_DPMS: {
        WORD st = lpIn ? *(LPWORD)lpIn : 0;
        if (st == 0xFFFF) return (SHORT)(0x100 | vbe_pm_caps());
        if (st != 0 && st != 1 && st != 2 && st != 4) return -1;
        if (!g.enabled || g.in_background) return 0;
        screen_busy++;
        vbe_dpms(st);
        screen_busy--;
        return 1;
    }
#endif
    case DCICOMMAND:
        return dci_command((DCICMD FAR *)lpIn, lpOut);
    case WINVBE_GETINFO: {
        WINVBEINFO FAR *wi = (WINVBEINFO FAR *)lpOut;
        WORD i;
        far_memset(wi, 0, sizeof(WINVBEINFO));
        wi->size = sizeof(WINVBEINFO);
        wi->version = 0x0110;
        wi->mode = g.mi.mode; wi->width = g.xres; wi->height = g.yres; wi->bpp = g.bpp;
        /* access: bit 7 = LFB unusable, 6 = write combining, 5 = shadow */
        wi->pitch = g.pitch; wi->access = g.access | (g.lfb_state == 2 ? 0x80 : 0) | (g.wc ? 0x40 : 0) |
                                          (g.shadow ? 0x20 : 0);
        wi->vbever = g.vbever;
        wi->refresh = g.refresh_set ? g.refresh_cur : 0;
        wi->dpi = g.dpi; wi->memkb = g.totalmem64k * 64;
        for (i = 0; i < 63 && g.oemstr[i]; i++) wi->oem[i] = g.oemstr[i];
        return 1;
    }
    case GETCOLORTABLE: {
        WORD i = *(LPWORD)lpIn;
        if (!g.palettized || i > 255) return 0;
        *(LPDWORD)lpOut = (DWORD)g.pal[i * 4] | ((DWORD)g.pal[i * 4 + 1] << 8) | ((DWORD)g.pal[i * 4 + 2] << 16);
        return 1;
    }
    }
    return 0;
}

WORD EXPORT DeviceMode(WORD hWnd, WORD hInst, LPSTR lpDevType, LPSTR lpOutFile)
{
    TRACE("DeviceMode\n");
    return 1;
}

WORD EXPORT SetAttribute(LPVOID lpDev, WORD state, WORD index, WORD attr)
{
    TRACE("SetAttribute\n");
    return 0;
}

WORD EXPORT DeviceBitmap(LPVOID lpDev, WORD cmd, LPVOID lpBitmap, LPBYTE lpBits)
{
    TRACE("DeviceBitmap\n");
    return 0;
}

WORD EXPORT FastBorder(LPGRECT lpRect, WORD hb, WORD vb, DWORD rop, LPVOID lpDev,
                       LPPBRUSH br, LPDRAWMODE dm, LPGRECT clip)
{
    TRACE("FastBorder\n");
    return 0;
}

WORD EXPORT EnumDFonts(LPVOID lpDev, LPSTR face, LPVOID cb, LPVOID data)
{
    TRACE("EnumDFonts\n");
    return 1;
}

typedef SHORT (__far __pascal *ENUMPROC)(LPVOID obj, LPVOID data);

static BYTE enum_colors[16 * 3] = {
    0,0,0, 0x80,0,0, 0,0x80,0, 0x80,0x80,0, 0,0,0x80, 0x80,0,0x80, 0,0x80,0x80, 0x80,0x80,0x80,
    0xC0,0xC0,0xC0, 0xFF,0,0, 0,0xFF,0, 0xFF,0xFF,0, 0,0,0xFF, 0xFF,0,0xFF, 0,0xFF,0xFF, 0xFF,0xFF,0xFF
};

SHORT EXPORT EnumObj(LPVOID lpDev, WORD style, ENUMPROC cb, LPVOID data)
{
    TRACE("EnumObj\n");
    WORD i, s;
    SHORT r = 1;
    if (style == OBJ_PEN) {
        LOGPEN lp;
        for (s = 0; s <= PS_DASHDOTDOT; s++)
            for (i = 0; i < 16; i++) {
                lp.lopnStyle = s; lp.lopnWidth.x = 1; lp.lopnWidth.y = 0;
                lp.lopnColor = (DWORD)enum_colors[i * 3] | ((DWORD)enum_colors[i * 3 + 1] << 8) |
                               ((DWORD)enum_colors[i * 3 + 2] << 16);
                r = cb((LPVOID)&lp, data);
                if (!r) return 0;
            }
    } else if (style == OBJ_BRUSH) {
        LOGBRUSH lb;
        for (i = 0; i < 16; i++) {
            lb.lbStyle = BS_SOLID; lb.lbHatch = 0; lb.lbBkColor = 0;
            lb.lbColor = (DWORD)enum_colors[i * 3] | ((DWORD)enum_colors[i * 3 + 1] << 8) |
                         ((DWORD)enum_colors[i * 3 + 2] << 16);
            r = cb((LPVOID)&lb, data);
            if (!r) return 0;
        }
        for (s = 0; s < 6; s++) {
            lb.lbStyle = BS_HATCHED; lb.lbHatch = s; lb.lbColor = 0; lb.lbBkColor = 0x00FFFFFFUL;
            r = cb((LPVOID)&lb, data);
            if (!r) return 0;
        }
        lb.lbStyle = BS_HOLLOW;
        r = cb((LPVOID)&lb, data);
    }
    return r;
}
