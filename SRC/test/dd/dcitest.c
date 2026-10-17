/*
 * DCITEST - DCI primary surface test (Windows 95/98/ME, 3.1 with Win32s
 * not needed: uses DCIMAN32.DLL).  Draws colour bars straight into the
 * frame buffer at the top left of the screen, writes DCITEST.TXT.
 */
#include <windows.h>
#include <dciddi.h>

static HANDLE lg;
void *memset(void *d, int c, size_t n) { char *p = d; while (n--) *p++ = (char)c; return d; }
static void logf_(const char *f, ...)
{
    char b[512]; DWORD n; va_list ap;
    va_start(ap, f); n = wvsprintfA(b, f, ap); va_end(ap);
    WriteFile(lg, b, n, &n, NULL); FlushFileBuffers(lg);
}

typedef HDC (WINAPI *P_OPEN)(void);
typedef void (WINAPI *P_CLOSE)(HDC);
typedef int (WINAPI *P_CREATE)(HDC, LPDCISURFACEINFO *);
typedef DCIRVAL (WINAPI *P_BEGIN)(LPDCISURFACEINFO, int, int, int, int);
typedef void (WINAPI *P_END)(LPDCISURFACEINFO);
typedef void (WINAPI *P_DESTROY)(LPDCISURFACEINFO);

void WINAPI start(void)
{
    char path[MAX_PATH], *e;
    HMODULE m;
    P_OPEN op; P_CLOSE cl; P_CREATE cr; P_BEGIN be; P_END en; P_DESTROY de;
    HDC dc;
    LPDCISURFACEINFO s = NULL;
    int r, x, y, k;
    DWORD base = 0;

    GetModuleFileName(NULL, path, sizeof(path));
    for (e = path + lstrlen(path); e > path && e[-1] != '\\'; e--) ;
    lstrcpy(e, "DCITEST.TXT");
    lg = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    m = LoadLibrary("DCIMAN32.DLL");
    if (!m) { logf_("no DCIMAN32.DLL\n"); ExitProcess(1); }
    op = (P_OPEN)GetProcAddress(m, "DCIOpenProvider");
    cl = (P_CLOSE)GetProcAddress(m, "DCICloseProvider");
    cr = (P_CREATE)GetProcAddress(m, "DCICreatePrimary");
    be = (P_BEGIN)GetProcAddress(m, "DCIBeginAccess");
    en = (P_END)GetProcAddress(m, "DCIEndAccess");
    de = (P_DESTROY)GetProcAddress(m, "DCIDestroy");
    dc = op();
    logf_("DCIOpenProvider %08lx\n", (DWORD)dc);
    if (!dc) ExitProcess(1);
    r = cr(dc, &s);
    logf_("DCICreatePrimary %d %08lx\n", r, (DWORD)s);
    if (r < 0 || !s) { cl(dc); ExitProcess(1); }
    logf_("caps %08lx comp %lu masks %06lx %06lx %06lx %lux%lu stride %ld bits %lu off %08lx sel %04x\n",
          s->dwDCICaps, s->dwCompression, s->dwMask[0], s->dwMask[1], s->dwMask[2], s->dwWidth, s->dwHeight,
          s->lStride, s->dwBitCount, s->dwOffSurface, s->wSelSurface);
    for (k = 0; k < 3; k++) {
        r = be(s, 0, 0, 256, 256);
        logf_("DCIBeginAccess %d off %08lx sel %04x\n", r, s->dwOffSurface, s->wSelSurface);
        if (r < 0) break;
        if (s->wSelSurface) {       /* 16:32: flat address from the selector base */
            LDT_ENTRY le;
            if (GetThreadSelectorEntry(GetCurrentThread(), s->wSelSurface, &le)) {
                base = le.BaseLow | ((DWORD)le.HighWord.Bytes.BaseMid << 16) | ((DWORD)le.HighWord.Bytes.BaseHi << 24);
                logf_("selector base %08lx\n", base);
            } else logf_("GetThreadSelectorEntry failed\n");
        }
        if (base || s->dwOffSurface) {
            for (y = 0; y < 256; y++) {
                BYTE *row = (BYTE *)(base + s->dwOffSurface) + y * s->lStride;
                for (x = 0; x < 256; x++) {
                    int c = x < 85 ? 0 : x < 170 ? 1 : 2, v = 255 - y;
                    if (s->dwBitCount == 8) row[x] = (BYTE)(y < 128 ? 9 + c : 0);
                    else if (s->dwBitCount == 16 && s->dwCompression == 3)
                        ((WORD *)row)[x] = (WORD)(c == 0 ? (v >> 3) << 11 : c == 1 ? (v >> 2) << 5 : v >> 3);
                    else if (s->dwBitCount == 16)
                        ((WORD *)row)[x] = (WORD)(c == 0 ? (v >> 3) << 10 : c == 1 ? (v >> 3) << 5 : v >> 3);
                    else if (s->dwBitCount == 24) { row[x*3] = (BYTE)(c == 2 ? v : 0); row[x*3+1] = (BYTE)(c == 1 ? v : 0); row[x*3+2] = (BYTE)(c == 0 ? v : 0); }
                    else ((DWORD *)row)[x] = (DWORD)v << (c == 0 ? 16 : c == 1 ? 8 : 0);
                }
            }
            logf_("drawn\n");
        } else logf_("no flat pointer\n");
        en(s);
        Sleep(1000);
    }
    Sleep(8000);
    de(s);
    cl(dc);
    logf_("done\n");
    CloseHandle(lg);
    ExitProcess(0);
}
