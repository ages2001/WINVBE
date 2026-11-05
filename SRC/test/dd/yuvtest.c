/*
 * YUVTEST - DirectDraw YUV / blit test for WINVBE (Windows NT 4.0 and later,
 * Windows 95/98/ME)
 *   YUVTEST [seconds]
 * Windowed: a 640x480 window on the primary surface with a clipper. For
 * YUY2, UYVY and YV12 it makes a 320x240 off-screen surface in video
 * memory with eight colour bars, blits it stretched to the window, reads
 * the bars back from the primary surface and compares them with the RGB
 * they should give; then it times the blits. RGB fills, copies and
 * source colour keys are checked the same way. With an overlay (real or
 * emulated) the bars are also shown as an overlay. Writes YUVTEST.TXT next
 * to the program.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <ddraw.h>

static HANDLE lg;
static HWND hwnd;
static int fails;

void *memset(void *d, int c, size_t n) { char *p = d; while (n--) *p++ = (char)c; return d; }

static void logf_(const char *f, ...)
{
    char b[512];
    DWORD n;
    va_list ap;
    va_start(ap, f);
    n = wvsprintfA(b, f, ap);
    va_end(ap);
    WriteFile(lg, b, n, &n, NULL);
    FlushFileBuffers(lg);
}

static LRESULT CALLBACK wp(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProc(h, m, w, l);
}

static void pump(void)
{
    MSG m;
    while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessage(&m); }
}

/* the bars: RGB and their BT.601 YUV (video levels) */
static const BYTE bar_rgb[8][3] = {
    {255,255,255}, {255,255,0}, {0,255,255}, {0,255,0}, {255,0,255}, {255,0,0}, {0,0,255}, {0,0,0}
};
static const BYTE bar_yuv[8][3] = {
    {235,128,128}, {210,16,146}, {170,166,16}, {145,54,34}, {106,202,222}, {81,90,240}, {41,240,110}, {16,128,128}
};

static int shift_of(DWORD m) { int s = 0; if (!m) return 0; while (!(m & 1)) { m >>= 1; s++; } return s; }
static int bits_of(DWORD m) { int b = 0; while (m) { b += m & 1; m >>= 1; } return b; }

static void unpack(DDPIXELFORMAT *pf, DWORD v, int *r, int *g, int *b)
{
#define UN(mask, out) { int s = shift_of(mask), n = bits_of(mask); DWORD c = (v & mask) >> s; \
        *out = n ? (int)(c * 255 / ((1UL << n) - 1)) : 0; }
    UN(pf->dwRBitMask, r) UN(pf->dwGBitMask, g) UN(pf->dwBBitMask, b)
}

static DWORD pack(DDPIXELFORMAT *pf, int r, int g, int b)
{
    DWORD v = 0;
#define PK(mask, c) { int s = shift_of(mask), n = bits_of(mask); v |= ((DWORD)(c) * ((1UL << n) - 1) / 255) << s; }
    PK(pf->dwRBitMask, r) PK(pf->dwGBitMask, g) PK(pf->dwBBitMask, b)
    return v;
}

static DWORD getpx(BYTE *p, int bpp)
{
    switch (bpp) {
    case 32: return *(DWORD *)p;
    case 24: return p[0] | (p[1] << 8) | (p[2] << 16);
    case 16: return *(WORD *)p;
    default: return *p;
    }
}

static void putpx(BYTE *p, int bpp, DWORD v)
{
    switch (bpp) {
    case 32: *(DWORD *)p = v; break;
    case 24: p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); break;
    case 16: *(WORD *)p = (WORD)v; break;
    default: *p = (BYTE)v; break;
    }
}

static void fill_bars(DDSURFACEDESC *sd, DWORD fourcc)
{
    BYTE *b = sd->lpSurface;
    LONG p = sd->lPitch;
    DWORD w = sd->dwWidth, h = sd->dwHeight, x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            const BYTE *c = bar_yuv[x * 8 / w];
            if (fourcc == MAKEFOURCC('Y','V','1','2')) {
                BYTE *vp = b + p * h, *up = vp + (p / 2) * (h / 2);
                b[y * p + x] = c[0];
                vp[(y / 2) * (p / 2) + x / 2] = c[2];
                up[(y / 2) * (p / 2) + x / 2] = c[1];
            } else {
                BYTE *q = b + y * p + (x & ~1UL) * 2;
                if (fourcc == MAKEFOURCC('Y','U','Y','2')) { q[(x & 1) * 2] = c[0]; q[1] = c[1]; q[3] = c[2]; }
                else { q[1 + (x & 1) * 2] = c[0]; q[0] = c[1]; q[2] = c[2]; }
            }
        }
}

/* the eight bars read back from the primary surface inside rc */
static int check_bars(IDirectDrawSurface *prim, RECT *rc, const char *what, const BYTE exp[8][3])
{
    DDSURFACEDESC sd;
    int i, bad = 0, r, g, b;
    char line[200], *o = line;
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    if (IDirectDrawSurface_Lock(prim, NULL, &sd, DDLOCK_WAIT | DDLOCK_READONLY, NULL) != DD_OK) {
        logf_("%s: primary lock failed\r\n", what);
        fails++;
        return 0;
    }
    for (i = 0; i < 8; i++) {
        LONG x = rc->left + (rc->right - rc->left) * (2 * i + 1) / 16;
        LONG y = (rc->top + rc->bottom) / 2;
        int bpp = (int)sd.ddpfPixelFormat.dwRGBBitCount;
        DWORD v = getpx((BYTE *)sd.lpSurface + y * sd.lPitch + x * (bpp / 8), bpp);
        unpack(&sd.ddpfPixelFormat, v, &r, &g, &b);
        if (r - exp[i][0] > 12 || exp[i][0] - r > 12 || g - exp[i][1] > 12 || exp[i][1] - g > 12 ||
            b - exp[i][2] > 12 || exp[i][2] - b > 12) bad++;
        o += wsprintfA(o, " %d,%d,%d", r, g, b);
    }
    IDirectDrawSurface_Unlock(prim, NULL);
    logf_("%s: %s%s\r\n", what, bad ? "FAIL" : "ok", line);
    if (bad) fails++;
    return !bad;
}

static IDirectDrawSurface *make_surf(IDirectDraw *dd, DWORD w, DWORD h, DWORD fourcc, DDPIXELFORMAT *rgb,
                                     DWORD caps)
{
    DDSURFACEDESC sd;
    IDirectDrawSurface *s = NULL;
    HRESULT hr;
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.ddsCaps.dwCaps = caps;
    sd.dwWidth = w; sd.dwHeight = h;
    if (fourcc) {
        sd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        sd.ddpfPixelFormat.dwFlags = DDPF_FOURCC;
        sd.ddpfPixelFormat.dwFourCC = fourcc;
    } else
        sd.ddpfPixelFormat = *rgb;
    hr = IDirectDraw_CreateSurface(dd, &sd, &s, NULL);
    if (hr != DD_OK) { logf_("  CreateSurface %lux%lu %.4s caps %lx: %lx\r\n", w, h, fourcc ? (char *)&fourcc : "RGB ", caps, hr); return NULL; }
    return s;
}

void WINAPI start(void)
{
    char path[MAX_PATH], *p;
    IDirectDraw *dd;
    IDirectDrawSurface *prim = NULL, *s;
    IDirectDrawClipper *clip = NULL;
    DDSURFACEDESC sd;
    DDCAPS hal, hel;
    DWORD codes[32], ncodes = 32, i, t;
    RECT rc;
    POINT pt;
    WNDCLASSA wc;
    HRESULT hr;
    static const DWORD fcc[3] = { MAKEFOURCC('Y','U','Y','2'), MAKEFOURCC('U','Y','V','Y'), MAKEFOURCC('Y','V','1','2') };

    GetModuleFileNameA(NULL, path, sizeof(path));
    for (p = path + lstrlenA(path); p > path && p[-1] != '\\'; p--) ;
    lstrcpyA(p, "YUVTEST.TXT");
    lg = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wp;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hbrBackground = GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "YUVTEST";
    RegisterClassA(&wc);
    {
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        int ww = sw >= 680 ? 640 : sw / 2 * 2, wh = sh >= 520 ? 480 : sh / 2 * 2;
        hwnd = CreateWindowExA(WS_EX_TOPMOST, "YUVTEST", "YUVTEST", WS_POPUP | WS_VISIBLE, sw >= 680 ? 40 : 0,
                               sh >= 520 ? 40 : 0, ww, wh, NULL, NULL, wc.hInstance, NULL);
    }
    pump();
    if (DirectDrawCreate(NULL, &dd, NULL) != DD_OK) { logf_("DirectDrawCreate failed\r\n"); ExitProcess(1); }
    IDirectDraw_SetCooperativeLevel(dd, hwnd, DDSCL_NORMAL);
    memset(&hal, 0, sizeof(hal)); hal.dwSize = sizeof(hal);
    memset(&hel, 0, sizeof(hel)); hel.dwSize = sizeof(hel);
    IDirectDraw_GetCaps(dd, &hal, &hel);
    logf_("HAL caps %lx caps2 %lx ckey %lx fx %lx fourcc %lu vidmem %lu free %lu overlays %lu\r\n",
          hal.dwCaps, hal.dwCaps2, hal.dwCKeyCaps, hal.dwFXCaps, hal.dwNumFourCCCodes, hal.dwVidMemTotal,
          hal.dwVidMemFree, hal.dwMaxVisibleOverlays);
    if (IDirectDraw_GetFourCCCodes(dd, &ncodes, codes) == DD_OK)
        for (i = 0; i < ncodes; i++) logf_("  FourCC %.4s\r\n", (char *)&codes[i]);

    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    if ((hr = IDirectDraw_CreateSurface(dd, &sd, &prim, NULL)) != DD_OK) { logf_("primary: %lx\r\n", hr); ExitProcess(1); }
    IDirectDraw_CreateClipper(dd, 0, &clip, NULL);
    IDirectDrawClipper_SetHWnd(clip, 0, hwnd);
    IDirectDrawSurface_SetClipper(prim, clip);
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    IDirectDrawSurface_GetSurfaceDesc(prim, &sd);
    logf_("primary %lux%lux%lu pitch %ld\r\n", sd.dwWidth, sd.dwHeight, sd.ddpfPixelFormat.dwRGBBitCount, sd.lPitch);

    GetClientRect(hwnd, &rc);
    pt.x = pt.y = 0;
    ClientToScreen(hwnd, &pt);
    OffsetRect(&rc, pt.x, pt.y);

    for (i = 0; i < 3; i++) {
        DDSURFACEDESC ld;
        DWORD n;
        logf_("%.4s:\r\n", (char *)&fcc[i]);
        s = make_surf(dd, 320, 240, fcc[i], NULL, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY);
        if (!s) { fails++; continue; }
        memset(&ld, 0, sizeof(ld));
        ld.dwSize = sizeof(ld);
        if (IDirectDrawSurface_Lock(s, NULL, &ld, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL) != DD_OK) {
            logf_("  lock failed\r\n"); fails++; IDirectDrawSurface_Release(s); continue;
        }
        logf_("  pitch %ld\r\n", ld.lPitch);
        fill_bars(&ld, fcc[i]);
        IDirectDrawSurface_Unlock(s, NULL);
        hr = IDirectDrawSurface_Blt(prim, &rc, s, NULL, DDBLT_WAIT, NULL);
        if (hr != DD_OK) { logf_("  Blt: %lx\r\n", hr); fails++; }
        else check_bars(prim, &rc, "  stretched 320x240 -> 640x480", bar_rgb);
        t = GetTickCount();
        for (n = 0; n < 60 && GetTickCount() - t < 4000; n++) IDirectDrawSurface_Blt(prim, &rc, s, NULL, DDBLT_WAIT, NULL);
        t = GetTickCount() - t;
        logf_("  %lu blits in %lu ms (%lu per second)\r\n", n, t, t ? n * 1000 / t : 0);
        pump();
        Sleep(700);
        IDirectDrawSurface_Release(s);
    }

    /* RGB: fill, copy with a source colour key, stretch */
    {
        DDBLTFX fx;
        DDPIXELFORMAT pf = sd.ddpfPixelFormat;
        BYTE exp[8][3];
        logf_("RGB:\r\n");
        s = make_surf(dd, 320, 240, 0, &pf, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY);
        if (s && pf.dwRGBBitCount > 8) {
            DDSURFACEDESC ld;
            DDCOLORKEY ck;
            DWORD x, y;
            memset(&fx, 0, sizeof(fx));
            fx.dwSize = sizeof(fx);
            fx.dwFillColor = pack(&pf, 0, 0, 255);
            IDirectDrawSurface_Blt(prim, &rc, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
            for (i = 0; i < 8; i++) { exp[i][0] = 0; exp[i][1] = 0; exp[i][2] = 255; }
            check_bars(prim, &rc, "  colour fill blue", exp);
            /* bars in the off-screen surface, every other one the key colour */
            memset(&ld, 0, sizeof(ld));
            ld.dwSize = sizeof(ld);
            if (IDirectDrawSurface_Lock(s, NULL, &ld, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL) == DD_OK) {
                for (y = 0; y < 240; y++)
                    for (x = 0; x < 320; x++) {
                        int k = (int)(x * 8 / 320);
                        DWORD v = (k & 1) ? pack(&pf, 0, 128, 128) : pack(&pf, bar_rgb[k][0], bar_rgb[k][1], bar_rgb[k][2]);
                        putpx((BYTE *)ld.lpSurface + y * ld.lPitch + x * (pf.dwRGBBitCount / 8), (int)pf.dwRGBBitCount, v);
                    }
                IDirectDrawSurface_Unlock(s, NULL);
            }
            ck.dwColorSpaceLowValue = ck.dwColorSpaceHighValue = pack(&pf, 0, 128, 128);
            IDirectDrawSurface_SetColorKey(s, DDCKEY_SRCBLT, &ck);
            hr = IDirectDrawSurface_Blt(prim, &rc, s, NULL, DDBLT_KEYSRC | DDBLT_WAIT, NULL);
            for (i = 0; i < 8; i++) {
                exp[i][0] = (i & 1) ? 0 : bar_rgb[i][0]; exp[i][1] = (i & 1) ? 0 : bar_rgb[i][1];
                exp[i][2] = (i & 1) ? 255 : bar_rgb[i][2];
            }
            if (hr != DD_OK) { logf_("  keyed Blt: %lx\r\n", hr); fails++; }
            else check_bars(prim, &rc, "  stretched with source colour key", exp);
            pump();
            Sleep(700);
            IDirectDrawSurface_Release(s);
        }
    }

    /* overlay */
    if (hal.dwCaps & DDCAPS_OVERLAY) {
        DDSURFACEDESC ld;
        logf_("overlay (YUY2):\r\n");
        s = make_surf(dd, 320, 240, fcc[0], NULL, DDSCAPS_OVERLAY | DDSCAPS_VIDEOMEMORY);
        if (s) {
            DDOVERLAYFX ofx;
            DDBLTFX fx;
            DWORD key = pack(&sd.ddpfPixelFormat, 255, 0, 255);
            memset(&ld, 0, sizeof(ld));
            ld.dwSize = sizeof(ld);
            if (IDirectDrawSurface_Lock(s, NULL, &ld, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL) == DD_OK) {
                fill_bars(&ld, fcc[0]);
                IDirectDrawSurface_Unlock(s, NULL);
            }
            memset(&fx, 0, sizeof(fx));
            fx.dwSize = sizeof(fx);
            fx.dwFillColor = key;
            IDirectDrawSurface_Blt(prim, &rc, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
            memset(&ofx, 0, sizeof(ofx));
            ofx.dwSize = sizeof(ofx);
            ofx.dckDestColorkey.dwColorSpaceLowValue = ofx.dckDestColorkey.dwColorSpaceHighValue = key;
            hr = IDirectDrawSurface_UpdateOverlay(s, NULL, prim, &rc, DDOVER_SHOW | DDOVER_KEYDESTOVERRIDE, &ofx);
            if (hr != DD_OK) { logf_("  UpdateOverlay: %lx\r\n", hr); fails++; }
            else {
                Sleep(200);
                check_bars(prim, &rc, "  overlay over the key colour", bar_rgb);
                Sleep(700);
                IDirectDrawSurface_UpdateOverlay(s, NULL, prim, NULL, DDOVER_HIDE, NULL);
            }
            IDirectDrawSurface_Release(s);
        } else fails++;
    } else
        logf_("no overlay\r\n");

    logf_("%s (%d failures)\r\n", fails ? "FAILED" : "PASSED", fails);
    IDirectDrawSurface_Release(prim);
    IDirectDraw_Release(dd);
    CloseHandle(lg);
    ExitProcess(fails ? 1 : 0);
}
