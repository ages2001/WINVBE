/*
 * SWR - WINVBE's software rasterizer for its Direct3D HAL.
 *
 * Platform-neutral C (no C library, no Windows headers): the Direct3D
 * driver parts (dd9x/wvbd3d.c for Windows 9x) decode the DrawPrimitives2
 * stream into SWVTX vertices and render states and call this. The pixel
 * pipeline is the fixed-function one of Direct3D 7: Gouraud or flat
 * colour, up to four texture stages (perspective-correct, point or
 * bilinear filtering, mipmaps, wrap/mirror/clamp/border), the texture
 * stage operations, specular, vertex and table fog, alpha test, colour
 * keys, Z and stencil tests and alpha blending.
 *
 * Coordinates follow Direct3D 7: pixel centres at integer coordinates, the
 * top-left fill rule, z in 0..1, rhw = 1/w.
 */
#ifndef SWR_H
#define SWR_H

typedef unsigned char  swu8;
typedef unsigned short swu16;
typedef unsigned int   swu32;
typedef int            sws32;
typedef long long      sws64;

/* surface formats */
enum {
    SWF_NONE = 0,
    SWF_P8, SWF_R3G3B2, SWF_R5G6B5, SWF_X1R5G5B5, SWF_A1R5G5B5, SWF_A4R4G4B4, SWF_X4R4G4B4,
    SWF_R8G8B8, SWF_X8R8G8B8, SWF_A8R8G8B8, SWF_L8, SWF_A8, SWF_A8L8, SWF_A4L4,
    SWF_DXT1, SWF_DXT2, SWF_DXT3, SWF_DXT4, SWF_DXT5,
    SWF_UYVY, SWF_YUY2,
    SWF_Z16, SWF_Z32, SWF_Z24S8, SWF_Z24X8, SWF_S8Z24, SWF_Z15S1
};

typedef struct {
    swu8  *bits;
    sws32  pitch;                   /* bytes per row (DXT: per row of blocks) */
    sws32  w, h;
    int    fmt;
} SWSURF;

#define SW_MAXLEVELS 16

typedef struct {
    SWSURF lvl[SW_MAXLEVELS];       /* mipmap levels, the largest first */
    int    nlvl;
    swu32 *pal;                     /* SWF_P8: 256 ARGB entries */
    swu32  ckey_lo, ckey_hi;        /* source colour key (texture format) */
    int    has_ckey;
    int    lod;                     /* SetTexLOD: the first level used */
} SWTEX;

#define SW_MAXTEX 8                 /* texture coordinate sets */
#define SW_STAGES 4                 /* texture stages done */

typedef struct {
    float x, y, z, rhw;
    float c[4];                     /* diffuse r g b a, 0..1 */
    float s[4];                     /* specular r g b, fog factor (alpha) */
    float t[SW_MAXTEX][2];
} SWVTX;

/* render states (D3DRENDERSTATETYPE numbers) used here */
#define SWRS_MAX 256

typedef struct {
    SWSURF rt;                      /* render target */
    SWSURF zb;                      /* depth/stencil, fmt SWF_NONE when none */
    swu32  rs[SWRS_MAX];            /* render states */
    swu32  tss[SW_MAXTEX][32];      /* texture stage states */
    SWTEX *tex[SW_MAXTEX];
    sws32  clip[4];                 /* x0, y0, x1, y1 (exclusive) */
    float  wnear, wfar;
    /* filled by sw_prepare() */
    int    nstages;
    int    stage_tc[SW_STAGES];
} SWCTX;

void sw_init(SWCTX *c);
void sw_prepare(SWCTX *c);          /* after state changes, before drawing */
void sw_triangle(SWCTX *c, const SWVTX *a, const SWVTX *b, const SWVTX *d);
void sw_line(SWCTX *c, const SWVTX *a, const SWVTX *b);
void sw_point(SWCTX *c, const SWVTX *a);
/* Clear (D3DCLEAR_TARGET 1, ZBUFFER 2, STENCIL 4) of a rectangle */
void sw_clear(SWCTX *c, swu32 flags, swu32 argb, float z, swu32 stencil, sws32 x0, sws32 y0, sws32 x1, sws32 y1);
/* pixel helpers for the driver (texture blits, colour keys) */
swu32 sw_fetch(const SWSURF *s, const swu32 *pal, sws32 x, sws32 y);   /* ARGB */
int sw_bpp(int fmt);                /* bits per pixel; DXT: 4 or 8 */

#endif
