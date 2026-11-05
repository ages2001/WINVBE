/*
 * WVBDD32.DLL - Direct3D HAL for Windows 95/98/ME, included by wvbdd32.c:
 * the Windows 9x side of d3d/d3ddi.c (the DirectDraw 9x structures, the
 * HAL tables, GetDriverInfo).
 */
#include <d3dhal.h>

/* ---------------- missing DDK structures ---------------- */
struct _DDHAL_CREATESURFACEEXDATA {
    DWORD dwFlags;
    LPDDRAWI_DIRECTDRAW_LCL lpDDLcl;
    LPDDRAWI_DDRAWSURFACE_LCL lpDDSLcl;
    HRESULT ddRVal;
};
struct _DDHAL_GETDRIVERSTATEDATA {
    DWORD dwFlags;
    ULONG_PTR dwhContext;
    LPDWORD lpdwStates;
    DWORD dwLength;
    HRESULT ddRVal;
};
struct _DDHAL_DESTROYDDLOCALDATA {
    DWORD dwFlags;
    LPDDRAWI_DIRECTDRAW_LCL pDDLcl;
    HRESULT ddRVal;
};
struct _D3DHAL_TEXTURECREATEDATA { ULONG_PTR dwhContext; LPVOID lpDDS; DWORD dwHandle; HRESULT ddrval; };
struct _D3DHAL_TEXTUREDESTROYDATA { ULONG_PTR dwhContext; DWORD dwHandle; HRESULT ddrval; };
struct _D3DHAL_TEXTURESWAPDATA { ULONG_PTR dwhContext; DWORD dwHandle1; DWORD dwHandle2; HRESULT ddrval; };
struct _D3DHAL_TEXTUREGETSURFDATA { ULONG_PTR dwhContext; ULONG_PTR lpDDS; DWORD dwHandle; HRESULT ddrval; };


/* ---------------- the platform for d3ddi.c ---------------- */
typedef LPDDRAWI_DDRAWSURFACE_LCL D3SURF;
typedef LPDDRAWI_DDRAWSURFACE_GBL D3SGBL;
typedef LPATTACHLIST D3ATTACH;
typedef LPDDHAL_CREATESURFACEEXDATA D3_CSEXDATA;
typedef LPDDHAL_DESTROYDDLOCALDATA D3_DDLDATA;
typedef LPDDHAL_GETDRIVERSTATEDATA D3_GDSDATA;
typedef DDHAL_DDMISCELLANEOUS2CALLBACKS D3_MISC2CB;
#define D3_ADDR(s)      ((s)->lpGbl->fpVidMem)          /* linear addresses */
#define D3_LIVE(s)      ((s)->lpGbl->fpVidMem != 0)
#define D3_PRIM_BPP     g_dd->bpp
#define D3_PRIM_R       g_dd->rmask
#define D3_PRIM_G       g_dd->gmask
#define D3_PRIM_B       g_dd->bmask
#define D3_ENABLED      (g_dd && g_dd->d3d_on)
#define D3_FPU_BEGIN    do { } while (0)
#define D3_FPU_END      do { } while (0)
#define D3_SHARED       DD_SHARED
#define D3_DBG          dbg
#define D3_LEGACY

#include "../d3d/d3ddi.c"

static D3DHAL_CALLBACKS d3d_cb DD_SHARED = { 0 };

static DWORD WINAPI Dd9xGetDriverInfo(LPDDHAL_GETDRIVERINFODATA in)
{
    if (d3d_info(&in->guidInfo, in->dwExpectedSize, in->lpvData, &in->dwActualSize)) in->ddRVal = DD_OK;
    else in->ddRVal = DDERR_CURRENTLYNOTAVAIL;
    return DDHAL_DRIVER_HANDLED;
}

/* DriverInit: what the display driver adds to the HAL information */
static void d3d_init(WVBDD *d)
{
    d3d_global.lpTextureFormats = tex_formats;
    memset(&d3d_cb, 0, sizeof(d3d_cb));
    d3d_cb.dwSize = sizeof(d3d_cb);
    d3d_cb.ContextCreate = D3ContextCreate;
    d3d_cb.ContextDestroy = D3ContextDestroy;
    d3d_cb.ContextDestroyAll = D3ContextDestroyAll;
    d3d_cb.TextureCreate = D3TextureCreate;
    d3d_cb.TextureDestroy = D3TextureDestroy;
    d3d_cb.TextureSwap = D3TextureSwap;
    d3d_cb.TextureGetSurf = D3TextureGetSurf;
    d->d3dglobal = (WD32)&d3d_global;
    d->d3dcb = (WD32)&d3d_cb;
    d->getdriverinfo = (WD32)Dd9xGetDriverInfo;
}
