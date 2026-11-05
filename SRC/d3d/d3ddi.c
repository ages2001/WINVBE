/*
 * The Direct3D driver interface (DirectX 7 DDI: DrawPrimitives2,
 * CreateSurfaceEx; TextureCreate and friends for the DirectX 5/6 runtime
 * of Windows 9x) in front of WINVBE's software rasterizer (swr.c), for
 * the Windows 9x HAL (dd9x/wvbd3d.c) and the Windows 2000/XP display
 * driver (nt/disp/d3dnt.c). VBE cards have no 3D hardware: the rendering
 * runs on the CPU, but programs that insist on a hardware device
 * (D3DDEVTYPE_HAL, "Direct3D HAL") run, into render targets, Z buffers
 * and textures in video memory. No transform and lighting: the runtime
 * does those and sends transformed vertices.
 *
 * The includer defines (before including this file and d3dhal.h's types):
 *   D3SURF, D3SGBL, D3ATTACH  surface local/global and attach list pointers
 *   D3_ADDR(s)                the address of a surface's memory
 *   D3_LIVE(s)                the surface has memory
 *   D3_PRIM_BPP, _R, _G, _B   the primary surface's format
 *   D3_ENABLED                the Direct3D part is on
 *   D3_CSEXDATA, D3_DDLDATA, D3_GDSDATA, D3_MISC2CB  the DirectDraw 7 types
 *   D3_FPU_BEGIN, D3_FPU_END  around floating point (the NT kernel)
 *   D3_SHARED, D3_DBG, mem_alloc, mem_free, memcpy, memset
 *   D3_LEGACY                 DirectX 5/6 texture handles (Windows 9x)
 */

#include "swr.h"

/* ---------------- DDK parts missing from the headers ---------------- */
struct _D3DHAL_CLEAR2DATA {
    ULONG_PTR dwhContext;
    DWORD dwFlags;
    DWORD dwFillColor;
    D3DVALUE dvFillDepth;
    DWORD dwFillStencil;
    LPD3DRECT lpRects;
    DWORD dwNumRects;
    HRESULT ddrval;
};
struct _D3DHAL_VALIDATETEXTURESTAGESTATEDATA {
    ULONG_PTR dwhContext;
    DWORD dwFlags;
    ULONG_PTR dwReserved;
    DWORD dwNumPasses;
    HRESULT ddrval;
};
#ifndef D3DHAL3_CB32_CLEAR2
#define D3DHAL3_CB32_CLEAR2                     0x00000001
#define D3DHAL3_CB32_VALIDATETEXTURESTAGESTATE  0x00000004
#define D3DHAL3_CB32_DRAWPRIMITIVES2            0x00000008
#endif
#define D3DHAL2_CB32_SETRENDERTARGET    0x00000001
#ifndef DDHAL_MISC2CB32_CREATESURFACEEX
#define DDHAL_MISC2CB32_CREATESURFACEEX 0x00000002
#define DDHAL_MISC2CB32_GETDRIVERSTATE  0x00000004
#endif
#ifndef DDHAL_MISC2CB32_DESTROYDDLOCAL
#define DDHAL_MISC2CB32_DESTROYDDLOCAL  0x00000008
#endif
#define D3DHAL_OUTOFCONTEXTS            ((HRESULT)0x8876006EL)
#define D3DERR_COMMAND_UNPARSED_        ((HRESULT)0x88760BB8L)
#define DDHALINFO_GETDRIVERINFOSET_     0x00000004
#define D3DDEVCAPS_DRAWPRIMITIVES2_     0x00002000
#define D3DDEVCAPS_DRAWPRIMITIVES2EX_   0x00008000
#define D3DDEVCAPS_CANRENDERAFTERFLIP_  0x00000800
#define D3DDEVCAPS_HWRASTERIZATION_     0x00080000

static const GUID G_D3DCallbacks2 = { 0x0BA584E1, 0x70B6, 0x11D0, { 0x88, 0x9D, 0x00, 0xAA, 0x00, 0xBB, 0xB7, 0x6A } };
static const GUID G_D3DCallbacks3 = { 0xDDF41230, 0xEC0A, 0x11D0, { 0xA9, 0xB6, 0x00, 0xAA, 0x00, 0xC0, 0x99, 0x3E } };
static const GUID G_D3DExtendedCaps = { 0x7DE41F80, 0x9D93, 0x11D0, { 0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29 } };
static const GUID G_ZPixelFormats = { 0x93869880, 0x36CF, 0x11D1, { 0x9B, 0x1B, 0x00, 0xAA, 0x00, 0xBB, 0xB8, 0xAE } };
static const GUID G_Misc2Callbacks = { 0x406B2F00, 0x3E5A, 0x11D1, { 0xB6, 0x40, 0x00, 0xAA, 0x00, 0xA1, 0xF9, 0x6A } };
static const GUID G_ParseUnknown = { 0x2E04FFA0, 0x98E4, 0x11D1, { 0x8C, 0xE1, 0x00, 0xA0, 0xC9, 0x06, 0x29, 0xA8 } };

static int guid_eq(const GUID *a, const GUID *b)
{
    const DWORD *x = (const DWORD *)a, *y = (const DWORD *)b;
    return x[0] == y[0] && x[1] == y[1] && x[2] == y[2] && x[3] == y[3];
}

/* ---------------- surface handles ---------------- */
/* DirectX 7: a surface handle per local DirectDraw object (CreateSurfaceEx);
   DirectX 6: texture handles of our own (TextureCreate, dd = context) */
typedef struct {
    void *dd;
    DWORD h;
    D3SURF s;
    int dx7;                            /* CreateSurfaceEx: the handle is the surface's own */
} HENT;

static HENT *h_tab D3_SHARED = NULL;
static DWORD h_n D3_SHARED = 0, h_cap D3_SHARED = 0;
#ifdef D3_LEGACY
static DWORD h_next D3_SHARED = 1;              /* DirectX 6 texture handles */
#endif

static D3SURF h_get(void *dd, DWORD h)
{
    DWORD i;
    D3SURF r = NULL;
    for (i = 0; i < h_n; i++) {
        D3SURF s;
        if (h_tab[i].dd != dd) continue;
        s = h_tab[i].s;
        /* DirectX 7: a flip exchanges the handles of the surfaces of the
           chain - the surface that has the handle now */
        if (h_tab[i].dx7) {
            if (s->lpSurfMore && s->lpSurfMore->dwSurfaceHandle == h) return s;
        } else if (h_tab[i].h == h)
            r = s;
    }
    if (!r)
        for (i = 0; i < h_n; i++)
            if (h_tab[i].dd == dd && h_tab[i].h == h) return h_tab[i].s;
    return r;
}

static void h_add(void *dd, DWORD h, D3SURF s, int dx7)
{
    DWORD i;
    for (i = 0; i < h_n; i++)
        if (h_tab[i].dd == dd && (dx7 ? h_tab[i].s == s : h_tab[i].h == h)) {
            h_tab[i].s = s; h_tab[i].h = h; h_tab[i].dx7 = dx7;
            return;
        }
    if (h_n == h_cap) {
        DWORD nc = h_cap ? h_cap * 2 : 256;
        HENT *n = mem_alloc(nc * sizeof(HENT));
        if (!n) return;
        if (h_tab) { memcpy(n, h_tab, h_n * sizeof(HENT)); mem_free(h_tab); }
        h_tab = n; h_cap = nc;
    }
    h_tab[h_n].dd = dd; h_tab[h_n].h = h; h_tab[h_n].s = s; h_tab[h_n].dx7 = dx7;
    h_n++;
}

/* forget a handle (DX6) or a surface (DX7) */
static void h_del(void *dd, DWORD h, D3SURF s)
{
    DWORD i = 0;
    while (i < h_n)
        if (h_tab[i].dd == dd && (s ? h_tab[i].s == s : h_tab[i].h == h)) h_tab[i] = h_tab[--h_n];
        else i++;
}

static void h_set(void *dd, DWORD h, D3SURF s)
{
    if (s) h_add(dd, h, s, 0);
    else h_del(dd, h, NULL);
}

/* a surface DirectDraw destroys: no handle may point to it any more */
static void d3d_forget(D3SURF s)
{
    DWORD i = 0;
    while (i < h_n)
        if (h_tab[i].s == s) h_tab[i] = h_tab[--h_n];
        else i++;
}

static void h_drop(void *dd)
{
    DWORD i = 0;
    while (i < h_n)
        if (h_tab[i].dd == dd) h_tab[i] = h_tab[--h_n];
        else i++;
}

/* ---------------- surface formats ---------------- */

static int fmt_pf(const DDPIXELFORMAT *ppf);

static int fmt_of(D3SURF s)
{
    DDPIXELFORMAT pf;
    if (s->dwFlags & DDRAWISURF_HASPIXELFORMAT) pf = s->lpGbl->ddpfSurface;
    else {
        memset(&pf, 0, sizeof(pf));
        pf.dwFlags = DDPF_RGB;
        pf.dwRGBBitCount = D3_PRIM_BPP;
        pf.dwRBitMask = D3_PRIM_R; pf.dwGBitMask = D3_PRIM_G; pf.dwBBitMask = D3_PRIM_B;
    }
    return fmt_pf(&pf);
}

static int fmt_pf(const DDPIXELFORMAT *ppf)
{
    DDPIXELFORMAT pf = *ppf;
    if (pf.dwFlags & DDPF_FOURCC) {
        switch (pf.dwFourCC) {
        case MAKEFOURCC('D','X','T','1'): return SWF_DXT1;
        case MAKEFOURCC('D','X','T','2'): return SWF_DXT2;
        case MAKEFOURCC('D','X','T','3'): return SWF_DXT3;
        case MAKEFOURCC('D','X','T','4'): return SWF_DXT4;
        case MAKEFOURCC('D','X','T','5'): return SWF_DXT5;
        case MAKEFOURCC('Y','U','Y','2'): return SWF_YUY2;
        case MAKEFOURCC('U','Y','V','Y'): return SWF_UYVY;
        }
        return SWF_NONE;
    }
    if (pf.dwFlags & DDPF_ZBUFFER) {
        DWORD zm = pf.dwZBitMask, sm = pf.dwStencilBitMask;
        if (pf.dwZBufferBitDepth == 16) return sm ? SWF_Z15S1 : SWF_Z16;
        if (zm == 0xFFFFFFFFu) return SWF_Z32;
        if (zm == 0xFFFFFF00u) return sm ? SWF_Z24S8 : SWF_Z24X8;
        return SWF_S8Z24;                       /* 00FFFFFF, stencil (or nothing) in the top byte */
    }
    if (pf.dwFlags & DDPF_PALETTEINDEXED8) return SWF_P8;
    if (pf.dwFlags & 0x00020000) {              /* DDPF_LUMINANCE */
        if (pf.dwRGBBitCount == 8) return (pf.dwFlags & DDPF_ALPHAPIXELS) ? SWF_A4L4 : SWF_L8;
        return SWF_A8L8;
    }
    if ((pf.dwFlags & DDPF_ALPHA) && !(pf.dwFlags & DDPF_RGB)) return SWF_A8;
    switch (pf.dwRGBBitCount) {
    case 8: return SWF_R3G3B2;
    case 16:
        if (pf.dwRBitMask == 0xF800) return SWF_R5G6B5;
        if (pf.dwRBitMask == 0x7C00) return (pf.dwFlags & DDPF_ALPHAPIXELS) && pf.dwRGBAlphaBitMask ? SWF_A1R5G5B5 : SWF_X1R5G5B5;
        if (pf.dwRBitMask == 0x0F00) return (pf.dwFlags & DDPF_ALPHAPIXELS) && pf.dwRGBAlphaBitMask ? SWF_A4R4G4B4 : SWF_X4R4G4B4;
        return SWF_NONE;
    case 24: return SWF_R8G8B8;
    case 32:
        if (pf.dwRBitMask != 0xFF0000) return SWF_NONE;
        return (pf.dwFlags & DDPF_ALPHAPIXELS) && pf.dwRGBAlphaBitMask ? SWF_A8R8G8B8 : SWF_X8R8G8B8;
    }
    return SWF_NONE;
}

static int surf_sw(D3SURF s, SWSURF *o)
{
    D3SGBL g;
    memset(o, 0, sizeof(*o));
    if (!s) return 0;
    g = s->lpGbl;
    if (!g || !D3_LIVE(s)) return 0;
    o->bits = (swu8 *)D3_ADDR(s);
    o->w = g->wWidth; o->h = g->wHeight;
    o->fmt = fmt_of(s);
    /* compressed: lPitch of a surface in system memory is its size; in
       video memory the driver set the pitch of a row of blocks */
    if (o->fmt >= SWF_DXT1 && o->fmt <= SWF_DXT5 && !(s->ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY))
        o->pitch = (sws32)((o->w + 3) / 4 * (o->fmt == SWF_DXT1 ? 8 : 16));
    else
        o->pitch = g->lPitch;
    return o->fmt != SWF_NONE && o->w > 0 && o->h > 0;
}

/* a texture and its mipmap levels (attached, half the size each) */
static void tex_build(D3SURF s, SWTEX *t)
{
    D3SURF cur = s;
    memset(t, 0, sizeof(*t));
    while (cur && t->nlvl < SW_MAXLEVELS) {
        D3ATTACH al;
        D3SURF next = NULL;
        if (!surf_sw(cur, &t->lvl[t->nlvl])) break;
        t->nlvl++;
        if (!(cur->ddsCaps.dwCaps & DDSCAPS_MIPMAP)) break;
        for (al = cur->lpAttachList; al; al = al->lpLink)
            if (al->lpAttached && (al->lpAttached->ddsCaps.dwCaps & DDSCAPS_MIPMAP) &&
                al->lpAttached->lpGbl->wWidth <= cur->lpGbl->wWidth &&
                al->lpAttached->lpGbl->wHeight <= cur->lpGbl->wHeight &&
                al->lpAttached != cur) { next = al->lpAttached; break; }
        cur = next;
    }
    if (s->dwFlags & DDRAWISURF_HASCKEYSRCBLT) {
        t->has_ckey = 1;
        t->ckey_lo = s->ddckCKSrcBlt.dwColorSpaceLowValue;
        t->ckey_hi = s->ddckCKSrcBlt.dwColorSpaceHighValue;
        if (t->ckey_hi < t->ckey_lo) t->ckey_hi = t->ckey_lo;
    }
}

/* ---------------- contexts ---------------- */

#define MAXSETS 64

typedef struct {
    DWORD n, cap;
    DWORD *ent;                         /* (kind << 24 | stage << 16 | state), value */
} STSET;

typedef struct {
    SWCTX sw;
    void *dd;                           /* handle owner: DD local (DX7) or the context (DX6) */
    int dx7;
    D3SURF rt, zb;
    DWORD texh[SW_MAXTEX];
    SWTEX tex[SW_MAXTEX];
    STSET sets[MAXSETS];
    int rec;                            /* recording a state set: its number + 1 */
    float vx, vy, vw, vh;               /* viewport */
} D3CTX;

typedef HRESULT (CALLBACK *PARSEUNKNOWN)(LPVOID cmd, LPVOID *next);
static PARSEUNKNOWN parse_unknown D3_SHARED = NULL;

static void ctx_targets(D3CTX *c)
{
    surf_sw(c->rt, &c->sw.rt);
    if (!surf_sw(c->zb, &c->sw.zb)) c->sw.zb.fmt = SWF_NONE;
    c->sw.clip[0] = c->sw.clip[1] = 0;
    c->sw.clip[2] = c->sw.rt.w; c->sw.clip[3] = c->sw.rt.h;
}

/* a surface pointer of the DirectX 6 interface (IDirectDrawSurface) or
   DirectX 7 (the local surface structure) */
#ifdef D3_LEGACY
static D3SURF lcl_of(void *p, int dx7)
{
    if (!p) return NULL;
    return dx7 ? (D3SURF)p : ((LPDDRAWI_DDRAWSURFACE_INT)p)->lpLcl;
}

/* DirectX 7 passes local structures, older runtimes interfaces: an
   interface starts with its vtable inside DDRAW.DLL */
static int is_dx7_ptr(void *p)
{
    static ULONG lo D3_SHARED = 0, hi D3_SHARED = 0;
    ULONG v;
    if (!lo) {
        HMODULE m = GetModuleHandleA("DDRAW.DLL");
        if (m) {
            IMAGE_DOS_HEADER *dh = (IMAGE_DOS_HEADER *)m;
            IMAGE_NT_HEADERS *nh = (IMAGE_NT_HEADERS *)((BYTE *)m + dh->e_lfanew);
            lo = (ULONG)m; hi = lo + nh->OptionalHeader.SizeOfImage;
        }
    }
    if (!p) return 1;
    v = *(ULONG *)p;
    return !(v >= lo && v < hi);
}
#else
/* Windows 2000/XP: always the DirectX 7 interface */
#define lcl_of(p, dx7) ((D3SURF)(p))
#define is_dx7_ptr(p) 1
#endif

static DWORD WINAPI D3ContextCreate(LPD3DHAL_CONTEXTCREATEDATA p)
{
    D3CTX *c = mem_alloc(sizeof(D3CTX));
    p->ddrval = D3DHAL_OUTOFCONTEXTS;
    if (!c) return DDHAL_DRIVER_HANDLED;
    memset(c, 0, sizeof(*c));
    c->dx7 = is_dx7_ptr(p->lpDDS);
    c->dd = c->dx7 ? (void *)p->lpDDLcl : (void *)c;
    c->rt = lcl_of(p->lpDDS, c->dx7);
    c->zb = lcl_of(p->lpDDSZ, c->dx7);
    D3_FPU_BEGIN;
    sw_init(&c->sw);
    if (c->zb) c->sw.rs[7] = 1;                 /* D3DRENDERSTATE_ZENABLE */
    ctx_targets(c);
    D3_FPU_END;
    D3_DBG("D3D ContextCreate", (ULONG)(ULONG_PTR)c, c->dx7);
    p->dwhContext = (ULONG_PTR)c;
    p->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static void ctx_free(D3CTX *c)
{
    int i;
    for (i = 0; i < MAXSETS; i++) mem_free(c->sets[i].ent);
    if (!c->dx7) h_drop(c);
    mem_free(c);
}

static DWORD WINAPI D3ContextDestroy(LPD3DHAL_CONTEXTDESTROYDATA p)
{
    if (p->dwhContext) ctx_free((D3CTX *)p->dwhContext);
    p->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI D3ContextDestroyAll(LPD3DHAL_CONTEXTDESTROYALLDATA p)
{
    p->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

#ifdef D3_LEGACY
/* DirectX 6: texture handles */
static DWORD WINAPI D3TextureCreate(LPD3DHAL_TEXTURECREATEDATA p)
{
    D3CTX *c = (D3CTX *)p->dwhContext;
    D3SURF s = lcl_of(p->lpDDS, 0);
    if (!c || !s) { p->ddrval = DDERR_INVALIDPARAMS; return DDHAL_DRIVER_HANDLED; }
    p->dwHandle = h_next++;
    h_set(c->dd, p->dwHandle, s);
    p->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI D3TextureDestroy(LPD3DHAL_TEXTUREDESTROYDATA p)
{
    D3CTX *c = (D3CTX *)p->dwhContext;
    if (c) h_set(c->dd, p->dwHandle, NULL);
    p->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI D3TextureSwap(LPD3DHAL_TEXTURESWAPDATA p)
{
    D3CTX *c = (D3CTX *)p->dwhContext;
    if (c) {
        D3SURF a = h_get(c->dd, p->dwHandle1), b = h_get(c->dd, p->dwHandle2);
        if (a && b) { h_set(c->dd, p->dwHandle1, b); h_set(c->dd, p->dwHandle2, a); }
    }
    p->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI D3TextureGetSurf(LPD3DHAL_TEXTUREGETSURFDATA p)
{
    p->ddrval = DDERR_UNSUPPORTED;
    return DDHAL_DRIVER_HANDLED;
}

/* DirectX 5/6: SetRenderTarget */
static DWORD WINAPI D3SetRenderTarget(LPD3DHAL_SETRENDERTARGETDATA p)
{
    D3CTX *c = (D3CTX *)p->dwhContext;
    if (c) {
        c->rt = lcl_of(p->lpDDS, c->dx7);
        c->zb = lcl_of(p->lpDDSZ, c->dx7);
        ctx_targets(c);
    }
    p->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}
#endif

/* DirectX 7: surface handles */
static void reg_chain(void *dd, D3SURF s, int depth)
{
    D3ATTACH al;
    if (!s || !s->lpSurfMore || depth > 32) return;
    {
        DWORD i;
        for (i = 0; i < h_n; i++)
            if (h_tab[i].dd == dd && h_tab[i].s == s && h_tab[i].h == s->lpSurfMore->dwSurfaceHandle) return;  /* seen */
        h_add(dd, s->lpSurfMore->dwSurfaceHandle, s, 1);
    }
    for (al = s->lpAttachList; al; al = al->lpLink) reg_chain(dd, al->lpAttached, depth + 1);
}

static DWORD WINAPI D3CreateSurfaceEx(D3_CSEXDATA p)
{
    D3SURF s = p->lpDDSLcl;
    p->ddRVal = DD_OK;
    if (s) D3_DBG("CreateSurfaceEx", s->ddsCaps.dwCaps, s->lpSurfMore ? s->lpSurfMore->dwSurfaceHandle : 0);
    if (!s || !p->lpDDLcl) return DDHAL_DRIVER_HANDLED;
    if (!s->lpGbl || !D3_LIVE(s)) {             /* the surface goes away */
        h_del(p->lpDDLcl, 0, s);
        return DDHAL_DRIVER_HANDLED;
    }
    reg_chain(p->lpDDLcl, s, 0);
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI D3DestroyDDLocal(D3_DDLDATA p)
{
    h_drop(p->pDDLcl);
    p->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI D3GetDriverState(D3_GDSDATA p)
{
    p->ddRVal = DDERR_UNSUPPORTED;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI D3ValidateTSS(LPD3DHAL_VALIDATETEXTURESTAGESTATEDATA p)
{
    p->dwNumPasses = 1;
    p->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI D3Clear2(LPD3DHAL_CLEAR2DATA p)
{
    D3CTX *c = (D3CTX *)p->dwhContext;
    DWORD i;
    if (c) {
        D3_FPU_BEGIN;
        ctx_targets(c);
        sw_prepare(&c->sw);
        for (i = 0; i < p->dwNumRects; i++)
            sw_clear(&c->sw, p->dwFlags & 7, p->dwFillColor, p->dvFillDepth, p->dwFillStencil,
                     p->lpRects[i].x1, p->lpRects[i].y1, p->lpRects[i].x2, p->lpRects[i].y2);
        D3_FPU_END;
    }
    p->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* ---------------- DrawPrimitives2 ---------------- */

/* the position of each element in a vertex of format fvf */
typedef struct {
    DWORD size;
    int dif, spec, ntex;
    int tex[8], tdim[8];
} FVF;

static void fvf_parse(DWORD fvf, FVF *f)
{
    int o = 0, i;
    memset(f, 0, sizeof(*f));
    f->dif = f->spec = -1;
    switch (fvf & 0x0E) {                       /* D3DFVF_POSITION_MASK */
    case 0x02: o = 12; break;                   /* XYZ */
    case 0x04: o = 16; break;                   /* XYZRHW */
    case 0x06: o = 16; break; case 0x08: o = 20; break; case 0x0A: o = 24; break;
    case 0x0C: o = 28; break; case 0x0E: o = 32; break;
    default: o = 16; break;
    }
    if (fvf & 0x10) o += 12;                    /* NORMAL */
    if (fvf & 0x20) o += 4;                     /* RESERVED1 / PSIZE */
    if (fvf & 0x40) { f->dif = o; o += 4; }
    if (fvf & 0x80) { f->spec = o; o += 4; }
    f->ntex = (int)((fvf >> 8) & 15);
    if (f->ntex > 8) f->ntex = 8;
    for (i = 0; i < f->ntex; i++) {
        static const int dims[4] = { 2, 3, 4, 1 };
        int d = dims[(fvf >> (16 + i * 2)) & 3];
        f->tex[i] = o; f->tdim[i] = d;
        o += d * 4;
    }
    f->size = (DWORD)o;
}

static void vtx_load(const FVF *f, const BYTE *p, SWVTX *v)
{
    const float *fp = (const float *)p;
    DWORD d;
    int i;
    v->x = fp[0]; v->y = fp[1]; v->z = fp[2]; v->rhw = fp[3];
    d = f->dif >= 0 ? *(const DWORD *)(p + f->dif) : 0xFFFFFFFFu;
    v->c[0] = (float)((d >> 16) & 255) * (1.0f / 255); v->c[1] = (float)((d >> 8) & 255) * (1.0f / 255);
    v->c[2] = (float)(d & 255) * (1.0f / 255); v->c[3] = (float)(d >> 24) * (1.0f / 255);
    d = f->spec >= 0 ? *(const DWORD *)(p + f->spec) : 0xFF000000u;
    v->s[0] = (float)((d >> 16) & 255) * (1.0f / 255); v->s[1] = (float)((d >> 8) & 255) * (1.0f / 255);
    v->s[2] = (float)(d & 255) * (1.0f / 255); v->s[3] = (float)(d >> 24) * (1.0f / 255);
    for (i = 0; i < SW_MAXTEX; i++) {
        if (i < f->ntex) {
            const float *t = (const float *)(p + f->tex[i]);
            v->t[i][0] = t[0];
            v->t[i][1] = f->tdim[i] > 1 ? t[1] : 0;
        } else
            v->t[i][0] = v->t[i][1] = 0;
    }
}

typedef struct {
    D3CTX *c;
    const BYTE *vb;
    DWORD vbsize, stride;
    FVF f;
} DRAW;

static int vget(DRAW *d, DWORD i, SWVTX *v)
{
    if ((i + 1) * d->stride > d->vbsize) return 0;
    vtx_load(&d->f, d->vb + i * d->stride, v);
    return 1;
}

static void tri(DRAW *d, DWORD a, DWORD b, DWORD e)
{
    SWVTX va, vb, ve;
    if (vget(d, a, &va) && vget(d, b, &vb) && vget(d, e, &ve)) sw_triangle(&d->c->sw, &va, &vb, &ve);
}

static void line(DRAW *d, DWORD a, DWORD b)
{
    SWVTX va, vb;
    if (vget(d, a, &va) && vget(d, b, &vb)) sw_line(&d->c->sw, &va, &vb);
}

/* bind textures and the stage states before drawing */
static void ctx_bind(D3CTX *c)
{
    int i;
    for (i = 0; i < SW_MAXTEX; i++) {
        D3SURF s = c->texh[i] ? h_get(c->dd, c->texh[i]) : NULL;
        if (s) { tex_build(s, &c->tex[i]); c->sw.tex[i] = c->tex[i].nlvl ? &c->tex[i] : NULL; }
        else c->sw.tex[i] = NULL;
    }
    sw_prepare(&c->sw);
}

static void set_rs(D3CTX *c, LPD3DHAL_DRAWPRIMITIVES2DATA pd, DWORD st, DWORD v)
{
    if (st < SWRS_MAX) c->sw.rs[st] = v;
    if (pd->lpdwRStates && st < 256) pd->lpdwRStates[st] = v;
    /* legacy states of DirectX 5 programs */
    switch (st) {
    case 1: c->texh[0] = v; break;                              /* TEXTUREHANDLE */
    case 3: c->sw.tss[0][12] = c->sw.tss[0][13] = c->sw.tss[0][14] = v; break;    /* TEXTUREADDRESS */
    case 44: c->sw.tss[0][13] = v; break;                       /* TEXTUREADDRESSU */
    case 45: c->sw.tss[0][14] = v; break;                       /* TEXTUREADDRESSV */
    case 17: c->sw.tss[0][16] = v == 1 ? 1 : 2; break;          /* TEXTUREMAG */
    case 18:                                                    /* TEXTUREMIN */
        c->sw.tss[0][17] = (v == 1 || v == 3 || v == 5) ? 1 : 2;
        c->sw.tss[0][18] = v >= 3 ? (v >= 5 ? 3 : 2) : 1;
        break;
    case 21:                                                    /* TEXTUREMAPBLEND */
        switch (v) {
        case 1: case 3:                                         /* DECAL, COPY */
            c->sw.tss[0][1] = 2; c->sw.tss[0][2] = 2; c->sw.tss[0][4] = 2; c->sw.tss[0][5] = 2; break;
        case 2:                                                 /* MODULATE */
            c->sw.tss[0][1] = 4; c->sw.tss[0][2] = 2; c->sw.tss[0][3] = 1;
            c->sw.tss[0][4] = 2; c->sw.tss[0][5] = 2; break;
        case 4:                                                 /* MODULATEALPHA */
            c->sw.tss[0][1] = 4; c->sw.tss[0][2] = 2; c->sw.tss[0][3] = 1;
            c->sw.tss[0][4] = 4; c->sw.tss[0][5] = 2; c->sw.tss[0][6] = 1; break;
        case 5:                                                 /* DECALALPHA */
            c->sw.tss[0][1] = 13; c->sw.tss[0][2] = 2; c->sw.tss[0][3] = 1;
            c->sw.tss[0][4] = 3; c->sw.tss[0][6] = 1; break;
        case 8:                                                 /* ADD */
            c->sw.tss[0][1] = 7; c->sw.tss[0][2] = 2; c->sw.tss[0][3] = 1;
            c->sw.tss[0][4] = 3; c->sw.tss[0][6] = 1; break;
        }
        break;
    }
}

static void set_tss(D3CTX *c, DWORD stage, DWORD st, DWORD v)
{
    if (stage >= SW_MAXTEX) return;
    if (st == 0) { c->texh[stage] = v; return; }               /* D3DTSS_TEXTUREMAP */
    if (st < 32) c->sw.tss[stage][st] = v;
    if (st == 12) c->sw.tss[stage][13] = c->sw.tss[stage][14] = v;     /* ADDRESS: both */
}

/* state sets (DirectX 7 state blocks) */
static void set_rec(D3CTX *c, DWORD kind, DWORD stage, DWORD st, DWORD v)
{
    STSET *s;
    if (!c->rec) return;
    s = &c->sets[c->rec - 1];
    if (s->n + 2 > s->cap) {
        DWORD nc = s->cap ? s->cap * 2 : 64;
        DWORD *n = mem_alloc(nc * sizeof(DWORD));
        if (!n) return;
        if (s->ent) { memcpy(n, s->ent, s->n * sizeof(DWORD)); mem_free(s->ent); }
        s->ent = n; s->cap = nc;
    }
    s->ent[s->n++] = (kind << 24) | (stage << 16) | (st & 0xFFFF);
    s->ent[s->n++] = v;
}

static void stateset(D3CTX *c, LPD3DHAL_DRAWPRIMITIVES2DATA pd, const D3DHAL_DP2STATESET *ss)
{
    DWORD n = ss->dwParam, i;
    STSET *s;
    if (n == 0 || n > MAXSETS) { if (ss->dwOperation == D3DHAL_STATESETEND) c->rec = 0; return; }
    s = &c->sets[n - 1];
    switch (ss->dwOperation) {
    case D3DHAL_STATESETBEGIN: s->n = 0; c->rec = (int)n; break;
    case D3DHAL_STATESETEND: c->rec = 0; break;
    case D3DHAL_STATESETDELETE: s->n = 0; break;
    case D3DHAL_STATESETEXECUTE:
        for (i = 0; i + 1 < s->n; i += 2) {
            DWORD k = s->ent[i] >> 24, stg = (s->ent[i] >> 16) & 255, st = s->ent[i] & 0xFFFF;
            if (k == 0) set_rs(c, pd, st, s->ent[i + 1]);
            else set_tss(c, stg, st, s->ent[i + 1]);
        }
        break;
    case D3DHAL_STATESETCAPTURE:
        for (i = 0; i + 1 < s->n; i += 2) {
            DWORD k = s->ent[i] >> 24, stg = (s->ent[i] >> 16) & 255, st = s->ent[i] & 0xFFFF;
            if (k == 0) s->ent[i + 1] = st < SWRS_MAX ? c->sw.rs[st] : 0;
            else if (stg < SW_MAXTEX) s->ent[i + 1] = st == 0 ? c->texh[stg] : st < 32 ? c->sw.tss[stg][st] : 0;
        }
        break;
    }
}

static DWORD WINAPI D3DrawPrimitives2(LPD3DHAL_DRAWPRIMITIVES2DATA pd)
{
    D3CTX *c = (D3CTX *)pd->dwhContext;
    const BYTE *cmd, *end;
    DRAW d;
    int bound = 0;
    if (!c || !pd->lpDDCommands || !pd->lpDDCommands->lpGbl) {
        pd->ddrval = DDERR_INVALIDPARAMS;
        return DDHAL_DRIVER_HANDLED;
    }
    D3_FPU_BEGIN;
    cmd = (const BYTE *)D3_ADDR(pd->lpDDCommands) + pd->dwCommandOffset;
    end = cmd + pd->dwCommandLength;
    d.c = c;
    fvf_parse(pd->dwVertexType, &d.f);
    if (pd->dwFlags & D3DHALDP2_USERMEMVERTICES) d.vb = (const BYTE *)pd->lpVertices;
    else d.vb = pd->lpDDVertex && pd->lpDDVertex->lpGbl ? (const BYTE *)D3_ADDR(pd->lpDDVertex) : NULL;
    d.stride = (pd->dwFlags & D3DHALDP2_USERMEMVERTICES) && pd->dwVertexSize ? pd->dwVertexSize : d.f.size;
    if (d.vb) d.vb += pd->dwVertexOffset;
    d.vbsize = pd->dwVertexLength * d.stride + 0x7FFFFFF;      /* the runtime's lengths are in vertices */
    ctx_targets(c);
    pd->ddrval = DD_OK;

    while (cmd + sizeof(D3DHAL_DP2COMMAND) <= end) {
        const D3DHAL_DP2COMMAND *h = (const D3DHAL_DP2COMMAND *)cmd;
        const BYTE *p = cmd + sizeof(D3DHAL_DP2COMMAND);
        DWORD n = h->wPrimitiveCount, i;
        int draw = h->bCommand <= 3 || (h->bCommand >= 15 && h->bCommand <= 24) || h->bCommand == 26 ||
                   h->bCommand == 27;
        if (draw && !bound) { ctx_bind(c); bound = 1; }
        switch (h->bCommand) {
        case D3DDP2OP_POINTS: {
            const D3DHAL_DP2POINTS *q = (const D3DHAL_DP2POINTS *)p;
            for (i = 0; i < n; i++) {
                DWORD k;
                for (k = 0; k < q[i].wCount; k++) {
                    SWVTX v;
                    if (vget(&d, q[i].wVStart + k, &v)) sw_point(&c->sw, &v);
                }
            }
            p += n * sizeof(*q);
            break;
        }
        case D3DDP2OP_LINELIST: {
            DWORD s = ((const D3DHAL_DP2LINELIST *)p)->wVStart;
            for (i = 0; i < n; i++) line(&d, s + i * 2, s + i * 2 + 1);
            p += sizeof(D3DHAL_DP2LINELIST);
            break;
        }
        case D3DDP2OP_INDEXEDLINELIST: {
            const WORD *q = (const WORD *)p;
            for (i = 0; i < n; i++) line(&d, q[i * 2], q[i * 2 + 1]);
            p += n * 4;
            break;
        }
        case D3DDP2OP_LINESTRIP: {
            DWORD s = ((const D3DHAL_DP2LINESTRIP *)p)->wVStart;
            for (i = 0; i < n; i++) line(&d, s + i, s + i + 1);
            p += sizeof(D3DHAL_DP2LINESTRIP);
            break;
        }
        case D3DDP2OP_INDEXEDLINESTRIP: {
            const WORD *q = (const WORD *)p;
            for (i = 0; i < n; i++) line(&d, q[i], q[i + 1]);
            p += (n + 1) * 2;
            break;
        }
        case D3DDP2OP_TRIANGLELIST: {
            DWORD s = ((const D3DHAL_DP2TRIANGLELIST *)p)->wVStart;
            for (i = 0; i < n; i++) tri(&d, s + i * 3, s + i * 3 + 1, s + i * 3 + 2);
            p += sizeof(D3DHAL_DP2TRIANGLELIST);
            break;
        }
        case D3DDP2OP_INDEXEDTRIANGLELIST: {
            const WORD *q = (const WORD *)p;
            for (i = 0; i < n; i++) tri(&d, q[i * 4], q[i * 4 + 1], q[i * 4 + 2]);
            p += n * 8;
            break;
        }
        case D3DDP2OP_TRIANGLESTRIP: {
            DWORD s = ((const D3DHAL_DP2TRIANGLESTRIP *)p)->wVStart;
            for (i = 0; i < n; i++)
                if (i & 1) tri(&d, s + i + 1, s + i, s + i + 2);
                else tri(&d, s + i, s + i + 1, s + i + 2);
            p += sizeof(D3DHAL_DP2TRIANGLESTRIP);
            break;
        }
        case D3DDP2OP_INDEXEDTRIANGLESTRIP: {
            const WORD *q = (const WORD *)p;
            for (i = 0; i < n; i++)
                if (i & 1) tri(&d, q[i + 1], q[i], q[i + 2]);
                else tri(&d, q[i], q[i + 1], q[i + 2]);
            p += (n + 2) * 2;
            break;
        }
        case D3DDP2OP_TRIANGLEFAN: {
            DWORD s = ((const D3DHAL_DP2TRIANGLEFAN *)p)->wVStart;
            for (i = 0; i < n; i++) tri(&d, s, s + i + 1, s + i + 2);
            p += sizeof(D3DHAL_DP2TRIANGLEFAN);
            break;
        }
        case D3DDP2OP_INDEXEDTRIANGLEFAN: {
            const WORD *q = (const WORD *)p;
            for (i = 0; i < n; i++) tri(&d, q[0], q[i + 1], q[i + 2]);
            p += (n + 2) * 2;
            break;
        }
        case D3DDP2OP_TRIANGLEFAN_IMM: {
            DRAW im = d;
            p += sizeof(D3DHAL_DP2TRIANGLEFAN_IMM);
            p = (const BYTE *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3);
            im.vb = p; im.vbsize = (n + 2) * d.f.size; im.stride = d.f.size;
            for (i = 0; i < n; i++) tri(&im, 0, i + 1, i + 2);
            p += (n + 2) * d.f.size;
            break;
        }
        case D3DDP2OP_LINELIST_IMM: {
            DRAW im = d;
            im.vb = p; im.vbsize = n * 2 * d.f.size; im.stride = d.f.size;
            for (i = 0; i < n; i++) line(&im, i * 2, i * 2 + 1);
            p += n * 2 * d.f.size;
            break;
        }
        case D3DDP2OP_INDEXEDTRIANGLELIST2: {
            DWORD s = ((const D3DHAL_DP2STARTVERTEX *)p)->wVStart;
            const WORD *q = (const WORD *)(p + sizeof(D3DHAL_DP2STARTVERTEX));
            for (i = 0; i < n; i++) tri(&d, s + q[i * 3], s + q[i * 3 + 1], s + q[i * 3 + 2]);
            p += sizeof(D3DHAL_DP2STARTVERTEX) + n * 6;
            break;
        }
        case D3DDP2OP_INDEXEDLINELIST2: {
            DWORD s = ((const D3DHAL_DP2STARTVERTEX *)p)->wVStart;
            const WORD *q = (const WORD *)(p + sizeof(D3DHAL_DP2STARTVERTEX));
            for (i = 0; i < n; i++) line(&d, s + q[i * 2], s + q[i * 2 + 1]);
            p += sizeof(D3DHAL_DP2STARTVERTEX) + n * 4;
            break;
        }
        case D3DDP2OP_RENDERSTATE: {
            const D3DHAL_DP2RENDERSTATE *q = (const D3DHAL_DP2RENDERSTATE *)p;
            for (i = 0; i < n; i++) {
                set_rs(c, pd, (DWORD)q[i].RenderState, q[i].dwState);
                set_rec(c, 0, 0, (DWORD)q[i].RenderState, q[i].dwState);
            }
            p += n * sizeof(*q);
            bound = 0;
            break;
        }
        case D3DDP2OP_TEXTURESTAGESTATE: {
            const D3DHAL_DP2TEXTURESTAGESTATE *q = (const D3DHAL_DP2TEXTURESTAGESTATE *)p;
            for (i = 0; i < n; i++) {
                set_tss(c, q[i].wStage, q[i].TSState, q[i].dwValue);
                set_rec(c, 1, q[i].wStage, q[i].TSState, q[i].dwValue);
            }
            p += n * sizeof(*q);
            bound = 0;
            break;
        }
        case D3DDP2OP_VIEWPORTINFO: {
            const D3DHAL_DP2VIEWPORTINFO *q = (const D3DHAL_DP2VIEWPORTINFO *)p;
            if (n) { c->vx = (float)q[n - 1].dwX; c->vy = (float)q[n - 1].dwY; c->vw = (float)q[n - 1].dwWidth; c->vh = (float)q[n - 1].dwHeight; }
            p += n * sizeof(*q);
            break;
        }
        case D3DDP2OP_WINFO: {
            const D3DHAL_DP2WINFO *q = (const D3DHAL_DP2WINFO *)p;
            if (n) { c->sw.wnear = q[n - 1].dwWNear; c->sw.wfar = q[n - 1].dwWFar; }
            p += n * sizeof(*q);
            break;
        }
        case D3DDP2OP_ZRANGE: p += n * sizeof(D3DHAL_DP2ZRANGE); break;
        case D3DDP2OP_SETMATERIAL: p += n * sizeof(D3DHAL_DP2SETMATERIAL); break;
        case D3DDP2OP_SETLIGHT: {
            for (i = 0; i < n; i++) {
                const D3DHAL_DP2SETLIGHT *q = (const D3DHAL_DP2SETLIGHT *)p;
                p += sizeof(*q);
                if (q->dwDataType == D3DHAL_SETLIGHT_DATA) p += sizeof(D3DLIGHT7);
            }
            break;
        }
        case D3DDP2OP_CREATELIGHT: p += n * sizeof(D3DHAL_DP2CREATELIGHT); break;
        case D3DDP2OP_SETTRANSFORM: p += n * sizeof(D3DHAL_DP2SETTRANSFORM); break;
        case D3DDP2OP_SETPRIORITY: p += n * sizeof(D3DHAL_DP2SETPRIORITY); break;
        case D3DDP2OP_SETTEXLOD: {
            const D3DHAL_DP2SETTEXLOD *q = (const D3DHAL_DP2SETTEXLOD *)p;
            p += n * sizeof(*q);
            (void)q;
            break;
        }
        case D3DPP2OP_SETCLIPPLANE: p += n * (sizeof(DWORD) + 4 * sizeof(D3DVALUE)); break;
        case D3DDP2OP_STATESET: {
            const D3DHAL_DP2STATESET *q = (const D3DHAL_DP2STATESET *)p;
            for (i = 0; i < n; i++) stateset(c, pd, &q[i]);
            p += n * sizeof(*q);
            bound = 0;
            break;
        }
        case D3DDP2OP_SETRENDERTARGET: {
            const D3DHAL_DP2SETRENDERTARGET *q = (const D3DHAL_DP2SETRENDERTARGET *)p;
            if (n) {
                D3SURF r = h_get(c->dd, q[n - 1].hRenderTarget);
                c->rt = r ? r : c->rt;
                c->zb = q[n - 1].hZBuffer ? h_get(c->dd, q[n - 1].hZBuffer) : NULL;
                ctx_targets(c);
            }
            p += n * sizeof(*q);
            bound = 0;
            break;
        }
        case D3DDP2OP_CLEAR: {
            const D3DHAL_DP2CLEAR *q = (const D3DHAL_DP2CLEAR *)p;
            ctx_targets(c);
            sw_prepare(&c->sw);
            for (i = 0; i < n; i++)
                sw_clear(&c->sw, q->dwFlags & 7, q->dwFillColor, q->dvFillDepth, q->dwFillStencil,
                         q->Rects[i].left, q->Rects[i].top, q->Rects[i].right, q->Rects[i].bottom);
            p += sizeof(D3DHAL_DP2CLEAR) - sizeof(RECT) + n * sizeof(RECT);
            bound = 0;
            break;
        }
        case D3DDP2OP_TEXBLT: {
            const D3DHAL_DP2TEXBLT *q = (const D3DHAL_DP2TEXBLT *)p;
            for (i = 0; i < n; i++) {
                D3SURF ds = h_get(c->dd, q[i].dwDDDestSurface);
                D3SURF ss = h_get(c->dd, q[i].dwDDSrcSurface);
                /* a copy of the source level by level (the same format and size) */
                while (ds && ss) {
                    SWSURF a, b;
                    D3ATTACH al;
                    D3SURF nd = NULL, ns = NULL;
                    if (surf_sw(ds, &a) && surf_sw(ss, &b) && a.fmt == b.fmt && a.w == b.w && a.h == b.h) {
                        int bpp = sw_bpp(a.fmt);
                        sws32 rows = a.fmt >= SWF_DXT1 && a.fmt <= SWF_DXT5 ? (a.h + 3) / 4 : a.h, y;
                        DWORD bytes = a.fmt >= SWF_DXT1 && a.fmt <= SWF_DXT5 ? (DWORD)b.pitch : (DWORD)(a.w * bpp / 8);
                        for (y = 0; y < rows; y++) memcpy(a.bits + y * a.pitch, b.bits + y * b.pitch, bytes);
                    }
                    if (!(ds->ddsCaps.dwCaps & DDSCAPS_MIPMAP)) break;
                    for (al = ds->lpAttachList; al; al = al->lpLink)
                        if (al->lpAttached && (al->lpAttached->ddsCaps.dwCaps & DDSCAPS_MIPMAP)) { nd = al->lpAttached; break; }
                    for (al = ss->lpAttachList; al; al = al->lpLink)
                        if (al->lpAttached && (al->lpAttached->ddsCaps.dwCaps & DDSCAPS_MIPMAP)) { ns = al->lpAttached; break; }
                    ds = nd; ss = ns;
                }
            }
            p += n * sizeof(*q);
            break;
        }
        case D3DDP2OP_SETPALETTE: p += n * sizeof(D3DHAL_DP2SETPALETTE); break;
        case D3DDP2OP_UPDATEPALETTE: {
            const D3DHAL_DP2UPDATEPALETTE *q = (const D3DHAL_DP2UPDATEPALETTE *)p;
            p += sizeof(*q) + q->wNumEntries * sizeof(DWORD);
            break;
        }
        default:
            if (parse_unknown) {
                LPDWORD next = NULL;
                HRESULT r = parse_unknown((LPVOID)cmd, (LPVOID *)&next);
                if (r == DD_OK && next) { cmd = (const BYTE *)next; continue; }
            }
            pd->ddrval = D3DERR_COMMAND_UNPARSED_;
            pd->dwErrorOffset = (DWORD)(cmd - ((const BYTE *)D3_ADDR(pd->lpDDCommands) + pd->dwCommandOffset));
            D3_FPU_END;
            return DDHAL_DRIVER_HANDLED;
        }
        cmd = p;
    }
    D3_FPU_END;
    return DDHAL_DRIVER_HANDLED;
}

/* ---------------- capabilities ---------------- */

#define TF_RGB(bits, r, g, b, a, fl) { sizeof(DDSURFACEDESC), DDSD_CAPS | DDSD_PIXELFORMAT, 0, 0, { 0 }, 0, { 0 }, 0, 0, NULL, \
    { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { sizeof(DDPIXELFORMAT), DDPF_RGB | (fl), 0, { bits }, { r }, { g }, { b }, { a } }, \
    { DDSCAPS_TEXTURE } }
#define TF_FOURCC(a, b, c, d) { sizeof(DDSURFACEDESC), DDSD_CAPS | DDSD_PIXELFORMAT, 0, 0, { 0 }, 0, { 0 }, 0, 0, NULL, \
    { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { sizeof(DDPIXELFORMAT), DDPF_FOURCC, MAKEFOURCC(a, b, c, d), { 0 }, { 0 }, { 0 }, { 0 }, { 0 } }, \
    { DDSCAPS_TEXTURE } }
#define TF_LUM(bits, l, a, fl) { sizeof(DDSURFACEDESC), DDSD_CAPS | DDSD_PIXELFORMAT, 0, 0, { 0 }, 0, { 0 }, 0, 0, NULL, \
    { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { sizeof(DDPIXELFORMAT), (fl), 0, { bits }, { l }, { 0 }, { 0 }, { a } }, \
    { DDSCAPS_TEXTURE } }

static DDSURFACEDESC tex_formats[] D3_SHARED = {
    TF_RGB(16, 0xF800, 0x07E0, 0x001F, 0, 0),
    TF_RGB(16, 0x7C00, 0x03E0, 0x001F, 0, 0),
    TF_RGB(16, 0x7C00, 0x03E0, 0x001F, 0x8000, DDPF_ALPHAPIXELS),
    TF_RGB(16, 0x0F00, 0x00F0, 0x000F, 0xF000, DDPF_ALPHAPIXELS),
    TF_RGB(32, 0xFF0000, 0xFF00, 0xFF, 0, 0),
    TF_RGB(32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000u, DDPF_ALPHAPIXELS),
    TF_RGB(8, 0xE0, 0x1C, 0x03, 0, 0),
    TF_LUM(8, 0xFF, 0, 0x00020000),                         /* L8 */
    TF_LUM(16, 0xFF, 0xFF00, 0x00020000 | DDPF_ALPHAPIXELS),  /* A8L8 */
    TF_FOURCC('D','X','T','1'),
    TF_FOURCC('D','X','T','2'),
    TF_FOURCC('D','X','T','3'),
    TF_FOURCC('D','X','T','4'),
    TF_FOURCC('D','X','T','5'),
};

#define PRIMCAPS { sizeof(D3DPRIMCAPS), \
    D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW | D3DPMISCCAPS_MASKZ, \
    D3DPRASTERCAPS_SUBPIXEL | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE | \
    D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_WBUFFER | D3DPRASTERCAPS_WFOG | D3DPRASTERCAPS_ZFOG | \
    D3DPRASTERCAPS_DITHER, \
    0xFF, 0x1FFF, 0x1FFF, 0xFF, \
    D3DPSHADECAPS_COLORFLATRGB | D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARFLATRGB | \
    D3DPSHADECAPS_SPECULARGOURAUDRGB | D3DPSHADECAPS_ALPHAFLATBLEND | D3DPSHADECAPS_ALPHAGOURAUDBLEND | \
    D3DPSHADECAPS_FOGFLAT | D3DPSHADECAPS_FOGGOURAUD, \
    D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_TRANSPARENCY | D3DPTEXTURECAPS_BORDER, \
    D3DPTFILTERCAPS_NEAREST | D3DPTFILTERCAPS_LINEAR | D3DPTFILTERCAPS_MIPNEAREST | D3DPTFILTERCAPS_MIPLINEAR | \
    D3DPTFILTERCAPS_LINEARMIPNEAREST | D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | \
    D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR, \
    D3DPTBLENDCAPS_DECAL | D3DPTBLENDCAPS_MODULATE | D3DPTBLENDCAPS_DECALALPHA | D3DPTBLENDCAPS_MODULATEALPHA | \
    D3DPTBLENDCAPS_COPY | D3DPTBLENDCAPS_ADD, \
    D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP | D3DPTADDRESSCAPS_BORDER | \
    D3DPTADDRESSCAPS_INDEPENDENTUV, \
    0, 0 }

static D3DHAL_GLOBALDRIVERDATA d3d_global D3_SHARED = {
    sizeof(D3DHAL_GLOBALDRIVERDATA),
    { sizeof(D3DDEVICEDESC_V1),
      D3DDD_COLORMODEL | D3DDD_DEVCAPS | D3DDD_TRICAPS | D3DDD_LINECAPS | D3DDD_DEVICERENDERBITDEPTH |
      D3DDD_DEVICEZBUFFERBITDEPTH | D3DDD_BCLIPPING,
      D3DCOLOR_RGB,
      D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
      D3DDEVCAPS_TEXTURESYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX |
      D3DDEVCAPS_DRAWPRIMITIVES2_ | D3DDEVCAPS_DRAWPRIMITIVES2EX_ | D3DDEVCAPS_CANRENDERAFTERFLIP_ |
      D3DDEVCAPS_HWRASTERIZATION_,
      { sizeof(D3DTRANSFORMCAPS), 0 },
      TRUE,
      { sizeof(D3DLIGHTINGCAPS), 0, 0, 0 },
      PRIMCAPS, PRIMCAPS,
      DDBD_16 | DDBD_24 | DDBD_32,
      DDBD_16 | DDBD_24 | DDBD_32,
      0, 0xFFFF },
    0, 0,
    sizeof(tex_formats) / sizeof(tex_formats[0]),
    NULL
};

#define COPY_INFO(s) do { DWORD n_ = expected < sizeof(s) ? expected : (DWORD)sizeof(s); \
    *actual = sizeof(s); memcpy(data, &(s), n_); } while (0)

/* GetDriverInfo: the Direct3D parts. 1 when the GUID is one of these (its
   data copied, *actual its size), 0 for the others */
static int d3d_info(const GUID *guid, DWORD expected, void *data, DWORD *actual)
{
    if (guid_eq(guid, &G_D3DCallbacks3)) {
        D3DHAL_CALLBACKS3 cb;
        memset(&cb, 0, sizeof(cb));
        cb.dwSize = sizeof(cb);
        cb.dwFlags = D3DHAL3_CB32_CLEAR2 | D3DHAL3_CB32_VALIDATETEXTURESTAGESTATE | D3DHAL3_CB32_DRAWPRIMITIVES2;
        cb.Clear2 = D3Clear2;
        cb.ValidateTextureStageState = D3ValidateTSS;
        cb.DrawPrimitives2 = D3DrawPrimitives2;
        COPY_INFO(cb); return 1;
#ifdef D3_LEGACY
    } else if (guid_eq(guid, &G_D3DCallbacks2)) {
        D3DHAL_CALLBACKS2 cb;
        memset(&cb, 0, sizeof(cb));
        cb.dwSize = sizeof(cb);
        cb.dwFlags = D3DHAL2_CB32_SETRENDERTARGET;
        cb.SetRenderTarget = (LPD3DHAL_SETRENDERTARGETCB)D3SetRenderTarget;
        COPY_INFO(cb); return 1;
#endif
    } else if (guid_eq(guid, &G_Misc2Callbacks)) {
        D3_MISC2CB cb;
        memset(&cb, 0, sizeof(cb));
        cb.dwSize = sizeof(cb);
        cb.dwFlags = DDHAL_MISC2CB32_CREATESURFACEEX | DDHAL_MISC2CB32_GETDRIVERSTATE | DDHAL_MISC2CB32_DESTROYDDLOCAL;
        cb.CreateSurfaceEx = (void *)D3CreateSurfaceEx;
        cb.GetDriverState = (void *)D3GetDriverState;
        cb.DestroyDDLocal = (void *)D3DestroyDDLocal;
        COPY_INFO(cb); return 1;
    } else if (guid_eq(guid, &G_D3DExtendedCaps)) {
        D3DHAL_D3DEXTENDEDCAPS x;
        memset(&x, 0, sizeof(x));
        x.dwSize = sizeof(x);
        x.dwMinTextureWidth = x.dwMinTextureHeight = 1;
        x.dwMaxTextureWidth = x.dwMaxTextureHeight = 2048;
        x.dwMinStippleWidth = x.dwMaxStippleWidth = x.dwMinStippleHeight = x.dwMaxStippleHeight = 32;
        x.dwMaxTextureRepeat = 2048;
        x.dwMaxTextureAspectRatio = 2048;
        x.dvGuardBandLeft = -16384.0f; x.dvGuardBandTop = -16384.0f;
        x.dvGuardBandRight = 16383.0f; x.dvGuardBandBottom = 16383.0f;
        x.dwStencilCaps = 0xFF;                 /* KEEP .. DECR */
        x.dwFVFCaps = 8;
        x.dwTextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 | D3DTEXOPCAPS_SELECTARG2 |
            D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_MODULATE2X | D3DTEXOPCAPS_MODULATE4X | D3DTEXOPCAPS_ADD |
            D3DTEXOPCAPS_ADDSIGNED | D3DTEXOPCAPS_ADDSIGNED2X | D3DTEXOPCAPS_SUBTRACT | D3DTEXOPCAPS_ADDSMOOTH |
            D3DTEXOPCAPS_BLENDDIFFUSEALPHA | D3DTEXOPCAPS_BLENDTEXTUREALPHA | D3DTEXOPCAPS_BLENDFACTORALPHA |
            D3DTEXOPCAPS_BLENDTEXTUREALPHAPM | D3DTEXOPCAPS_BLENDCURRENTALPHA | D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR |
            D3DTEXOPCAPS_MODULATECOLOR_ADDALPHA | D3DTEXOPCAPS_MODULATEINVALPHA_ADDCOLOR |
            D3DTEXOPCAPS_MODULATEINVCOLOR_ADDALPHA | D3DTEXOPCAPS_DOTPRODUCT3;
        x.wMaxTextureBlendStages = SW_STAGES;
        x.wMaxSimultaneousTextures = SW_STAGES;
        x.dvMaxVertexW = 1.0e10f;
        COPY_INFO(x); return 1;
    } else if (guid_eq(guid, &G_ZPixelFormats)) {
        struct { DWORD n; DDPIXELFORMAT f[4]; } __attribute__((packed)) z;
        DWORD sz = sizeof(z);
        memset(&z, 0, sizeof(z));
        z.n = 4;
        z.f[0].dwSize = sizeof(DDPIXELFORMAT); z.f[0].dwFlags = DDPF_ZBUFFER;
        z.f[0].dwZBufferBitDepth = 16; z.f[0].dwZBitMask = 0xFFFF;
        z.f[1].dwSize = sizeof(DDPIXELFORMAT); z.f[1].dwFlags = DDPF_ZBUFFER;
        z.f[1].dwZBufferBitDepth = 32; z.f[1].dwZBitMask = 0xFFFFFF00u;
        z.f[2].dwSize = sizeof(DDPIXELFORMAT); z.f[2].dwFlags = DDPF_ZBUFFER | DDPF_STENCILBUFFER;
        z.f[2].dwZBufferBitDepth = 32; z.f[2].dwStencilBitDepth = 8;
        z.f[2].dwZBitMask = 0xFFFFFF00u; z.f[2].dwStencilBitMask = 0xFF;
        z.f[3].dwSize = sizeof(DDPIXELFORMAT); z.f[3].dwFlags = DDPF_ZBUFFER;
        z.f[3].dwZBufferBitDepth = 32; z.f[3].dwZBitMask = 0xFFFFFFFFu;
        *actual = sz;
        memcpy(data, &z, expected < sz ? expected : sz);
        return 1;
    } else if (guid_eq(guid, &G_ParseUnknown)) {
        parse_unknown = (PARSEUNKNOWN)data;
        return 1;
    }
    return 0;
}
/* CanCreateSurface: Z buffers and textures of these formats */
static int d3d_can_create(const DDSURFACEDESC *sd)
{
    if (!D3_ENABLED) return 0;
    if (sd->ddsCaps.dwCaps & DDSCAPS_ZBUFFER) return 1;
    if (!(sd->dwFlags & DDSD_PIXELFORMAT)) return 0;
    if (!(sd->ddsCaps.dwCaps & DDSCAPS_TEXTURE)) return 0;
    switch (fmt_pf(&sd->ddpfPixelFormat)) {
    case SWF_NONE: case SWF_P8: case SWF_A4L4: case SWF_A8: case SWF_YUY2: case SWF_UYVY: return 0;
    }
    return 1;
}

/* CreateSurface: the block size of compressed textures in video memory */
static int dxt_create(D3SURF s, D3SGBL g, DWORD f)
{
    DWORD bs, bw, bh;
    (void)s;
    if (f == MAKEFOURCC('D','X','T','1')) bs = 8;
    else if (f >= MAKEFOURCC('D','X','T','2') && f <= MAKEFOURCC('D','X','T','5') &&
             (f & 0x00FFFFFF) == MAKEFOURCC('D','X','T',0)) bs = 16;
    else return 0;
    bw = (g->wWidth + 3u) / 4; bh = (g->wHeight + 3u) / 4;
    if (!bw) bw = 1;
    if (!bh) bh = 1;
    g->lPitch = (LONG)(bw * bs);
    g->dwBlockSizeX = bw * bh * bs;
    g->dwBlockSizeY = 1;
    g->fpVidMem = DDHAL_PLEASEALLOC_BLOCKSIZE;
    return 1;
}