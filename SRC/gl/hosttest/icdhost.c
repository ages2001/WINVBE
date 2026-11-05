/*
 * icdhost.c - Linux-side test of the WVBGL32 ICD code (icd.c + wvbdrv.c +
 * Mesa core) on a fake Win32 GDI (hosttest/windows.h).
 *
 * It plays the role of opengl32.dll: calls the Drv* entry points, routes the
 * OpenGL 1.1 calls used by the ICDTEST scene through the GLCLTPROCTABLE
 * returned by DrvSetContext (so a wrong table order makes checks fail), and
 * GL 1.2+ calls through DrvGetProcAddress.  It then runs the ICDTEST scene
 * and checks what reached the fake window, window resizing, front-buffer
 * drawing, a single-buffered format, list sharing and context deletion.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>

/* ---- fake GDI ---------------------------------------------------------- */

DWORD fake_last_error;
int fake_desktop_bpp = 32;

HDC GetDC(HWND w)
{
    struct fake_dc *dc = calloc(1, sizeof(*dc));
    dc->wnd = w;
    return dc;
}
int ReleaseDC(HWND w, HDC dc) { (void) w; free(dc); return 1; }
HDC CreateCompatibleDC(HDC dc) { (void) dc; return calloc(1, sizeof(struct fake_dc)); }
BOOL DeleteDC(HDC dc) { free(dc); return TRUE; }
HGDIOBJ SelectObject(HDC dc, HGDIOBJ obj)
{
    HGDIOBJ old = dc->bm;
    dc->bm = obj;
    return old;
}
BOOL DeleteObject(HGDIOBJ obj)
{
    struct fake_bitmap *bm = obj;
    if (bm) { free(bm->bits); free(bm); }
    return TRUE;
}
HBITMAP CreateDIBSection(HDC dc, const BITMAPINFO *bmi, UINT usage, void **bits, void *sec, DWORD off)
{
    struct fake_bitmap *bm;
    (void) dc; (void) usage; (void) sec; (void) off;
    if (bmi->bmiHeader.biBitCount != 32 || bmi->bmiHeader.biHeight <= 0) return NULL;
    bm = calloc(1, sizeof(*bm));
    bm->w = bmi->bmiHeader.biWidth;
    bm->h = bmi->bmiHeader.biHeight;
    bm->bits = calloc((size_t) bm->w * bm->h, 4);
    *bits = bm->bits;
    return bm;
}
/* Pixel (x, y) with y from the top, as GDI addresses DCs. */
static uint32_t *dc_pixel(HDC dc, int x, int y)
{
    if (dc->bm) {
        if (x < 0 || y < 0 || x >= dc->bm->w || y >= dc->bm->h) return NULL;
        return &dc->bm->bits[(size_t) (dc->bm->h - 1 - y) * dc->bm->w + x];   /* bottom-up */
    }
    if (dc->wnd) {
        struct fake_window *w = dc->wnd;
        if (x < 0 || y < 0 || x >= w->w || y >= w->h) return NULL;
        return &w->pixels[(size_t) y * w->w + x];
    }
    return NULL;
}
static int blits;
BOOL BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
    int i, j;
    (void) rop;
    blits++;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            uint32_t *s = dc_pixel(src, sx + i, sy + j), *d = dc_pixel(dst, x + i, y + j);
            if (s && d) *d = *s & 0xffffff;
        }
    return TRUE;
}

static struct fake_window *new_window(int w, int h)
{
    struct fake_window *wnd = calloc(1, sizeof(*wnd));
    wnd->w = w; wnd->h = h; wnd->valid = 1;
    wnd->pixels = calloc((size_t) w * h, 4);
    return wnd;
}
static void resize_window(struct fake_window *wnd, int w, int h)
{
    free(wnd->pixels);
    wnd->w = w; wnd->h = h;
    wnd->pixels = calloc((size_t) w * h, 4);
}
/* Window pixel in GL orientation (y from the bottom). */
static uint32_t win_pixel(struct fake_window *wnd, int x, int y)
{
    return wnd->pixels[(size_t) (wnd->h - 1 - y) * wnd->w + x];
}

/* ---- the ICD's exports -------------------------------------------------- */

typedef struct { int cEntries; PROC glDispatchTable[336]; } GLCLTPROCTABLE, *PGLCLTPROCTABLE;
typedef DWORD_PTR DHGLRC;
BOOL DrvValidateVersion(ULONG);
LONG DrvDescribePixelFormat(HDC, LONG, ULONG, PIXELFORMATDESCRIPTOR *);
BOOL DrvSetPixelFormat(HDC, LONG);
DHGLRC DrvCreateContext(HDC);
BOOL DrvDeleteContext(DHGLRC);
BOOL DrvShareLists(DHGLRC, DHGLRC);
PGLCLTPROCTABLE DrvSetContext(HDC, DHGLRC, void *);
BOOL DrvReleaseContext(DHGLRC);
BOOL DrvSwapBuffers(HDC);
PROC DrvGetProcAddress(LPCSTR);
BOOL WvbDllEntry(HINSTANCE, DWORD, LPVOID);

/* ---- scene glue: what opengl32.dll would do ------------------------------ */

static FILE *lgf;
static HDC dc;
static PGLCLTPROCTABLE tbl;

static void log_raw(const char *s, int n)
{
    if (n < 0) n = (int) strlen(s);
    fwrite(s, 1, (size_t) n, lgf);
    fwrite(s, 1, (size_t) n, stdout);
}
static void logf_(const char *f, ...)
{
    char b[2048]; int n; va_list ap;
    va_start(ap, f); n = vsnprintf(b, sizeof(b), f, ap); va_end(ap);
    log_raw(b, n);
}
#define wsprintfA sprintf
#define GETPROC(name) ((void *) DrvGetProcAddress(name))
#define SWAP() DrvSwapBuffers(dc)

/* OpenGL 1.1 calls go through the ICD dispatch table, by index. */
enum {
#define ICD_ENTRY(name) IDX_##name,
#include "../icdtable.h"
#undef ICD_ENTRY
    IDX_COUNT
};
#define T(name, type) ((type) tbl->glDispatchTable[IDX_##name])
#define glBegin T(Begin, void (*)(GLenum))
#define glBindTexture T(BindTexture, void (*)(GLenum, GLuint))
#define glBlendFunc T(BlendFunc, void (*)(GLenum, GLenum))
#define glClear T(Clear, void (*)(GLbitfield))
#define glClearColor T(ClearColor, void (*)(GLclampf, GLclampf, GLclampf, GLclampf))
#define glClearDepth T(ClearDepth, void (*)(GLclampd))
#define glColor3f T(Color3f, void (*)(GLfloat, GLfloat, GLfloat))
#define glColor4f T(Color4f, void (*)(GLfloat, GLfloat, GLfloat, GLfloat))
#define glDepthFunc T(DepthFunc, void (*)(GLenum))
#define glDisable T(Disable, void (*)(GLenum))
#define glDrawBuffer T(DrawBuffer, void (*)(GLenum))
#define glEnable T(Enable, void (*)(GLenum))
#define glEnd T(End, void (*)(void))
#define glFinish T(Finish, void (*)(void))
#define glFlush T(Flush, void (*)(void))
#define glGenTextures T(GenTextures, void (*)(GLsizei, GLuint *))
#define glGetError T(GetError, GLenum (*)(void))
#define glGetIntegerv T(GetIntegerv, void (*)(GLenum, GLint *))
#define glGetString T(GetString, const GLubyte *(*)(GLenum))
#define glIsTexture T(IsTexture, GLboolean (*)(GLuint))
#define glLoadIdentity T(LoadIdentity, void (*)(void))
#define glMatrixMode T(MatrixMode, void (*)(GLenum))
#define glOrtho T(Ortho, void (*)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble))
#define glPixelStorei T(PixelStorei, void (*)(GLenum, GLint))
#define glReadPixels T(ReadPixels, void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid *))
#define glShadeModel T(ShadeModel, void (*)(GLenum))
#define glTexCoord2f T(TexCoord2f, void (*)(GLfloat, GLfloat))
#define glTexEnvi T(TexEnvi, void (*)(GLenum, GLenum, GLint))
#define glTexImage2D T(TexImage2D, void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *))
#define glTexParameteri T(TexParameteri, void (*)(GLenum, GLenum, GLint))
#define glVertex2f T(Vertex2f, void (*)(GLfloat, GLfloat))
#define glVertex3f T(Vertex3f, void (*)(GLfloat, GLfloat, GLfloat))
#define glViewport T(Viewport, void (*)(GLint, GLint, GLsizei, GLsizei))

#define ICDTEST_HOST
#include "../icdtest.c"

static DWORD host_ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void check_window(const char *what, struct fake_window *w, int x, int y, uint32_t rgb)
{
    char msg[200];
    uint32_t p = win_pixel(w, x, y);
    int ok = abs((int) ((p >> 16) & 255) - (int) ((rgb >> 16) & 255)) <= 3 &&
             abs((int) ((p >> 8) & 255) - (int) ((rgb >> 8) & 255)) <= 3 &&
             abs((int) (p & 255) - (int) (rgb & 255)) <= 3;
    snprintf(msg, sizeof(msg), "window: %s at (%d,%d) = %06x (expected %06x)", what, x, y,
             (unsigned) p, (unsigned) rgb);
    check(msg, ok);
}

static int find_format(HDC hdc, int db, int depth)
{
    PIXELFORMATDESCRIPTOR pfd;
    int i, n = DrvDescribePixelFormat(hdc, 1, 0, NULL);
    for (i = 1; i <= n; i++) {
        DrvDescribePixelFormat(hdc, i, sizeof(pfd), &pfd);
        if (!!(pfd.dwFlags & PFD_DOUBLEBUFFER) == db && pfd.cDepthBits == depth && !pfd.cAlphaBits)
            return i;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct fake_window *w1, *w2;
    PIXELFORMATDESCRIPTOR pfd;
    DHGLRC rc1, rc2, rc3;
    HDC dc1, dc2;
    int i, n, pf;
    GLint vp[4];

    lgf = fopen(argc > 1 ? argv[1] : "icdhost.txt", "w");
    WvbDllEntry(NULL, DLL_PROCESS_ATTACH, NULL);
    check("DrvValidateVersion", DrvValidateVersion(1));

    w1 = new_window(W, H);
    dc1 = dc = GetDC(w1);
    n = DrvDescribePixelFormat(dc, 1, 0, NULL);
    logf_("%d pixel formats\n", n);
    for (i = 1; i <= n; i++) {
        DrvDescribePixelFormat(dc, i, sizeof(pfd), &pfd);
        logf_("  %d: flags %08x colour %d (r%d@%d g%d@%d b%d@%d a%d@%d) depth %d stencil %d accum %d\n",
              i, (unsigned) pfd.dwFlags, pfd.cColorBits, pfd.cRedBits, pfd.cRedShift, pfd.cGreenBits,
              pfd.cGreenShift, pfd.cBlueBits, pfd.cBlueShift, pfd.cAlphaBits, pfd.cAlphaShift,
              pfd.cDepthBits, pfd.cStencilBits, pfd.cAccumBits);
    }
    fake_desktop_bpp = 16;
    DrvDescribePixelFormat(dc, 1, sizeof(pfd), &pfd);
    check("16 bpp desktop reports 16-bit 565 formats", pfd.cColorBits == 16 && pfd.cGreenBits == 6);
    fake_desktop_bpp = 32;

    pf = find_format(dc, 1, 24);
    check("double-buffered Z24 format found", pf > 0);
    check("DrvSetPixelFormat", DrvSetPixelFormat(dc, pf));
    rc1 = DrvCreateContext(dc);
    check("DrvCreateContext", rc1 != 0);
    tbl = DrvSetContext(dc, rc1, NULL);
    check("DrvSetContext returns the 336-entry table", tbl && tbl->cEntries == 336);
    for (i = 0, n = 0; tbl && i < 336; i++) n += tbl->glDispatchTable[i] != NULL;
    check("all 336 entries non-NULL", n == 336);

    /* the ICDTEST scene, 1 second of frames */
    run_tests(1, host_ticks, NULL);
    check_window("swapped frame, red texel", w1, 150, 22, 0xff0000);
    check_window("swapped frame, white texel", w1, 234, 106, 0xffffff);
    check_window("swapped frame, clear colour", w1, 124, 124, 0x000040);
    check_window("swapped frame, depth overlap", w1, 70, 190, 0xff00ff);

    /* window resize, picked up by glViewport */
    resize_window(w1, 300, 200);
    glViewport(0, 0, 300, 200);
    glGetIntegerv(GL_VIEWPORT, vp);
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SWAP();
    check_window("resized window cleared green (top right)", w1, 299, 199, 0x00ff00);
    check_window("resized window cleared green (bottom left)", w1, 0, 0, 0x00ff00);

    /* front buffer drawing in a double-buffered window: shown on glFlush,
     * the rest of the window is kept */
    glDrawBuffer(GL_FRONT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0.0, 300, 0.0, 200, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glColor3f(1.0f, 0.0f, 0.0f);
    glBegin(GL_QUADS);
    glVertex2f(10.0f, 10.0f); glVertex2f(50.0f, 10.0f); glVertex2f(50.0f, 50.0f); glVertex2f(10.0f, 50.0f);
    glEnd();
    glFlush();
    check_window("front-buffer quad shown on glFlush", w1, 30, 30, 0xff0000);
    check_window("front-buffer drawing keeps the window", w1, 200, 150, 0x00ff00);
    glDrawBuffer(GL_BACK);
    check("no GL error after front-buffer test", glGetError() == GL_NO_ERROR);

    /* resize without glViewport: picked up at SwapBuffers */
    resize_window(w1, 320, 240);
    SWAP();
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SWAP();
    check_window("resize picked up at SwapBuffers", w1, 319, 239, 0x0000ff);

    /* second context sharing textures, made current on the same window */
    rc2 = DrvCreateContext(dc);
    check("DrvShareLists", DrvShareLists(rc1, rc2));
    check("DrvReleaseContext", DrvReleaseContext(rc1));
    tbl = DrvSetContext(dc, rc2, NULL);
    check("second context current", tbl != NULL);
    check("texture shared with second context", glIsTexture(tex));

    /* single-buffered format on a second window */
    w2 = new_window(64, 64);
    dc2 = dc = GetDC(w2);
    pf = find_format(dc2, 0, 16);
    check("single-buffered Z16 format found", pf > 0);
    DrvSetPixelFormat(dc2, pf);
    rc3 = DrvCreateContext(dc2);
    DrvReleaseContext(rc2);
    tbl = DrvSetContext(dc2, rc3, NULL);
    check("single-buffered context current", tbl != NULL);
    glViewport(0, 0, 64, 64);
    glClearColor(1.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFlush();
    check_window("single-buffered clear shown on glFlush", w2, 32, 32, 0xffff00);

    /* delete a context that is not current, then the current one */
    check("DrvDeleteContext (not current)", DrvDeleteContext(rc1));
    check("still current after deleting another context", glGetString(GL_VENDOR) != NULL);
    check("DrvDeleteContext (current)", DrvDeleteContext(rc3));
    check("DrvDeleteContext (last)", DrvDeleteContext(rc2));
    w1->valid = w2->valid = 0;
    WvbDllEntry(NULL, DLL_PROCESS_DETACH, NULL);
    ReleaseDC(w1, dc1);
    ReleaseDC(w2, dc2);

    logf_("blits: %d\n", blits);
    logf_("host checks: %d run\n", nchecks);
    logf_("checks: %d failed\n", nfailed);
    fclose(lgf);
    return nfailed ? 1 : 0;
}
