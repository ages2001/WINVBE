/*
 * DDGBL - shows what DirectDraw (Windows 95/98/ME) made of the display
 * driver's HAL: the flags, capabilities and video memory of its global
 * driver object, and the drivers DirectDrawEnumerate lists. Writes
 * DDGBL.TXT next to the program.
 */
#include <windows.h>
#include <ddraw.h>
#include <ddrawi.h>

static HANDLE out;

static void logf_(const char *fmt, ...)
{
    char b[512];
    DWORD n;
    va_list a;
    va_start(a, fmt);
    n = wvsprintfA(b, fmt, a);
    va_end(a);
    WriteFile(out, b, n, &n, NULL);
}

static BOOL WINAPI enum_cb(GUID *g, LPSTR desc, LPSTR name, LPVOID ctx)
{
    (void)ctx;
    logf_("driver %s \"%s\" \"%s\"\r\n", g ? "guid" : "NULL", desc ? desc : "", name ? name : "");
    return TRUE;
}

void WINAPI start(void)
{
    char path[MAX_PATH], *p;
    LPDIRECTDRAW dd;
    LPDDRAWI_DIRECTDRAW_GBL gb;
    DWORD i;
    GetModuleFileNameA(NULL, path, sizeof(path));
    for (p = path; *p; p++) ;
    while (p > path && p[-1] != '\\') p--;
    lstrcpyA(p, "DDGBL.TXT");
    out = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DirectDrawEnumerateA(enum_cb, NULL);
    if (DirectDrawCreate(NULL, &dd, NULL) != DD_OK) { logf_("DirectDrawCreate failed\r\n"); ExitProcess(1); }
    gb = ((LPDDRAWI_DIRECTDRAW_INT)dd)->lpLcl->lpGbl;
    logf_("gbl %lx flags %lx caps %lx caps2 %lx ddscaps %lx vidmem %lu\r\n", (DWORD)gb, gb->dwFlags,
          gb->ddCaps.dwCaps, gb->ddCaps.dwCaps2, gb->ddCaps.ddsCaps.dwCaps, gb->ddCaps.dwVidMemTotal);
    logf_("primary %lx orig %lx %lux%lu pitch %ld bits %lu heaps %lu list %lx\r\n", gb->vmiData.fpPrimary,
          gb->fpPrimaryOrig, gb->vmiData.dwDisplayWidth, gb->vmiData.dwDisplayHeight, gb->vmiData.lDisplayPitch,
          gb->vmiData.ddpfDisplay.dwRGBBitCount, gb->vmiData.dwNumHeaps, (DWORD)gb->vmiData.pvmList);
    logf_("cb %lx driverhandle %lx hinst %lx pdevice %lx modes %lu index %lu fourcc %lu name \"%s\"\r\n",
          (DWORD)gb->lpDDCBtmp, (DWORD)gb->lpDriverHandle, gb->hInstance, gb->dwPDevice, gb->dwNumModes,
          gb->dwModeIndex, gb->dwNumFourCC, gb->cDriverName);
    if (gb->lpDDCBtmp) {
        LPDDHAL_CALLBACKS c = gb->lpDDCBtmp;
        logf_("HAL dd flags %lx blt %lx create %lx | surf flags %lx blt %lx\r\n", c->HALDD.dwFlags,
              (DWORD)c->HALDD.CreateSurface, (DWORD)c->HALDD.CanCreateSurface, c->HALDDSurface.dwFlags,
              (DWORD)c->HALDDSurface.Blt);
        logf_("used dd flags %lx | surf flags %lx blt %lx\r\n", c->cbDDCallbacks.dwFlags,
              c->cbDDSurfaceCallbacks.dwFlags, (DWORD)c->cbDDSurfaceCallbacks.Blt);
    }
    for (i = 0; i < gb->vmiData.dwNumHeaps && gb->vmiData.pvmList; i++)
        logf_("heap %lu flags %lx start %lx w %lu h %lu\r\n", i, gb->vmiData.pvmList[i].dwFlags,
              gb->vmiData.pvmList[i].fpStart, gb->vmiData.pvmList[i].dwWidth, gb->vmiData.pvmList[i].dwHeight);
    IDirectDraw_Release(dd);
    CloseHandle(out);
    ExitProcess(0);
}
