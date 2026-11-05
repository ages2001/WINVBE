/*
 * D3DTEST - Direct3D 7 test for WINVBE (Windows 2000/XP, XP x64, 9x with DX7+)
 *   D3DTEST [width height bpp [seconds]]
 * Full screen flip chain with DDSCAPS_3DDEVICE in video memory, a Direct3D
 * device on the back buffer (HAL if there is one, else the RGB software
 * rasterizer), a rotating shaded triangle; then checks read back from the
 * back buffer: Gouraud colours, the Z buffer, a point-sampled texture and
 * alpha blending. Writes D3DTEST.TXT ("checks: n failed" at the end).
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>

static HANDLE lg;
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

typedef struct { float x, y, z, rhw; DWORD c; } VTX;
typedef struct { float x, y, z, rhw; DWORD c; float u, v; } TVTX;

static LPDIRECTDRAWSURFACE7 zb_found;
static DDPIXELFORMAT zpf;
static HRESULT CALLBACK zenum(LPDDPIXELFORMAT pf, LPVOID ctx)
{
    (void)ctx;
    if (pf->dwZBufferBitDepth == 16 && !pf->dwStencilBitDepth) { zpf = *pf; return D3DENUMRET_CANCEL; }
    if (!zpf.dwSize) zpf = *pf;
    return D3DENUMRET_OK;
}

/* the pixel (x, y) of a locked surface as R, G, B */
static DWORD rgb_at(const DDSURFACEDESC2 *sd, int x, int y)
{
    const BYTE *p = (const BYTE *)sd->lpSurface + y * sd->lPitch;
    DWORD v, r, g, b, bits = sd->ddpfPixelFormat.dwRGBBitCount;
    if (bits == 16) {
        v = ((const WORD *)p)[x];
        if (sd->ddpfPixelFormat.dwGBitMask == 0x07E0) { r = (v >> 11) << 3; g = ((v >> 5) & 63) << 2; b = (v & 31) << 3; }
        else { r = ((v >> 10) & 31) << 3; g = ((v >> 5) & 31) << 3; b = (v & 31) << 3; }
    } else {
        v = bits == 24 ? (DWORD)p[x * 3] | ((DWORD)p[x * 3 + 1] << 8) | ((DWORD)p[x * 3 + 2] << 16) : ((const DWORD *)p)[x];
        r = (v >> 16) & 255; g = (v >> 8) & 255; b = v & 255;
    }
    return (r << 16) | (g << 8) | b;
}

static int fails;
static void check(LPDIRECTDRAWSURFACE7 s, const char *what, int x, int y, DWORD want, int tol)
{
    DDSURFACEDESC2 sd;
    DWORD got;
    int k, bad = 0;
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    if (IDirectDrawSurface7_Lock(s, NULL, &sd, DDLOCK_WAIT | DDLOCK_READONLY, NULL) != DD_OK) {
        logf_("  %s: lock failed\n", what); fails++; return;
    }
    got = rgb_at(&sd, x, y);
    IDirectDrawSurface7_Unlock(s, NULL);
    for (k = 0; k < 24; k += 8) {
        int d = (int)((got >> k) & 255) - (int)((want >> k) & 255);
        if (d < -tol || d > tol) bad = 1;
    }
    logf_("  %s at %d,%d: %06lx (want %06lx) %s\n", what, x, y, got, want, bad ? "FAILED" : "ok");
    fails += bad;
}

static void quad(LPDIRECT3DDEVICE7 dev, float x0, float y0, float x1, float y1, float z, DWORD c)
{
    VTX v[4];
    int i;
    for (i = 0; i < 4; i++) { v[i].z = z; v[i].rhw = 1.0f; v[i].c = c; }
    v[0].x = x0; v[0].y = y0; v[1].x = x1; v[1].y = y0; v[2].x = x0; v[2].y = y1; v[3].x = x1; v[3].y = y1;
    IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, v, 4, 0);
}

void WINAPI start(void)
{
    char path[MAX_PATH], *e;
    HINSTANCE hi = GetModuleHandle(NULL);
    const char *cmd = GetCommandLine();
    LPDIRECTDRAW7 dd = NULL;
    LPDIRECTDRAWSURFACE7 prim = NULL, back = NULL;
    LPDIRECT3D7 d3d = NULL;
    LPDIRECT3DDEVICE7 dev = NULL;
    DDSURFACEDESC2 sd;
    DDSCAPS2 caps;
    D3DVIEWPORT7 vp;
    WNDCLASS wc;
    HWND hwnd;
    HRESULT hr;
    int w = 640, h = 480, bpp = 16, secs = 5, frames = 0;
    DWORD t0, t;
    float ax = 0.0f, ay = -1.0f;            /* rotating unit vector */
    const float cs = 0.99862953f, sn = 0.05233596f;     /* 3 degrees */
    const char *devname = "HAL";

    GetModuleFileName(NULL, path, sizeof(path));
    for (e = path + lstrlen(path); e > path && e[-1] != '\\'; e--) ;
    lstrcpy(e, "D3DTEST.TXT");
    lg = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (*cmd == '"') { cmd++; while (*cmd && *cmd != '"') cmd++; if (*cmd) cmd++; }
    else while (*cmd && *cmd != ' ') cmd++;
    w = num(&cmd, w); h = num(&cmd, h); bpp = num(&cmd, bpp); secs = num(&cmd, secs);
    logf_("D3DTEST %dx%dx%d %d s\n", w, h, bpp, secs);

    hr = DirectDrawCreateEx(NULL, (void **)&dd, &IID_IDirectDraw7, NULL);
    logf_("DirectDrawCreateEx %08lx\n", hr);
    if (hr != DD_OK) goto out;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wp; wc.hInstance = hi; wc.lpszClassName = "D3DTEST";
    wc.hbrBackground = GetStockObject(BLACK_BRUSH);
    RegisterClass(&wc);
    hwnd = CreateWindowEx(WS_EX_TOPMOST, "D3DTEST", "D3DTEST", WS_POPUP | WS_VISIBLE, 0, 0,
                          GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), NULL, NULL, hi, NULL);
    hr = IDirectDraw7_SetCooperativeLevel(dd, hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_FPUSETUP);
    logf_("SetCooperativeLevel %08lx\n", hr);
    hr = IDirectDraw7_SetDisplayMode(dd, w, h, bpp, 0, 0);
    logf_("SetDisplayMode %08lx\n", hr);
    if (hr != DD_OK) goto out;
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE;
    sd.dwBackBufferCount = 1;
    hr = IDirectDraw7_CreateSurface(dd, &sd, &prim, NULL);
    logf_("CreateSurface 3D flip chain %08lx\n", hr);
    if (hr != DD_OK) goto out;
    memset(&caps, 0, sizeof(caps));
    caps.dwCaps = DDSCAPS_BACKBUFFER;
    hr = IDirectDrawSurface7_GetAttachedSurface(prim, &caps, &back);
    if (hr != DD_OK) goto out;
    IDirectDrawSurface7_GetCaps(back, &caps);
    logf_("back buffer caps %08lx (%s)\n", caps.dwCaps, caps.dwCaps & DDSCAPS_VIDEOMEMORY ? "video memory" : "system memory");
    hr = IDirectDraw7_QueryInterface(dd, &IID_IDirect3D7, (void **)&d3d);
    logf_("IDirect3D7 %08lx\n", hr);
    if (hr != DD_OK) goto out;
    /* a Z buffer on the back buffer */
    IDirect3D7_EnumZBufferFormats(d3d, &IID_IDirect3DHALDevice, zenum, NULL);
    if (zpf.dwSize) {
        DDSURFACEDESC2 zd;
        memset(&zd, 0, sizeof(zd));
        zd.dwSize = sizeof(zd);
        zd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        zd.dwWidth = w; zd.dwHeight = h;
        zd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
        zd.ddpfPixelFormat = zpf;
        hr = IDirectDraw7_CreateSurface(dd, &zd, &zb_found, NULL);
        if (hr == DD_OK) hr = IDirectDrawSurface7_AddAttachedSurface(back, zb_found);
        logf_("Z buffer %lu bits: %08lx\n", zpf.dwZBufferBitDepth, hr);
        if (hr != DD_OK && zb_found) { IDirectDrawSurface7_Release(zb_found); zb_found = NULL; }
    } else
        logf_("no Z buffer formats\n");
    hr = IDirect3D7_CreateDevice(d3d, &IID_IDirect3DHALDevice, back, &dev);
    logf_("HAL device %08lx\n", hr);
    if (hr != D3D_OK) {
        devname = "RGB";
        hr = IDirect3D7_CreateDevice(d3d, &IID_IDirect3DRGBDevice, back, &dev);
        logf_("RGB device %08lx\n", hr);
    }
    if (hr != D3D_OK) goto out;
    vp.dwX = 0; vp.dwY = 0; vp.dwWidth = w; vp.dwHeight = h; vp.dvMinZ = 0.0f; vp.dvMaxZ = 1.0f;
    IDirect3DDevice7_SetViewport(dev, &vp);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_LIGHTING, FALSE);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD);
    t0 = GetTickCount();
    while ((t = GetTickCount() - t0) < (DWORD)secs * 1000) {
        VTX v[3];
        float r = (float)(h * 2 / 5), cx = (float)(w / 2), cy = (float)(h / 2), bx, by;
        int i;
        /* vertices at 0, 120 and 240 degrees of the rotating vector */
        bx = ax; by = ay;
        for (i = 0; i < 3; i++) {
            float nx;
            v[i].x = cx + bx * r; v[i].y = cy + by * r; v[i].z = 0.5f; v[i].rhw = 1.0f;
            v[i].c = i == 0 ? 0xFFFF0000 : i == 1 ? 0xFF00FF00 : 0xFF0000FF;
            nx = bx * -0.5f - by * 0.8660254f; by = bx * 0.8660254f + by * -0.5f; bx = nx;
        }
        { float nx = ax * cs - ay * sn; ay = ax * sn + ay * cs; ax = nx; }
        IDirect3DDevice7_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF202060, 1.0f, 0);
        if (IDirect3DDevice7_BeginScene(dev) == D3D_OK) {
            hr = IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLELIST, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, v, 3, 0);
            IDirect3DDevice7_EndScene(dev);
            if (frames == 0) logf_("DrawPrimitive %08lx\n", hr);
        }
        hr = IDirectDrawSurface7_Flip(prim, NULL, DDFLIP_WAIT);
        if (hr != DD_OK) { logf_("Flip %08lx\n", hr); break; }
        frames++;
    }
    logf_("%s device: %d frames in %lu ms (%lu per second)\n", devname, frames, t, t ? (DWORD)frames * 1000UL / t : 0);

    /* checks, read back from the back buffer (not flipped) */
    logf_("checks (%s device):\n", devname);
    {
        VTX v[3];
        float fw = (float)w, fh = (float)h;
        /* Gouraud: red, green, blue corners; the centre is grey */
        IDirect3DDevice7_Clear(dev, 0, NULL, D3DCLEAR_TARGET | (zb_found ? D3DCLEAR_ZBUFFER : 0), 0xFF000000, 1.0f, 0);
        IDirect3DDevice7_BeginScene(dev);
        v[0].x = fw / 2; v[0].y = 10; v[1].x = fw - 10; v[1].y = fh - 10; v[2].x = 10; v[2].y = fh - 10;
        v[0].z = v[1].z = v[2].z = 0.5f; v[0].rhw = v[1].rhw = v[2].rhw = 1.0f;
        v[0].c = 0xFFFF0000; v[1].c = 0xFF00FF00; v[2].c = 0xFF0000FF;
        IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLELIST, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, v, 3, 0);
        IDirect3DDevice7_EndScene(dev);
        check(back, "gouraud centre", w / 2, (int)((10 + 2 * (fh - 10)) / 3), 0x555555, 24);
        check(back, "gouraud near red", w / 2, 16, 0xF00000, 40);
        check(back, "background", 3, 3, 0x000000, 8);
        /* Z buffer: a near green square, then a far red one over it */
        if (zb_found) {
            IDirect3DDevice7_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);
            IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_ZENABLE, D3DZB_TRUE);
            IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL);
            IDirect3DDevice7_BeginScene(dev);
            quad(dev, 100, 100, 200, 200, 0.2f, 0xFF00FF00);
            quad(dev, 50, 50, 250, 250, 0.7f, 0xFFFF0000);
            IDirect3DDevice7_EndScene(dev);
            check(back, "Z: near square in front", 150, 150, 0x00FF00, 16);
            check(back, "Z: far square around it", 60, 60, 0xFF0000, 16);
            IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_ZENABLE, D3DZB_FALSE);
        } else
            logf_("  Z: no Z buffer, skipped\n");
        /* alpha blending: 50 % red over blue */
        IDirect3DDevice7_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF0000FF, 1.0f, 0);
        IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
        IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
        IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
        IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        IDirect3DDevice7_BeginScene(dev);
        quad(dev, 20, 20, 120, 120, 0.5f, 0x80FF0000);
        IDirect3DDevice7_EndScene(dev);
        IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
        check(back, "alpha blend", 70, 70, 0x80007F, 24);
        /* a point-sampled texture: 2x2 checker of red and white */
        {
            DDSURFACEDESC2 td, ld;
            LPDIRECTDRAWSURFACE7 tex = NULL;
            memset(&td, 0, sizeof(td));
            td.dwSize = sizeof(td);
            td.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
            td.dwWidth = td.dwHeight = 64;
            td.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY;
            td.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            td.ddpfPixelFormat.dwFlags = DDPF_RGB;
            td.ddpfPixelFormat.dwRGBBitCount = 16;
            td.ddpfPixelFormat.dwRBitMask = 0xF800; td.ddpfPixelFormat.dwGBitMask = 0x07E0;
            td.ddpfPixelFormat.dwBBitMask = 0x001F;
            hr = IDirectDraw7_CreateSurface(dd, &td, &tex, NULL);
            if (hr != DD_OK) {
                td.ddsCaps.dwCaps = DDSCAPS_TEXTURE;     /* system memory */
                hr = IDirectDraw7_CreateSurface(dd, &td, &tex, NULL);
            }
            logf_("  texture 64x64 565: %08lx\n", hr);
            memset(&ld, 0, sizeof(ld));
            ld.dwSize = sizeof(ld);
            if (tex && IDirectDrawSurface7_Lock(tex, NULL, &ld, DDLOCK_WAIT, NULL) == DD_OK) {
                int x, y;
                for (y = 0; y < 64; y++)
                    for (x = 0; x < 64; x++)
                        ((WORD *)((BYTE *)ld.lpSurface + y * ld.lPitch))[x] = ((x < 32) ^ (y < 32)) ? 0xF800 : 0xFFFF;
                IDirectDrawSurface7_Unlock(tex, NULL);
                {
                    TVTX t4[4];
                    int i;
                    IDirect3DDevice7_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
                    IDirect3DDevice7_SetTexture(dev, 0, tex);
                    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_MAGFILTER, D3DTFG_POINT);
                    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_MINFILTER, D3DTFN_POINT);
                    for (i = 0; i < 4; i++) { t4[i].z = 0.5f; t4[i].rhw = 1.0f; t4[i].c = 0xFFFFFFFF; }
                    t4[0].x = 200; t4[0].y = 100; t4[0].u = 0; t4[0].v = 0;
                    t4[1].x = 328; t4[1].y = 100; t4[1].u = 1; t4[1].v = 0;
                    t4[2].x = 200; t4[2].y = 228; t4[2].u = 0; t4[2].v = 1;
                    t4[3].x = 328; t4[3].y = 228; t4[3].u = 1; t4[3].v = 1;
                    IDirect3DDevice7_BeginScene(dev);
                    IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1,
                                                   t4, 4, 0);
                    IDirect3DDevice7_EndScene(dev);
                    IDirect3DDevice7_SetTexture(dev, 0, NULL);
                    check(back, "texture top left (white)", 230, 130, 0xF8FCF8, 16);
                    check(back, "texture top right (red)", 300, 130, 0xF80000, 16);
                    check(back, "texture bottom right (white)", 300, 200, 0xF8FCF8, 16);
                }
            }
            if (tex) IDirectDrawSurface7_Release(tex);
        }
    }
    logf_("checks: %d failed\n", fails);
out:
    if (dev) IDirect3DDevice7_Release(dev);
    if (zb_found) IDirectDrawSurface7_Release(zb_found);
    if (d3d) IDirect3D7_Release(d3d);
    if (prim) IDirectDrawSurface7_Release(prim);
    if (dd) { IDirectDraw7_RestoreDisplayMode(dd); IDirectDraw7_Release(dd); }
    logf_("done\n");
    CloseHandle(lg);
    ExitProcess(0);
}
