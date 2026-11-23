/*
 * SCROLLB - scrolling benchmark for WINVBE (Win32: Windows 95 and NT 3.51+)
 *
 * What a text editor or Explorer does when it scrolls: ScrollWindowEx moves
 * the window contents with a screen-to-screen copy, then only the new line
 * is painted. That copy reads video memory, which is uncached and slow on
 * real graphics cards unless the display driver keeps a shadow copy of the
 * screen in system memory. A program that draws its own bitmap and copies
 * it to the screen (memory-to-screen) only writes video memory.
 *
 * Writes SCROLLB.TXT next to the program and shows the results.
 */
#include <windows.h>

#define W 800
#define H 560
#define N 300

static HWND wnd;
static int line;

static void paint_lines(HDC dc, int y0, int y1)
{
    char s[96];
    int y;
    for (y = y0 - (y0 % 16); y < y1; y += 16) {
        int n = wsprintfA(s, "Line %5d  The quick brown fox jumps over the lazy dog. 0123456789 ABCDEFGHIJ", line + y / 16);
        TextOutA(dc, 4, y, s, n);
    }
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint_lines(dc, ps.rcPaint.top, ps.rcPaint.bottom);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    char out[600], path[MAX_PATH], *p;
    DWORD t0, t_scroll, t_s2s, t_m2s, wr;
    HDC dc, mdc;
    HBITMAP bm;
    HANDLE f;
    RECT rc;
    int i;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = "SCROLLB";
    RegisterClassA(&wc);
    rc.left = 0; rc.top = 0; rc.right = W; rc.bottom = H;
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    wnd = CreateWindowA("SCROLLB", "SCROLLB - scrolling benchmark", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                        0, 0, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, hi, NULL);
    UpdateWindow(wnd);
    Sleep(500);

    /* 1: native scrolling, as Notepad, WordPad and Explorer do it */
    t0 = GetTickCount();
    for (i = 0; i < N; i++) {
        line++;
        ScrollWindowEx(wnd, 0, -16, NULL, NULL, NULL, NULL, SW_INVALIDATE);
        UpdateWindow(wnd);
    }
    GdiFlush();
    t_scroll = GetTickCount() - t0;

    /* 2: screen-to-screen copy of the whole client area */
    dc = GetDC(wnd);
    t0 = GetTickCount();
    for (i = 0; i < N; i++)
        BitBlt(dc, 0, 0, W, H - 16, dc, 0, 16, SRCCOPY);
    GdiFlush();
    t_s2s = GetTickCount() - t0;

    /* 3: memory-to-screen copy (a program with its own frame buffer) */
    mdc = CreateCompatibleDC(dc);
    bm = CreateCompatibleBitmap(dc, W, H);
    SelectObject(mdc, bm);
    PatBlt(mdc, 0, 0, W, H, WHITENESS);
    paint_lines(mdc, 0, H);
    t0 = GetTickCount();
    for (i = 0; i < N; i++)
        BitBlt(dc, 0, 0, W, H, mdc, 0, 0, SRCCOPY);
    GdiFlush();
    t_m2s = GetTickCount() - t0;
    DeleteDC(mdc);
    DeleteObject(bm);
    ReleaseDC(wnd, dc);

    wsprintfA(out, "SCROLLB: %d operations on %dx%d (%d bpp screen)\r\n"
              "ScrollWindowEx (native scrolling): %lu ms\r\n"
              "Screen-to-screen BitBlt:           %lu ms\r\n"
              "Memory-to-screen BitBlt:           %lu ms\r\n",
              N, W, H, GetDeviceCaps(GetDC(NULL), BITSPIXEL),
              t_scroll, t_s2s, t_m2s);
    GetModuleFileNameA(NULL, path, sizeof(path));
    p = path + lstrlenA(path);
    while (p > path && p[-1] != '\\') p--;
    lstrcpyA(p, "SCROLLB.TXT");
    f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        lstrcpyA(path, "C:\\SCROLLB.TXT");
        f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    }
    if (f != INVALID_HANDLE_VALUE) { WriteFile(f, out, lstrlenA(out), &wr, NULL); CloseHandle(f); }
    if (!cmd || !strstr(cmd, "/q"))
        MessageBoxA(wnd, out, "SCROLLB", MB_OK);
    return 0;
}
