/*
 * WINVBE - DirectDraw HAL, display driver part (Windows 95/98/ME).
 *
 * DirectDraw talks to a display driver with a HAL through the DCICOMMAND
 * escape (version DD_VERSION): it asks for the name of the driver's 32-bit
 * part (WVBDD32.DLL, which does the blits on the CPU and the YUV formats
 * and overlay that DirectDraw's own emulation does not have), gives the
 * driver its DDHAL_SetInfo function, and has the driver describe the
 * current mode and the driver's capabilities with it - again after every
 * mode change.
 *
 * Only with the DIB engine on a linear frame buffer (dibeng.c), 8 bits per
 * pixel and more, and without the shadow frame buffer: DirectDraw then
 * writes the same memory the DIB engine draws in. The video memory behind
 * the screen is given to DirectDraw as one linear heap for off-screen, YUV
 * and overlay surfaces; it is mapped for
 * DirectDraw as a whole (the driver's own mapping covers the screen only).
 * Flips of the screen move the display start where the hardware has a way
 * the 32-bit part can reach (the DISPI registers of Bochs, QEMU and
 * VirtualBox); elsewhere the back buffer is copied to the screen.
 * The 32-bit part also has a Direct3D HAL (dd9x/wvbd3d.c, a software
 * rasterizer); its capabilities are added here at 15 bpp and more.
 * [WINVBE] DirectDrawHAL=0 turns the HAL off (DirectDraw then emulates,
 * through DCI, as before); DirectDrawFlip=0 always copies; Direct3DHAL=0
 * leaves Direct3D to its own software devices.
 */
#include "drv.h"
#include "wvbdd.h"

#ifndef OLDWIN

extern WORD  __far __pascal GlobalAlloc(WORD, DWORD);
extern LPVOID __far __pascal GlobalLock(WORD);
int dibeng_mode_ok(void);

typedef struct {
    DWORD dwCommand, dwParam1, dwParam2, dwVersion, dwReserved;
} DDCMD;

typedef struct {
    DWORD dwSize;
    LPVOID lpSetInfo;
    DWORD lpVidMemAlloc, lpVidMemFree;
} DDHALDDRAWFNS16;

typedef WORD (__far __pascal *PF_SETINFO)(WD_HALINFO FAR *, WORD reset);

static WVBDD FAR *dd;           /* the shared block */
static DWORD dd_lin;            /* ... its linear address */
static PF_SETINFO dd_setinfo;   /* DirectDraw's DDHAL_SetInfo */
static DWORD dd_maplin, dd_mapphys, dd_mapsize;     /* video memory mapping */
static BYTE dd_ini = 0xFF;      /* [WINVBE] DirectDrawHAL */
static BYTE dd_iniflip;         /* [WINVBE] DirectDrawFlip (1: display start where possible, 0: copy) */
static BYTE dd_ini3d;           /* [WINVBE] Direct3DHAL */

#define FOURCC(a, b, c, d) ((DWORD)(BYTE)(a) | ((DWORD)(BYTE)(b) << 8) | ((DWORD)(BYTE)(c) << 16) | ((DWORD)(BYTE)(d) << 24))

/* DDCAPS_x, DDSCAPS_x ... (ddraw.h, ddrawi.h) */
#define DDCAPS_BLT              0x00000040UL
#define DDCAPS_BLTFOURCC        0x00000100UL
#define DDCAPS_BLTSTRETCH       0x00000200UL
#define DDCAPS_GDI              0x00000400UL
#define DDCAPS_OVERLAY          0x00000800UL
#define DDCAPS_OVERLAYFOURCC    0x00002000UL
#define DDCAPS_OVERLAYSTRETCH   0x00004000UL
#define DDCAPS_COLORKEY         0x00400000UL
#define DDCAPS_BLTCOLORFILL     0x04000000UL
#define DDCAPS_3D               0x00000001UL
#define DDCAPS_ALPHA            0x00800000UL
#define DDCAPS_BLTDEPTHFILL     0x10000000UL
#define DDSCAPS_ALPHA_          0x00000002UL
#define DDSCAPS_TEXTURE         0x00001000UL
#define DDSCAPS_ZBUFFER         0x00020000UL
#define DDSCAPS_MIPMAP          0x00400000UL
#define DDBD_16                 0x00000400UL
#define DDBD_24                 0x00000200UL
#define DDBD_32                 0x00000100UL
#define DDCAPS2_NOPAGELOCKREQUIRED 0x00000800UL
#define DDCAPS2_WIDESURFACES    0x00001000UL
#define DDCAPS2_PRIMARYGAMMA    0x00020000UL
#define DDCKEYCAPS_DESTOVERLAY  0x00000010UL
#define DDCKEYCAPS_SRCBLT       0x00000200UL
#define DDFXCAPS_BLTSHRINKX     0x00000400UL
#define DDFXCAPS_BLTSHRINKXN    0x00000800UL
#define DDFXCAPS_BLTSHRINKY     0x00001000UL
#define DDFXCAPS_BLTSHRINKYN    0x00002000UL
#define DDFXCAPS_BLTSTRETCHX    0x00004000UL
#define DDFXCAPS_BLTSTRETCHXN   0x00008000UL
#define DDFXCAPS_BLTSTRETCHY    0x00010000UL
#define DDFXCAPS_BLTSTRETCHYN   0x00020000UL
#define DDFXCAPS_OVERLAYALL     0x07F80000UL    /* OVERLAYSHRINKX .. OVERLAYSTRETCHYN */
#define DDSCAPS_FLIP            0x00000010UL
#define DDSCAPS_OFFSCREENPLAIN  0x00000040UL
#define DDSCAPS_OVERLAY         0x00000080UL
#define DDSCAPS_PRIMARYSURFACE  0x00000200UL
#define DDSCAPS_3DDEVICE        0x00002000UL
#define DDSCAPS_VIDEOMEMORY     0x00004000UL
#define DDPF_PALETTEINDEXED8    0x00000020UL
#define DDPF_RGB                0x00000040UL
#define VIDMEM_ISLINEAR         0x00000001UL
#define DDMODEINFO_PALETTIZED   0x0001

int dd_possible(void)
{
    if (dd_ini == 0xFF) {
        dd_ini = (BYTE)get_ini_int("DirectDrawHAL", 1);
        dd_iniflip = (BYTE)get_ini_int("DirectDrawFlip", 1);
        dd_ini3d = (BYTE)get_ini_int("Direct3DHAL", 1);
    }
    if (!dd_ini || !dibeng_on || !dibeng_mode_ok() || g.shadow || g.xf || !g.enabled) return 0;
    return g.win9x && g.cpu386 && g.mi.physbase != 0;
}

/* the shared block, allocated once (fixed, shared global memory) */
static int dd_block(void)
{
    WORD h;
    if (dd) return 1;
    h = GlobalAlloc(0x2042, sizeof(WVBDD));     /* GMEM_SHARE | GMEM_ZEROINIT | GMEM_FIXED */
    if (!h) return 0;
    dd = (WVBDD FAR *)GlobalLock(h);
    if (!dd) return 0;
    far_memset(dd, 0, sizeof(WVBDD));
    dd->size = sizeof(WVBDD);
    dd->version = WD_VERSION;
    dd->debug = g.debug;
    dd_lin = sel_getbase(FP_SEG(dd)) + FP_OFF(dd);
    DBG("DD HAL: block %x:%x lin %lx size %u\n", FP_SEG(dd), FP_OFF(dd), dd_lin, (WORD)sizeof(WVBDD));
    return 1;
}

/* all of video memory, mapped for DirectDraw (once; again for another
   frame buffer) */
static int dd_map(void)
{
    DWORD size = (DWORD)g.totalmem64k << 16;
    DWORD need = g.fbofs + (DWORD)g.pitch * g.yres;
    if (size > 0x4000000UL) size = 0x4000000UL;             /* 64 MB */
    if (size < need) size = need;
    if (dd_maplin && dd_mapphys == g.mi.physbase && dd_mapsize >= size) return 1;
    dd_maplin = map_phys(g.mi.physbase, size);
    if (!dd_maplin) { dd_mapsize = 0; return 0; }
    dd_mapphys = g.mi.physbase;
    dd_mapsize = size;
    if (g.ini_wc && g.wc) vxd_wc(dd_maplin, size, g.mi.physbase);
    DBG("DD HAL: video memory %lx size %lx at %lx\n", g.mi.physbase, size, dd_maplin);
    return 1;
}

static void masks(WORD bpp, BYTE pf, WD32 *r, WD32 *gm, WD32 *b)
{
    *r = *gm = *b = 0;
    if (bpp == 15 || pf == PF_555) { *r = 0x7C00; *gm = 0x03E0; *b = 0x001F; }
    else if (bpp == 16) { *r = 0xF800; *gm = 0x07E0; *b = 0x001F; }
    else if (bpp >= 24) { *r = 0xFF0000UL; *gm = 0xFF00; *b = 0xFF; }
}

static WORD add_ddmode(WORD n, WORD w, WORD h, WORD bpp)
{
    WD_MODE FAR *m;
    WORD i, b = bpp == 15 ? 16 : bpp;
    for (i = 0; i < n; i++)
        if (dd->modes[i].dwWidth == w && dd->modes[i].dwHeight == h && dd->modes[i].dwBPP == b) return n;
    if (n >= WD_MAXMODES) return n;
    m = &dd->modes[n];
    far_memset(m, 0, sizeof(*m));
    m->dwWidth = w; m->dwHeight = h; m->dwBPP = b;
    m->lPitch = ((DWORD)w * ((bpp + 7) >> 3) + 3) & ~3UL;
    m->wFlags = bpp == 8 ? DDMODEINFO_PALETTIZED : 0;
    masks(bpp, bpp == 15 ? PF_555 : 0, &m->dwRBitMask, &m->dwGBitMask, &m->dwBBitMask);
    return n + 1;
}

static const WORD std_res[] = {
    640,480, 800,600, 1024,768, 1152,864, 1280,720, 1280,768, 1280,800, 1280,960, 1280,1024,
    1360,768, 1366,768, 1400,1050, 1440,900, 1600,900, 1600,1200, 1680,1050, 1920,1080,
    1920,1200, 2048,1536, 2560,1440, 2560,1600
};

/* the modes DirectDraw may set (EnumDisplayModes): those Display
   Properties offers too */
static WORD dd_modes(void)
{
    static const WORD depth[] = { 8, 16, 24, 32 };
    WORD n = 0, i, j;
    n = add_ddmode(n, g.xres, g.yres, g.bpp);               /* the current mode first */
    for (j = 0; j < 4; j++)
        for (i = 0; i < sizeof(std_res) / sizeof(std_res[0]); i += 2) {
            WORD b = depth[j];
            if (vbe_mode_ok(std_res[i], std_res[i + 1], b) != 0) continue;
            /* 24: only where a real 24-bit mode exists (else 24 means 32) */
            if (b == 24) {
                WORD k, real = 0;
                for (k = 0; k < nmodes; k++)
                    if (modes[k].xres == std_res[i] && modes[k].yres == std_res[i + 1] && modes[k].bpp == 24) real = 1;
                if (!real) continue;
            }
            n = add_ddmode(n, std_res[i], std_res[i + 1], b);
        }
    return n;
}

/* the HAL information of the current mode */
static void dd_build(void)
{
    WD_HALINFO FAR *h = &dd->hal;
    WD_CORECAPS FAR *c = &h->ddCaps;
    WD_VIDMEMINFO FAR *v = &h->vmiData;
    DWORD scr = (DWORD)g.pitch * g.yres, lines = 0, i;
    WORD bpp = g.bpp == 15 ? 16 : g.bpp;
    int yuv;

    dd->valid = 0;
    dd->d3d_on = 0;
    far_memset(h, 0, sizeof(*h));
    far_memset(&dd->ddcb, 0, sizeof(dd->ddcb));
    far_memset(&dd->ddscb, 0, sizeof(dd->ddscb));
    far_memset(&dd->ddpcb, 0, sizeof(dd->ddpcb));
    far_memset(&dd->ddxcb, 0, sizeof(dd->ddxcb));
    far_memset(&dd->heap, 0, sizeof(dd->heap));

    dd->fblin = dd_maplin + g.fbofs;
    dd->vramlin = dd_maplin;
    dd->vramsize = dd_mapsize;
    dd->width = g.xres; dd->height = g.yres;
    dd->bpp = bpp; dd->Bpp = g.Bpp; dd->pitch = g.pitch;
    masks(g.bpp, g.mi.pf, &dd->rmask, &dd->gmask, &dd->bmask);
    if (g.bpp >= 24 && g.mi.rsize) {
        dd->rmask = ((1UL << g.mi.rsize) - 1) << g.mi.rpos;
        dd->gmask = ((1UL << g.mi.gsize) - 1) << g.mi.gpos;
        dd->bmask = ((1UL << g.mi.bsize) - 1) << g.mi.bpos;
    }
    if (dd_mapsize > g.fbofs + scr) lines = (dd_mapsize - g.fbofs - scr) / g.pitch;
    yuv = bpp >= 15 && lines > 0;
    dd->fourcc_ok = yuv;
    dd->flip = dd_iniflip && vbe_dispi_flip();
    dd->shown = dd->fblin;
    dd->debug = g.debug;

    h->dwSize = sizeof(WD_HALINFO);
    h->lpDDCallbacks = (WD32)(LPVOID)&dd->ddcb;
    h->lpDDSurfaceCallbacks = (WD32)(LPVOID)&dd->ddscb;
    h->lpDDPaletteCallbacks = (WD32)(LPVOID)&dd->ddpcb;

    v->fpPrimary = dd->fblin;
    v->dwDisplayWidth = g.xres;
    v->dwDisplayHeight = g.yres;
    v->lDisplayPitch = g.pitch;
    v->ddpfDisplay.dwSize = sizeof(WD_PIXFMT);
    v->ddpfDisplay.dwFlags = DDPF_RGB | (bpp == 8 ? DDPF_PALETTEINDEXED8 : 0);
    v->ddpfDisplay.dwRGBBitCount = bpp;
    v->ddpfDisplay.dwRBitMask = dd->rmask;
    v->ddpfDisplay.dwGBitMask = dd->gmask;
    v->ddpfDisplay.dwBBitMask = dd->bmask;
    v->dwOffscreenAlign = 8;
    v->dwOverlayAlign = 8;
    v->dwTextureAlign = 8;
    v->dwZBufferAlign = 8;
    v->dwAlphaAlign = 8;
    if (lines) {
        /* linear: DirectDraw 9x's allocator gets rectangular heaps wrong */
        dd->heap.dwFlags = VIDMEM_ISLINEAR;
        dd->heap.fpStart = (dd->fblin + scr + 0xFFF) & ~0xFFFUL;
        dd->heap.dwWidth = dd_maplin + dd_mapsize - 1;     /* fpEnd (inclusive) */
        v->dwNumHeaps = 1;
        v->pvmList = (WD32)(LPVOID)&dd->heap;
    }

    c->dwSize = sizeof(WD_CORECAPS);
    /* blits by the CPU (WVBDD32.DLL): copies, stretches, fills, source
       colour keys, YUV to RGB; one emulated overlay */
    c->dwCaps = DDCAPS_GDI | DDCAPS_BLT | DDCAPS_BLTSTRETCH | DDCAPS_BLTCOLORFILL | DDCAPS_COLORKEY;
    c->dwCaps2 = DDCAPS2_NOPAGELOCKREQUIRED | DDCAPS2_WIDESURFACES;
    /* the primary surface's gamma ramp (GammaRamp, enable.c): in 256 colours
       it goes into the palette; in high and true colour programs write the
       primary surface themselves, past the driver's colour tables */
    if (bpp == 8) c->dwCaps2 |= DDCAPS2_PRIMARYGAMMA;
    c->dwCKeyCaps = DDCKEYCAPS_SRCBLT;
    c->dwFXCaps = DDFXCAPS_BLTSTRETCHX | DDFXCAPS_BLTSTRETCHY | DDFXCAPS_BLTSHRINKX | DDFXCAPS_BLTSHRINKY |
                  DDFXCAPS_BLTSTRETCHXN | DDFXCAPS_BLTSTRETCHYN | DDFXCAPS_BLTSHRINKXN | DDFXCAPS_BLTSHRINKYN;
    c->dwRops[0xCC / 32] |= 1UL << (0xCC % 32);         /* SRCCOPY */
    /* flips: the display start (DISPI), else the back buffer is copied */
    c->ddsCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY |
                 (lines >= g.yres ? DDSCAPS_FLIP : 0);
    c->dwVidMemTotal = dd_mapsize;
    c->dwVidMemFree = lines * g.pitch;
    if (yuv) {
        dd->fourcc[0] = FOURCC('Y','U','Y','2');
        dd->fourcc[1] = FOURCC('U','Y','V','Y');
        dd->fourcc[2] = FOURCC('Y','V','1','2');
        c->dwNumFourCCCodes = 3;
        h->lpdwFourCC = (WD32)(LPVOID)dd->fourcc;
        c->dwCaps |= DDCAPS_BLTFOURCC | DDCAPS_OVERLAY | DDCAPS_OVERLAYSTRETCH | DDCAPS_OVERLAYFOURCC;
        c->ddsCaps |= DDSCAPS_OVERLAY;
        c->dwCKeyCaps |= DDCKEYCAPS_DESTOVERLAY;
        c->dwFXCaps |= DDFXCAPS_OVERLAYALL;
        c->dwMaxVisibleOverlays = 1;
        c->dwMinOverlayStretch = 1;
        c->dwMaxOverlayStretch = 32000;
        c->dwMinLiveVideoStretch = 1;
        c->dwMaxLiveVideoStretch = 32000;
        c->dwMinHwCodecStretch = 1;
        c->dwMaxHwCodecStretch = 32000;
    }
    h->dwMonitorFrequency = g.refresh_set ? (g.refresh_cur + 50) / 100 : 60;
    h->dwNumModes = dd_modes();
    h->dwModeIndex = 0;                                     /* dd_modes puts it first */
    h->lpModeInfo = (WD32)(LPVOID)dd->modes;
    h->lpPDevice = (WD32)(LPVOID)scr_pdev;
    dd->ddxcb.dwSize = sizeof(dd->ddxcb);               /* no execute buffers (Direct3D HAL) */
    h->lpDDExeBufCallbacks = (WD32)(LPVOID)&dd->ddxcb;
    h->hInstance = dd->hinst;
    h->dwFlags = 1;                                     /* DDHALINFO_ISPRIMARYDISPLAY */

    /* the callbacks the 32-bit part has */
    dd->ddcb.dwSize = sizeof(WD_DDCB);
    dd->ddscb.dwSize = sizeof(WD_SURFCB);
    dd->ddpcb.dwSize = sizeof(WD_PALCB);
    if (dd->hinst) {
        dd->ddcb = dd->cb;
        dd->ddcb.dwSize = sizeof(WD_DDCB);
        dd->ddscb = dd->scb;
        dd->ddscb.dwSize = sizeof(WD_SURFCB);
        if (!yuv) {
            dd->ddscb.dwFlags &= ~(WD_SCB_UPDATEOVERLAY | WD_SCB_SETOVERLAYPOS);
            dd->ddscb.UpdateOverlay = dd->ddscb.SetOverlayPosition = 0;
        }
        /* Direct3D (software rasterizer of the 32-bit part): 15 bpp and more */
        dd->d3d_on = dd_ini3d && dd->d3dglobal && dd->getdriverinfo && bpp >= 15;
        if (dd->d3d_on) {
            h->lpD3DGlobalDriverData = dd->d3dglobal;
            h->lpD3DHALCallbacks = dd->d3dcb;
            h->GetDriverInfo = dd->getdriverinfo;
            h->dwFlags |= 4;                            /* DDHALINFO_GETDRIVERINFOSET */
            c->dwCaps |= DDCAPS_3D | DDCAPS_ALPHA | DDCAPS_BLTDEPTHFILL;
            c->ddsCaps |= DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER | DDSCAPS_MIPMAP | DDSCAPS_ALPHA_;
            c->dwZBufferBitDepths = DDBD_16 | DDBD_24 | DDBD_32;
        }
    } else {
        /* no 32-bit part: no blits either (DirectDraw emulates) */
        c->dwCaps = DDCAPS_GDI;
        c->dwNumFourCCCodes = 0;
        h->lpdwFourCC = 0;
        c->ddsCaps &= ~DDSCAPS_OVERLAY;
        c->dwCKeyCaps = c->dwFXCaps = 0;
    }
    for (i = 0; i < 8; i++) c->dwSVBRops[i] = 0;
    dd->valid = 1;
    DBG("DD HAL: %ux%ux%u pitch %u primary %lx heap %lu lines, %u modes, cb %lx/%lx\n", g.xres, g.yres, bpp,
        g.pitch, dd->fblin, lines, (WORD)h->dwNumModes, dd->ddcb.dwFlags, dd->ddscb.dwFlags);
}

/* DirectDraw's DCICOMMAND requests */
SHORT dd_command(DDCMD FAR *cmd, LPVOID out)
{
    switch ((WORD)cmd->dwCommand) {
    case DDVERSIONINFO: {
        LPDWORD v = (LPDWORD)out;
        DBG("DD HAL: DirectDraw version %lx\n", cmd->dwParam1);
        if (!v) return 0;
        /* the version of DirectDraw, at most that of the DDK the HAL follows */
        v[0] = cmd->dwParam1 && cmd->dwParam1 < DD_RUNTIME_VERSION ? cmd->dwParam1 : DD_RUNTIME_VERSION;
        v[1] = 0; v[2] = 0;
        return 1;
    }
    case DDGET32BITDRIVERNAME: {
        LPSTR p = (LPSTR)out;
        static const char name[] = WD_DLLNAME, entry[] = "DriverInit";
        WORD i;
        if (!p || !dd_possible() || !dd_block() || !dd_map()) return 0;
        far_memset(p, 0, 260 + 64 + 4);
        for (i = 0; name[i]; i++) p[i] = name[i];
        for (i = 0; entry[i]; i++) p[260 + i] = entry[i];
        *(LPDWORD)(p + 260 + 64) = dd_lin;
        DBG("DD HAL: 32-bit part %s context %lx\n", (LPSTR)name, dd_lin);
        return 1;
    }
    case DDNEWCALLBACKFNS: {
        DDHALDDRAWFNS16 FAR *f = (DDHALDDRAWFNS16 FAR *)cmd->dwParam1;
        if (!f) return 0;
        dd_setinfo = (PF_SETINFO)f->lpSetInfo;
        DBG("DD HAL: SetInfo %lx\n", (DWORD)f->lpSetInfo);
        return 1;
    }
    case DDCREATEDRIVEROBJECT: {
        WORD r;
        if (!dd_setinfo || !dd_possible() || !dd_block() || !dd_map()) return 0;
        dd_build();
        r = dd_setinfo(&dd->hal, 0);
        DBG("DD HAL: SetInfo -> %u, hinst %lx\n", r, dd->hinst);
        if (!r) return 0;
        if (out) *(LPDWORD)out = dd->hinst;
        return 1;
    }
    }
    return 0;
}

/* after a mode change (Enable, ReEnable): DirectDraw gets the new mode */
void dd_mode_changed(void)
{
    if (!dd || !dd_setinfo) return;
    if (!dd_possible() || !dd_map()) { dd->valid = 0; return; }
    dd_build();
    dd_setinfo(&dd->hal, 1);
}

#endif
