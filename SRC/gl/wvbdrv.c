/*
 * wvbdrv.c - WINVBE OpenGL software driver: Mesa 7.8.2 classic swrast back
 * end that renders into GDI DIB sections.
 *
 * Derived from Mesa's Windows GDI driver (src/mesa/drivers/windows/gdi/
 * wmesa.c) and the OSMesa span code, reworked for use as an ICD:
 *
 *  - every colour buffer (front and, for double-buffered formats, back) is a
 *    32-bit bottom-up BGRA DIB section, independent of the desktop depth;
 *    GDI converts on BitBlt, so 8/15/16/24/32 bpp desktops all work;
 *  - framebuffers are kept per window (HWND), not per HDC, and the
 *    application's DC is never released by the driver;
 *  - the front buffer of a double-buffered window is a shadow DIB that is
 *    filled from the window the first time it is accessed and kept equal to
 *    the last swapped frame afterwards; drawing into it is shown on glFlush;
 *  - single-buffered formats render into the front DIB, shown on glFlush,
 *    glFinish and SwapBuffers;
 *  - depth, stencil and accumulation buffers are Mesa software buffers.
 *
 * The Mesa-derived code is Copyright (C) 1999-2008 Brian Paul and others,
 * MIT license (see MESA-LICENSE.TXT).
 */

#include "wvbdrv.h"

#include "main/glheader.h"
#include "main/context.h"
#include "main/extensions.h"
#include "main/framebuffer.h"
#include "main/renderbuffer.h"
#include "main/imports.h"
#include "main/macros.h"
#include "main/formats.h"
#include "main/mtypes.h"
#include "main/state.h"
#include "glapi/glapi.h"
#include "glapi/glthread.h"
#include "drivers/common/driverfuncs.h"
#include "drivers/common/meta.h"
#include "vbo/vbo.h"
#include "swrast/swrast.h"
#include "swrast_setup/swrast_setup.h"
#include "tnl/tnl.h"
#include "tnl/t_context.h"
#include "tnl/t_pipeline.h"

#define WVB_RB_CLASS  0x57564231u   /* 'WVB1' */
#define WVB_FB_MAGIC  0x57564246u   /* 'WVBF' */

/* ------------------------------------------------------------------------
 * Pixel formats
 * ------------------------------------------------------------------------ */

#include "wvbpf.h"

/* Pixel format chosen for each window (SetPixelFormat). */
#define WVB_MAX_WINDOWS 256
static struct { HWND hwnd; int pf; } wvb_winpf[WVB_MAX_WINDOWS];
static int wvb_last_pf;

static CRITICAL_SECTION wvb_lock;

/* Windows NT version and the NT 3.1 sub-case (see the long note further down).
 * Declared here because wvb_get_pixelformat (below) already uses them. */
static int wvb_old_nt;
static int wvb_nt31;
#define wvb_dd_present (wvb_old_nt && !wvb_nt31)
/* resolved at run time: not on every GDI32 (NT 3.1 lacks the first two) */
static HBITMAP (WINAPI *wvb_CreateDIBSection)(HDC, const BITMAPINFO *, UINT, void **, HANDLE, DWORD);
static int (WINAPI *wvb_GetPixelFormat)(HDC);

/* Mesa's static mutexes; with WIN32_THREADS they are plain CRITICAL_SECTIONs
 * that must be initialised before use (Mesa's own initialiser is broken on
 * Windows, see glthread.h). */
#ifdef WIN32_THREADS
extern _glthread_Mutex OneTimeLock;
#endif

int wvb_pixelformat_count(void)
{
    return WVB_NUM_PF;
}

int wvb_describe_pixelformat(HDC hdc, int index, PIXELFORMATDESCRIPTOR *pfd)
{
    int bpp = hdc ? GetDeviceCaps(hdc, BITSPIXEL) * GetDeviceCaps(hdc, PLANES) : 32;
    return wvb_pf_describe(index, bpp, pfd);
}

BOOL wvb_set_pixelformat(HDC hdc, int index)
{
    HWND hwnd = WindowFromDC(hdc);
    int i, free_slot = -1;

    if (index < 1 || index > WVB_NUM_PF)
        return FALSE;
    EnterCriticalSection(&wvb_lock);
    wvb_last_pf = index;
    if (hwnd) {
        for (i = 0; i < WVB_MAX_WINDOWS; i++) {
            if (wvb_winpf[i].hwnd == hwnd) {
                wvb_winpf[i].pf = index;
                LeaveCriticalSection(&wvb_lock);
                return TRUE;
            }
            if (free_slot < 0 && (!wvb_winpf[i].hwnd || !IsWindow(wvb_winpf[i].hwnd)))
                free_slot = i;
        }
        if (free_slot < 0)
            free_slot = (int) ((DWORD_PTR) hwnd % WVB_MAX_WINDOWS);
        wvb_winpf[free_slot].hwnd = hwnd;
        wvb_winpf[free_slot].pf = index;
    }
    LeaveCriticalSection(&wvb_lock);
    return TRUE;
}

int wvb_get_pixelformat(HDC hdc)
{
    HWND hwnd = WindowFromDC(hdc);
    int i, pf = 0;

    EnterCriticalSection(&wvb_lock);
    if (hwnd) {
        for (i = 0; i < WVB_MAX_WINDOWS; i++) {
            if (wvb_winpf[i].hwnd == hwnd) {
                pf = wvb_winpf[i].pf;
                break;
            }
        }
    }
    LeaveCriticalSection(&wvb_lock);
    /* NT 3.5x/4.0: OPENGL32.DLL sets the format through the display driver
     * (DrvSetPixelFormat there) and never calls ours; our formats come first
     * in GDI's list, so its index is ours */
    if (!pf && wvb_GetPixelFormat) {
        int g = wvb_GetPixelFormat(hdc);
        if (g >= 1 && g <= WVB_NUM_PF)
            pf = g;
    }
    if (!pf)
        pf = wvb_last_pf;
    return pf;
}

/* ------------------------------------------------------------------------
 * Driver structures
 * ------------------------------------------------------------------------ */

struct wvb_framebuffer;

/* A colour buffer: a 32-bit bottom-up DIB section (Mesa's y = DIB row). */
struct wvb_buffer {
    struct gl_renderbuffer Base;
    struct wvb_framebuffer *fb;
    HBITMAP hbm, oldbm;
    HDC memdc;
    GLubyte *bits;
    GLint pitch;            /* bytes per row */
    GLboolean alpha;        /* store destination alpha */
    GLboolean is_front;
    GLboolean used;         /* front: accessed since (re)allocation */
    GLboolean dirty;        /* front: modified since last shown */
};

struct wvb_framebuffer {
    struct gl_framebuffer Base;
    DWORD magic;
    HWND hwnd;
    int pf;
    struct wvb_buffer *front, *back;
    struct wvb_framebuffer *next;
};

struct wvb_context {
    GLcontext gl;           /* must be first */
    int pf;
    HDC hdc;                /* DC passed to the last make-current */
};

static struct wvb_framebuffer *wvb_fb_list;
static int wvb_context_count;

/* GdiFlush is resolved at run time so that the DLL never fails to load on
 * a GDI32 without it (it only matters for NT's batched GDI anyway). */
static BOOL (WINAPI *wvb_GdiFlush)(void);

static void wvb_gdi_flush(void)
{
    if (wvb_GdiFlush)
        wvb_GdiFlush();
}

/* Windows NT 3.5x and 4.0: OPENGL32.DLL swaps through the display driver
 * (glFinish, then the kernel's / CSRSS's DrvSwapBuffers). WINVBE's display
 * driver shows the DIB section registered here (nt/disp/glnt.c); the ICD's
 * own swap code is not called there. */
/* NT 3.1 (wvb_nt31): GDI has no DIB sections (CreateDIBSection) and no OpenGL
 * plumbing (no DrvSetPixelFormat/DrvSwapBuffers dispatch, no EngCreateWnd).
 * The ICD then allocates plain bottom-up 32-bit buffers and presents them
 * itself with SetDIBitsToDevice; the display driver (nt/disp/glnt.c) is not
 * involved, and WINVBE's OPENGL32.DLL calls the ICD's DrvSetPixelFormat /
 * DrvSwapBuffers directly (ogl3x.c). wvb_old_nt/wvb_nt31/wvb_dd_present and
 * the two GDI32 pointers are declared near the top (used by
 * wvb_get_pixelformat). */
/* NT 3.x: GDI handles of the client are not the server's (CSRSS) ones */
static HANDLE (WINAPI *wvb_GdiConvertBitmap)(HBITMAP);
static GLboolean wvb_nt_resize_check;   /* a frame ended: follow the window size */

static struct wvb_framebuffer *wvb_fb(struct gl_framebuffer *fb)
{
    struct wvb_framebuffer *w = (struct wvb_framebuffer *) fb;
    return (w && fb->Name == 0 && w->magic == WVB_FB_MAGIC) ? w : NULL;
}

static void wvb_window_size(HWND hwnd, GLuint *w, GLuint *h)
{
    RECT r;
    if (hwnd && GetClientRect(hwnd, &r)) {
        *w = (GLuint) (r.right > r.left ? r.right - r.left : 0);
        *h = (GLuint) (r.bottom > r.top ? r.bottom - r.top : 0);
    }
    else
        *w = *h = 0;
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
    if (*w > MAX_WIDTH) *w = MAX_WIDTH;
    if (*h > MAX_HEIGHT) *h = MAX_HEIGHT;
}

/* Copy a colour buffer to the window. */
static void wvb_present(struct wvb_buffer *wb, HDC hdc)
{
    HWND hwnd = wb->fb->hwnd;
    HDC dc = hdc;

    if (!wb->Base.Width || !wb->Base.Height || (!wb->memdc && !wb->bits))
        return;
    if (!dc)
        dc = GetDC(hwnd);
    if (dc) {
        if (wb->memdc)
            BitBlt(dc, 0, 0, wb->Base.Width, wb->Base.Height, wb->memdc, 0, 0, SRCCOPY);
        else {
            /* NT 3.1: no DIB section/memory DC; show the bottom-up 32-bit
               buffer straight through GDI (SetDIBitsToDevice is on every
               GDI32 since Windows 3.0). */
            BITMAPINFO bmi;
            memset(&bmi, 0, sizeof(bmi));
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = (LONG) wb->Base.Width;
            bmi.bmiHeader.biHeight = (LONG) wb->Base.Height;    /* bottom-up, like GL */
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            SetDIBitsToDevice(dc, 0, 0, wb->Base.Width, wb->Base.Height,
                              0, 0, 0, wb->Base.Height, wb->bits, &bmi, DIB_RGB_COLORS);
        }
        if (!hdc)
            ReleaseDC(hwnd, dc);
    }
    wvb_gdi_flush();
}

/* NT 3.x: GDI32 sends SwapBuffers to the display driver without calling
 * glFinish (or anything of the ICD) first: glEnd draws what it collected at
 * once, so that the frame is complete when it is shown (icd.c). */
int wvb_nt3(void)
{
    return wvb_old_nt == 3 && !wvb_nt31;
}

void wvb_flush_vertices(void)
{
    GET_CURRENT_CONTEXT(ctx);
    if (ctx && (ctx->Driver.NeedFlush & FLUSH_STORED_VERTICES))
        ctx->Driver.FlushVertices(ctx, FLUSH_STORED_VERTICES);
}

/* NT 3.5x/4.0: tell the display driver which DIB to show on SwapBuffers. */
static void wvb_nt_register(struct wvb_buffer *wb)
{
    struct wvb_framebuffer *fb = wb->fb;
    WVBGL_BUFFER b;
    HDC dc;

    if (!wvb_dd_present || !fb || !fb->hwnd)
        return;
    if (fb->back ? wb != fb->back : wb != fb->front)
        return;
    b.magic = WVBGL_ESC_MAGIC;
    b.hwnd = (unsigned long) (DWORD_PTR) fb->hwnd;
    b.hbm = (unsigned long) (DWORD_PTR) wb->hbm;
    if (wb->hbm && wvb_GdiConvertBitmap) {
        HANDLE h = wvb_GdiConvertBitmap(wb->hbm);
        if (h)
            b.hbm = (unsigned long) (DWORD_PTR) h;
    }
    b.width = (long) wb->Base.Width;
    b.height = (long) wb->Base.Height;
    b.bits = (unsigned long) (DWORD_PTR) (wb->hbm ? wb->bits : NULL);
    b.pitch = wb->pitch;
    dc = GetDC(fb->hwnd);
    if (dc) {
        wvb_gdi_flush();
        ExtEscape(dc, WVBGL_ESC_BUFFER, sizeof(b), (LPCSTR) &b, 0, NULL);
        ReleaseDC(fb->hwnd, dc);
    }
}

/* First access to a front buffer: start from what the window shows. */
static void wvb_front_first_use(struct wvb_buffer *wb)
{
    HWND hwnd = wb->fb->hwnd;
    HDC dc;

    wb->used = GL_TRUE;
    if (!wb->memdc)
        return;
    dc = GetDC(hwnd);
    if (dc) {
        BitBlt(wb->memdc, 0, 0, wb->Base.Width, wb->Base.Height, dc, 0, 0, SRCCOPY);
        ReleaseDC(hwnd, dc);
    }
    wvb_gdi_flush();
}

/* ------------------------------------------------------------------------
 * Span functions (swrast/s_spantemp.h): 0xAARRGGBB words, i.e. B,G,R,A bytes.
 * ------------------------------------------------------------------------ */

#define WVB_PACK(A, R, G, B) \
    (((GLuint) (A) << 24) | ((GLuint) (R) << 16) | ((GLuint) (G) << 8) | (GLuint) (B))

/* Back buffers (and any buffer that needs no tracking), with alpha. */
#define NAME(PREFIX) PREFIX##_argb
#define RB_TYPE GLubyte
#define SPAN_VARS struct wvb_buffer *wb = (struct wvb_buffer *) rb;
#define INIT_PIXEL_PTR(P, X, Y) GLuint *P = (GLuint *) (wb->bits + (Y) * wb->pitch) + (X)
#define INC_PIXEL_PTR(P) P++
#define STORE_PIXEL(DST, X, Y, VALUE) \
    *DST = WVB_PACK(VALUE[ACOMP], VALUE[RCOMP], VALUE[GCOMP], VALUE[BCOMP])
#define STORE_PIXEL_RGB(DST, X, Y, VALUE) \
    *DST = WVB_PACK(0xff, VALUE[RCOMP], VALUE[GCOMP], VALUE[BCOMP])
#define FETCH_PIXEL(DST, SRC) do { GLuint p_ = *SRC; \
    DST[RCOMP] = (GLubyte) (p_ >> 16); DST[GCOMP] = (GLubyte) (p_ >> 8); \
    DST[BCOMP] = (GLubyte) p_; DST[ACOMP] = (GLubyte) (p_ >> 24); } while (0)
#include "swrast/s_spantemp.h"

/* Back buffers without alpha: alpha reads back as 1.0. */
#define NAME(PREFIX) PREFIX##_xrgb
#define RB_TYPE GLubyte
#define SPAN_VARS struct wvb_buffer *wb = (struct wvb_buffer *) rb;
#define INIT_PIXEL_PTR(P, X, Y) GLuint *P = (GLuint *) (wb->bits + (Y) * wb->pitch) + (X)
#define INC_PIXEL_PTR(P) P++
#define STORE_PIXEL(DST, X, Y, VALUE) \
    *DST = WVB_PACK(0xff, VALUE[RCOMP], VALUE[GCOMP], VALUE[BCOMP])
#define FETCH_PIXEL(DST, SRC) do { GLuint p_ = *SRC; \
    DST[RCOMP] = (GLubyte) (p_ >> 16); DST[GCOMP] = (GLubyte) (p_ >> 8); \
    DST[BCOMP] = (GLubyte) p_; DST[ACOMP] = 0xff; } while (0)
#include "swrast/s_spantemp.h"

/* Front buffers: as above, plus first-use and dirty tracking. */
#define WVB_FRONT_VARS \
    struct wvb_buffer *wb = (struct wvb_buffer *) rb; \
    if (!wb->used) wvb_front_first_use(wb); \
    wb->dirty = GL_TRUE;

#define NAME(PREFIX) PREFIX##_argb_front
#define RB_TYPE GLubyte
#define SPAN_VARS WVB_FRONT_VARS
#define INIT_PIXEL_PTR(P, X, Y) GLuint *P = (GLuint *) (wb->bits + (Y) * wb->pitch) + (X)
#define INC_PIXEL_PTR(P) P++
#define STORE_PIXEL(DST, X, Y, VALUE) \
    *DST = WVB_PACK(VALUE[ACOMP], VALUE[RCOMP], VALUE[GCOMP], VALUE[BCOMP])
#define STORE_PIXEL_RGB(DST, X, Y, VALUE) \
    *DST = WVB_PACK(0xff, VALUE[RCOMP], VALUE[GCOMP], VALUE[BCOMP])
#define FETCH_PIXEL(DST, SRC) do { GLuint p_ = *SRC; \
    DST[RCOMP] = (GLubyte) (p_ >> 16); DST[GCOMP] = (GLubyte) (p_ >> 8); \
    DST[BCOMP] = (GLubyte) p_; DST[ACOMP] = (GLubyte) (p_ >> 24); } while (0)
#include "swrast/s_spantemp.h"

#define NAME(PREFIX) PREFIX##_xrgb_front
#define RB_TYPE GLubyte
#define SPAN_VARS WVB_FRONT_VARS
#define INIT_PIXEL_PTR(P, X, Y) GLuint *P = (GLuint *) (wb->bits + (Y) * wb->pitch) + (X)
#define INC_PIXEL_PTR(P) P++
#define STORE_PIXEL(DST, X, Y, VALUE) \
    *DST = WVB_PACK(0xff, VALUE[RCOMP], VALUE[GCOMP], VALUE[BCOMP])
#define FETCH_PIXEL(DST, SRC) do { GLuint p_ = *SRC; \
    DST[RCOMP] = (GLubyte) (p_ >> 16); DST[GCOMP] = (GLubyte) (p_ >> 8); \
    DST[BCOMP] = (GLubyte) p_; DST[ACOMP] = 0xff; } while (0)
#include "swrast/s_spantemp.h"

/* ------------------------------------------------------------------------
 * Colour renderbuffers
 * ------------------------------------------------------------------------ */

static void wvb_free_dib(struct wvb_buffer *wb)
{
    if (wb->hbm && wvb_dd_present) {
        HBITMAP h = wb->hbm;
        wb->hbm = NULL;
        wb->Base.Width = wb->Base.Height = 0;
        wvb_nt_register(wb);            /* the display driver lets go of it */
        wb->hbm = h;
    }
    if (wb->memdc) {
        SelectObject(wb->memdc, wb->oldbm);
        DeleteDC(wb->memdc);
    }
    if (wb->hbm)
        DeleteObject(wb->hbm);
    else if (wvb_nt31 && wb->bits)
        free(wb->bits);                 /* NT 3.1: plain malloc'd buffer */
    wb->memdc = NULL;
    wb->hbm = wb->oldbm = NULL;
    wb->bits = NULL;
    wb->pitch = 0;
}

static void wvb_delete_buffer(struct gl_renderbuffer *rb)
{
    struct wvb_buffer *wb = (struct wvb_buffer *) rb;
    wvb_free_dib(wb);
    _glthread_DESTROY_MUTEX(rb->Mutex);
    free(wb);
}

static GLboolean wvb_alloc_storage(GLcontext *ctx, struct gl_renderbuffer *rb,
                                   GLenum internalFormat, GLuint width, GLuint height)
{
    struct wvb_buffer *wb = (struct wvb_buffer *) rb;
    BITMAPINFO bmi;
    void *bits = NULL;
    HDC sdc;
    (void) ctx; (void) internalFormat;

    if (wb->bits && rb->Width == width && rb->Height == height)
        return GL_TRUE;
    wvb_free_dib(wb);
    rb->Width = rb->Height = 0;
    wb->used = wb->dirty = GL_FALSE;
    if (!width || !height)
        return GL_TRUE;

    if (wvb_nt31) {
        /* NT 3.1: no CreateDIBSection. A plain bottom-up 32-bit buffer that
           Mesa renders into and wvb_present shows with SetDIBitsToDevice. */
        wb->pitch = (GLint) width * 4;
        wb->bits = (GLubyte *) malloc((size_t) wb->pitch * height);
        if (!wb->bits) {
            wvb_free_dib(wb);
            return GL_FALSE;
        }
        memset(wb->bits, 0, (size_t) wb->pitch * height);
        rb->Width = width;
        rb->Height = height;
        return GL_TRUE;
    }

    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = (LONG) width;
    bmi.bmiHeader.biHeight = (LONG) height;      /* bottom-up, like GL */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    sdc = GetDC(NULL);
    wb->hbm = wvb_CreateDIBSection ? wvb_CreateDIBSection(sdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (sdc)
        ReleaseDC(NULL, sdc);
    if (!wb->hbm || !bits) {
        wvb_free_dib(wb);
        return GL_FALSE;
    }
    wb->memdc = CreateCompatibleDC(NULL);
    if (!wb->memdc) {
        wvb_free_dib(wb);
        return GL_FALSE;
    }
    wb->oldbm = (HBITMAP) SelectObject(wb->memdc, wb->hbm);
    wvb_gdi_flush();
    wb->bits = (GLubyte *) bits;
    wb->pitch = (GLint) width * 4;
    memset(wb->bits, 0, (size_t) wb->pitch * height);
    rb->Width = width;
    rb->Height = height;
    wvb_nt_register(wb);
    return GL_TRUE;
}

static struct wvb_buffer *wvb_new_buffer(struct wvb_framebuffer *fb, GLboolean alpha,
                                         GLboolean front)
{
    struct wvb_buffer *wb = (struct wvb_buffer *) calloc(1, sizeof(*wb));
    struct gl_renderbuffer *rb;

    if (!wb)
        return NULL;
    rb = &wb->Base;
    _mesa_init_renderbuffer(rb, 0);
    rb->ClassID = WVB_RB_CLASS;
    rb->InternalFormat = alpha ? GL_RGBA8 : GL_RGB8;
    rb->_BaseFormat = GL_RGBA;
    rb->Format = alpha ? MESA_FORMAT_ARGB8888 : MESA_FORMAT_XRGB8888;
    rb->DataType = GL_UNSIGNED_BYTE;
    rb->Delete = wvb_delete_buffer;
    rb->AllocStorage = wvb_alloc_storage;
    wb->fb = fb;
    wb->alpha = alpha;
    wb->is_front = front;
    if (front && alpha) {
        rb->GetRow = get_row_argb_front;           rb->GetValues = get_values_argb_front;
        rb->PutRow = put_row_argb_front;           rb->PutRowRGB = put_row_rgb_argb_front;
        rb->PutMonoRow = put_mono_row_argb_front;  rb->PutValues = put_values_argb_front;
        rb->PutMonoValues = put_mono_values_argb_front;
    }
    else if (front) {
        rb->GetRow = get_row_xrgb_front;           rb->GetValues = get_values_xrgb_front;
        rb->PutRow = put_row_xrgb_front;           rb->PutRowRGB = put_row_rgb_xrgb_front;
        rb->PutMonoRow = put_mono_row_xrgb_front;  rb->PutValues = put_values_xrgb_front;
        rb->PutMonoValues = put_mono_values_xrgb_front;
    }
    else if (alpha) {
        rb->GetRow = get_row_argb;                 rb->GetValues = get_values_argb;
        rb->PutRow = put_row_argb;                 rb->PutRowRGB = put_row_rgb_argb;
        rb->PutMonoRow = put_mono_row_argb;        rb->PutValues = put_values_argb;
        rb->PutMonoValues = put_mono_values_argb;
    }
    else {
        rb->GetRow = get_row_xrgb;                 rb->GetValues = get_values_xrgb;
        rb->PutRow = put_row_xrgb;                 rb->PutRowRGB = put_row_rgb_xrgb;
        rb->PutMonoRow = put_mono_row_xrgb;        rb->PutValues = put_values_xrgb;
        rb->PutMonoValues = put_mono_values_xrgb;
    }
    return wb;
}

/* ------------------------------------------------------------------------
 * Framebuffers (one per window)
 * ------------------------------------------------------------------------ */

static struct wvb_framebuffer *wvb_new_framebuffer(HWND hwnd, const GLvisual *vis, int pf)
{
    struct wvb_framebuffer *fb = (struct wvb_framebuffer *) calloc(1, sizeof(*fb));
    GLboolean alpha = vis->alphaBits > 0;

    if (!fb)
        return NULL;
    _mesa_initialize_window_framebuffer(&fb->Base, vis);
    fb->magic = WVB_FB_MAGIC;
    fb->hwnd = hwnd;
    fb->pf = pf;
    fb->front = wvb_new_buffer(fb, alpha, GL_TRUE);
    if (fb->front)
        _mesa_add_renderbuffer(&fb->Base, BUFFER_FRONT_LEFT, &fb->front->Base);
    if (vis->doubleBufferMode) {
        fb->back = wvb_new_buffer(fb, alpha, GL_FALSE);
        if (fb->back)
            _mesa_add_renderbuffer(&fb->Base, BUFFER_BACK_LEFT, &fb->back->Base);
    }
    if (!fb->front || (vis->doubleBufferMode && !fb->back)) {
        struct gl_framebuffer *p = &fb->Base;
        _mesa_reference_framebuffer(&p, NULL);
        return NULL;
    }
    /* Mesa owns depth, stencil and accumulation buffers. */
    _mesa_add_soft_renderbuffers(&fb->Base, GL_FALSE,
                                 vis->depthBits > 0, vis->stencilBits > 0,
                                 vis->accumRedBits > 0, GL_FALSE, GL_FALSE);
    return fb;
}

/* Caller holds wvb_lock. */
static void wvb_unlink_framebuffer(struct wvb_framebuffer *fb)
{
    struct wvb_framebuffer **pp;
    struct gl_framebuffer *p = &fb->Base;

    for (pp = &wvb_fb_list; *pp; pp = &(*pp)->next) {
        if (*pp == fb) {
            *pp = fb->next;
            break;
        }
    }
    fb->next = NULL;
    /* Contexts may still reference it; it is freed with the last one. */
    _mesa_reference_framebuffer(&p, NULL);  /* drop the list's reference */
}

/* Caller holds wvb_lock. */
static struct wvb_framebuffer *wvb_find_framebuffer(HWND hwnd)
{
    struct wvb_framebuffer *fb;
    for (fb = wvb_fb_list; fb; fb = fb->next)
        if (fb->hwnd == hwnd)
            return fb;
    return NULL;
}

/* Drop framebuffers of destroyed windows (all of them when no context is
 * left).  Caller holds wvb_lock. */
static void wvb_collect_framebuffers(void)
{
    struct wvb_framebuffer *fb = wvb_fb_list, *next;
    while (fb) {
        next = fb->next;
        if (!wvb_context_count || !IsWindow(fb->hwnd))
            wvb_unlink_framebuffer(fb);
        fb = next;
    }
}

/* ------------------------------------------------------------------------
 * Driver hooks
 * ------------------------------------------------------------------------ */

static const GLubyte *wvb_get_string(GLcontext *ctx, GLenum name)
{
    (void) ctx;
    switch (name) {
    case GL_VENDOR:   return (const GLubyte *) WVB_VENDOR;
    case GL_RENDERER: return (const GLubyte *) WVB_RENDERER;
    default:          return NULL;
    }
}

static void wvb_update_state(GLcontext *ctx, GLuint new_state)
{
    _swrast_InvalidateState(ctx, new_state);
    _swsetup_InvalidateState(ctx, new_state);
    _vbo_InvalidateState(ctx, new_state);
    _tnl_InvalidateState(ctx, new_state);
}

static void wvb_get_buffer_size(GLframebuffer *buffer, GLuint *width, GLuint *height)
{
    struct wvb_framebuffer *fb = wvb_fb(buffer);
    if (fb)
        wvb_window_size(fb->hwnd, width, height);
    else {
        *width = buffer->Width;
        *height = buffer->Height;
    }
}

/* Resize the window's buffers if its client area changed. */
static void wvb_check_size(GLcontext *ctx, GLframebuffer *buffer)
{
    struct wvb_framebuffer *fb = wvb_fb(buffer);
    GLuint w, h;

    if (!fb)
        return;
    wvb_window_size(fb->hwnd, &w, &h);
    if (w != buffer->Width || h != buffer->Height) {
        _mesa_resize_framebuffer(ctx, buffer, w, h);
        ctx->NewState |= _NEW_BUFFERS;
    }
}

/* Called by glViewport: the driver has no message loop, so poll the
 * window size here (and on make-current), like wmesa and xlib do. */
static void wvb_viewport(GLcontext *ctx, GLint x, GLint y, GLsizei w, GLsizei h)
{
    (void) x; (void) y; (void) w; (void) h;
    wvb_check_size(ctx, ctx->WinSysDrawBuffer);
    if (ctx->WinSysReadBuffer != ctx->WinSysDrawBuffer)
        wvb_check_size(ctx, ctx->WinSysReadBuffer);
}

/* glFlush/glFinish: show front-buffer rendering. */
static void wvb_flush(GLcontext *ctx)
{
    struct wvb_framebuffer *fb = wvb_fb(ctx->WinSysDrawBuffer);

    /* Use a fresh window DC: the one given to wglMakeCurrent may have been
     * released by the application in the meantime. */
    if (fb && fb->front && fb->front->dirty) {
        fb->front->dirty = GL_FALSE;
        wvb_present(fb->front, NULL);
    }
    /* NT 3.5x/4.0: OPENGL32.DLL calls glFinish right before each swap, which
     * the display driver does: the front buffer may show the back buffer
     * afterwards (read it from the window again when it is used next), and
     * the window size is checked when the next frame starts (glClear) */
    if (wvb_dd_present && fb) {
        if (fb->back && fb->front)
            fb->front->used = GL_FALSE;
        wvb_nt_resize_check = GL_TRUE;
    }
}

/* Fast colour clear straight into the DIBs. */
static void wvb_clear(GLcontext *ctx, GLbitfield mask)
{
    struct gl_framebuffer *fb = ctx->DrawBuffer;
    GLuint i;
    GLboolean fast;

    /* NT 3.x: SwapBuffers goes from GDI32 straight to the display driver
     * without the ICD: a frame starts with glClear of the back buffer */
    if (wvb_nt3() && ctx->WinSysDrawBuffer) {
        struct wvb_framebuffer *wf = wvb_fb(ctx->WinSysDrawBuffer);
        if (wf && wf->back && wf->front)
            wf->front->used = GL_FALSE;
        wvb_nt_resize_check = GL_TRUE;
    }
    if (wvb_nt_resize_check && ctx->WinSysDrawBuffer) {
        GLuint w0 = ctx->WinSysDrawBuffer->Width, h0 = ctx->WinSysDrawBuffer->Height;
        wvb_nt_resize_check = GL_FALSE;
        wvb_check_size(ctx, ctx->WinSysDrawBuffer);
        if (ctx->WinSysDrawBuffer->Width != w0 || ctx->WinSysDrawBuffer->Height != h0)
            _mesa_update_state(ctx);
        fb = ctx->DrawBuffer;
    }
    fast = (mask & BUFFER_BITS_COLOR) != 0;

    for (i = 0; fast && i < fb->_NumColorDrawBuffers; i++) {
        struct gl_renderbuffer *rb = fb->_ColorDrawBuffers[i];
        const GLubyte *cm = ctx->Color.ColorMask[i];
        if (!rb || rb->ClassID != WVB_RB_CLASS || !((struct wvb_buffer *) rb)->bits ||
            !cm[0] || !cm[1] || !cm[2] || (!cm[3] && ((struct wvb_buffer *) rb)->alpha))
            fast = GL_FALSE;
    }
    if (fast && fb->_NumColorDrawBuffers > 0) {
        const GLint x0 = fb->_Xmin, x1 = fb->_Xmax, y0 = fb->_Ymin, y1 = fb->_Ymax;
        GLubyte c[4];
        GLuint pixel;

        UNCLAMPED_FLOAT_TO_UBYTE(c[0], ctx->Color.ClearColor[0]);
        UNCLAMPED_FLOAT_TO_UBYTE(c[1], ctx->Color.ClearColor[1]);
        UNCLAMPED_FLOAT_TO_UBYTE(c[2], ctx->Color.ClearColor[2]);
        UNCLAMPED_FLOAT_TO_UBYTE(c[3], ctx->Color.ClearColor[3]);
        for (i = 0; i < fb->_NumColorDrawBuffers; i++) {
            struct wvb_buffer *wb = (struct wvb_buffer *) fb->_ColorDrawBuffers[i];
            GLint x, y;
            pixel = WVB_PACK(wb->alpha ? c[3] : 0xff, c[0], c[1], c[2]);
            if (wb->is_front) {
                if (!wb->used && (x0 > 0 || y0 > 0 || x1 < (GLint) wb->Base.Width ||
                                  y1 < (GLint) wb->Base.Height))
                    wvb_front_first_use(wb);
                wb->used = wb->dirty = GL_TRUE;
            }
            for (y = y0; y < y1; y++) {
                GLuint *p = (GLuint *) (wb->bits + y * wb->pitch) + x0;
                for (x = x0; x < x1; x++)
                    *p++ = pixel;
            }
        }
        mask &= ~BUFFER_BITS_COLOR;
    }
    if (mask)
        _swrast_Clear(ctx, mask);
}

/* ------------------------------------------------------------------------
 * Context management
 * ------------------------------------------------------------------------ */

void wvb_process_attach(void)
{
    HMODULE gdi = GetModuleHandleA("GDI32.DLL");
    DWORD v = GetVersion();
    /* 3 = NT 3.5x, 4 = NT 4.0, 0 = Windows 9x or NT 5 and later */
    wvb_old_nt = !(v & 0x80000000UL) && (v & 0xFF) < 5 ? (int) (v & 0xFF) : 0;
    if (gdi) {
        wvb_GdiFlush = (BOOL (WINAPI *)(void)) GetProcAddress(gdi, "GdiFlush");
        wvb_CreateDIBSection = (HBITMAP (WINAPI *)(HDC, const BITMAPINFO *, UINT, void **, HANDLE, DWORD))
                               GetProcAddress(gdi, "CreateDIBSection");
        wvb_GetPixelFormat = (int (WINAPI *)(HDC)) GetProcAddress(gdi, "GetPixelFormat");
        if (wvb_old_nt)
            wvb_GdiConvertBitmap = (HANDLE (WINAPI *)(HBITMAP)) GetProcAddress(gdi, "GdiConvertBitmap");
    }
    /* NT 3.1: no DIB sections -> the ICD allocates and presents buffers itself */
    wvb_nt31 = wvb_old_nt && !wvb_CreateDIBSection;
    InitializeCriticalSection(&wvb_lock);
#ifdef WIN32_THREADS
    InitializeCriticalSection(&OneTimeLock);
    /* Allocate glapi's TLS slots now, race-free (Mesa does it lazily). The
     * loading thread is taken as the first rendering thread; as soon as
     * another thread makes a context current glapi switches to per-thread
     * (TlsGetValue) context and dispatch pointers. */
    _glapi_check_multithread();
#endif
}

void wvb_process_detach(void)
{
    _glapi_destroy_multithread();
#ifdef WIN32_THREADS
    DeleteCriticalSection(&OneTimeLock);
#endif
    DeleteCriticalSection(&wvb_lock);
}

struct wvb_context *wvb_create_context(HDC hdc)
{
    int pf = wvb_get_pixelformat(hdc);
    const struct wvb_pf *p;
    struct dd_function_table functions;
    struct wvb_context *c;
    GLcontext *ctx;
    GLvisual *visual;

    if (pf < 1 || pf > WVB_NUM_PF) {
        SetLastError(ERROR_INVALID_PIXEL_FORMAT);
        return NULL;
    }
    p = &wvb_pfs[pf - 1];
    visual = _mesa_create_visual(p->db, GL_FALSE, 8, 8, 8, p->alpha,
                                 p->depth, p->stencil,
                                 p->accum, p->accum, p->accum, p->alpha ? p->accum : 0,
                                 1);
    if (!visual)
        return NULL;
    c = (struct wvb_context *) calloc(1, sizeof(*c));
    if (!c) {
        _mesa_destroy_visual(visual);
        return NULL;
    }
    c->pf = pf;
    c->hdc = hdc;

    _mesa_init_driver_functions(&functions);
    functions.GetString = wvb_get_string;
    functions.UpdateState = wvb_update_state;
    functions.GetBufferSize = wvb_get_buffer_size;
    functions.Flush = wvb_flush;
    functions.Finish = wvb_flush;
    functions.Clear = wvb_clear;
    functions.Viewport = wvb_viewport;

    ctx = &c->gl;
    if (!_mesa_initialize_context(ctx, visual, NULL, &functions, (void *) c)) {
        _mesa_destroy_visual(visual);
        free(c);
        return NULL;
    }
    _mesa_destroy_visual(visual);

    _mesa_enable_sw_extensions(ctx);
    _mesa_enable_1_3_extensions(ctx);
    _mesa_enable_1_4_extensions(ctx);
    _mesa_enable_1_5_extensions(ctx);
    _mesa_enable_2_0_extensions(ctx);
    _mesa_enable_2_1_extensions(ctx);

    _mesa_meta_init(ctx);

    if (!_swrast_CreateContext(ctx) || !_vbo_CreateContext(ctx) ||
        !_tnl_CreateContext(ctx) || !_swsetup_CreateContext(ctx)) {
        _mesa_free_context_data(ctx);
        free(c);
        return NULL;
    }
    _swsetup_Wakeup(ctx);
    TNL_CONTEXT(ctx)->Driver.RunPipeline = _tnl_run_pipeline;

    EnterCriticalSection(&wvb_lock);
    wvb_context_count++;
    LeaveCriticalSection(&wvb_lock);
    return c;
}

BOOL wvb_make_current(struct wvb_context *c, HDC hdc)
{
    struct wvb_framebuffer *fb;
    HWND hwnd;
    GLboolean ok;

    if (!c)
        return wvb_release_current(NULL);
    hwnd = WindowFromDC(hdc);
    if (!hwnd) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    EnterCriticalSection(&wvb_lock);
    fb = wvb_find_framebuffer(hwnd);
    if (fb && fb->pf != c->pf) {
        /* Different pixel format: the window was recreated or reused. */
        wvb_unlink_framebuffer(fb);
        fb = NULL;
    }
    if (!fb) {
        fb = wvb_new_framebuffer(hwnd, &c->gl.Visual, c->pf);
        if (fb) {
            fb->next = wvb_fb_list;
            wvb_fb_list = fb;
        }
    }
    LeaveCriticalSection(&wvb_lock);
    if (!fb) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    c->hdc = hdc;
    _glapi_check_multithread();
    ok = _mesa_make_current(&c->gl, &fb->Base, &fb->Base);
    if (ok)
        wvb_check_size(&c->gl, &fb->Base);
    return ok ? TRUE : FALSE;
}

BOOL wvb_release_current(struct wvb_context *c)
{
    GET_CURRENT_CONTEXT(cur);

    if (cur && (!c || cur == &c->gl)) {
        _mesa_flush(cur);
        _mesa_make_current(NULL, NULL, NULL);
    }
    return TRUE;
}

BOOL wvb_destroy_context(struct wvb_context *c)
{
    GLcontext *ctx = &c->gl;
    GLcontext *prev = (GLcontext *) _glapi_get_context();

    /* Mesa needs the context current while it frees its objects. */
    if (prev != ctx)
        _mesa_make_current(ctx, NULL, NULL);
    _mesa_meta_free(ctx);
    _swsetup_DestroyContext(ctx);
    _tnl_DestroyContext(ctx);
    _vbo_DestroyContext(ctx);
    _swrast_DestroyContext(ctx);
    _mesa_free_context_data(ctx);   /* also unbinds it */
    free(c);
    if (prev && prev != ctx)
        _mesa_make_current(prev, prev->WinSysDrawBuffer, prev->WinSysReadBuffer);
    else
        _mesa_make_current(NULL, NULL, NULL);

    EnterCriticalSection(&wvb_lock);
    wvb_context_count--;
    wvb_collect_framebuffers();
    LeaveCriticalSection(&wvb_lock);
    return TRUE;
}

BOOL wvb_copy_context(struct wvb_context *src, struct wvb_context *dst, UINT mask)
{
    if (!src || !dst)
        return FALSE;
    _mesa_copy_context(&src->gl, &dst->gl, mask);
    return TRUE;
}

BOOL wvb_share_lists(struct wvb_context *c1, struct wvb_context *c2)
{
    /* c2 starts using the display lists / textures of c1. */
    if (!c1 || !c2)
        return FALSE;
    return _mesa_share_state(&c2->gl, &c1->gl) ? TRUE : FALSE;
}

BOOL wvb_swap_buffers(HDC hdc)
{
    HWND hwnd = WindowFromDC(hdc);
    struct wvb_framebuffer *fb;
    GET_CURRENT_CONTEXT(ctx);

    if (!hwnd)
        return FALSE;
    EnterCriticalSection(&wvb_lock);
    fb = wvb_find_framebuffer(hwnd);
    if (!fb) {
        LeaveCriticalSection(&wvb_lock);
        return FALSE;
    }
    if (ctx && ctx->WinSysDrawBuffer == &fb->Base)
        _mesa_notifySwapBuffers(ctx);   /* flush vertices, show dirty front */
    if (fb->back) {
        struct wvb_buffer *back = fb->back, *front = fb->front;
        wvb_present(back, hdc);
        /* PFD_SWAP_COPY: the front buffer now holds the back buffer. */
        if (front && front->used && front->bits && back->bits &&
            front->Base.Width == back->Base.Width && front->Base.Height == back->Base.Height)
            memcpy(front->bits, back->bits, (size_t) back->pitch * back->Base.Height);
        if (front)
            front->dirty = GL_FALSE;
    }
    else if (fb->front) {
        wvb_present(fb->front, hdc);
        fb->front->dirty = GL_FALSE;
    }
    /* Follow window size changes even if the application does not call
     * glViewport (the viewport itself is left alone, as GL requires). */
    if (ctx && ctx->WinSysDrawBuffer == &fb->Base)
        wvb_check_size(ctx, &fb->Base);
    LeaveCriticalSection(&wvb_lock);
    return TRUE;
}
