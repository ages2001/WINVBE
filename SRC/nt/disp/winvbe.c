/*
 * WINVBE.DLL - display driver for the WINVBE miniport (Windows NT)
 *
 * A linear frame buffer driver: GDI draws into the frame buffer itself
 * (engine managed bitmap), the miniport sets the VBE mode and maps the
 * frame buffer.  Built twice for x86:
 *   - NT3X: Windows NT 3.1/3.5/3.51, runs in user mode inside CSRSS, the
 *     GDI engine is WINSRV.DLL, the miniport is called with DeviceIoControl;
 *   - otherwise Windows NT 4.0, 2000, XP, 2003 (kernel mode, WIN32K.SYS);
 * and once for x64.
 */
#include <stdarg.h>
#include <windef.h>
#include <wingdi.h>
#include <winddi.h>
#include <winioctl.h>
#include <ntddvdeo.h>
#include "../common/wvbnt.h"

#if defined(NT3X) && !defined(_WINBASE_)
__declspec(dllimport) BOOL WINAPI DeviceIoControl(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPVOID);
__declspec(dllimport) HLOCAL WINAPI LocalAlloc(UINT, SIZE_T);
__declspec(dllimport) HLOCAL WINAPI LocalFree(HLOCAL);
#endif
#ifdef NT3X
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}

static void *mem_alloc(ULONG n)
{
#ifdef NT3X
    return LocalAlloc(0x40 /* LMEM_ZEROINIT */, n);
#else
    return EngAllocMem(FL_ZERO_MEMORY, n, 0x77766265);
#endif
}

static void mem_free(void *p)
{
    if (!p) return;
#ifdef NT3X
    LocalFree(p);
#else
    EngFreeMem(p);
#endif
}

static int ioctl(HANDLE h, ULONG code, void *in, ULONG inl, void *out, ULONG outl, ULONG *ret)
{
#ifdef NT3X
    DWORD r = 0;
    BOOL ok = DeviceIoControl(h, code, in, inl, out, outl, &r, NULL);
    if (ret) *ret = r;
    return ok ? 0 : 1;
#else
    ULONG r = 0;
    DWORD e = EngDeviceIoControl(h, code, in, inl, out, outl, &r);
    if (ret) *ret = r;
    return e != 0;
#endif
}

typedef struct {
    HANDLE   hDriver;
    HDEV     hdev;
    HSURF    hsurf;
    HPALETTE hpal;
    ULONG    mode;              /* miniport mode index */
    ULONG    w, h, bpp, freq;   /* bpp: 8, 16, 24, 32 (15 is reported as 16) */
    LONG     stride;
    ULONG    rmask, gmask, bmask;
    ULONG    dacbits;
    PVOID    fb;
    ULONG    vram;              /* mapped video memory (bytes from fb) */
    ULONG    start;             /* display start (DirectDraw flip) */
    PALETTEENTRY *pal;          /* 256 entries in 8 bpp */
    HSURF    pat[HS_DDI_MAX];   /* standard hatch patterns */
} PDEV;

/* standard hatch patterns, 8x8 1 bpp (0 = line, 1 = gap), DWORD rows */
static const BYTE hatch[HS_DDI_MAX][8] = {
    { 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF },     /* HS_HORIZONTAL */
    { 0xF7, 0xF7, 0xF7, 0xF7, 0xF7, 0xF7, 0xF7, 0xF7 },     /* HS_VERTICAL */
    { 0x7F, 0xBF, 0xDF, 0xEF, 0xF7, 0xFB, 0xFD, 0xFE },     /* HS_FDIAGONAL */
    { 0xFE, 0xFD, 0xFB, 0xF7, 0xEF, 0xDF, 0xBF, 0x7F },     /* HS_BDIAGONAL */
    { 0xF7, 0xF7, 0xF7, 0x00, 0xF7, 0xF7, 0xF7, 0xF7 },     /* HS_CROSS */
    { 0x7E, 0xBD, 0xDB, 0xE7, 0xE7, 0xDB, 0xBD, 0x7E },     /* HS_DIAGCROSS */
};

static void make_patterns(PDEV *pd, ULONG cPat, HSURF *out)
{
    ULONG i, r;
    SIZEL sz;
    sz.cx = 8; sz.cy = 8;
    for (i = 0; i < HS_DDI_MAX && i < cPat; i++) {
        pd->pat[i] = (HSURF)EngCreateBitmap(sz, 4, BMF_1BPP, BMF_TOPDOWN, NULL);
        if (pd->pat[i]) {
            SURFOBJ *so = EngLockSurface(pd->pat[i]);
            if (so) {
                BYTE *p = so->pvScan0;
                for (r = 0; r < 8; r++) p[r * so->lDelta] = hatch[i][r];
                EngUnlockSurface(so);
            }
        }
        out[i] = pd->pat[i];
    }
}

static ULONG g_engine;          /* engine version */
static HANDLE g_hdrv;

#ifdef DEVDEBUG
static char *fmt_num(char *o, char *end, ULONG v, int base)
{
    char t[12];
    int n = 0;
    do { int d = (int)(v % base); t[n++] = (char)(d < 10 ? '0' + d : 'A' + d - 10); v /= base; } while (v);
    while (n && o < end) *o++ = t[--n];
    return o;
}
static void trace(const char *f, ...)
{
    char b[160], *o = b, *end = b + sizeof(b) - 1;
    va_list ap;
    ULONG r;
    if (!g_hdrv) return;
    va_start(ap, f);
    while (*f && o < end) {
        if (*f != '%') { *o++ = *f++; continue; }
        f++;
        if (*f == 'x') o = fmt_num(o, end, va_arg(ap, ULONG), 16);
        else if (*f == 'u') o = fmt_num(o, end, va_arg(ap, ULONG), 10);
        f++;
    }
    va_end(ap);
    ioctl(g_hdrv, IOCTL_WVB_DEBUG, b, (ULONG)(o - b), NULL, 0, &r);
}
#define TRACE(x) trace x
#else
#define TRACE(x)
#endif

/* ------------------------------------------------------------------ */
/* modes                                                               */

static VIDEO_MODE_INFORMATION *get_modes(HANDLE h, ULONG *count)
{
    VIDEO_NUM_MODES nm;
    VIDEO_MODE_INFORMATION *m;
    ULONG ret;
    *count = 0;
    if (ioctl(h, IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES, NULL, 0, &nm, sizeof(nm), &ret) || !nm.NumModes)
        return NULL;
    m = mem_alloc(nm.NumModes * sizeof(VIDEO_MODE_INFORMATION));
    if (!m) return NULL;
    if (ioctl(h, IOCTL_VIDEO_QUERY_AVAIL_MODES, NULL, 0, m, nm.NumModes * sizeof(VIDEO_MODE_INFORMATION), &ret)) {
        mem_free(m);
        return NULL;
    }
    *count = ret / sizeof(VIDEO_MODE_INFORMATION);
    return m;
}

static ULONG mode_bpp(VIDEO_MODE_INFORMATION *v)
{
    return v->BitsPerPlane == 15 ? 16 : v->BitsPerPlane;
}

/* pick the mode for a DEVMODE: exact match, else the same size and depth
   at any refresh rate, else the largest mode not bigger than asked in that
   depth, else 640x480 */
static VIDEO_MODE_INFORMATION *pick_mode(VIDEO_MODE_INFORMATION *m, ULONG n, DEVMODEW *pdm)
{
    ULONG w = 0, h = 0, bpp = 0, hz = 0, i;
    VIDEO_MODE_INFORMATION *best = NULL;
    if (pdm) {
        if (pdm->dmSize > FIELD_OFFSET(DEVMODEW, dmPelsHeight)) {
            w = pdm->dmPelsWidth; h = pdm->dmPelsHeight; bpp = pdm->dmBitsPerPel;
        }
        if (pdm->dmSize > FIELD_OFFSET(DEVMODEW, dmDisplayFrequency)) hz = pdm->dmDisplayFrequency;
    }
    if (bpp == 15) bpp = 16;
    if (hz <= 1) hz = 60;
#ifdef TEST_BPP
    bpp = TEST_BPP;
#endif
    for (i = 0; i < n; i++)
        if (m[i].VisScreenWidth == w && m[i].VisScreenHeight == h && mode_bpp(&m[i]) == bpp) {
            if (m[i].Frequency == hz) return &m[i];
            if (!best || m[i].Frequency == 60) best = &m[i];
        }
    if (best) return best;
    for (i = 0; i < n; i++)
        if (m[i].Frequency == 60 && mode_bpp(&m[i]) == (bpp ? bpp : 8) &&
            (!w || (m[i].VisScreenWidth <= w && m[i].VisScreenHeight <= h)) &&
            (!best || m[i].VisScreenWidth * m[i].VisScreenHeight > best->VisScreenWidth * best->VisScreenHeight))
            best = &m[i];
    if (best) return best;
    for (i = 0; i < n; i++)
        if (m[i].VisScreenWidth == 640 && m[i].VisScreenHeight == 480 && m[i].Frequency == 60) return &m[i];
    return n ? &m[0] : NULL;
}

/* ------------------------------------------------------------------ */
/* palette                                                             */

static const UCHAR syscolors[20][3] = {
    {0,0,0}, {0x80,0,0}, {0,0x80,0}, {0x80,0x80,0}, {0,0,0x80}, {0x80,0,0x80}, {0,0x80,0x80},
    {0xC0,0xC0,0xC0}, {0xC0,0xDC,0xC0}, {0xA6,0xCA,0xF0},
    {0xFF,0xFB,0xF0}, {0xA0,0xA0,0xA4}, {0x80,0x80,0x80}, {0xFF,0,0}, {0,0xFF,0}, {0xFF,0xFF,0},
    {0,0,0xFF}, {0xFF,0,0xFF}, {0,0xFF,0xFF}, {0xFF,0xFF,0xFF}
};

static void default_palette(PALETTEENTRY *p)
{
    ULONG i, r, g, b, k = 10;
    for (i = 0; i < 10; i++) {
        p[i].peRed = syscolors[i][0]; p[i].peGreen = syscolors[i][1]; p[i].peBlue = syscolors[i][2];
        p[246 + i].peRed = syscolors[10 + i][0]; p[246 + i].peGreen = syscolors[10 + i][1];
        p[246 + i].peBlue = syscolors[10 + i][2];
    }
    /* 6x6x6 colour cube (216) + 20 greys */
    for (r = 0; r < 6; r++)
        for (g = 0; g < 6; g++)
            for (b = 0; b < 6; b++, k++) {
                p[k].peRed = (BYTE)(r * 51); p[k].peGreen = (BYTE)(g * 51); p[k].peBlue = (BYTE)(b * 51);
            }
    for (i = 0; k < 246; k++, i++)
        p[k].peRed = p[k].peGreen = p[k].peBlue = (BYTE)(8 + i * 12);
    for (i = 0; i < 256; i++) p[i].peFlags = 0;
}

static BOOL load_palette(PDEV *pd, ULONG start, ULONG n)
{
    VIDEO_CLUT *c;
    ULONG i, ret;
    BOOL ok;
    c = mem_alloc(sizeof(VIDEO_CLUT) + 256 * sizeof(VIDEO_CLUTDATA));
    if (!c) return FALSE;
    c->NumEntries = (USHORT)n;
    c->FirstEntry = (USHORT)start;
    for (i = 0; i < n; i++) {
        c->LookupTable[i].RgbArray.Red = pd->pal[start + i].peRed;
        c->LookupTable[i].RgbArray.Green = pd->pal[start + i].peGreen;
        c->LookupTable[i].RgbArray.Blue = pd->pal[start + i].peBlue;
        c->LookupTable[i].RgbArray.Unused = 0;
    }
    ok = !ioctl(pd->hDriver, IOCTL_VIDEO_SET_COLOR_REGISTERS, c,
                sizeof(VIDEO_CLUT) + n * sizeof(VIDEO_CLUTDATA), NULL, 0, &ret);
    mem_free(c);
    return ok;
}

/* ------------------------------------------------------------------ */
/* DDI                                                                 */

static const LOGFONTW lf_default = {16,7,0,0,700,0,0,0,ANSI_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
    DEFAULT_QUALITY,VARIABLE_PITCH|FF_DONTCARE,L"System"};
static const LOGFONTW lf_ansivar = {12,9,0,0,400,0,0,0,ANSI_CHARSET,OUT_DEFAULT_PRECIS,CLIP_STROKE_PRECIS,
    PROOF_QUALITY,VARIABLE_PITCH|FF_DONTCARE,L"MS Sans Serif"};
static const LOGFONTW lf_ansifix = {12,9,0,0,400,0,0,0,ANSI_CHARSET,OUT_DEFAULT_PRECIS,CLIP_STROKE_PRECIS,
    PROOF_QUALITY,FIXED_PITCH|FF_DONTCARE,L"Courier"};

DHPDEV APIENTRY DrvEnablePDEV(DEVMODEW *pdm, LPWSTR logaddr, ULONG cPat, HSURF *phsurfPatterns,
                              ULONG cjCaps, ULONG *pdevcaps, ULONG cjDevInfo, DEVINFO *pdi,
                              HDEV hdev, LPWSTR devname, HANDLE hDriver)
{
    PDEV *pd;
    VIDEO_MODE_INFORMATION *modes, *v;
    ULONG n, dpi = 96;
    GDIINFO gi;
    DEVINFO di;

    g_hdrv = hDriver;
    TRACE(("EnablePDEV engine %x cj %u %u\n", g_engine, cjCaps, cjDevInfo));
    pd = mem_alloc(sizeof(PDEV));
    if (!pd) return NULL;
    pd->hDriver = hDriver;
    modes = get_modes(hDriver, &n);
    v = modes ? pick_mode(modes, n, pdm) : NULL;
    if (!v) { mem_free(modes); mem_free(pd); return NULL; }
    pd->mode = v->ModeIndex;
    pd->w = v->VisScreenWidth; pd->h = v->VisScreenHeight;
    pd->bpp = mode_bpp(v); pd->freq = v->Frequency;
    pd->stride = v->ScreenStride;
    pd->rmask = v->RedMask; pd->gmask = v->GreenMask; pd->bmask = v->BlueMask;
    pd->dacbits = v->NumberRedBits;
    mem_free(modes);
#ifdef FBCALLS
    { VIDEO_COLOR_CAPABILITIES cc; VIDEO_POINTER_CAPABILITIES pc; ULONG r;
      ioctl(hDriver, IOCTL_VIDEO_QUERY_COLOR_CAPABILITIES, NULL, 0, &cc, sizeof(cc), &r);
      ioctl(hDriver, IOCTL_VIDEO_QUERY_POINTER_CAPABILITIES, &r, 4, &pc, sizeof(pc), &r); }
#endif
    if (pdm && pdm->dmSize > FIELD_OFFSET(DEVMODEW, dmLogPixels) && pdm->dmLogPixels >= 72 &&
        pdm->dmLogPixels <= 240)
        dpi = pdm->dmLogPixels;

    memset(&gi, 0, sizeof(gi));
    gi.ulVersion = g_engine >= 0x30000 ? 0x5000 : g_engine >= 0x20000 ? 0x4000 : 0x3500;
#ifdef FORCE5000
    gi.ulVersion = 0x5000;
#endif
    gi.ulTechnology = DT_RASDISPLAY;
#ifdef FBLIKE
    gi.ulHorzSize = 320;
    gi.ulVertSize = 240;
    dpi = pdm ? pdm->dmLogPixels : 96;
#else
    gi.ulHorzSize = pd->w * 254 / (dpi * 10);
    gi.ulVertSize = pd->h * 254 / (dpi * 10);
#endif
    gi.ulHorzRes = pd->w;
    gi.ulVertRes = pd->h;
    gi.cBitsPixel = pd->bpp;
    gi.cPlanes = 1;
    gi.ulNumColors = pd->bpp == 8 ? 20 : (ULONG)-1;
    gi.flRaster = 0;
    gi.ulLogPixelsX = dpi;
    gi.ulLogPixelsY = dpi;
    gi.flTextCaps = TC_RA_ABLE;
    gi.ulDACRed = gi.ulDACGreen = gi.ulDACBlue = pd->bpp == 8 ? pd->dacbits : 8;
#ifdef FBLIKE
    if (pd->bpp == 16) { gi.ulDACRed = 5; gi.ulDACGreen = 6; gi.ulDACBlue = 5; }
#endif
    gi.ulAspectX = 0x24;
    gi.ulAspectY = 0x24;
    gi.ulAspectXY = 0x33;
    gi.xStyleStep = 1;
    gi.yStyleStep = 1;
    gi.denStyleStep = 3;
    gi.ciDevice.Red.x = 6700;   gi.ciDevice.Red.y = 3300;   gi.ciDevice.Red.Y = 0;
    gi.ciDevice.Green.x = 2100; gi.ciDevice.Green.y = 7100; gi.ciDevice.Green.Y = 0;
    gi.ciDevice.Blue.x = 1400;  gi.ciDevice.Blue.y = 800;   gi.ciDevice.Blue.Y = 0;
    /* Cyan/Magenta/Yellow stay 0 as in the DDK framebuf driver: non-zero
       dyes send NT 4.0's halftone setup through a path that corrupts the
       USER metrics (dialogs end up with garbage sizes and never show) */
    gi.ciDevice.AlignmentWhite.x = 3127; gi.ciDevice.AlignmentWhite.y = 3290; gi.ciDevice.AlignmentWhite.Y = 0;
    gi.ciDevice.RedGamma = gi.ciDevice.GreenGamma = gi.ciDevice.BlueGamma = 20000;
    gi.ulDevicePelsDPI = 0;
    gi.ulPrimaryOrder = PRIMARY_ORDER_CBA;
    gi.ulHTPatternSize = HT_PATSIZE_4x4_M;
    gi.flHTFlags = HT_FLAG_ADDITIVE_PRIMS;
    gi.ulNumPalReg = pd->bpp == 8 ? 256 : 0;
    gi.ulHTOutputFormat = pd->bpp == 8 ? HT_FORMAT_8BPP : pd->bpp == 16 ? HT_FORMAT_16BPP :
                          pd->bpp == 24 ? HT_FORMAT_24BPP : HT_FORMAT_32BPP;
    gi.ulVRefresh = pd->freq;
    /* tail of GDIINFO: NT 3.x has ulVRefresh, ulDesktopHorzRes,
       ulDesktopVertRes, ulBltAlignment; NT 4.0 and later ulVRefresh,
       ulBltAlignment, ulDesktop/PanningHorzRes, ...VertRes */
    if (g_engine < 0x20000) {
        ULONG *p = &gi.ulVRefresh;
        p[1] = pd->w;
        p[2] = pd->h;
        p[3] = 1;
    } else {
        gi.ulBltAlignment = 1;
        gi.ulPanningHorzRes = pd->w;
        gi.ulPanningVertRes = pd->h;
    }
    memcpy(pdevcaps, &gi, cjCaps < sizeof(gi) ? cjCaps : sizeof(gi));
#ifdef DEVDEBUG
    { ULONG k; for (k = 0; k < cjCaps / 4; k += 8)
        TRACE(("gi %x: %x %x %x %x %x %x %x %x\n", k * 4, pdevcaps[k], pdevcaps[k+1], pdevcaps[k+2], pdevcaps[k+3],
               pdevcaps[k+4], pdevcaps[k+5], pdevcaps[k+6], pdevcaps[k+7])); }
#endif

    memset(&di, 0, sizeof(di));
#ifdef NODITHER
    di.flGraphicsCaps = GCAPS_OPAQUERECT;
#else
    di.flGraphicsCaps = GCAPS_OPAQUERECT | GCAPS_MONO_DITHER;
#endif
    di.lfDefaultFont = lf_default;
    di.lfAnsiVarFont = lf_ansivar;
    di.lfAnsiFixFont = lf_ansifix;
    di.cFonts = 0;
    di.cxDither = di.cyDither = 8;
    if (pd->bpp == 8) {
        pd->pal = mem_alloc(256 * sizeof(PALETTEENTRY));
        if (!pd->pal) { mem_free(pd); return NULL; }
        default_palette(pd->pal);
        pd->hpal = EngCreatePalette(PAL_INDEXED, 256, (ULONG *)pd->pal, 0, 0, 0);
        di.flGraphicsCaps |= GCAPS_PALMANAGED | GCAPS_COLOR_DITHER;
        di.iDitherFormat = BMF_8BPP;
    } else {
        pd->hpal = EngCreatePalette(PAL_BITFIELDS, 0, NULL, pd->rmask, pd->gmask, pd->bmask);
        di.iDitherFormat = pd->bpp == 16 ? BMF_16BPP : pd->bpp == 24 ? BMF_24BPP : BMF_32BPP;
    }
    if (!pd->hpal) { mem_free(pd->pal); mem_free(pd); return NULL; }
    TRACE(("EnablePDEV mode %x %ux%ux%u stride %u\n", pd->mode, pd->w, pd->h, pd->bpp, pd->stride));
    di.hpalDefault = pd->hpal;
    memcpy(pdi, &di, cjDevInfo < sizeof(di) ? cjDevInfo : sizeof(di));
#ifdef DEVDEBUG
    { ULONG *q = (ULONG *)pdi; TRACE(("di caps %x tail %x %x %x %x\n", q[0], q[70], q[71], q[72], q[73])); }
#endif
#ifndef NOPAT
    if (phsurfPatterns && cPat) make_patterns(pd, cPat, phsurfPatterns);
#endif
    return (DHPDEV)pd;
}

VOID APIENTRY DrvCompletePDEV(DHPDEV dhpdev, HDEV hdev)
{
    TRACE(("CompletePDEV\n"));
    ((PDEV *)dhpdev)->hdev = hdev;
}

VOID APIENTRY DrvDisablePDEV(DHPDEV dhpdev)
{
    PDEV *pd = (PDEV *)dhpdev;
    ULONG i;
    for (i = 0; i < HS_DDI_MAX; i++)
        if (pd->pat[i]) EngDeleteSurface(pd->pat[i]);
    if (pd->hpal) EngDeletePalette(pd->hpal);
    mem_free(pd->pal);
    mem_free(pd);
}

static BOOL set_mode(PDEV *pd)
{
    VIDEO_MODE vm;
    VIDEO_MODE_INFORMATION cur;
    ULONG ret;
    vm.RequestedMode = pd->mode;
    if (ioctl(pd->hDriver, IOCTL_VIDEO_SET_CURRENT_MODE, &vm, sizeof(vm), NULL, 0, &ret)) return FALSE;
    pd->start = 0;
#ifndef NOQCM
    if (!ioctl(pd->hDriver, IOCTL_VIDEO_QUERY_CURRENT_MODE, NULL, 0, &cur, sizeof(cur), &ret)) {
        pd->stride = cur.ScreenStride;
        pd->dacbits = cur.NumberRedBits;
    }
#endif
    return TRUE;
}

HSURF APIENTRY DrvEnableSurface(DHPDEV dhpdev)
{
    PDEV *pd = (PDEV *)dhpdev;
    VIDEO_MEMORY vm;
    VIDEO_MEMORY_INFORMATION mi;
    ULONG ret, fmt;
    SIZEL sz;
    HSURF hs;

    if (!set_mode(pd)) return NULL;
    vm.RequestedVirtualAddress = NULL;
    if (ioctl(pd->hDriver, IOCTL_VIDEO_MAP_VIDEO_MEMORY, &vm, sizeof(vm), &mi, sizeof(mi), &ret))
        return NULL;
    pd->fb = mi.FrameBufferBase;
    pd->vram = mi.VideoRamLength;
    pd->start = 0;
    sz.cx = pd->w;
    sz.cy = pd->h;
    fmt = pd->bpp == 8 ? BMF_8BPP : pd->bpp == 16 ? BMF_16BPP : pd->bpp == 24 ? BMF_24BPP : BMF_32BPP;
    hs = (HSURF)EngCreateBitmap(sz, pd->stride, fmt, BMF_TOPDOWN, pd->fb);
    if (!hs) goto fail;
#ifdef HOOKTEST
    if (!EngAssociateSurface(hs, pd->hdev, HOOK_BITBLT | HOOK_TEXTOUT | HOOK_COPYBITS | HOOK_STROKEPATH | HOOK_PAINT)) { EngDeleteSurface(hs); goto fail; }
#else
    if (!EngAssociateSurface(hs, pd->hdev, 0)) { EngDeleteSurface(hs); goto fail; }
#endif
    pd->hsurf = hs;
    if (pd->bpp == 8) load_palette(pd, 0, 256);
#ifdef TEST_FILL
    memset(pd->fb, 0x55, pd->stride * 100);
#endif
    TRACE(("EnableSurface fb %x stride %u hs %x\n", (ULONG)(ULONG_PTR)pd->fb, pd->stride, (ULONG)(ULONG_PTR)hs));
    return hs;
fail:
    vm.RequestedVirtualAddress = pd->fb;
    ioctl(pd->hDriver, IOCTL_VIDEO_UNMAP_VIDEO_MEMORY, &vm, sizeof(vm), NULL, 0, &ret);
    pd->fb = NULL;
    return NULL;
}

VOID APIENTRY DrvDisableSurface(DHPDEV dhpdev)
{
    PDEV *pd = (PDEV *)dhpdev;
    VIDEO_MEMORY vm;
    ULONG ret;
    TRACE(("DisableSurface\n"));
    if (pd->hsurf) EngDeleteSurface(pd->hsurf);
    pd->hsurf = NULL;
    if (pd->fb) {
        vm.RequestedVirtualAddress = pd->fb;
        ioctl(pd->hDriver, IOCTL_VIDEO_UNMAP_VIDEO_MEMORY, &vm, sizeof(vm), NULL, 0, &ret);
        pd->fb = NULL;
    }
}

BOOL APIENTRY DrvAssertMode(DHPDEV dhpdev, BOOL enable)
{
    PDEV *pd = (PDEV *)dhpdev;
    ULONG ret;
    TRACE(("AssertMode %u\n", enable));
    if (enable) {
        if (!set_mode(pd)) return FALSE;
        if (pd->bpp == 8) load_palette(pd, 0, 256);
        return TRUE;
    }
    return !ioctl(pd->hDriver, IOCTL_VIDEO_RESET_DEVICE, NULL, 0, NULL, 0, &ret);
}

BOOL APIENTRY DrvSetPalette(DHPDEV dhpdev, PALOBJ *ppalo, FLONG fl, ULONG start, ULONG n)
{
    PDEV *pd = (PDEV *)dhpdev;
    TRACE(("SetPalette %u %u\n", start, n));
    if (!pd->pal || start >= 256) return FALSE;
    if (start + n > 256) n = 256 - start;
    if (PALOBJ_cGetColors(ppalo, start, n, (ULONG *)&pd->pal[start]) != n) return FALSE;
    return load_palette(pd, start, n);
}

ULONG APIENTRY DrvGetModes(HANDLE hDriver, ULONG cjSize, DEVMODEW *pdm)
{
    VIDEO_MODE_INFORMATION *m;
    ULONG n, i, out = 0, dmsz;
    if (!g_hdrv) g_hdrv = hDriver;
    TRACE(("GetModes %u %x\n", cjSize, (ULONG)(ULONG_PTR)pdm));
#ifdef GM0
    return 0;
#endif
    /* DEVMODEW as the engine knows it: NT 3.x ends at dmDisplayFrequency
       (0xBC), NT 4.0 adds the ICM fields (0xD4), 2000 the panning size */
    dmsz = g_engine < 0x20000 ? 0xBC : g_engine < 0x30000 ? 0xD4 : sizeof(DEVMODEW);
    m = get_modes(hDriver, &n);
    TRACE(("GetModes n=%u\n", n));
    if (!m) return 0;
    if (!pdm) { mem_free(m); return n * dmsz; }
    for (i = 0; i < n && (out + 1) * dmsz <= cjSize; i++, out++) {
        DEVMODEW *d = (DEVMODEW *)((char *)pdm + out * dmsz);
        static const WCHAR name[] = L"winvbe";
        memset(d, 0, dmsz);
        memcpy(d->dmDeviceName, name, sizeof(name));
        d->dmSpecVersion = g_engine < 0x20000 ? 0x320 : g_engine < 0x30000 ? 0x400 : DM_SPECVERSION;
        d->dmDriverVersion = d->dmSpecVersion;
        d->dmSize = (WORD)dmsz;
        d->dmBitsPerPel = mode_bpp(&m[i]);
        d->dmPelsWidth = m[i].VisScreenWidth;
        d->dmPelsHeight = m[i].VisScreenHeight;
        d->dmDisplayFrequency = m[i].Frequency;
        d->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_DISPLAYFLAGS;
    }
    mem_free(m);
    return out * dmsz;
}

#ifdef HOOKTEST
static ULONG nblt, ntxt, ncpy, nmisc;
static void trace_clip(CLIPOBJ *co)
{
    struct { ULONG c; RECTL arcl[8]; } er;
    BOOL more;
    if (!co || co->iDComplexity != DC_COMPLEX) return;
    CLIPOBJ_cEnumStart(co, FALSE, CT_RECTANGLES, CD_ANY, 0);
    do {
        ULONG i;
        more = CLIPOBJ_bEnum(co, sizeof(er), (ULONG *)&er);
        for (i = 0; i < er.c; i++)
            TRACE(("  clip %u,%u-%u,%u\n", er.arcl[i].left, er.arcl[i].top, er.arcl[i].right, er.arcl[i].bottom));
    } while (more);
}
BOOL APIENTRY DrvBitBlt(SURFOBJ *dst, SURFOBJ *src, SURFOBJ *msk, CLIPOBJ *co, XLATEOBJ *xo, RECTL *rd,
                        POINTL *ps, POINTL *pm, BRUSHOBJ *bo, POINTL *pb, ROP4 rop)
{
    if (nblt++ < 60) {
        TRACE(("BitBlt pid %u rop %x dst %u,%u-%u,%u dc %u\n", 0UL, rop,
               rd->left, rd->top, rd->right, rd->bottom, co ? co->iDComplexity : 9));
        trace_clip(co);
    }
    return EngBitBlt(dst, src, msk, co, xo, rd, ps, pm, bo, pb, rop);
}
BOOL APIENTRY DrvTextOut(SURFOBJ *so, STROBJ *str, FONTOBJ *fo, CLIPOBJ *co, RECTL *rx, RECTL *ro,
                         BRUSHOBJ *bf, BRUSHOBJ *bo, POINTL *po, MIX mix)
{
    if (ntxt++ < 40) TRACE(("TextOut pid %u %u,%u\n", 0UL, str->rclBkGround.left, str->rclBkGround.top));
    return EngTextOut(so, str, fo, co, rx, ro, bf, bo, po, mix);
}
BOOL APIENTRY DrvCopyBits(SURFOBJ *dst, SURFOBJ *src, CLIPOBJ *co, XLATEOBJ *xo, RECTL *rd, POINTL *ps)
{
    if (ncpy++ < 60) TRACE(("CopyBits pid %u dst %u,%u-%u,%u\n", 0UL, rd->left, rd->top, rd->right, rd->bottom));
    return EngCopyBits(dst, src, co, xo, rd, ps);
}
BOOL APIENTRY DrvStrokePath(SURFOBJ *so, PATHOBJ *po, CLIPOBJ *co, XFORMOBJ *xo, BRUSHOBJ *bo, POINTL *pb,
                            LINEATTRS *la, MIX mix)
{
    if (nmisc++ < 40) TRACE(("StrokePath pid %u\n", 0UL));
    return EngStrokePath(so, po, co, xo, bo, pb, la, mix);
}
BOOL APIENTRY DrvPaint(SURFOBJ *so, CLIPOBJ *co, BRUSHOBJ *bo, POINTL *pb, MIX mix)
{
    if (nmisc++ < 40) TRACE(("Paint pid %u\n", 0UL));
    return EngPaint(so, co, bo, pb, mix);
}
#endif

/* the pointer is drawn by GDI (simulated pointer) */
ULONG APIENTRY DrvSetPointerShape(SURFOBJ *pso, SURFOBJ *psoMask, SURFOBJ *psoColor, XLATEOBJ *pxlo,
                                  LONG xHot, LONG yHot, LONG x, LONG y, RECTL *prcl, FLONG fl)
{
    TRACE(("SetPointerShape\n"));
    return SPS_ERROR;
}

VOID APIENTRY DrvMovePointer(SURFOBJ *pso, LONG x, LONG y, RECTL *prcl)
{
#ifdef FBCALLS
    PDEV *pd = (PDEV *)pso->dhpdev; ULONG r; VIDEO_POINTER_POSITION pp;
    if (x == -1) ioctl(pd->hDriver, IOCTL_VIDEO_DISABLE_POINTER, NULL, 0, NULL, 0, &r);
    else { pp.Column = (SHORT)x; pp.Row = (SHORT)y; ioctl(pd->hDriver, IOCTL_VIDEO_SET_POINTER_POSITION, &pp, sizeof(pp), NULL, 0, &r); }
#endif
}

ULONG APIENTRY DrvDitherColor(DHPDEV dhpdev, ULONG iMode, ULONG rgb, ULONG *pul)
{
    TRACE(("DitherColor %x %x\n", iMode, rgb));
    return DCR_SOLID;
}

#ifndef NT3X
/* ------------------------------------------------------------------ */
/* DirectDraw (Windows NT 4.0 and later): the primary surface, page   */
/* flipping in video memory and off-screen surfaces (blits by the     */
/* DirectDraw HEL), vertical blank status                              */

static inline UCHAR in_port(USHORT p)
{
    UCHAR v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}

static BOOL dd_set_start(PDEV *pd, ULONG off)
{
    ULONG r;
    if (off == pd->start) return TRUE;
    if (ioctl(pd->hDriver, IOCTL_WVB_SET_START, &off, sizeof(off), NULL, 0, &r)) return FALSE;
    pd->start = off;
    return TRUE;
}

/* lines of video memory after the visible screen */
static ULONG dd_heap_lines(PDEV *pd)
{
    ULONG lines = pd->stride ? pd->vram / (ULONG)pd->stride : 0;
    return lines > pd->h ? lines - pd->h : 0;
}

static DWORD APIENTRY DdMapMemory(PDD_MAPMEMORYDATA d)
{
    PDEV *pd = (PDEV *)d->lpDD->dhpdev;
    ULONG r;
    if (d->bMap) {
        VIDEO_SHARE_MEMORY sm;
        VIDEO_SHARE_MEMORY_INFORMATION si;
        sm.ProcessHandle = d->hProcess;
        sm.ViewOffset = 0;
        sm.ViewSize = pd->vram;
        sm.RequestedVirtualAddress = NULL;
        if (ioctl(pd->hDriver, IOCTL_VIDEO_SHARE_VIDEO_MEMORY, &sm, sizeof(sm), &si, sizeof(si), &r)) {
            d->ddRVal = DDERR_GENERIC;
            return DDHAL_DRIVER_HANDLED;
        }
        d->fpProcess = (FLATPTR)si.VirtualAddress;
    } else {
        VIDEO_SHARE_MEMORY sm;
        sm.ProcessHandle = d->hProcess;
        sm.ViewOffset = 0;
        sm.ViewSize = 0;
        sm.RequestedVirtualAddress = (PVOID)d->fpProcess;
        ioctl(pd->hDriver, IOCTL_VIDEO_UNSHARE_VIDEO_MEMORY, &sm, sizeof(sm), NULL, 0, &r);
        /* an application that ends while a back buffer is shown */
        dd_set_start(pd, 0);
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY DdFlip(PDD_FLIPDATA d)
{
    PDEV *pd = (PDEV *)d->lpDD->dhpdev;
    ULONG off = (ULONG)d->lpSurfTarg->lpGbl->fpVidMem;
    d->ddRVal = off < pd->vram && dd_set_start(pd, off) ? DD_OK : DDERR_GENERIC;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY DdGetFlipStatus(PDD_GETFLIPSTATUSDATA d)
{
    d->ddRVal = DD_OK;              /* flips are done in the vertical retrace */
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY DdWaitForVerticalBlank(PDD_WAITFORVERTICALBLANKDATA d)
{
    ULONG n;
    switch (d->dwFlags) {
    case DDWAITVB_I_TESTVB:
        d->bIsInVB = (in_port(0x3DA) & 8) != 0;
        break;
    case DDWAITVB_BLOCKBEGIN:
        for (n = 0; n < 2000000 && (in_port(0x3DA) & 8); n++) ;
        for (n = 0; n < 2000000 && !(in_port(0x3DA) & 8); n++) ;
        break;
    case DDWAITVB_BLOCKEND:
        for (n = 0; n < 2000000 && !(in_port(0x3DA) & 8); n++) ;
        for (n = 0; n < 2000000 && (in_port(0x3DA) & 8); n++) ;
        break;
    default:
        d->ddRVal = DDERR_UNSUPPORTED;
        return DDHAL_DRIVER_HANDLED;
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* surfaces in the format of the screen only */
static DWORD APIENTRY DdCanCreateSurface(PDD_CANCREATESURFACEDATA d)
{
    d->ddRVal = d->bIsDifferentPixelFormat ? DDERR_INVALIDPIXELFORMAT : DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

BOOL APIENTRY DrvGetDirectDrawInfo(DHPDEV dhpdev, DD_HALINFO *phi, DWORD *pdwNumHeaps,
                                   VIDEOMEMORY *pvmList, DWORD *pdwNumFourCC, DWORD *pdwFourCC)
{
    PDEV *pd = (PDEV *)dhpdev;
    ULONG lines = dd_heap_lines(pd), n;
    DD_HALINFO h;
    VIDEOMEMORYINFO *vi = &h.vmiData;
    DDNTCORECAPS *c = &h.ddCaps;

    TRACE(("GetDirectDrawInfo vram %x heap lines %u list %x\n", pd->vram, lines, (ULONG)(ULONG_PTR)pvmList));
    if (!pd->fb) return FALSE;
    *pdwNumFourCC = 0;
    *pdwNumHeaps = lines ? 1 : 0;
    if (pvmList && lines) {
        memset(pvmList, 0, sizeof(*pvmList));
        pvmList->dwFlags = VIDMEM_ISRECTANGULAR;
        pvmList->fpStart = (FLATPTR)pd->h * pd->stride;
        pvmList->dwWidth = pd->stride;
        pvmList->dwHeight = lines;
    }
    memset(&h, 0, sizeof(h));
    vi->fpPrimary = 0;
    vi->dwDisplayWidth = pd->w;
    vi->dwDisplayHeight = pd->h;
    vi->lDisplayPitch = pd->stride;
    vi->ddpfDisplay.dwSize = sizeof(DDPIXELFORMAT);
    vi->ddpfDisplay.dwFlags = DDPF_RGB | (pd->bpp == 8 ? DDPF_PALETTEINDEXED8 : 0);
    vi->ddpfDisplay.dwRGBBitCount = pd->bpp;
    if (pd->bpp > 8) {
        vi->ddpfDisplay.dwRBitMask = pd->rmask;
        vi->ddpfDisplay.dwGBitMask = pd->gmask;
        vi->ddpfDisplay.dwBBitMask = pd->bmask;
    }
    vi->dwOffscreenAlign = 8;
    vi->pvPrimary = pd->fb;
    c->dwSize = sizeof(*c);
    /* 8 bpp: palettes of the primary surface go through GDI (DrvSetPalette).
       3DDEVICE: Direct3D's software rasterizers (RGB, MMX) may render into
       flip chains and off-screen surfaces in video memory; there is no
       Direct3D HAL (no 3D hardware behind VBE) */
    c->ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE |
                        (lines >= pd->h ? DDSCAPS_FLIP : 0);
    c->dwVidMemTotal = pd->vram;
    c->dwVidMemFree = lines * pd->stride;
    if (g_engine >= DDI_DRIVER_VERSION_NT5) {
        h.dwSize = sizeof(DD_HALINFO);
        n = sizeof(DD_HALINFO);
    } else {
        /* Windows NT 4.0 (DirectX 3): the older, shorter structure; only
           the part that is the same in every version is written (the
           rest is zero: no GetDriverInfo, no flags) */
        h.dwSize = sizeof(DD_HALINFO_V4);
        n = FIELD_OFFSET(DD_HALINFO, ddCaps) + FIELD_OFFSET(DDNTCORECAPS, dwMaxVideoPorts);
    }
    memcpy(phi, &h, n);
    return TRUE;
}

BOOL APIENTRY DrvEnableDirectDraw(DHPDEV dhpdev, DD_CALLBACKS *cb, DD_SURFACECALLBACKS *scb,
                                  DD_PALETTECALLBACKS *pcb)
{
    TRACE(("EnableDirectDraw\n"));
    cb->dwFlags = DDHAL_CB32_MAPMEMORY | DDHAL_CB32_WAITFORVERTICALBLANK | DDHAL_CB32_CANCREATESURFACE;
    cb->MapMemory = DdMapMemory;
    cb->WaitForVerticalBlank = DdWaitForVerticalBlank;
    cb->CanCreateSurface = DdCanCreateSurface;
    scb->dwFlags = DDHAL_SURFCB32_FLIP | DDHAL_SURFCB32_GETFLIPSTATUS;
    scb->Flip = DdFlip;
    scb->GetFlipStatus = DdGetFlipStatus;
    return TRUE;
}

VOID APIENTRY DrvDisableDirectDraw(DHPDEV dhpdev)
{
    TRACE(("DisableDirectDraw\n"));
    dd_set_start((PDEV *)dhpdev, 0);
}
#endif

EXPORT VOID APIENTRY DrvDisableDriver(VOID)
{
}

static DRVFN fns[] = {
    { INDEX_DrvEnablePDEV,      (PFN)DrvEnablePDEV },
    { INDEX_DrvCompletePDEV,    (PFN)DrvCompletePDEV },
    { INDEX_DrvDisablePDEV,     (PFN)DrvDisablePDEV },
    { INDEX_DrvEnableSurface,   (PFN)DrvEnableSurface },
    { INDEX_DrvDisableSurface,  (PFN)DrvDisableSurface },
    { INDEX_DrvAssertMode,      (PFN)DrvAssertMode },
    { INDEX_DrvSetPalette,      (PFN)DrvSetPalette },
#ifdef HOOKTEST
    { INDEX_DrvBitBlt,          (PFN)DrvBitBlt },
    { INDEX_DrvTextOut,         (PFN)DrvTextOut },
    { INDEX_DrvCopyBits,        (PFN)DrvCopyBits },
    { INDEX_DrvStrokePath,      (PFN)DrvStrokePath },
    { INDEX_DrvPaint,           (PFN)DrvPaint },
#endif
#ifndef NOPTR
    { INDEX_DrvSetPointerShape, (PFN)DrvSetPointerShape },
    { INDEX_DrvMovePointer,     (PFN)DrvMovePointer },
#endif
#ifndef NODITHER
    { INDEX_DrvDitherColor,     (PFN)DrvDitherColor },
#endif
#ifndef NOGETMODES
    { INDEX_DrvGetModes,        (PFN)DrvGetModes },     /* not on NT 3.1/3.5 */
#endif
#if !defined(NT3X) && !defined(NODDRAW)
    /* last: Windows NT 4.0 (DirectX 3) and later */
    { INDEX_DrvGetDirectDrawInfo, (PFN)DrvGetDirectDrawInfo },
    { INDEX_DrvEnableDirectDraw,  (PFN)DrvEnableDirectDraw },
    { INDEX_DrvDisableDirectDraw, (PFN)DrvDisableDirectDraw },
#define NDDFN 3
#endif
};

EXPORT BOOL APIENTRY DrvEnableDriver(ULONG engine, ULONG cj, DRVENABLEDATA *ded)
{
    ULONG nfn = sizeof(fns) / sizeof(fns[0]);
    g_engine = engine;
#ifdef NT3X
    if (engine < 0x10002) nfn--;            /* NT 3.1 / 3.5: no DrvGetModes */
#endif
#if defined(NDDFN) && defined(NODDRAW4)
    if (engine < DDI_DRIVER_VERSION_NT5) nfn -= NDDFN;     /* NT 4.0: no DirectDraw */
#endif
    if (cj >= sizeof(ULONG))
#ifdef NT3X
        ded->iDriverVersion = engine >= 0x10002 ? 0x10002 : 0x10000;
#else
        ded->iDriverVersion = engine >= DDI_DRIVER_VERSION_NT5 ? DDI_DRIVER_VERSION_NT5 : DDI_DRIVER_VERSION_NT4;
#endif
    if (cj >= 2 * sizeof(ULONG)) ded->c = nfn;
    if (cj >= sizeof(DRVENABLEDATA)) ded->pdrvfn = fns;
    return TRUE;
}
