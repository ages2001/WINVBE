/*
 * DDTEST - DirectDraw test for WINVBE (Windows NT/2000/XP and 9x)
 *   DDTEST [width height bpp [seconds]]
 * Full screen exclusive mode, primary surface with one back buffer:
 * page flipping, locks, vertical blank, palette (8 bpp).  Writes DDTEST.TXT
 * next to the program.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <ddraw.h>
/* no C runtime: runs on every Windows with DirectDraw */
static HANDLE lg;
static HWND hwnd;

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

static int num(const char **p, int def)
{
    int v = 0, any = 0;
    while (**p == ' ') (*p)++;
    while (**p >= '0' && **p <= '9') { v = v * 10 + *(*p)++ - '0'; any = 1; }
    return any ? v : def;
}

static LRESULT CALLBACK wp(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_SETCURSOR) { SetCursor(NULL); return TRUE; }
    return DefWindowProc(h, m, w, l);
}

static DWORD pack(DDPIXELFORMAT *pf, int r, int g, int b)
{
    DWORD v = 0, m;
    int sh;
    if (pf->dwRGBBitCount == 8) return (DWORD)r;      /* palette index */
#define CH(mask, c) m = mask; sh = 0; while (m && !(m & 1)) { m >>= 1; sh++; } \
    v |= (((DWORD)c * m + 127) / 255 << sh) & mask;
    CH(pf->dwRBitMask, r) CH(pf->dwGBitMask, g) CH(pf->dwBBitMask, b)
    return v;
}

static void put(BYTE *p, int bpp, DWORD v)
{
    switch (bpp) {
    case 8: *p = (BYTE)v; break;
    case 16: *(WORD *)p = (WORD)v; break;
    case 24: p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); break;
    default: *(DWORD *)p = v;
    }
}

/* background colour c, a white bar at x, a black box with buffer number */
static int draw(LPDIRECTDRAWSURFACE s, int w, int h, int bpp, DWORD c, DWORD white, int x)
{
    DDSURFACEDESC sd;
    HRESULT hr;
    int y, i, B = bpp / 8;
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    hr = IDirectDrawSurface_Lock(s, NULL, &sd, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL);
    if (hr != DD_OK) { logf_("Lock failed %08lx\n", hr); return 0; }
    for (y = 0; y < h; y++) {
        BYTE *row = (BYTE *)sd.lpSurface + y * sd.lPitch;
        for (i = 0; i < w; i++) put(row + i * B, bpp, (i >= x && i < x + 32) ? white : c);
    }
    IDirectDrawSurface_Unlock(s, NULL);
    return 1;
}

void WINAPI start(void)
{
    char path[MAX_PATH], *e;
    HINSTANCE hi = GetModuleHandle(NULL);
    const char *cmd = GetCommandLine();
    LPDIRECTDRAW dd;
    LPDIRECTDRAWSURFACE prim = NULL, back = NULL;
    LPDIRECTDRAWPALETTE pal = NULL;
    DDSURFACEDESC sd;
    DDSCAPS caps;
    DDCAPS hal, hel;
    HRESULT hr;
    WNDCLASS wc;
    int w = 640, h = 480, bpp = 16, secs = 4, n, frames = 0, d3 = 0;
    DWORD t0, t;
    DWORD red, green, blue, white;

    GetModuleFileName(NULL, path, sizeof(path));
    for (e = path + lstrlen(path); e > path && e[-1] != '\\'; e--) ;
    lstrcpy(e, "DDTEST.TXT");
    lg = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (lg == INVALID_HANDLE_VALUE) ExitProcess(1);
    if (*cmd == '"') { cmd++; while (*cmd && *cmd != '"') cmd++; if (*cmd) cmd++; }
    else while (*cmd && *cmd != ' ') cmd++;
    w = num(&cmd, w); h = num(&cmd, h); bpp = num(&cmd, bpp); secs = num(&cmd, secs); d3 = num(&cmd, 0);
    logf_("DDTEST %dx%dx%d %d s\n", w, h, bpp, secs);

    hr = DirectDrawCreate(NULL, &dd, NULL);
    if (hr != DD_OK) { logf_("DirectDrawCreate failed %08lx\n", hr); ExitProcess(1); }
    /* DirectX 3 size: accepted by every DirectDraw version (NT 4.0: DX3) */
    memset(&hal, 0, sizeof(hal)); hal.dwSize = sizeof(DDCAPS_DX3);
    memset(&hel, 0, sizeof(hel)); hel.dwSize = sizeof(DDCAPS_DX3);
    hr = IDirectDraw_GetCaps(dd, &hal, &hel);
    logf_("GetCaps %08lx HAL caps %08lx caps2 %08lx palcaps %08lx scaps %08lx vidmem %lu free %lu\n",
          hr, hal.dwCaps, hal.dwCaps2, hal.dwPalCaps, hal.ddsCaps.dwCaps, hal.dwVidMemTotal, hal.dwVidMemFree);
    logf_("HEL caps %08lx scaps %08lx\n", hel.dwCaps, hel.ddsCaps.dwCaps);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wp;
    wc.hInstance = hi;
    wc.hbrBackground = GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "DDTEST";
    RegisterClass(&wc);
    hwnd = CreateWindowEx(WS_EX_TOPMOST, "DDTEST", "DDTEST", WS_POPUP | WS_VISIBLE, 0, 0,
                          GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), NULL, NULL, hi, NULL);
    hr = IDirectDraw_SetCooperativeLevel(dd, hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    logf_("SetCooperativeLevel %08lx\n", hr);
    hr = IDirectDraw_SetDisplayMode(dd, w, h, bpp);
    logf_("SetDisplayMode %08lx\n", hr);
    if (hr != DD_OK) goto out;

    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | (d3 ? DDSCAPS_3DDEVICE : 0);
    sd.dwBackBufferCount = 1;
    hr = IDirectDraw_CreateSurface(dd, &sd, &prim, NULL);
    logf_("CreateSurface flip chain %08lx\n", hr);
    if (hr != DD_OK) goto out;
    caps.dwCaps = DDSCAPS_BACKBUFFER;
    hr = IDirectDrawSurface_GetAttachedSurface(prim, &caps, &back);
    logf_("GetAttachedSurface %08lx\n", hr);
    if (hr != DD_OK) goto out;
    IDirectDrawSurface_GetCaps(prim, &caps);
    logf_("primary caps %08lx\n", caps.dwCaps);
    IDirectDrawSurface_GetCaps(back, &caps);
    logf_("back caps %08lx (%s)\n", caps.dwCaps, caps.dwCaps & DDSCAPS_VIDEOMEMORY ? "video memory" : "system memory");
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    IDirectDrawSurface_GetSurfaceDesc(prim, &sd);
    logf_("primary %lux%lu pitch %ld bits %lu masks %06lx %06lx %06lx\n", sd.dwWidth, sd.dwHeight, sd.lPitch,
          sd.ddpfPixelFormat.dwRGBBitCount, sd.ddpfPixelFormat.dwRBitMask, sd.ddpfPixelFormat.dwGBitMask,
          sd.ddpfPixelFormat.dwBBitMask);
    if (bpp == 8) {
        PALETTEENTRY pe[256];
        memset(pe, 0, sizeof(pe));
        pe[1].peRed = 255; pe[2].peGreen = 255; pe[3].peBlue = 255;
        pe[4].peRed = pe[4].peGreen = pe[4].peBlue = 255;
        hr = IDirectDraw_CreatePalette(dd, DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &pal, NULL);
        logf_("CreatePalette %08lx\n", hr);
        if (pal) logf_("SetPalette %08lx\n", IDirectDrawSurface_SetPalette(prim, pal));
        red = 1; green = 2; blue = 3; white = 4;
    } else {
        red = pack(&sd.ddpfPixelFormat, 255, 0, 0);
        green = pack(&sd.ddpfPixelFormat, 0, 255, 0);
        blue = pack(&sd.ddpfPixelFormat, 0, 0, 255);
        white = pack(&sd.ddpfPixelFormat, 255, 255, 255);
    }
    /* 1: red visible, green in the back buffer, then flip: green visible */
    draw(prim, w, h, bpp, red, white, 0);
    draw(back, w, h, bpp, green, white, 0);
    logf_("frame red on screen\n");
    Sleep(secs * 1000);
    hr = IDirectDrawSurface_Flip(prim, NULL, DDFLIP_WAIT);
    logf_("Flip %08lx, green on screen\n", hr);
    Sleep(secs * 1000);
    /* 2: flipping speed with a moving bar */
    t0 = GetTickCount();
    for (n = 0; GetTickCount() - t0 < (DWORD)secs * 1000; n++) {
        if (!draw(back, w, h, bpp, blue, white, (n * 8) % (w - 32))) break;
        hr = IDirectDrawSurface_Flip(prim, NULL, DDFLIP_WAIT);
        if (hr != DD_OK) { logf_("Flip %d failed %08lx\n", n, hr); break; }
        frames++;
    }
    t = GetTickCount() - t0;
    logf_("%d flips in %lu ms (%lu per second)\n", frames, t, t ? (DWORD)frames * 1000UL / t : 0);
    {
        BOOL vb = 0;
        hr = IDirectDraw_WaitForVerticalBlank(dd, DDWAITVB_BLOCKBEGIN, NULL);
        logf_("WaitForVerticalBlank %08lx\n", hr);
        hr = IDirectDraw_GetVerticalBlankStatus(dd, &vb);
        logf_("GetVerticalBlankStatus %08lx %d\n", hr, vb);
    }
out:
    if (pal) IDirectDrawPalette_Release(pal);
    if (prim) IDirectDrawSurface_Release(prim);
    IDirectDraw_RestoreDisplayMode(dd);
    IDirectDraw_SetCooperativeLevel(dd, hwnd, DDSCL_NORMAL);
    IDirectDraw_Release(dd);
    DestroyWindow(hwnd);
    logf_("done\n");
    CloseHandle(lg);
    ExitProcess(0);
}
