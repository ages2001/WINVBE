/*
 * wvbpf.h - the pixel formats of the WINVBE OpenGL ICD, shared by the ICD
 * (wvbdrv.c: DrvDescribePixelFormat of Windows 2000 and later) and the NT
 * display driver (nt/disp/glnt.c: DrvDescribePixelFormat of the kernel /
 * CSRSS part, which OPENGL32.DLL of NT 3.5x and 4.0 asks). Both must give
 * the same list in the same order.
 *
 * Needs PIXELFORMATDESCRIPTOR (wingdi.h) and a memset.
 */
#ifndef WVBPF_H
#define WVBPF_H

struct wvb_pf {
    unsigned char db;       /* double buffered */
    unsigned char alpha;    /* destination alpha bits */
    unsigned char depth;    /* depth bits */
    unsigned char stencil;  /* stencil bits */
    unsigned char accum;    /* accumulation bits per channel */
};

/* Formats without an accumulation buffer come first: ChoosePixelFormat
 * prefers earlier formats on ties and the accumulation buffer costs 8 bytes
 * per pixel.  Every format has 8 stencil bits (Z16 requests often also want
 * stencil, and ChoosePixelFormat ranks depth above stencil). */
static const struct wvb_pf wvb_pfs[] = {
    { 1, 8, 24, 8, 0 },     /*  1: double, RGBA, Z24 S8 */
    { 1, 0, 24, 8, 0 },     /*  2: double, RGB,  Z24 S8 */
    { 1, 8, 16, 8, 0 },     /*  3: double, RGBA, Z16 S8 */
    { 1, 0, 16, 8, 0 },     /*  4: double, RGB,  Z16 S8 */
    { 1, 8, 24, 8, 16 },    /*  5: double, RGBA, Z24 S8, accum 64 */
    { 1, 0, 24, 8, 16 },    /*  6: double, RGB,  Z24 S8, accum 48 */
    { 0, 8, 24, 8, 0 },     /*  7: single, RGBA, Z24 S8 */
    { 0, 0, 24, 8, 0 },     /*  8: single, RGB,  Z24 S8 */
    { 0, 0, 16, 8, 0 },     /*  9: single, RGB,  Z16 S8 */
    { 0, 8, 24, 8, 16 },    /* 10: single, RGBA, Z24 S8, accum 64 */
};
#define WVB_NUM_PF ((int) (sizeof(wvb_pfs) / sizeof(wvb_pfs[0])))

/* Fill *pfd for format index (1-based) on a desktop of bpp bits per pixel;
 * returns the number of formats (0 for a bad index). */
static int wvb_pf_describe(int index, int bpp, PIXELFORMATDESCRIPTOR *pfd)
{
    const struct wvb_pf *p;

    if (index < 1 || index > WVB_NUM_PF || !pfd)
        return 0;
    p = &wvb_pfs[index - 1];
    memset(pfd, 0, sizeof(*pfd));
    pfd->nSize = sizeof(*pfd);
    pfd->nVersion = 1;
    pfd->dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    if (p->db)
        pfd->dwFlags |= PFD_DOUBLEBUFFER | PFD_SWAP_COPY;
    pfd->iPixelType = PFD_TYPE_RGBA;

    /* Rendering is always 8 bits per channel internally.  Report the colour
     * depth of the desktop so that ChoosePixelFormat ranks these formats at
     * least as well as Microsoft's generic ones. */
    if (bpp == 15 || bpp == 16) {
        pfd->cColorBits = 16;
        pfd->cRedBits = 5;   pfd->cRedShift = 11;
        pfd->cGreenBits = 6; pfd->cGreenShift = 5;
        pfd->cBlueBits = 5;  pfd->cBlueShift = 0;
    }
    else {
        pfd->cColorBits = (bpp == 24) ? 24 : 32;
        pfd->cRedBits = 8;   pfd->cRedShift = 16;
        pfd->cGreenBits = 8; pfd->cGreenShift = 8;
        pfd->cBlueBits = 8;  pfd->cBlueShift = 0;
    }
    if (p->alpha) {
        pfd->cAlphaBits = p->alpha;
        pfd->cAlphaShift = 24;
    }
    pfd->cAccumRedBits = pfd->cAccumGreenBits = pfd->cAccumBlueBits = p->accum;
    pfd->cAccumAlphaBits = p->alpha ? p->accum : 0;
    pfd->cAccumBits = pfd->cAccumRedBits + pfd->cAccumGreenBits +
                      pfd->cAccumBlueBits + pfd->cAccumAlphaBits;
    pfd->cDepthBits = p->depth;
    pfd->cStencilBits = p->stencil;
    pfd->cAuxBuffers = 0;
    pfd->iLayerType = PFD_MAIN_PLANE;
    return WVB_NUM_PF;
}

/* The ICD and the NT 3.5x/4.0 display driver: the buffer the display
 * driver's DrvSwapBuffers shows (ExtEscape on a DC of the window). */
#define WVBGL_ESC_BUFFER 0x7745
typedef struct {
    unsigned long magic;    /* WVBGL_ESC_MAGIC */
    unsigned long hwnd;     /* the window (32-bit handle value) */
    unsigned long hbm;      /* 32-bit bottom-up DIB section, 0 = none */
    long width, height;
    unsigned long bits;     /* its memory (bottom row first) in the program */
    long pitch;             /* bytes per row */
} WVBGL_BUFFER;
#define WVBGL_ESC_MAGIC 0x57564742UL   /* 'WVGB' */
/* the display driver's counters (diagnostics): out ULONG[8] = SetPixelFormat
 * calls, windows tracked, buffers named, swaps, swaps shown, swaps without
 * a buffer, buffers that could not be locked, DescribePixelFormat calls */
#define WVBGL_ESC_STATUS 0x7746

#endif
