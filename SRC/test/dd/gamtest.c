/*
 * GAMTEST - gamma ramp test for WINVBE (Windows 95/98/ME, NT 4.0 and later)
 *   GAMTEST [seconds]
 * GDI: GetDeviceGammaRamp / SetDeviceGammaRamp on the screen DC (a darker
 * ramp, read back, then the original restored). DirectDraw: the primary
 * surface's IDirectDrawGammaControl when the driver reports
 * DDCAPS2_PRIMARYGAMMA, the same round trip. Shows a grey wedge while the
 * dark ramp is set. Writes GAMTEST.TXT next to the program; the last line
 * is "checks: n failed".
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <ddraw.h>

static HANDLE lg;

void *memset(void *d, int c, size_t n) { char *p = d; while (n--) *p++ = (char)c; return d; }
static int fails;
static WORD orig[3][256], ramp[3][256], back[3][256];

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

static void check(int ok, const char *what)
{
    logf_("%s: %s\r\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

/* the ramp read back may be rounded to what the hardware keeps (8 bits) */
static int same(WORD a[3][256], WORD b[3][256])
{
    int c, i;
    for (c = 0; c < 3; c++)
        for (i = 0; i < 256; i++)
            if ((a[c][i] >> 8) != (b[c][i] >> 8)) return 0;
    return 1;
}

static void make_ramp(void)
{
    int i;
    /* red at full scale, green at 3/4, blue at 1/2: tells the channels apart */
    for (i = 0; i < 256; i++) {
        ramp[0][i] = (WORD)(i * 257);
        ramp[1][i] = (WORD)(i * 257 * 3 / 4);
        ramp[2][i] = (WORD)(i * 257 / 2);
    }
}

static LRESULT CALLBACK wp(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        RECT r;
        int x;
        HDC dc = BeginPaint(h, &ps);
        GetClientRect(h, &r);
        for (x = 0; x < 16; x++) {
            RECT b = { r.right * x / 16, 0, r.right * (x + 1) / 16, r.bottom };
            HBRUSH br = CreateSolidBrush(RGB(x * 17, x * 17, x * 17));
            FillRect(dc, &b, br);
            DeleteObject(br);
        }
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

static void pump(DWORD ms)
{
    DWORD t0 = GetTickCount();
    MSG m;
    while (GetTickCount() - t0 < ms) {
        while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessage(&m); }
        Sleep(20);
    }
}

static void test_gdi(DWORD secs)
{
    HDC dc = GetDC(NULL);
    BOOL r;
    logf_("GDI gamma ramp\r\n");
    r = GetDeviceGammaRamp(dc, orig);
    logf_("GetDeviceGammaRamp %d (%u %u %u at 128)\r\n", r, orig[0][128], orig[1][128], orig[2][128]);
    check(r, "GetDeviceGammaRamp");
    make_ramp();
    r = SetDeviceGammaRamp(dc, ramp);
    logf_("SetDeviceGammaRamp %d\r\n", r);
    /* Windows 9x in high or true colour with the DIB engine: no ramp (the
       driver cannot apply it); 256 colours and Windows NT: required */
    if (!r && GetDeviceCaps(dc, BITSPIXEL) > 8 && (GetVersion() & 0x80000000UL))
        logf_("info: no gamma ramp in this mode (Windows 9x DIB engine mode, high/true colour)\r\n");
    else
        check(r, "SetDeviceGammaRamp");
    if (r) {
        r = GetDeviceGammaRamp(dc, back);
        check(r && same(ramp, back), "ramp read back");
        logf_("read back at 128: %u %u %u\r\n", back[0][128], back[1][128], back[2][128]);
        pump(secs * 1000);
        r = SetDeviceGammaRamp(dc, orig);
        check(r, "original ramp restored");
    }
    ReleaseDC(NULL, dc);
}

static void test_dd(HWND hwnd)
{
    LPDIRECTDRAW dd;
    LPDIRECTDRAWSURFACE ps;
    LPDIRECTDRAWGAMMACONTROL gc;
    DDCAPS hal, hel;
    DDSURFACEDESC sd;
    static DDGAMMARAMP o, n, b;
    HRESULT hr;
    int c, i;

    logf_("DirectDraw gamma control\r\n");
    if (DirectDrawCreate(NULL, &dd, NULL) != DD_OK) { logf_("no DirectDraw\r\n"); return; }
    memset(&hal, 0, sizeof(hal)); hal.dwSize = sizeof(hal);
    memset(&hel, 0, sizeof(hel)); hel.dwSize = sizeof(hel);
    IDirectDraw_GetCaps(dd, &hal, &hel);
    logf_("caps2 %08lx: PRIMARYGAMMA %s\r\n", hal.dwCaps2, hal.dwCaps2 & DDCAPS2_PRIMARYGAMMA ? "yes" : "no");
    check((hal.dwCaps2 & DDCAPS2_PRIMARYGAMMA) != 0, "DDCAPS2_PRIMARYGAMMA");
    /* full screen: DirectDraw applies a primary gamma ramp in exclusive mode */
    hr = IDirectDraw_SetCooperativeLevel(dd, hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    if (hr == DD_OK) hr = IDirectDraw_SetDisplayMode(dd, 640, 480, 8);
    logf_("exclusive 640x480x8 %08lx\r\n", hr);
    memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS; sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    if (IDirectDraw_CreateSurface(dd, &sd, &ps, NULL) != DD_OK) { check(0, "primary surface"); goto out; }
    hr = IDirectDrawSurface_QueryInterface(ps, &IID_IDirectDrawGammaControl, (void **)&gc);
    logf_("QueryInterface(IDirectDrawGammaControl) %08lx\r\n", hr);
    check(hr == DD_OK, "IDirectDrawGammaControl");
    if (hr == DD_OK) {
        hr = IDirectDrawGammaControl_GetGammaRamp(gc, 0, &o);
        logf_("GetGammaRamp %08lx\r\n", hr);
        check(hr == DD_OK, "GetGammaRamp");
        for (i = 0; i < 256; i++) {
            n.red[i] = (WORD)(i * 257 / 2);
            n.green[i] = (WORD)(i * 257);
            n.blue[i] = (WORD)(i * 257 * 3 / 4);
        }
        hr = IDirectDrawGammaControl_SetGammaRamp(gc, 0, &n);
        logf_("SetGammaRamp %08lx\r\n", hr);
        check(hr == DD_OK, "SetGammaRamp");
        hr = IDirectDrawGammaControl_GetGammaRamp(gc, 0, &b);
        for (c = 1, i = 0; i < 256; i++)
            if ((b.red[i] >> 8) != (n.red[i] >> 8) || (b.green[i] >> 8) != (n.green[i] >> 8) ||
                (b.blue[i] >> 8) != (n.blue[i] >> 8)) c = 0;
        check(hr == DD_OK && c, "DirectDraw ramp read back");
        {   /* the GDI view of the same ramp */
            HDC dc = GetDC(NULL);
            BOOL r = GetDeviceGammaRamp(dc, back);
            logf_("GDI sees at 128: %u %u %u\r\n", back[0][128], back[1][128], back[2][128]);
            check(r && (back[0][128] >> 8) == (n.red[128] >> 8) && (back[1][128] >> 8) == (n.green[128] >> 8),
                  "GDI sees the DirectDraw ramp");
            ReleaseDC(NULL, dc);
        }
        pump(4000);
        IDirectDrawGammaControl_SetGammaRamp(gc, 0, &o);
        IDirectDrawGammaControl_Release(gc);
    }
    IDirectDrawSurface_Release(ps);
out:
    IDirectDraw_RestoreDisplayMode(dd);
    IDirectDraw_SetCooperativeLevel(dd, hwnd, DDSCL_NORMAL);
    IDirectDraw_Release(dd);
}

void WINAPI start(void)
{
    char path[MAX_PATH], *e;
    WNDCLASS wc;
    HWND hwnd;
    DWORD secs = 8;
    HINSTANCE hi = GetModuleHandle(NULL);
    const char *cmd = GetCommandLine();

    if (*cmd == '"') { cmd++; while (*cmd && *cmd != '"') cmd++; if (*cmd) cmd++; }
    else while (*cmd && *cmd != ' ') cmd++;
    while (*cmd == ' ') cmd++;
    if (*cmd >= '0' && *cmd <= '9') { secs = 0; while (*cmd >= '0' && *cmd <= '9') secs = secs * 10 + (DWORD)(*cmd++ - '0'); }
    GetModuleFileName(NULL, path, sizeof(path));
    for (e = path + lstrlen(path); e > path && e[-1] != '\\'; e--) ;
    lstrcpy(e, "GAMTEST.TXT");
    lg = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    logf_("GAMTEST - gamma ramp test\r\n");

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wp; wc.hInstance = hi; wc.lpszClassName = "GAMTEST";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);
    hwnd = CreateWindow("GAMTEST", "GAMTEST", WS_POPUP | WS_VISIBLE, 0, 0,
                        GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), NULL, NULL, hi, NULL);
    pump(500);
    test_gdi(secs);
    test_dd(hwnd);
    DestroyWindow(hwnd);
    logf_("checks: %d failed\r\n", fails);
    CloseHandle(lg);
    ExitProcess(fails);
}
