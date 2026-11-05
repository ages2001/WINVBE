/*
 * D3D6TEST - Direct3D 6 (IDirect3D3) test for WINVBE's Windows 9x HAL:
 * what Windows 98 SE's own DirectX 6.1 offers to programs.
 *   D3D6TEST [width height bpp]
 * Full screen flip chain, the HAL device (else the RGB device) on the back
 * buffer, a viewport; a Gouraud triangle, a point-sampled texture
 * (IDirect3DTexture2) and a few flips; colours read back. Writes
 * D3D6TEST.TXT ("checks: n failed" at the end).
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>

static HANDLE lg;
static int fails;
void *memset(void *d, int c, size_t n) { char *p = d; while (n--) *p++ = (char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { char *p = d; const char *q = s; while (n--) *p++ = *q++; return d; }
static void logf_(const char *f, ...)
{
    char b[512]; DWORD n; va_list ap;
    va_start(ap, f); n = wvsprintfA(b, f, ap); va_end(ap);
    WriteFile(lg, b, n, &n, NULL); FlushFileBuffers(lg);
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

typedef struct { float x, y, z, rhw; DWORD c, s; float u, v; } TLV;

static DWORD rgb_at(const DDSURFACEDESC2 *sd, int x, int y)
{
    const BYTE *p = (const BYTE *)sd->lpSurface + y * sd->lPitch;
    DWORD v, bits = sd->ddpfPixelFormat.dwRGBBitCount;
    if (bits == 16) {
        v = ((const WORD *)p)[x];
        if (sd->ddpfPixelFormat.dwGBitMask == 0x07E0) return ((v >> 11) << 19) | (((v >> 5) & 63) << 10) | ((v & 31) << 3);
        return (((v >> 10) & 31) << 19) | (((v >> 5) & 31) << 11) | ((v & 31) << 3);
    }
    v = bits == 24 ? (DWORD)p[x * 3] | ((DWORD)p[x * 3 + 1] << 8) | ((DWORD)p[x * 3 + 2] << 16) : ((const DWORD *)p)[x];
    return v & 0xFFFFFF;
}

static void check(LPDIRECTDRAWSURFACE4 s, const char *what, int x, int y, DWORD want, int tol)
{
    DDSURFACEDESC2 sd;
    DWORD got;
    int k, bad = 0;
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    if (IDirectDrawSurface4_Lock(s, NULL, &sd, DDLOCK_WAIT, NULL) != DD_OK) { logf_("  %s: lock failed\n", what); fails++; return; }
    got = rgb_at(&sd, x, y);
    IDirectDrawSurface4_Unlock(s, NULL);
    for (k = 0; k < 24; k += 8) {
        int d = (int)((got >> k) & 255) - (int)((want >> k) & 255);
        if (d < -tol || d > tol) bad = 1;
    }
    logf_("  %s at %d,%d: %06lx (want %06lx) %s\n", what, x, y, got, want, bad ? "FAILED" : "ok");
    fails += bad;
}

void WINAPI start(void)
{
    char path[MAX_PATH], *e;
    HINSTANCE hi = GetModuleHandle(NULL);
    const char *cmd = GetCommandLine();
    LPDIRECTDRAW dd1 = NULL;
    LPDIRECTDRAW4 dd = NULL;
    LPDIRECTDRAWSURFACE4 prim = NULL, back = NULL, tex = NULL;
    LPDIRECT3D3 d3d = NULL;
    LPDIRECT3DDEVICE3 dev = NULL;
    LPDIRECT3DVIEWPORT3 vp = NULL;
    LPDIRECT3DTEXTURE2 t2 = NULL;
    DDSURFACEDESC2 sd;
    DDSCAPS2 caps;
    D3DVIEWPORT2 v2;
    WNDCLASS wc;
    HWND hwnd;
    HRESULT hr;
    int w = 640, h = 480, bpp = 16, i;
    const char *devname = "HAL";

    GetModuleFileName(NULL, path, sizeof(path));
    for (e = path + lstrlen(path); e > path && e[-1] != '\\'; e--) ;
    lstrcpy(e, "D3D6TEST.TXT");
    lg = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (*cmd == '"') { cmd++; while (*cmd && *cmd != '"') cmd++; if (*cmd) cmd++; }
    else while (*cmd && *cmd != ' ') cmd++;
    w = num(&cmd, w); h = num(&cmd, h); bpp = num(&cmd, bpp);
    logf_("D3D6TEST %dx%dx%d\n", w, h, bpp);

    hr = DirectDrawCreate(NULL, &dd1, NULL);
    if (hr == DD_OK) hr = IDirectDraw_QueryInterface(dd1, &IID_IDirectDraw4, (void **)&dd);
    logf_("IDirectDraw4 %08lx\n", hr);
    if (hr != DD_OK) goto out;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wp; wc.hInstance = hi; wc.lpszClassName = "D3D6TEST";
    wc.hbrBackground = GetStockObject(BLACK_BRUSH);
    RegisterClass(&wc);
    hwnd = CreateWindowEx(WS_EX_TOPMOST, "D3D6TEST", "D3D6TEST", WS_POPUP | WS_VISIBLE, 0, 0,
                          GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), NULL, NULL, hi, NULL);
    hr = IDirectDraw4_SetCooperativeLevel(dd, hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    hr = IDirectDraw4_SetDisplayMode(dd, w, h, bpp, 0, 0);
    logf_("SetDisplayMode %08lx\n", hr);
    if (hr != DD_OK) goto out;
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE;
    sd.dwBackBufferCount = 1;
    hr = IDirectDraw4_CreateSurface(dd, &sd, &prim, NULL);
    logf_("CreateSurface flip chain %08lx\n", hr);
    if (hr != DD_OK) goto out;
    memset(&caps, 0, sizeof(caps));
    caps.dwCaps = DDSCAPS_BACKBUFFER;
    if (IDirectDrawSurface4_GetAttachedSurface(prim, &caps, &back) != DD_OK) goto out;
    hr = IDirectDraw4_QueryInterface(dd, &IID_IDirect3D3, (void **)&d3d);
    logf_("IDirect3D3 %08lx\n", hr);
    if (hr != DD_OK) goto out;
    hr = IDirect3D3_CreateDevice(d3d, &IID_IDirect3DHALDevice, back, &dev, NULL);
    logf_("HAL device %08lx\n", hr);
    if (hr != D3D_OK) {
        devname = "RGB";
        hr = IDirect3D3_CreateDevice(d3d, &IID_IDirect3DRGBDevice, back, &dev, NULL);
        logf_("RGB device %08lx\n", hr);
    }
    if (hr != D3D_OK) goto out;
    hr = IDirect3D3_CreateViewport(d3d, &vp, NULL);
    if (hr == D3D_OK) hr = IDirect3DDevice3_AddViewport(dev, vp);
    memset(&v2, 0, sizeof(v2));
    v2.dwSize = sizeof(v2);
    v2.dwX = 0; v2.dwY = 0; v2.dwWidth = w; v2.dwHeight = h;
    v2.dvClipX = -1.0f; v2.dvClipY = 1.0f; v2.dvClipWidth = 2.0f; v2.dvClipHeight = 2.0f;
    v2.dvMinZ = 0.0f; v2.dvMaxZ = 1.0f;
    if (hr == D3D_OK) hr = IDirect3DViewport3_SetViewport2(vp, &v2);
    if (hr == D3D_OK) hr = IDirect3DDevice3_SetCurrentViewport(dev, vp);
    logf_("viewport %08lx\n", hr);
    if (hr != D3D_OK) goto out;
    IDirect3DDevice3_SetRenderState(dev, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice3_SetRenderState(dev, D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD);

    /* a few frames */
    for (i = 0; i < 30; i++) {
        D3DRECT r;
        TLV v[3];
        int k;
        r.x1 = 0; r.y1 = 0; r.x2 = w; r.y2 = h;
        IDirect3DViewport3_Clear2(vp, 1, &r, D3DCLEAR_TARGET, 0xFF000000 | (i * 8), 1.0f, 0);
        for (k = 0; k < 3; k++) { v[k].z = 0.5f; v[k].rhw = 1.0f; v[k].s = 0xFF000000; v[k].u = v[k].v = 0; }
        v[0].x = (float)(w / 2); v[0].y = 10; v[1].x = (float)(w - 10); v[1].y = (float)(h - 10);
        v[2].x = 10; v[2].y = (float)(h - 10);
        v[0].c = 0xFFFF0000; v[1].c = 0xFF00FF00; v[2].c = 0xFF0000FF;
        IDirect3DDevice3_BeginScene(dev);
        hr = IDirect3DDevice3_DrawPrimitive(dev, D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, v, 3, 0);
        IDirect3DDevice3_EndScene(dev);
        if (i == 0) logf_("DrawPrimitive %08lx\n", hr);
        if (i < 29) IDirectDrawSurface4_Flip(prim, NULL, DDFLIP_WAIT);
    }
    logf_("checks (%s device):\n", devname);
    check(back, "gouraud centre", w / 2, (10 + 2 * (h - 10)) / 3, 0x555555, 24);
    check(back, "background", 3, 3, 0x0000E8, 16);

    /* a texture */
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.dwWidth = sd.dwHeight = 64;
    sd.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY;
    sd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    sd.ddpfPixelFormat.dwFlags = DDPF_RGB;
    sd.ddpfPixelFormat.dwRGBBitCount = 16;
    sd.ddpfPixelFormat.dwRBitMask = 0xF800; sd.ddpfPixelFormat.dwGBitMask = 0x07E0; sd.ddpfPixelFormat.dwBBitMask = 0x1F;
    hr = IDirectDraw4_CreateSurface(dd, &sd, &tex, NULL);
    logf_("texture %08lx\n", hr);
    if (hr == DD_OK) hr = IDirectDrawSurface4_QueryInterface(tex, &IID_IDirect3DTexture2, (void **)&t2);
    if (hr == DD_OK) {
        DDSURFACEDESC2 ld;
        memset(&ld, 0, sizeof(ld));
        ld.dwSize = sizeof(ld);
        if (IDirectDrawSurface4_Lock(tex, NULL, &ld, DDLOCK_WAIT, NULL) == DD_OK) {
            int x, y;
            for (y = 0; y < 64; y++)
                for (x = 0; x < 64; x++)
                    ((WORD *)((BYTE *)ld.lpSurface + y * ld.lPitch))[x] = ((x < 32) ^ (y < 32)) ? 0xF800 : 0xFFFF;
            IDirectDrawSurface4_Unlock(tex, NULL);
        }
        {
            D3DRECT r;
            TLV t4[4];
            r.x1 = 0; r.y1 = 0; r.x2 = w; r.y2 = h;
            IDirect3DViewport3_Clear2(vp, 1, &r, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
            IDirect3DDevice3_SetTexture(dev, 0, t2);
            IDirect3DDevice3_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            IDirect3DDevice3_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            IDirect3DDevice3_SetTextureStageState(dev, 0, D3DTSS_MAGFILTER, D3DTFG_POINT);
            IDirect3DDevice3_SetTextureStageState(dev, 0, D3DTSS_MINFILTER, D3DTFN_POINT);
            for (i = 0; i < 4; i++) { t4[i].z = 0.5f; t4[i].rhw = 1.0f; t4[i].c = 0xFFFFFFFF; t4[i].s = 0xFF000000; }
            t4[0].x = 200; t4[0].y = 100; t4[0].u = 0; t4[0].v = 0;
            t4[1].x = 328; t4[1].y = 100; t4[1].u = 1; t4[1].v = 0;
            t4[2].x = 200; t4[2].y = 228; t4[2].u = 0; t4[2].v = 1;
            t4[3].x = 328; t4[3].y = 228; t4[3].u = 1; t4[3].v = 1;
            IDirect3DDevice3_BeginScene(dev);
            hr = IDirect3DDevice3_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, D3DFVF_TLVERTEX, t4, 4, 0);
            IDirect3DDevice3_EndScene(dev);
            logf_("textured DrawPrimitive %08lx\n", hr);
            check(back, "texture top left (white)", 230, 130, 0xF8FCF8, 16);
            check(back, "texture top right (red)", 300, 130, 0xF80000, 16);
            IDirect3DDevice3_SetTexture(dev, 0, NULL);
        }
    } else
        fails++;
    logf_("checks: %d failed\n", fails);
out:
    if (t2) IDirect3DTexture2_Release(t2);
    if (tex) IDirectDrawSurface4_Release(tex);
    if (vp) IDirect3DViewport3_Release(vp);
    if (dev) IDirect3DDevice3_Release(dev);
    if (d3d) IDirect3D3_Release(d3d);
    if (prim) IDirectDrawSurface4_Release(prim);
    if (dd) { IDirectDraw4_RestoreDisplayMode(dd); IDirectDraw4_Release(dd); }
    if (dd1) IDirectDraw_Release(dd1);
    logf_("done\n");
    CloseHandle(lg);
    ExitProcess(0);
}
