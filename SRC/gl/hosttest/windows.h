/*
 * hosttest/windows.h - minimal fake Win32 API so that wvbdrv.c, icd.c and
 * the ICDTEST scene can be built and run on Linux (see build-host.sh).
 * "Windows" are plain top-down 0x00RRGGBB pixel arrays; DIB sections are
 * malloc'd bottom-up 32-bit bitmaps; BitBlt copies between them.
 */
#ifndef FAKE_WINDOWS_H
#define FAKE_WINDOWS_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#define WINAPI
#define APIENTRY
#define CONST const
#define VOID void
#define FALSE 0
#define TRUE 1

typedef int BOOL;
typedef int INT;
typedef unsigned int UINT;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef uint32_t DWORD;
typedef uint16_t WORD;
typedef uint8_t BYTE;
typedef uintptr_t DWORD_PTR;
typedef uint32_t COLORREF;
typedef const char *LPCSTR;
typedef void *LPVOID;
typedef void *HINSTANCE;
typedef void *HGDIOBJ;
typedef void *HMODULE;
typedef int (*PROC)(void);

struct fake_window { int w, h; uint32_t *pixels; int valid; };
struct fake_bitmap { int w, h; uint32_t *bits; };
struct fake_dc { struct fake_window *wnd; struct fake_bitmap *bm; int bpp; };
typedef struct fake_window *HWND;
typedef struct fake_dc *HDC;
typedef struct fake_bitmap *HBITMAP;

typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { int count; } CRITICAL_SECTION;

typedef struct {
    WORD nSize, nVersion; DWORD dwFlags; BYTE iPixelType, cColorBits, cRedBits, cRedShift,
    cGreenBits, cGreenShift, cBlueBits, cBlueShift, cAlphaBits, cAlphaShift, cAccumBits,
    cAccumRedBits, cAccumGreenBits, cAccumBlueBits, cAccumAlphaBits, cDepthBits,
    cStencilBits, cAuxBuffers, iLayerType, bReserved; DWORD dwLayerMask, dwVisibleMask,
    dwDamageMask;
} PIXELFORMATDESCRIPTOR;
typedef struct { WORD nSize; } LAYERPLANEDESCRIPTOR, *LPLAYERPLANEDESCRIPTOR;

typedef struct {
    DWORD biSize; LONG biWidth, biHeight; WORD biPlanes, biBitCount; DWORD biCompression,
    biSizeImage; LONG biXPelsPerMeter, biYPelsPerMeter; DWORD biClrUsed, biClrImportant;
} BITMAPINFOHEADER;
typedef struct { BITMAPINFOHEADER bmiHeader; DWORD bmiColors[1]; } BITMAPINFO;

#define PFD_DOUBLEBUFFER 0x1
#define PFD_DRAW_TO_WINDOW 0x4
#define PFD_DRAW_TO_BITMAP 0x8
#define PFD_SUPPORT_GDI 0x10
#define PFD_SUPPORT_OPENGL 0x20
#define PFD_GENERIC_FORMAT 0x40
#define PFD_SWAP_COPY 0x400
#define PFD_TYPE_RGBA 0
#define PFD_MAIN_PLANE 0
#define WGL_SWAP_MAIN_PLANE 1
#define BI_RGB 0
#define DIB_RGB_COLORS 0
#define SRCCOPY 0xCC0020
#define BITSPIXEL 12
#define PLANES 14
#define DLL_PROCESS_DETACH 0
#define DLL_PROCESS_ATTACH 1
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_INVALID_HANDLE 6
#define ERROR_INVALID_PARAMETER 87
#define ERROR_PROC_NOT_FOUND 127
#define ERROR_INVALID_PIXEL_FORMAT 2000

extern DWORD fake_last_error;
extern int fake_desktop_bpp;
static inline void SetLastError(DWORD e) { fake_last_error = e; }
static inline DWORD GetLastError(void) { return fake_last_error; }
static inline void InitializeCriticalSection(CRITICAL_SECTION *c) { c->count = 0; }
static inline void DeleteCriticalSection(CRITICAL_SECTION *c) { (void) c; }
static inline void EnterCriticalSection(CRITICAL_SECTION *c) { c->count++; }
static inline void LeaveCriticalSection(CRITICAL_SECTION *c) { c->count--; }
static inline BOOL DisableThreadLibraryCalls(HINSTANCE h) { (void) h; return TRUE; }
static inline HMODULE GetModuleHandleA(const char *n) { (void) n; return NULL; }
static inline void *GetProcAddress(HMODULE m, const char *n) { (void) m; (void) n; return NULL; }
static inline int GetDeviceCaps(HDC dc, int what)
{
    if (what == BITSPIXEL) return dc && dc->bpp ? dc->bpp : fake_desktop_bpp;
    return 1;
}
static inline HWND WindowFromDC(HDC dc) { return dc ? dc->wnd : NULL; }
static inline BOOL IsWindow(HWND w) { return w && w->valid; }
static inline BOOL GetClientRect(HWND w, RECT *r)
{
    if (!w || !w->valid) return FALSE;
    r->left = r->top = 0; r->right = w->w; r->bottom = w->h; return TRUE;
}
HDC GetDC(HWND w);
int ReleaseDC(HWND w, HDC dc);
HDC CreateCompatibleDC(HDC dc);
BOOL DeleteDC(HDC dc);
HGDIOBJ SelectObject(HDC dc, HGDIOBJ obj);
BOOL DeleteObject(HGDIOBJ obj);
HBITMAP CreateDIBSection(HDC dc, const BITMAPINFO *bmi, UINT usage, void **bits, void *sec, DWORD off);
BOOL BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop);

#endif
