/*
 * W32GDI - Win32 GDI stress test for Windows 95/98/ME and NT: the drawing
 * that Win32 applications (Delphi VCL, common controls) do through the
 * display driver - DIB sections in every format, GetDIBits queries,
 * device dependent bitmaps, image lists, icons, pattern brushes, wide
 * pens, regions, text, scrolling.  Each step is written to W32GDI.TXT
 * before it runs, so after a crash the last line names it; calls that
 * must succeed are checked ("FAILED" lines, a count at the end), and DIBs
 * written with SetDIBits are read back and compared.
 *   W32GDI [rounds]
 */
#include <windows.h>
#include <commctrl.h>

static HANDLE lg;
static int step_no;
void *memset(void *d, int c, size_t n) { char *p = d; while (n--) *p++ = (char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { char *p = d; const char *q = s; while (n--) *p++ = *q++; return d; }

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

#define STEP(name) logf_("%d %s\r\n", ++step_no, name)
static int failures;
#define CHECK(what, v) do { if (!(v)) { failures++; logf_("  ** FAILED: %s\r\n", what); } } while (0)

static HWND wnd;
static int W = 400, H = 300;

typedef struct {
    BITMAPINFOHEADER h;
    DWORD c[256];
} BMI;

static void fill_bmi(BMI *b, int w, int h, int bpp, int bitfields)
{
    int i;
    memset(b, 0, sizeof(*b));
    b->h.biSize = sizeof(BITMAPINFOHEADER);
    b->h.biWidth = w;
    b->h.biHeight = h;
    b->h.biPlanes = 1;
    b->h.biBitCount = (WORD)bpp;
    b->h.biCompression = bitfields ? BI_BITFIELDS : BI_RGB;
    if (bpp <= 8)
        for (i = 0; i < (1 << bpp); i++)
            b->c[i] = RGB((i * 37) & 255, (i * 91) & 255, (i * 53) & 255);
    if (bitfields) {
        if (bpp == 16) { b->c[0] = 0xF800; b->c[1] = 0x07E0; b->c[2] = 0x001F; }
        else { b->c[0] = 0xFF0000; b->c[1] = 0xFF00; b->c[2] = 0xFF; }
    }
}

static void draw_stuff(HDC dc, int x, int y, int seed)
{
    HPEN p = CreatePen(PS_SOLID, 1 + (seed & 3), RGB(seed * 40 & 255, 0, 200));
    HBRUSH b = CreateSolidBrush(RGB(0, seed * 70 & 255, 120));
    HGDIOBJ op = SelectObject(dc, p), ob = SelectObject(dc, b);
    Rectangle(dc, x, y, x + 60, y + 40);
    Ellipse(dc, x + 10, y + 10, x + 70, y + 50);
    MoveToEx(dc, x, y, NULL);
    LineTo(dc, x + 80, y + 60);
    TextOutA(dc, x + 5, y + 5, "W32GDI", 6);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(p);
    DeleteObject(b);
}

static void dib_sections(HDC sdc, int round)
{
    static const int fmts[][2] = { {1, 0}, {4, 0}, {8, 0}, {16, 0}, {16, 1}, {24, 0}, {32, 0}, {32, 1} };
    int f, td;
    char nm[80];
    for (f = 0; f < 8; f++)
        for (td = 0; td < 2; td++) {
            BMI bi;
            void *bits = NULL;
            HBITMAP hb, ob;
            HDC mdc;
            int w = 97 + f, h = 61;
            fill_bmi(&bi, w, td ? -h : h, fmts[f][0], fmts[f][1]);
            wsprintfA(nm, "CreateDIBSection %d bpp%s%s", fmts[f][0], fmts[f][1] ? " bitfields" : "",
                      td ? " top-down" : "");
            STEP(nm);
            hb = CreateDIBSection(sdc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
            if (!hb) { logf_("  failed %lu\r\n", GetLastError()); continue; }
            if (bits) memset(bits, 0x5A + f, (((w * fmts[f][0] + 31) / 32) * 4) * h);
            mdc = CreateCompatibleDC(sdc);
            ob = SelectObject(mdc, hb);
            STEP("  draw into DIB section");
            draw_stuff(mdc, 2, 2, round + f);
            GdiFlush();
            STEP("  BitBlt DIB section -> screen");
            BitBlt(sdc, 10 + f * 30, 10 + td * 70, w, h, mdc, 0, 0, SRCCOPY);
            STEP("  BitBlt screen -> DIB section");
            BitBlt(mdc, 0, 0, w, h, sdc, 5, 5, SRCCOPY);
            STEP("  StretchBlt DIB section -> screen");
            StretchBlt(sdc, 20, 150, w * 2, h + 13, mdc, 0, 0, w, h, SRCCOPY);
            STEP("  StretchBlt screen -> DIB section");
            StretchBlt(mdc, 0, 0, w, h, sdc, 0, 0, w * 2 + 3, h * 2, SRCCOPY);
            STEP("  MaskBlt-like: SRCAND/SRCPAINT DIB section -> screen");
            BitBlt(sdc, 200, 10, w, h, mdc, 0, 0, SRCAND);
            BitBlt(sdc, 200, 10, w, h, mdc, 0, 0, SRCPAINT);
            STEP("  GetDIBits from DIB section");
            {
                BMI q;
                static BYTE buf[200 * 200 * 4];
                fill_bmi(&q, w, h, 24, 0);
                GetDIBits(mdc, hb, 0, h, buf, (BITMAPINFO *)&q, DIB_RGB_COLORS);
            }
            STEP("  pattern brush from DIB section");
            {
                HBRUSH br = CreatePatternBrush(hb);
                if (br) {
                    HGDIOBJ o = SelectObject(sdc, br);
                    PatBlt(sdc, 250, 100, 120, 80, PATCOPY);
                    SelectObject(sdc, o);
                    DeleteObject(br);
                }
            }
            SelectObject(mdc, ob);
            DeleteDC(mdc);
            DeleteObject(hb);
        }
}

/* SetDIBits of a DIB, GetDIBits back in the same format: equal (in the
   bits the screen keeps); DIBs in the screen's own format are passed to
   the driver unconverted (Windows 9x) */
static void roundtrip(HDC sdc)
{
    static const int fmt[][2] = { { 32, 0 }, { 32, 1 }, { 24, 0 }, { 16, 0 }, { 8, 0 }, { 4, 0 }, { 1, 0 } };
    static const int wds[] = { 7, 160, 2049 };
    static BYTE a1[2049 * 4 * 5], a2[2049 * 4 * 5];
    int sb = GetDeviceCaps(sdc, BITSPIXEL) * GetDeviceCaps(sdc, PLANES);
    unsigned f, wi;
    for (f = 0; f < sizeof(fmt) / sizeof(fmt[0]); f++)
        for (wi = 0; wi < sizeof(wds) / sizeof(wds[0]); wi++) {
            int bp = fmt[f][0], ww = wds[wi], hh = 5, x, y, bad = 0, stride = ((ww * bp + 31) / 32) * 4, r1, r2;
            BYTE keep = sb == 16 ? 0xF8 : 0xFF;
            HBITMAP hb = CreateCompatibleBitmap(sdc, ww, hh);
            BMI q;
            char nm[80];
            wsprintfA(nm, "SetDIBits/GetDIBits round trip %d bpp%s, %d wide", bp, fmt[f][1] ? " bitfields" : "", ww);
            STEP(nm);
            fill_bmi(&q, ww, hh, bp, fmt[f][1]);
            if (bp <= 8) {      /* black and white only: exact on every screen */
                for (x = 0; x < (1 << bp); x++) q.c[x] = (x & 1) ? 0xFFFFFF : 0;
            }
            for (y = 0; y < hh; y++)
                for (x = 0; x < stride; x++) {
                    BYTE v = (BYTE)(x * 7 + y * 13);
                    if (bp <= 8) v = (BYTE)(bp == 1 ? v : bp == 4 ? (v & 0x11) : (v & 1));
                    else if (bp == 16) v &= (x & 1) ? 0x7F : 0xFF;
                    else if (bp == 32 && (x & 3) == 3) v = 0;
                    a1[y * stride + x] = v;
                }
            r1 = SetDIBits(sdc, hb, 0, hh, a1, (BITMAPINFO *)&q, DIB_RGB_COLORS);
            memset(a2, 0x5A, sizeof(a2));
            fill_bmi(&q, ww, hh, bp, fmt[f][1]);
            if (bp <= 8) for (x = 0; x < (1 << bp); x++) q.c[x] = (x & 1) ? 0xFFFFFF : 0;
            r2 = GetDIBits(sdc, hb, 0, hh, a2, (BITMAPINFO *)&q, DIB_RGB_COLORS);
            CHECK("SetDIBits", r1 == hh);
            CHECK("GetDIBits", r2 == hh);
            if (sb > 8 && bp > 8 && bp != 16) {     /* 24/32 bpp: every bit the screen keeps */
                for (y = 0; y < hh; y++)
                    for (x = 0; x < ww * bp / 8; x++) {
                        if (bp == 32 && (x & 3) == 3) continue;
                        if ((a1[y * stride + x] ^ a2[y * stride + x]) & keep) bad++;
                    }
                if (bad) logf_("  %d bytes differ\r\n", bad);
                CHECK("round trip equal", !bad);
            }
            DeleteObject(hb);
        }
}

static void ddb_tests(HDC sdc)
{
    HBITMAP hb = CreateCompatibleBitmap(sdc, 160, 120), ob;
    HDC mdc = CreateCompatibleDC(sdc);
    static const int bpps[] = { 0, 1, 4, 8, 16, 24, 32 };
    static BYTE buf[160 * 120 * 4 + 4096];
    int i;
    STEP("DDB: CreateCompatibleBitmap + draw");
    ob = SelectObject(mdc, hb);
    PatBlt(mdc, 0, 0, 160, 120, WHITENESS);
    draw_stuff(mdc, 10, 10, 3);
    BitBlt(sdc, 30, 200, 160, 120, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, ob);
    for (i = 0; i < 7; i++) {
        int bf;
        for (bf = 0; bf < 2; bf++) {
            BMI q;
            char nm[80];
            if (bf && bpps[i] != 16 && bpps[i] != 32) continue;
            fill_bmi(&q, 160, 120, bpps[i], bf);
            wsprintfA(nm, "GetDIBits DDB query (bits NULL) %d bpp%s", bpps[i], bf ? " bitfields" : "");
            STEP(nm);
            GetDIBits(mdc, hb, 0, 120, NULL, (BITMAPINFO *)&q, DIB_RGB_COLORS);
            logf_("  -> %ldx%ld %d bpp comp %lu size %lu\r\n", q.h.biWidth, q.h.biHeight, q.h.biBitCount,
                  q.h.biCompression, q.h.biSizeImage);
            if (!bpps[i]) continue;
            fill_bmi(&q, 160, 120, bpps[i], bf);
            wsprintfA(nm, "GetDIBits DDB %d bpp%s", bpps[i], bf ? " bitfields" : "");
            STEP(nm);
            GetDIBits(mdc, hb, 0, 120, buf, (BITMAPINFO *)&q, DIB_RGB_COLORS);
            STEP("  top-down");
            fill_bmi(&q, 160, -120, bpps[i], bf);
            GetDIBits(mdc, hb, 0, 120, buf, (BITMAPINFO *)&q, DIB_RGB_COLORS);
            STEP("  SetDIBits back");
            CHECK("SetDIBits", SetDIBits(mdc, hb, 0, 120, buf, (BITMAPINFO *)&q, DIB_RGB_COLORS) == 120);
            STEP("  SetDIBitsToDevice / StretchDIBits");
            CHECK("SetDIBitsToDevice", SetDIBitsToDevice(sdc, 200, 200, 160, 120, 0, 0, 0, 120, buf, (BITMAPINFO *)&q, DIB_RGB_COLORS));
            CHECK("StretchDIBits", StretchDIBits(sdc, 0, 0, 250, 90, 0, 0, 160, 120, buf, (BITMAPINFO *)&q, DIB_RGB_COLORS, SRCCOPY));
        }
    }
    STEP("GetDIBits DDB in bands");
    {
        BMI q;
        int y;
        fill_bmi(&q, 160, 120, 24, 0);
        for (y = 0; y < 120; y += 17)
            GetDIBits(mdc, hb, y, 17, buf, (BITMAPINFO *)&q, DIB_RGB_COLORS);
    }
    STEP("CreateDIBitmap from 8 bpp");
    {
        BMI q;
        HBITMAP h2;
        fill_bmi(&q, 160, 120, 8, 0);
        memset(buf, 7, 160 * 120);
        h2 = CreateDIBitmap(sdc, &q.h, CBM_INIT, buf, (BITMAPINFO *)&q, DIB_RGB_COLORS);
        if (h2) DeleteObject(h2);
    }
    STEP("DIB pattern brush");
    {
        struct { BITMAPINFOHEADER h; DWORD c[2]; BYTE b[8 * 4]; } pb;
        HBRUSH br;
        memset(&pb, 0, sizeof(pb));
        pb.h.biSize = sizeof(pb.h); pb.h.biWidth = 8; pb.h.biHeight = 8; pb.h.biPlanes = 1; pb.h.biBitCount = 1;
        pb.c[0] = 0; pb.c[1] = 0xFFFFFF;
        for (i = 0; i < 8; i++) pb.b[i * 4] = (BYTE)(0x55 << (i & 1));
        br = CreateDIBPatternBrushPt(&pb, DIB_RGB_COLORS);
        if (br) {
            HGDIOBJ o = SelectObject(sdc, br);
            PatBlt(sdc, 300, 250, 90, 40, PATCOPY);
            SelectObject(sdc, o);
            DeleteObject(br);
        }
    }
    DeleteDC(mdc);
    DeleteObject(hb);
}

static void imagelists(HDC sdc)
{
    static const UINT flags[] = { ILC_COLOR, ILC_COLOR4, ILC_COLOR8, ILC_COLOR16, ILC_COLOR24, ILC_COLOR32, ILC_COLORDDB };
    int i, m;
    for (i = 0; i < 7; i++)
        for (m = 0; m < 2; m++) {
            HIMAGELIST il;
            HBITMAP hb;
            HDC mdc;
            HGDIOBJ ob;
            char nm[64];
            int k;
            wsprintfA(nm, "ImageList flags %x%s", flags[i], m ? " | MASK" : "");
            STEP(nm);
            il = ImageList_Create(16, 16, flags[i] | (m ? ILC_MASK : 0), 4, 4);
            if (!il) { logf_("  failed\r\n"); continue; }
            hb = CreateCompatibleBitmap(sdc, 64, 16);
            mdc = CreateCompatibleDC(sdc);
            ob = SelectObject(mdc, hb);
            PatBlt(mdc, 0, 0, 64, 16, WHITENESS);
            draw_stuff(mdc, 0, 0, i);
            SelectObject(mdc, ob);
            STEP("  ImageList_AddMasked");
            ImageList_AddMasked(il, hb, RGB(255, 255, 255));
            STEP("  ImageList_AddIcon");
            ImageList_AddIcon(il, LoadIcon(NULL, IDI_QUESTION));
            for (k = 0; k < ImageList_GetImageCount(il); k++) {
                STEP("  ImageList_Draw normal / transparent / blend");
                ImageList_Draw(il, k, sdc, 10 + k * 20, 260, ILD_NORMAL);
                ImageList_Draw(il, k, sdc, 10 + k * 20, 280, ILD_TRANSPARENT);
                ImageList_Draw(il, k, sdc, 100 + k * 20, 260, ILD_BLEND50);
                ImageList_Draw(il, k, sdc, 100 + k * 20, 280, ILD_SELECTED | ILD_FOCUS);
            }
            STEP("  ImageList_GetIcon / DrawIconEx");
            {
                HICON ic = ImageList_GetIcon(il, 0, ILD_NORMAL);
                if (ic) { DrawIconEx(sdc, 300, 10, ic, 32, 32, 0, NULL, DI_NORMAL); DestroyIcon(ic); }
            }
            DeleteDC(mdc);
            DeleteObject(hb);
            ImageList_Destroy(il);
        }
}

static void misc(HDC sdc, int round)
{
    STEP("icons: DrawIcon / DrawIconEx stretched");
    DrawIcon(sdc, 10, 10, LoadIcon(NULL, IDI_EXCLAMATION));
    DrawIconEx(sdc, 50, 10, LoadIcon(NULL, IDI_HAND), 48, 48, 0, NULL, DI_NORMAL);
    STEP("CreateIconIndirect with a 24 bpp colour DIB section");
    {
        BMI bi;
        void *bits;
        ICONINFO ii;
        HICON ic;
        fill_bmi(&bi, 32, 32, 24, 0);
        ii.fIcon = TRUE; ii.xHotspot = ii.yHotspot = 0;
        ii.hbmColor = CreateDIBSection(sdc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
        ii.hbmMask = CreateBitmap(32, 32, 1, 1, NULL);
        if (ii.hbmColor && bits) memset(bits, 0x80, 32 * 32 * 3);
        ic = CreateIconIndirect(&ii);
        if (ic) { DrawIconEx(sdc, 120, 10, ic, 0, 0, 0, NULL, DI_NORMAL); DestroyIcon(ic); }
        DeleteObject(ii.hbmColor);
        DeleteObject(ii.hbmMask);
    }
    STEP("wide and styled pens, curves");
    {
        LOGBRUSH lb;
        HPEN p;
        HGDIOBJ o;
        POINT pt[7] = { {10, 100}, {60, 40}, {110, 160}, {160, 90}, {210, 30}, {260, 150}, {310, 100} };
        lb.lbStyle = BS_SOLID; lb.lbColor = RGB(200, 30, 30); lb.lbHatch = 0;
        p = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, 9, &lb, 0, NULL);
        o = SelectObject(sdc, p);
        PolyBezier(sdc, pt, 7);
        Polyline(sdc, pt, 7);
        SelectObject(sdc, o);
        DeleteObject(p);
        p = CreatePen(PS_DASHDOT, 1, RGB(0, 0, 255));
        o = SelectObject(sdc, p);
        Arc(sdc, 20, 20, 200, 180, 0, 0, 300, 300);
        Pie(sdc, 220, 20, 330, 130, 0, 0, 400, 0);
        RoundRect(sdc, 30, 180, 200, 280, 30, 30);
        SelectObject(sdc, o);
        DeleteObject(p);
    }
    STEP("regions");
    {
        HRGN r1 = CreateEllipticRgn(50, 50, 250, 200), r2 = CreateRectRgn(100, 100, 350, 250);
        HBRUSH b = CreateHatchBrush(HS_DIAGCROSS, RGB(0, 120, 0));
        CombineRgn(r1, r1, r2, RGN_XOR);
        FillRgn(sdc, r1, b);
        FrameRgn(sdc, r1, (HBRUSH)GetStockObject(BLACK_BRUSH), 2, 2);
        InvertRgn(sdc, r2);
        DeleteObject(r1);
        DeleteObject(r2);
        DeleteObject(b);
    }
    STEP("fonts: TrueType, rotated, opaque, clipped");
    {
        HFONT f = CreateFontA(-24 - (round & 7), 0, (round & 1) * 450, 0, FW_BOLD, TRUE, TRUE, FALSE, ANSI_CHARSET,
                              0, 0, 0, 0, "Arial");
        HGDIOBJ o = SelectObject(sdc, f);
        RECT rc = { 20, 120, 260, 160 };
        SetBkColor(sdc, RGB(255, 255, 0));
        ExtTextOutA(sdc, 25, 125, ETO_OPAQUE | ETO_CLIPPED, &rc, "Clipped TrueType text WxyQ", 26, NULL);
        DrawTextA(sdc, "DrawText &underline\nsecond line", -1, &rc, DT_LEFT | DT_WORDBREAK);
        SelectObject(sdc, o);
        DeleteObject(f);
    }
    STEP("DrawEdge / DrawFrameControl / DrawFocusRect");
    {
        RECT rc = { 280, 180, 380, 220 };
        DrawEdge(sdc, &rc, EDGE_RAISED, BF_RECT | BF_MIDDLE);
        rc.top += 50; rc.bottom += 50;
        DrawFrameControl(sdc, &rc, DFC_BUTTON, DFCS_BUTTONCHECK | DFCS_CHECKED);
        DrawFocusRect(sdc, &rc);
    }
    STEP("GetPixel / SetPixel");
    {
        int i;
        for (i = 0; i < 50; i++) SetPixel(sdc, 5 + i, 5, GetPixel(sdc, 5 + i, 6) ^ 0xFFFFFF);
    }
    STEP("ScrollDC / ScrollWindowEx");
    ScrollDC(sdc, 0, 7, NULL, NULL, NULL, NULL);
    ScrollWindowEx(wnd, 3, -5, NULL, NULL, NULL, NULL, SW_INVALIDATE);
    STEP("large DDB (screen size) + GetDIBits 32 bpp top-down");
    {
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        HDC s = GetDC(NULL), m = CreateCompatibleDC(s);
        HBITMAP hb = CreateCompatibleBitmap(s, sw, sh);
        HGDIOBJ o = SelectObject(m, hb);
        BMI q;
        DWORD *mem;
        BitBlt(m, 0, 0, sw, sh, s, 0, 0, SRCCOPY);
        SelectObject(m, o);
        fill_bmi(&q, sw, -sh, 32, 0);
        mem = (DWORD *)GlobalAlloc(GMEM_FIXED, (DWORD)sw * sh * 4);
        if (mem) {
            GetDIBits(m, hb, 0, sh, mem, (BITMAPINFO *)&q, DIB_RGB_COLORS);
            GlobalFree((HGLOBAL)mem);
        }
        DeleteDC(m);
        DeleteObject(hb);
        ReleaseDC(NULL, s);
    }
    STEP("LoadImage LR_CREATEDIBSECTION (system bitmap) + CopyImage");
    {
        HBITMAP b = (HBITMAP)LoadImageA(NULL, (LPCSTR)32754 /* OBM_CLOSE */, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
        if (b) {
            HBITMAP c = (HBITMAP)CopyImage(b, IMAGE_BITMAP, 50, 40, LR_CREATEDIBSECTION);
            if (c) DeleteObject(c);
            DeleteObject(b);
        }
    }
}

static int CALLBACK enum_cb(const LOGFONTA *lf, const TEXTMETRICA *tm, DWORD type, LPARAM l)
{
    (*(int *)l)++;
    return *(int *)l < 400;
}

static void vcl_like(HDC sdc, int round)
{
    STEP("palettes: CreateHalftonePalette, Select/RealizePalette on screen DC");
    {
        HPALETTE hp = CreateHalftonePalette(sdc), op;
        PALETTEENTRY pe[256];
        if (hp) {
            op = SelectPalette(sdc, hp, FALSE);
            RealizePalette(sdc);
            GetSystemPaletteEntries(sdc, 0, 256, pe);
            GetNearestColor(sdc, RGB(123, 45, 67));
            GetNearestPaletteIndex(hp, RGB(12, 200, 100));
            SelectPalette(sdc, op, FALSE);
            RealizePalette(sdc);
            DeleteObject(hp);
        }
    }
    STEP("DIB section 8 bpp: Get/SetDIBColorTable, DIB_PAL_COLORS");
    {
        BMI bi;
        void *bits;
        HBITMAP hb;
        HDC m = CreateCompatibleDC(sdc);
        RGBQUAD q[256];
        int i;
        fill_bmi(&bi, 64, 64, 8, 0);
        hb = CreateDIBSection(sdc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (hb) {
            HGDIOBJ o = SelectObject(m, hb);
            GetDIBColorTable(m, 0, 256, q);
            for (i = 0; i < 256; i++) q[i].rgbRed = (BYTE)i;
            SetDIBColorTable(m, 0, 256, q);
            BitBlt(sdc, 10, 10, 64, 64, m, 0, 0, SRCCOPY);
            SelectObject(m, o);
            DeleteObject(hb);
        }
        {
            struct { BITMAPINFOHEADER h; WORD idx[256]; } pb;
            static BYTE b8[64 * 64];
            memset(&pb, 0, sizeof(pb));
            pb.h.biSize = sizeof(pb.h); pb.h.biWidth = 64; pb.h.biHeight = 64; pb.h.biPlanes = 1;
            pb.h.biBitCount = 8;
            for (i = 0; i < 256; i++) pb.idx[i] = (WORD)i;
            for (i = 0; i < 64 * 64; i++) b8[i] = (BYTE)i;
            StretchDIBits(sdc, 80, 10, 64, 64, 0, 0, 64, 64, b8, (BITMAPINFO *)&pb, DIB_PAL_COLORS, SRCCOPY);
            SetDIBitsToDevice(sdc, 150, 10, 64, 64, 0, 0, 0, 64, b8, (BITMAPINFO *)&pb, DIB_PAL_COLORS);
            hb = CreateDIBSection(sdc, (BITMAPINFO *)&pb, DIB_PAL_COLORS, &bits, NULL, 0);
            if (hb) DeleteObject(hb);
        }
        DeleteDC(m);
    }
    STEP("RLE8 SetDIBitsToDevice");
    {
        struct { BITMAPINFOHEADER h; RGBQUAD c[256]; } rb;
        static BYTE rle[] = { 10, 1, 5, 2, 0, 0, 20, 3, 0, 0, 0, 3, 4, 5, 6, 0, 0, 1 };
        int i;
        memset(&rb, 0, sizeof(rb));
        rb.h.biSize = sizeof(rb.h); rb.h.biWidth = 30; rb.h.biHeight = 3; rb.h.biPlanes = 1;
        rb.h.biBitCount = 8; rb.h.biCompression = BI_RLE8; rb.h.biSizeImage = sizeof(rle);
        for (i = 0; i < 256; i++) { rb.c[i].rgbRed = (BYTE)i; rb.c[i].rgbBlue = (BYTE)(255 - i); }
        SetDIBitsToDevice(sdc, 10, 90, 30, 3, 0, 0, 0, 3, rle, (BITMAPINFO *)&rb, DIB_RGB_COLORS);
        StretchDIBits(sdc, 10, 100, 90, 12, 0, 0, 30, 3, rle, (BITMAPINFO *)&rb, DIB_RGB_COLORS, SRCCOPY);
    }
    STEP("mono pattern brush PATINVERT (splitter drag)");
    {
        static const WORD pat[8] = { 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA };
        HBITMAP mb = CreateBitmap(8, 8, 1, 1, pat);
        HBRUSH br = CreatePatternBrush(mb);
        HGDIOBJ o = SelectObject(sdc, br);
        int i;
        for (i = 0; i < 6; i++) PatBlt(sdc, 30 + i * 20, 50, 4, 200, PATINVERT);
        for (i = 0; i < 6; i++) PatBlt(sdc, 30 + i * 20, 50, 4, 200, PATINVERT);
        SelectObject(sdc, o);
        DeleteObject(br);
        DeleteObject(mb);
    }
    STEP("ROP2 modes: XOR / NOT lines and rectangles");
    {
        int m;
        HPEN p = CreatePen(PS_DOT, 1, RGB(255, 0, 0));
        HGDIOBJ o = SelectObject(sdc, p);
        for (m = R2_BLACK; m <= R2_WHITE; m++) {
            SetROP2(sdc, m);
            MoveToEx(sdc, 10, 120 + m * 3, NULL);
            LineTo(sdc, 300, 125 + m * 3);
            Rectangle(sdc, 310, 120 + m * 2, 350, 140 + m * 2);
        }
        SetROP2(sdc, R2_COPYPEN);
        SelectObject(sdc, o);
        DeleteObject(p);
    }
    STEP("font enumeration and metrics");
    {
        LOGFONTA lf;
        int n = 0;
        TEXTMETRICA tm;
        ABC abc[96];
        SIZE sz;
        int fit, dx[64];
        HFONT f;
        HGDIOBJ o;
        memset(&lf, 0, sizeof(lf));
        lf.lfCharSet = DEFAULT_CHARSET;
        EnumFontFamiliesExA(sdc, &lf, (FONTENUMPROCA)enum_cb, (LPARAM)&n, 0);
        logf_("  %d fonts\r\n", n);
        f = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH, "Courier New");
        o = SelectObject(sdc, f);
        GetTextMetricsA(sdc, &tm);
        GetCharABCWidthsA(sdc, 32, 127, abc);
        GetTextExtentExPointA(sdc, "The quick brown fox jumps over the lazy dog", 43, 200, &fit, dx, &sz);
        TabbedTextOutA(sdc, 10, 250, "a\tb\tc\tTabbed", 13, 0, NULL, 10);
        SelectObject(sdc, o);
        DeleteObject(f);
        f = CreateFontA(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "MS Sans Serif");
        o = SelectObject(sdc, f);
        {
            static char longs[600];
            int i;
            for (i = 0; i < 599; i++) longs[i] = (char)('A' + i % 26);
            TextOutA(sdc, 0, 270, longs, 599);
            GetTextExtentPoint32A(sdc, longs, 599, &sz);
            ExtTextOutA(sdc, -500, 285, 0, NULL, longs, 599, NULL);
        }
        SelectObject(sdc, o);
        DeleteObject(f);
    }
    STEP("SetStretchBltMode HALFTONE + StretchBlt screen to screen");
    SetStretchBltMode(sdc, HALFTONE);
    StretchBlt(sdc, 0, 0, 100, 80, sdc, 0, 0, 300, 240, SRCCOPY);
    SetStretchBltMode(sdc, COLORONCOLOR);
    STEP("CreateCompatibleBitmap on a memory DC (mono) + BitBlt mono -> screen");
    {
        HDC m = CreateCompatibleDC(sdc);
        HBITMAP mb = CreateCompatibleBitmap(m, 50, 30);
        HGDIOBJ o = SelectObject(m, mb);
        PatBlt(m, 0, 0, 50, 30, WHITENESS);
        TextOutA(m, 2, 2, "mono", 4);
        SetTextColor(sdc, RGB(255, 0, 0));
        SetBkColor(sdc, RGB(0, 0, 255));
        BitBlt(sdc, 200, 200, 50, 30, m, 0, 0, SRCCOPY);
        BitBlt(sdc, 260, 200, 50, 30, m, 0, 0, 0x00B8074A);     /* PSDPxax (masked) */
        SelectObject(m, o);
        DeleteObject(mb);
        DeleteDC(m);
    }
    (void)round;
}

static void sysres(HDC sdc)
{
    static const WORD icons[] = { 32512, 32513, 32514, 32515, 32516, 32517, 32518 };
    static const WORD curs[] = { 32512, 32513, 32514, 32515, 32516, 32640, 32641, 32642, 32643, 32644, 32645,
                                 32646, 32648, 32649, 32650, 32651 };
    static const int sizes[] = { 0, 16, 32, 48 };
    int i, k;
    char nm[80];
    for (i = 0; i < 7; i++)
        for (k = 0; k < 4; k++) {
            HICON ic;
            ICONINFO ii;
            wsprintfA(nm, "system icon %u size %d", icons[i], sizes[k]);
            STEP(nm);
            ic = (HICON)LoadImageA(NULL, (LPCSTR)(DWORD)icons[i], IMAGE_ICON, sizes[k], sizes[k],
                                   LR_SHARED | (sizes[k] ? 0 : LR_DEFAULTSIZE));
            if (!ic) { logf_("  none\r\n"); continue; }
            DrawIconEx(sdc, 10 + i * 40, 10 + k * 40, ic, 0, 0, 0, NULL, DI_NORMAL);
            DrawIconEx(sdc, 300, 10, ic, 16, 16, 0, NULL, DI_NORMAL);
            if (GetIconInfo(ic, &ii)) {
                BITMAP bm;
                GetObjectA(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof(bm), &bm);
                logf_("  %dx%d %d bpp\r\n", bm.bmWidth, bm.bmHeight, bm.bmBitsPixel);
                if (ii.hbmColor) DeleteObject(ii.hbmColor);
                DeleteObject(ii.hbmMask);
            }
            {
                HICON c = (HICON)CopyImage(ic, IMAGE_ICON, 16, 16, 0);
                if (c) { DrawIconEx(sdc, 330, 10, c, 0, 0, 0, NULL, DI_NORMAL); DestroyIcon(c); }
            }
        }
    for (i = 0; i < 16; i++) {
        HCURSOR c;
        wsprintfA(nm, "system cursor %u", curs[i]);
        STEP(nm);
        c = LoadCursorA(NULL, (LPCSTR)(DWORD)curs[i]);
        if (!c) { logf_("  none\r\n"); continue; }
        DrawIconEx(sdc, 10 + i * 36, 200, (HICON)c, 0, 0, 0, NULL, DI_NORMAL);
        c = (HCURSOR)LoadImageA(NULL, (LPCSTR)(DWORD)curs[i], IMAGE_CURSOR, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
        if (c) SetCursor(c);
    }
    for (i = 32734; i <= 32767; i++) {
        HBITMAP b;
        wsprintfA(nm, "OEM bitmap %d", i);
        STEP(nm);
        b = LoadBitmapA(NULL, (LPCSTR)(DWORD)i);
        if (b) {
            HDC m = CreateCompatibleDC(sdc);
            HGDIOBJ o = SelectObject(m, b);
            BITMAP bm;
            GetObjectA(b, sizeof(bm), &bm);
            BitBlt(sdc, 10 + (i - 32734) % 16 * 24, 250 + (i - 32734) / 16 * 24, bm.bmWidth, bm.bmHeight, m, 0, 0, SRCCOPY);
            SelectObject(m, o);
            DeleteDC(m);
            DeleteObject(b);
        }
    }
    SetCursor(LoadCursor(NULL, IDC_ARROW));
}

static LRESULT CALLBACK wproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcA(h, m, w, l);
}

void WINAPI start(void)
{
    char path[MAX_PATH], *e, *cmd;
    WNDCLASSA wc;
    MSG msg;
    int rounds = 3, r;
    HDC dc;

    GetModuleFileNameA(NULL, path, sizeof(path));
    for (e = path; *e; e++) ;
    while (e > path && e[-1] != '\\') e--;
    lstrcpyA(e, "W32GDI.TXT");
    lg = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    cmd = GetCommandLineA();
    if (*cmd == '"') { cmd++; while (*cmd && *cmd != '"') cmd++; if (*cmd) cmd++; }
    else while (*cmd && *cmd != ' ') cmd++;
    while (*cmd == ' ') cmd++;
    if (*cmd >= '0' && *cmd <= '9') { rounds = 0; while (*cmd >= '0' && *cmd <= '9') rounds = rounds * 10 + *cmd++ - '0'; }
    InitCommonControls();
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "W32GDI";
    RegisterClassA(&wc);
    wnd = CreateWindowA("W32GDI", "W32GDI", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 20, 20, W + 20, H + 40, NULL, NULL,
                        wc.hInstance, NULL);
    dc = GetDC(NULL);
    logf_("screen %d bpp, %d planes, raster %x, colors %d, palette %d\r\n", GetDeviceCaps(dc, BITSPIXEL),
          GetDeviceCaps(dc, PLANES), GetDeviceCaps(dc, RASTERCAPS), GetDeviceCaps(dc, NUMCOLORS),
          GetDeviceCaps(dc, SIZEPALETTE));
    ReleaseDC(NULL, dc);
    for (r = 0; r < rounds; r++) {
        HDC wdc;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        logf_("round %d\r\n", r);
        wdc = GetDC(wnd);
        dib_sections(wdc, r);
        roundtrip(wdc);
        ddb_tests(wdc);
        imagelists(wdc);
        misc(wdc, r);
        vcl_like(wdc, r);
        sysres(wdc);
        ReleaseDC(wnd, wdc);
        dc = GetDC(NULL);
        STEP("screen DC: VCL-like");
        vcl_like(dc, r);
        ReleaseDC(NULL, dc);
        dc = GetDC(NULL);
        STEP("screen DC: DIB sections");
        dib_sections(dc, r + 1);
        ReleaseDC(NULL, dc);
    }
    logf_("done, %d failed checks\r\n", failures);
    CloseHandle(lg);
    DestroyWindow(wnd);
    ExitProcess(0);
}
