/*
 * D3DTEST - Direct3D 7 test for WINVBE (Windows 2000/XP, XP x64, 9x with DX7+)
 *   D3DTEST [width height bpp [seconds]]
 * Full screen flip chain with DDSCAPS_3DDEVICE in video memory, a Direct3D
 * device on the back buffer (HAL if there is one, else the RGB software
 * rasterizer), a rotating shaded triangle. Writes D3DTEST.TXT.
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
out:
    if (dev) IDirect3DDevice7_Release(dev);
    if (d3d) IDirect3D7_Release(d3d);
    if (prim) IDirectDrawSurface7_Release(prim);
    if (dd) { IDirectDraw7_RestoreDisplayMode(dd); IDirectDraw7_Release(dd); }
    logf_("done\n");
    CloseHandle(lg);
    ExitProcess(0);
}
