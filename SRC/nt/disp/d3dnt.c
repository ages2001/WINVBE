/*
 * WINVBE.DLL - the Direct3D HAL of Windows 2000 and XP (the DirectX 7
 * driver interface): the Windows NT side of d3d/d3ddi.c, which drives the
 * software rasterizer d3d/swr.c.
 *
 * Compiled on its own: it uses floating point (x87 on i386, SSE on amd64),
 * which the kernel allows between EngSaveFloatingPointState and
 * EngRestoreFloatingPointState. winvbe.c calls the functions of d3dnt.h.
 *
 * Surfaces: fpVidMem of a surface in video memory counts from the start of
 * the frame buffer (the driver's own mapping); surfaces in system memory
 * (command and vertex buffers, textures there) carry the address in the
 * calling process, which the DirectDraw callbacks run in.
 */
#include <stdarg.h>
#include <windef.h>
#include <wingdi.h>
#include <winddi.h>
/* d3dhal.h uses the Windows 9x names of the structures it points to */
typedef DD_SURFACE_LOCAL *LPDDRAWI_DDRAWSURFACE_LCL;
typedef DD_DIRECTDRAW_LOCAL *LPDDRAWI_DIRECTDRAW_LCL;
typedef DD_DIRECTDRAW_GLOBAL *LPDDRAWI_DIRECTDRAW_GBL;
#include <d3dhal.h>
#include "d3dnt.h"

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

static void *mem_alloc(ULONG n) { return EngAllocMem(FL_ZERO_MEMORY, n, 0x64337677); }
static void mem_free(void *p) { if (p) EngFreeMem(p); }

/* the mode (DrvGetDirectDrawInfo) */
static BYTE *nt_fb;
static ULONG nt_vram, nt_bpp, nt_r, nt_g, nt_b, nt_on;

/* surface memory: video memory is an offset into the frame buffer */
static BYTE *nt_addr(PDD_SURFACE_LOCAL s)
{
    FLATPTR f = s->lpGbl->fpVidMem;
    if ((s->ddsCaps.dwCaps & (DDSCAPS_VIDEOMEMORY | DDSCAPS_PRIMARYSURFACE)) &&
        !(s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) && f < nt_vram)
        return nt_fb + f;
    return (BYTE *)f;
}

/* ---------------- the platform for d3ddi.c ---------------- */
typedef PDD_SURFACE_LOCAL D3SURF;
typedef PDD_SURFACE_GLOBAL D3SGBL;
typedef PDD_ATTACHLIST D3ATTACH;
typedef PDD_CREATESURFACEEXDATA D3_CSEXDATA;
typedef PDD_DESTROYDDLOCALDATA D3_DDLDATA;
typedef PDD_GETDRIVERSTATEDATA D3_GDSDATA;
typedef DD_MISCELLANEOUS2CALLBACKS D3_MISC2CB;
#define D3_ADDR(s)      nt_addr(s)
/* CreateSurfaceEx with fpVidMem 0: a surface in system memory goes away.
   In video memory 0 is the start of the frame buffer: the primary
   surface, or after an odd number of flips the back buffer */
#define D3_LIVE(s)      ((s)->lpGbl->fpVidMem != 0 || ((s)->ddsCaps.dwCaps & (DDSCAPS_VIDEOMEMORY | DDSCAPS_PRIMARYSURFACE)))
#define D3_PRIM_BPP     nt_bpp
#define D3_PRIM_R       nt_r
#define D3_PRIM_G       nt_g
#define D3_PRIM_B       nt_b
#define D3_ENABLED      nt_on
#define D3_FPU_BEGIN    BYTE fpu_[FPU_SAVE]; BOOL fpu_ok_ = EngSaveFloatingPointState(fpu_, sizeof(fpu_))
#define D3_FPU_END      do { if (fpu_ok_) EngRestoreFloatingPointState(fpu_); } while (0)
#define D3_SHARED
void (*d3dnt_log)(const char *f, ULONG a, ULONG b);
#define D3_DBG(s, a, b) do { if (d3dnt_log) d3dnt_log(s " %x %x\n", (ULONG)(a), (ULONG)(b)); } while (0)
#define FPU_SAVE        512

#include "../../d3d/swr.c"
#include "../../d3d/d3ddi.c"

/* D3DNTHAL_CALLBACKS: the 9x layout with pointer-sized reserved fields */
typedef struct {
    DWORD dwSize;
    LPVOID ContextCreate, ContextDestroy, ContextDestroyAll, SceneCapture;
    LPVOID dwReserved10, dwReserved11, dwReserved22, dwReserved23;
    ULONG_PTR dwReserved;
    LPVOID TextureCreate, TextureDestroy, TextureSwap, TextureGetSurf;
    LPVOID dwReserved12, dwReserved13, dwReserved14, dwReserved15, dwReserved16;
    LPVOID dwReserved17, dwReserved18, dwReserved19, dwReserved20, dwReserved21;
    LPVOID dwReserved24;
    ULONG_PTR dwReserved0, dwReserved1, dwReserved2, dwReserved3, dwReserved4;
    ULONG_PTR dwReserved5, dwReserved6, dwReserved7, dwReserved8, dwReserved9;
} NTCALLBACKS;

static NTCALLBACKS nt_cb;

int d3dnt_init(void)
{
    ULONG need = EngSaveFloatingPointState(NULL, 0);
    if (!need || need > FPU_SAVE) return 0;
    memset(&nt_cb, 0, sizeof(nt_cb));
    nt_cb.dwSize = sizeof(nt_cb);
    nt_cb.ContextCreate = (LPVOID)D3ContextCreate;
    nt_cb.ContextDestroy = (LPVOID)D3ContextDestroy;
    nt_cb.ContextDestroyAll = (LPVOID)D3ContextDestroyAll;
    d3d_global.lpTextureFormats = tex_formats;
    return 1;
}

void d3dnt_mode(void *fb, ULONG vram, ULONG bpp, ULONG r, ULONG g, ULONG b, int on)
{
    nt_fb = fb; nt_vram = vram;
    nt_bpp = bpp; nt_r = r; nt_g = g; nt_b = b;
    nt_on = on && bpp >= 15;
}

/* the Direct3D part of DrvGetDirectDrawInfo */
void d3dnt_halinfo(DD_HALINFO *h)
{
    if (!nt_on) return;
    /* BLTDEPTHFILL: Z buffers cleared with DirectDraw blits (ddblt.c);
       Direct3D's own clears come as D3DDP2OP_CLEAR (there is no Clear2
       on Windows 2000/XP) */
    h->ddCaps.dwCaps |= DDCAPS_3D | DDCAPS_ALPHA | DDCAPS_BLTDEPTHFILL;
    h->ddCaps.ddsCaps.dwCaps |= DDSCAPS_3DDEVICE | DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER | DDSCAPS_MIPMAP | DDSCAPS_ALPHA;
    h->ddCaps.dwZBufferBitDepths = DDBD_16 | DDBD_24 | DDBD_32;
    h->lpD3DGlobalDriverData = &d3d_global;
    h->lpD3DHALCallbacks = &nt_cb;
    h->lpD3DBufCallbacks = NULL;
}

int d3dnt_info(PDD_GETDRIVERINFODATA d)
{
    int r;
    D3_FPU_BEGIN;
    r = nt_on && d3d_info(&d->guidInfo, d->dwExpectedSize, d->lpvData, &d->dwActualSize);
    D3_FPU_END;
    if (r) d->ddRVal = DD_OK;
    return r;
}

void d3dnt_destroy(PDD_SURFACE_LOCAL s)
{
    d3d_forget(s);
}

int d3dnt_can_create(DDSURFACEDESC *sd)
{
    return d3d_can_create(sd);
}

/* compressed textures in the rectangular heap: rows of blocks */
int d3dnt_dxt(PDD_SURFACE_GLOBAL g, DWORD f, LONG stride)
{
    DWORD bs, bw, bh;
    if (!nt_on) return 0;
    if (f == MAKEFOURCC('D','X','T','1')) bs = 8;
    else if (f >= MAKEFOURCC('D','X','T','2') && f <= MAKEFOURCC('D','X','T','5') &&
             (f & 0x00FFFFFF) == MAKEFOURCC('D','X','T',0)) bs = 16;
    else return 0;
    bw = (g->wWidth + 3u) / 4; bh = (g->wHeight + 3u) / 4;
    if (!bw) bw = 1;
    if (!bh) bh = 1;
    if ((LONG)(bw * bs) > stride) return 0;
    g->lPitch = stride;
    g->dwBlockSizeX = bw * bs;
    g->dwBlockSizeY = bh;
    g->fpVidMem = DDHAL_PLEASEALLOC_BLOCKSIZE;
    return 1;
}
