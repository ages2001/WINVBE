/* GDITEST - exercises display driver primitives (Windows 3.0 app) */
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <conio.h>

static HANDLE hInst;
static int page = 0;

static void lines_page(HDC dc)
{
    int i, y = 10;
    static int styles[] = { PS_SOLID, PS_DASH, PS_DOT, PS_DASHDOT, PS_DASHDOTDOT };
    for (i = 0; i < 5; i++) {
        HPEN p = CreatePen(styles[i], 1, RGB(0, 0, 128));
        HPEN o = SelectObject(dc, p);
        MoveTo(dc, 10, y); LineTo(dc, 200, y);
        MoveTo(dc, 10 + i * 20, 70); LineTo(dc, 30 + i * 20 + 40, 150);
        SelectObject(dc, o); DeleteObject(p);
        y += 10;
    }
    SetBkMode(dc, OPAQUE); SetBkColor(dc, RGB(255, 255, 0));
    {
        HPEN p = CreatePen(PS_DASH, 1, RGB(255, 0, 0)), o = SelectObject(dc, p);
        MoveTo(dc, 10, 62); LineTo(dc, 200, 62);
        SelectObject(dc, o); DeleteObject(p);
    }
    SetBkMode(dc, TRANSPARENT);
    for (i = 1; i <= 7; i += 2) {
        HPEN p = CreatePen(PS_SOLID, i, RGB(0, 128, 0)), o = SelectObject(dc, p);
        MoveTo(dc, 220, 10 + i * 8); LineTo(dc, 400, 30 + i * 12);
        SelectObject(dc, o); DeleteObject(p);
    }
    /* star fan */
    for (i = 0; i < 360; i += 15) {
        static int cx = 500, cy = 80;
        int dx = (i < 90 || i > 270) ? 70 : -70, dy = i < 180 ? 70 : -70;
        int k = i % 90;
        MoveTo(dc, cx, cy);
        if ((i / 90) % 2 == 0) LineTo(dc, cx + dx * (90 - k) / 90, cy + dy * k / 90);
        else LineTo(dc, cx + dx * k / 90, cy + dy * (90 - k) / 90);
    }
    /* XOR lines */
    SetROP2(dc, R2_XORPEN);
    {
        HPEN p = CreatePen(PS_SOLID, 1, RGB(255, 255, 255)), o = SelectObject(dc, p);
        for (i = 0; i < 20; i++) { MoveTo(dc, 600, 10 + i * 4); LineTo(dc, 780, 10 + i * 4 + 20); }
        SelectObject(dc, o); DeleteObject(p);
    }
    SetROP2(dc, R2_COPYPEN);
}

static void shapes_page(HDC dc)
{
    int i;
    static COLORREF cols[] = { RGB(255,0,0), RGB(0,255,0), RGB(0,0,255), RGB(255,255,0), RGB(0,255,255), RGB(255,0,255),
                               RGB(128,64,0), RGB(200,200,200) };
    for (i = 0; i < 8; i++) {
        HBRUSH b = CreateSolidBrush(cols[i]), o = SelectObject(dc, b);
        switch (i % 4) {
        case 0: Rectangle(dc, 10 + i * 95, 170, 95 + i * 95, 230); break;
        case 1: Ellipse(dc, 10 + i * 95, 170, 95 + i * 95, 230); break;
        case 2: RoundRect(dc, 10 + i * 95, 170, 95 + i * 95, 230, 20, 20); break;
        case 3: Pie(dc, 10 + i * 95, 170, 95 + i * 95, 230, 95 + i * 95, 170, 10 + i * 95, 170); break;
        }
        SelectObject(dc, o); DeleteObject(b);
    }
    for (i = 0; i < 6; i++) {
        HBRUSH b = CreateHatchBrush(i, RGB(0, 0, 128)), o = SelectObject(dc, b);
        SetBkColor(dc, RGB(255, 255, 255));
        SetBkMode(dc, i & 1 ? TRANSPARENT : OPAQUE);
        Rectangle(dc, 10 + i * 60, 240, 65 + i * 60, 290);
        SelectObject(dc, o); DeleteObject(b);
    }
    {
        static WORD pat[8] = { 0xAA, 0x55, 0xAA, 0x55, 0xF0, 0xF0, 0x0F, 0x0F };
        HBITMAP bm = CreateBitmap(8, 8, 1, 1, pat);
        HBRUSH b = CreatePatternBrush(bm), o = SelectObject(dc, b);
        SetTextColor(dc, RGB(255, 0, 0)); SetBkColor(dc, RGB(0, 0, 255));
        Rectangle(dc, 380, 240, 470, 290);
        SelectObject(dc, o); DeleteObject(b); DeleteObject(bm);
        SetTextColor(dc, 0); SetBkColor(dc, RGB(255, 255, 255));
    }
    {
        /* color pattern brush */
        HDC m = CreateCompatibleDC(dc);
        HBITMAP cb = CreateCompatibleBitmap(dc, 8, 8), ob = SelectObject(m, cb);
        HBRUSH b, o;
        int k;
        for (k = 0; k < 8; k++) {
            HBRUSH t = CreateSolidBrush(k & 1 ? RGB(255, 0, 0) : RGB(0, 128, 0));
            RECT r; r.left = k; r.top = 0; r.right = k + 1; r.bottom = 8 - k;
            FillRect(m, &r, t); DeleteObject(t);
            t = CreateSolidBrush(RGB(255, 255, 0));
            r.top = 8 - k; r.bottom = 8; FillRect(m, &r, t); DeleteObject(t);
        }
        SelectObject(m, ob); DeleteDC(m);
        b = CreatePatternBrush(cb); o = SelectObject(dc, b);
        Rectangle(dc, 620, 240, 700, 290);
        SelectObject(dc, o); DeleteObject(b); DeleteObject(cb);
    }
    {
        POINT pts[5];
        HBRUSH b = CreateSolidBrush(RGB(0, 128, 128)), o = SelectObject(dc, b);
        pts[0].x = 520; pts[0].y = 240; pts[1].x = 600; pts[1].y = 290; pts[2].x = 490; pts[2].y = 260;
        pts[3].x = 610; pts[3].y = 255; pts[4].x = 500; pts[4].y = 295;
        SetPolyFillMode(dc, ALTERNATE);
        Polygon(dc, pts, 5);
        SelectObject(dc, o); DeleteObject(b);
    }
    /* gray dithers */
    for (i = 0; i < 16; i++) {
        HBRUSH b = CreateSolidBrush(RGB(i * 17, i * 17, i * 17));
        RECT r; r.left = 10 + i * 30; r.top = 300; r.right = r.left + 30; r.bottom = 330;
        FillRect(dc, &r, b); DeleteObject(b);
        b = CreateSolidBrush(RGB(i * 17, 0, 255 - i * 17));
        r.top = 330; r.bottom = 360;
        FillRect(dc, &r, b); DeleteObject(b);
    }
    /* pixels + floodfill */
    for (i = 0; i < 100; i++) SetPixel(dc, 520 + i, 320 + (i % 10), RGB(255, 0, 0));
    {
        HBRUSH b = CreateSolidBrush(RGB(255, 128, 0)), o = SelectObject(dc, b);
        HPEN p = GetStockObject(BLACK_PEN);
        SelectObject(dc, p);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, 640, 300, 720, 360);
        SelectObject(dc, b);
        FloodFill(dc, 680, 330, RGB(0, 0, 0));
        SelectObject(dc, o); DeleteObject(b);
    }
}

static void text_page(HDC dc)
{
    static char *faces[] = { "Helv", "Tms Rmn", "Courier", "Modern", "Script", "Roman", "Symbol" };
    int i, y = 10;
    char buf[80];
    for (i = 0; i < 7; i++) {
        HFONT f = CreateFont(i < 3 ? 18 : 20, 0, 0, 0, i == 1 ? FW_BOLD : FW_NORMAL, i == 2, i == 0, 0, ANSI_CHARSET,
                             0, 0, 0, 0, faces[i]);
        HFONT o = SelectObject(dc, f);
        sprintf(buf, "%s: The quick brown fox jumps 0123456789", faces[i]);
        SetTextColor(dc, RGB(0, 0, i * 30));
        TextOut(dc, 10, y, buf, strlen(buf));
        y += 26;
        SelectObject(dc, o); DeleteObject(f);
    }
    SetBkMode(dc, OPAQUE); SetBkColor(dc, RGB(255, 255, 0)); SetTextColor(dc, RGB(255, 0, 0));
    TextOut(dc, 10, y, "Opaque system font text", 23); y += 20;
    SetBkMode(dc, TRANSPARENT);
    {
        RECT r; r.left = 10; r.top = y; r.right = 120; r.bottom = y + 10;
        ExtTextOut(dc, 10, y, ETO_CLIPPED | ETO_OPAQUE, &r, "Clipped ExtTextOut long string", 30, NULL);
        y += 20;
    }
    SetTextColor(dc, 0);
    SetTextCharacterExtra(dc, 3);
    TextOut(dc, 10, y, "Character extra 3", 17); y += 20;
    SetTextCharacterExtra(dc, 0);
    SetTextJustification(dc, 60, 3);
    TextOut(dc, 10, y, "Justified a b c d", 17); y += 20;
    SetTextJustification(dc, 0, 0);
    {
        HFONT f = CreateFont(48, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, 0, 0, "Tms Rmn");
        HFONT o = SelectObject(dc, f);
        TextOut(dc, 10, y, "Big 48px", 8);
        SelectObject(dc, o); DeleteObject(f);
    }
}

static void blt_page(HDC dc)
{
    HDC m = CreateCompatibleDC(dc);
    HBITMAP bm = CreateCompatibleBitmap(dc, 64, 64), ob;
    HBITMAP mono;
    static DWORD rops[] = { SRCCOPY, SRCPAINT, SRCAND, SRCINVERT, SRCERASE, NOTSRCCOPY, NOTSRCERASE, MERGECOPY,
                            MERGEPAINT, PATCOPY, PATPAINT, PATINVERT, DSTINVERT, BLACKNESS, WHITENESS, 0x00B8074AL };
    int i;
    HBRUSH hb;
    ob = SelectObject(m, bm);
    PatBlt(m, 0, 0, 64, 64, WHITENESS);
    for (i = 0; i < 8; i++) {
        HBRUSH b = CreateSolidBrush(RGB(i * 32, 255 - i * 32, (i & 1) * 255));
        RECT r; r.left = i * 8; r.top = 0; r.right = r.left + 8; r.bottom = 64;
        FillRect(m, &r, b); DeleteObject(b);
    }
    Ellipse(m, 8, 8, 56, 56);
    TextOut(m, 12, 24, "Mem", 3);
    /* background stripes on screen */
    for (i = 0; i < 16; i++) {
        HBRUSH b = CreateSolidBrush(i & 1 ? RGB(0, 0, 160) : RGB(255, 255, 255));
        RECT r; r.left = 10 + (i % 8) * 90; r.top = 10 + (i / 8) * 100; r.right = r.left + 80; r.bottom = r.top + 80;
        FillRect(dc, &r, b); DeleteObject(b);
        b = CreateSolidBrush(RGB(255, 0, 0));
        r.right = r.left + 40; r.bottom = r.top + 40; FillRect(dc, &r, b); DeleteObject(b);
    }
    hb = CreateHatchBrush(HS_DIAGCROSS, RGB(0, 160, 0));
    SelectObject(dc, hb);
    for (i = 0; i < 16; i++)
        BitBlt(dc, 18 + (i % 8) * 90, 18 + (i / 8) * 100, 64, 64, m, 0, 0, rops[i]);
    SelectObject(dc, GetStockObject(WHITE_BRUSH));
    DeleteObject(hb);
    /* mono -> color */
    mono = CreateBitmap(64, 64, 1, 1, NULL);
    SelectObject(m, mono);
    PatBlt(m, 0, 0, 64, 64, WHITENESS);
    Ellipse(m, 0, 0, 64, 64);
    TextOut(m, 8, 24, "Mono", 4);
    SetTextColor(dc, RGB(255, 0, 0)); SetBkColor(dc, RGB(0, 255, 0));
    BitBlt(dc, 10, 220, 64, 64, m, 0, 0, SRCCOPY);
    SetTextColor(dc, 0); SetBkColor(dc, RGB(255, 255, 255));
    /* color -> mono -> screen */
    {
        HDC m2 = CreateCompatibleDC(dc);
        HBITMAP mono2 = CreateBitmap(64, 64, 1, 1, NULL), o2 = SelectObject(m2, mono2);
        SelectObject(m, bm);
        SetBkColor(m, RGB(255, 255, 255));
        BitBlt(m2, 0, 0, 64, 64, m, 0, 0, SRCCOPY);
        BitBlt(dc, 90, 220, 64, 64, m2, 0, 0, SRCCOPY);
        SelectObject(m2, o2); DeleteObject(mono2); DeleteDC(m2);
    }
    /* stretch */
    SelectObject(m, bm);
    StretchBlt(dc, 170, 220, 128, 96, m, 0, 0, 64, 64, SRCCOPY);
    StretchBlt(dc, 310, 220, 32, 32, m, 0, 0, 64, 64, SRCCOPY);
    /* screen to screen overlapping scroll */
    BitBlt(dc, 360, 230, 200, 80, dc, 350, 220, SRCCOPY);
    SelectObject(m, ob);
    DeleteObject(bm); DeleteObject(mono); DeleteDC(m);
}

static void dib_page(HDC dc)
{
    struct { BITMAPINFOHEADER h; RGBQUAD c[256]; } bi;
    static BYTE bits[64 * 64 * 3 + 64];
    int x, y, i;
    HDC m;
    HBITMAP hb;
    /* 8bpp gradient */
    memset(&bi, 0, sizeof bi);
    bi.h.biSize = sizeof(BITMAPINFOHEADER); bi.h.biWidth = 64; bi.h.biHeight = 64; bi.h.biPlanes = 1;
    bi.h.biBitCount = 8;
    for (i = 0; i < 256; i++) { bi.c[i].rgbRed = i; bi.c[i].rgbGreen = 255 - i; bi.c[i].rgbBlue = (i * 4) & 255; }
    for (y = 0; y < 64; y++) for (x = 0; x < 64; x++) bits[y * 64 + x] = (BYTE)(x * 4);
    SetDIBitsToDevice(dc, 10, 10, 64, 64, 0, 0, 0, 64, bits, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
    /* 24bpp */
    bi.h.biBitCount = 24;
    for (y = 0; y < 64; y++) for (x = 0; x < 64; x++) {
        bits[(y * 64 + x) * 3 + 0] = (BYTE)(x * 4);
        bits[(y * 64 + x) * 3 + 1] = (BYTE)(y * 4);
        bits[(y * 64 + x) * 3 + 2] = (BYTE)(255 - x * 4);
    }
    SetDIBitsToDevice(dc, 90, 10, 64, 64, 0, 0, 0, 64, bits, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
    /* 4bpp */
    bi.h.biBitCount = 4;
    for (i = 0; i < 16; i++) { bi.c[i].rgbRed = (i & 1) ? 255 : 0; bi.c[i].rgbGreen = (i & 2) ? 255 : 0; bi.c[i].rgbBlue = (i & 4) ? 255 : (i & 8) ? 128 : 0; }
    for (y = 0; y < 64; y++) for (x = 0; x < 32; x++) bits[y * 32 + x] = (BYTE)(((x / 2) << 4) | ((x / 2 + y / 8) & 15));
    SetDIBitsToDevice(dc, 170, 10, 64, 64, 0, 0, 0, 64, bits, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
    /* 1bpp */
    bi.h.biBitCount = 1;
    bi.c[0].rgbRed = 0; bi.c[0].rgbGreen = 0; bi.c[0].rgbBlue = 128;
    bi.c[1].rgbRed = 255; bi.c[1].rgbGreen = 255; bi.c[1].rgbBlue = 0;
    for (y = 0; y < 64; y++) for (x = 0; x < 8; x++) bits[y * 8 + x] = (BYTE)((y & 8) ? 0xF0 : 0x0F);
    SetDIBitsToDevice(dc, 250, 10, 64, 64, 0, 0, 0, 64, bits, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
    /* CreateDIBitmap + StretchDIBits */
    bi.h.biBitCount = 24;
    for (y = 0; y < 64; y++) for (x = 0; x < 64; x++) {
        bits[(y * 64 + x) * 3 + 0] = (BYTE)(((x ^ y) & 8) ? 255 : 0);
        bits[(y * 64 + x) * 3 + 1] = (BYTE)(y * 4);
        bits[(y * 64 + x) * 3 + 2] = (BYTE)(x * 4);
    }
    hb = CreateDIBitmap(dc, &bi.h, CBM_INIT, bits, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
    m = CreateCompatibleDC(dc);
    SelectObject(m, hb);
    BitBlt(dc, 330, 10, 64, 64, m, 0, 0, SRCCOPY);
    DeleteDC(m);
    StretchDIBits(dc, 410, 10, 128, 128, 0, 0, 64, 64, bits, (BITMAPINFO *)&bi, DIB_RGB_COLORS, SRCCOPY);
    /* GetDIBits roundtrip into 8bpp then display */
    {
        struct { BITMAPINFOHEADER h; RGBQUAD c[256]; } b2;
        static BYTE bits2[64 * 64];
        memset(&b2, 0, sizeof b2);
        b2.h.biSize = sizeof(BITMAPINFOHEADER); b2.h.biWidth = 64; b2.h.biHeight = 64; b2.h.biPlanes = 1; b2.h.biBitCount = 8;
        GetDIBits(dc, hb, 0, 64, bits2, (BITMAPINFO *)&b2, DIB_RGB_COLORS);
        {
            char t[80]; int k;
            for (k = 0; k < 256; k += 8) {
                sprintf(t, "tab[%d]=%02X%02X%02X px=%02X\r\n", k, b2.c[k].rgbRed, b2.c[k].rgbGreen, b2.c[k].rgbBlue, bits2[k * 16]);
                { char *q = t; while (*q) outp(0xE9, *q++); }
            }
            for (k = 246; k < 256; k++) {
                sprintf(t, "tab[%d]=%02X%02X%02X\r\n", k, b2.c[k].rgbRed, b2.c[k].rgbGreen, b2.c[k].rgbBlue);
                { char *q = t; while (*q) outp(0xE9, *q++); }
            }
        }
        SetDIBitsToDevice(dc, 560, 10, 64, 64, 0, 0, 0, 64, bits2, (BITMAPINFO *)&b2, DIB_RGB_COLORS);
        b2.h.biBitCount = 24;
        GetDIBits(dc, hb, 0, 64, bits, (BITMAPINFO *)&b2, DIB_RGB_COLORS);
        SetDIBitsToDevice(dc, 640, 10, 64, 64, 0, 0, 0, 64, bits, (BITMAPINFO *)&b2, DIB_RGB_COLORS);
    }
    DeleteObject(hb);
    /* palette */
    {
        struct { WORD v, n; PALETTEENTRY e[64]; } lp;
        HPALETTE hp, op;
        lp.v = 0x300; lp.n = 64;
        for (i = 0; i < 64; i++) { lp.e[i].peRed = i * 4; lp.e[i].peGreen = 0; lp.e[i].peBlue = 255 - i * 4; lp.e[i].peFlags = 0; }
        hp = CreatePalette((LOGPALETTE *)&lp);
        op = SelectPalette(dc, hp, FALSE);
        RealizePalette(dc);
        for (i = 0; i < 64; i++) {
            HBRUSH b = CreateSolidBrush(PALETTEINDEX(i));
            RECT r; r.left = 10 + i * 10; r.top = 160; r.right = r.left + 10; r.bottom = 200;
            FillRect(dc, &r, b); DeleteObject(b);
        }
        SelectPalette(dc, op, FALSE);
        DeleteObject(hp);
    }
}

LRESULT FAR PASCAL WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        switch (page) {
        case 0: lines_page(dc); shapes_page(dc); break;
        case 1: text_page(dc); break;
        case 2: blt_page(dc); break;
        case 3: dib_page(dc); break;
        }
        EndPaint(w, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_SPACE) { page = (page + 1) % 4; InvalidateRect(w, NULL, TRUE); }
        if (wp == VK_ESCAPE) DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(w, msg, wp, lp);
}

int PASCAL WinMain(HANDLE hi, HANDLE hp, LPSTR cmd, int show)
{
    WNDCLASS wc;
    MSG msg;
    HWND w;
    hInst = hi;
    if (cmd && *cmd >= '0' && *cmd <= '3') page = *cmd - '0';
    if (!hp) {
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = GetStockObject(WHITE_BRUSH); wc.lpszClassName = "GDITEST";
        RegisterClass(&wc);
    }
    w = CreateWindow("GDITEST", "GDI Test", WS_POPUP | WS_VISIBLE, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                     GetSystemMetrics(SM_CYSCREEN), NULL, NULL, hi, NULL);
    ShowWindow(w, SW_SHOW);
    while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return msg.wParam;
}
