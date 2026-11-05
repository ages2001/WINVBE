/*
 * ICDTEST - WVBGL32.DLL (WINVBE OpenGL ICD) test.
 *   ICDTEST [seconds]
 *
 * Creates a 256x256 window, sets a double-buffered RGBA pixel format through
 * opengl32.dll, draws a Gouraud-shaded triangle, a textured quad, two
 * depth-tested overlapping quads (four triangles), an alpha-blended quad and
 * (OpenGL 2.0) a GLSL-shaded quad, reads pixels back with glReadPixels and
 * checks them.  Prints GL_VENDOR/RENDERER/VERSION/EXTENSIONS and writes
 * ICDTEST.TXT next to the executable; the last line is "checks: n failed".
 * Then the scene is redrawn for [seconds] (default 3) to measure the frame
 * rate.  No C runtime is used.
 *
 * The scene code is also built on Linux by hosttest/icdhost.c (with
 * ICDTEST_HOST defined), which drives the ICD's Drv* functions directly.
 */
#ifndef ICDTEST_HOST
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>

static HANDLE lg;
static HDC dc;

void *memset(void *d, int c, size_t n) { char *p = d; while (n--) *p++ = (char)c; return d; }

static void log_raw(const char *s, int n)
{
    DWORD w;
    if (n < 0) n = lstrlen(s);
    WriteFile(lg, s, (DWORD) n, &w, NULL);
}

static void logf_(const char *f, ...)
{
    char b[1024]; int n; va_list ap;
    va_start(ap, f); n = wvsprintfA(b, f, ap); va_end(ap);
    log_raw(b, n);
    FlushFileBuffers(lg);
}

#define GETPROC(name) ((void *) wglGetProcAddress(name))
#define SWAP() SwapBuffers(dc)
#endif /* !ICDTEST_HOST */

#define W 256
#define H 256

static int nchecks, nfailed;

static int str_has(const char *s, const char *word)
{
    const char *start = s, *p, *q;
    if (!s) return 0;
    for (; *s; s++) {
        if (s != start && s[-1] != ' ') continue;
        for (p = s, q = word; *q && *p == *q; p++, q++) ;
        if (!*q && (*p == ' ' || *p == 0)) return 1;
    }
    return 0;
}

static void check(const char *what, int ok)
{
    nchecks++;
    if (!ok) nfailed++;
    logf_("%s: %s\n", ok ? "ok  " : "FAIL", what);
}

static int iabs_(int v) { return v < 0 ? -v : v; }

/* Read one pixel (from the back buffer) and compare with tolerance. */
static void check_pixel(const char *what, int x, int y, int r, int g, int b, int tol)
{
    GLubyte p[4] = { 0, 0, 0, 0 };
    char msg[160];
    int ok;
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    ok = iabs_(p[0] - r) <= tol && iabs_(p[1] - g) <= tol && iabs_(p[2] - b) <= tol;
    wsprintfA(msg, "%s at (%d,%d) = %d,%d,%d (expected %d,%d,%d +-%d)", what, x, y,
              p[0], p[1], p[2], r, g, b, tol);
    check(msg, ok);
}

static void quad(float x0, float y0, float x1, float y1, float z)
{
    /* two triangles */
    glVertex3f(x0, y0, z); glVertex3f(x1, y0, z); glVertex3f(x1, y1, z);
    glVertex3f(x0, y0, z); glVertex3f(x1, y1, z); glVertex3f(x0, y1, z);
}

static GLuint tex;
static GLuint prog;
static PFNGLUSEPROGRAMPROC p_glUseProgram;

static void setup_texture(void)
{
    static const GLubyte texels[2 * 2 * 4] = {
        255, 0, 0, 255,      0, 255, 0, 255,      /* bottom: red, green */
        0, 0, 255, 255,      255, 255, 255, 255   /* top: blue, white */
    };
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
}

/* GLSL program (OpenGL 2.0 entry points via wglGetProcAddress). */
static void setup_shader(void)
{
    static const char *vs_src = "void main() { gl_Position = ftransform(); }";
    static const char *fs_src = "uniform vec4 c;\n"
                                "void main() { gl_FragColor = c * vec4(1.0, 0.5, 0.0, 1.0); }";
    PFNGLCREATESHADERPROC p_glCreateShader = (PFNGLCREATESHADERPROC) GETPROC("glCreateShader");
    PFNGLSHADERSOURCEPROC p_glShaderSource = (PFNGLSHADERSOURCEPROC) GETPROC("glShaderSource");
    PFNGLCOMPILESHADERPROC p_glCompileShader = (PFNGLCOMPILESHADERPROC) GETPROC("glCompileShader");
    PFNGLGETSHADERIVPROC p_glGetShaderiv = (PFNGLGETSHADERIVPROC) GETPROC("glGetShaderiv");
    PFNGLGETSHADERINFOLOGPROC p_glGetShaderInfoLog = (PFNGLGETSHADERINFOLOGPROC) GETPROC("glGetShaderInfoLog");
    PFNGLCREATEPROGRAMPROC p_glCreateProgram = (PFNGLCREATEPROGRAMPROC) GETPROC("glCreateProgram");
    PFNGLATTACHSHADERPROC p_glAttachShader = (PFNGLATTACHSHADERPROC) GETPROC("glAttachShader");
    PFNGLLINKPROGRAMPROC p_glLinkProgram = (PFNGLLINKPROGRAMPROC) GETPROC("glLinkProgram");
    PFNGLGETPROGRAMIVPROC p_glGetProgramiv = (PFNGLGETPROGRAMIVPROC) GETPROC("glGetProgramiv");
    PFNGLGETUNIFORMLOCATIONPROC p_glGetUniformLocation = (PFNGLGETUNIFORMLOCATIONPROC) GETPROC("glGetUniformLocation");
    PFNGLUNIFORM4FPROC p_glUniform4f = (PFNGLUNIFORM4FPROC) GETPROC("glUniform4f");
    GLuint vs, fs;
    GLint ok = 0, loc;
    char info[512];

    p_glUseProgram = (PFNGLUSEPROGRAMPROC) GETPROC("glUseProgram");
    check("GL 2.0 shader entry points found", p_glCreateShader && p_glShaderSource &&
          p_glCompileShader && p_glGetShaderiv && p_glCreateProgram && p_glAttachShader &&
          p_glLinkProgram && p_glGetProgramiv && p_glUseProgram && p_glGetUniformLocation &&
          p_glUniform4f && p_glGetShaderInfoLog);
    if (!p_glCreateShader || !p_glShaderSource || !p_glCompileShader || !p_glGetShaderiv ||
        !p_glCreateProgram || !p_glAttachShader || !p_glLinkProgram || !p_glGetProgramiv ||
        !p_glUseProgram || !p_glGetUniformLocation || !p_glUniform4f || !p_glGetShaderInfoLog) {
        p_glUseProgram = 0;
        return;
    }
    vs = p_glCreateShader(GL_VERTEX_SHADER);
    p_glShaderSource(vs, 1, &vs_src, NULL);
    p_glCompileShader(vs);
    p_glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) { info[0] = 0; p_glGetShaderInfoLog(vs, sizeof(info), NULL, info); logf_("vertex shader log: %s\n", info); }
    check("vertex shader compiles", ok);
    fs = p_glCreateShader(GL_FRAGMENT_SHADER);
    p_glShaderSource(fs, 1, &fs_src, NULL);
    p_glCompileShader(fs);
    ok = 0;
    p_glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) { info[0] = 0; p_glGetShaderInfoLog(fs, sizeof(info), NULL, info); logf_("fragment shader log: %s\n", info); }
    check("fragment shader compiles", ok);
    prog = p_glCreateProgram();
    p_glAttachShader(prog, vs);
    p_glAttachShader(prog, fs);
    p_glLinkProgram(prog);
    ok = 0;
    p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    check("GLSL program links", ok);
    if (!ok) { p_glUseProgram = 0; return; }
    p_glUseProgram(prog);
    loc = p_glGetUniformLocation(prog, "c");
    check("uniform location found", loc >= 0);
    p_glUniform4f(loc, 1.0f, 1.0f, 1.0f, 1.0f);
    p_glUseProgram(0);
}

static void draw_scene(void)
{
    glViewport(0, 0, W, H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, W, 0.0, H, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glClearColor(0.0f, 0.0f, 0.25f, 1.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* 1. Gouraud triangle, lower left */
    glShadeModel(GL_SMOOTH);
    glBegin(GL_TRIANGLES);
    glColor3f(1.0f, 0.0f, 0.0f); glVertex2f(8.0f, 8.0f);
    glColor3f(0.0f, 1.0f, 0.0f); glVertex2f(120.0f, 8.0f);
    glColor3f(0.0f, 0.0f, 1.0f); glVertex2f(8.0f, 120.0f);
    glEnd();

    /* 2. textured quad (2x2 texture, nearest), lower right */
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glColor3f(1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(136.0f, 8.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(248.0f, 8.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(248.0f, 120.0f);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(136.0f, 120.0f);
    glEnd();
    glDisable(GL_TEXTURE_2D);

    /* 3. depth test, upper left: near magenta first, then far cyan over it */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glBegin(GL_TRIANGLES);
    glColor3f(1.0f, 0.0f, 1.0f); quad(8.0f, 136.0f, 88.0f, 216.0f, 0.5f);
    glColor3f(0.0f, 1.0f, 1.0f); quad(48.0f, 168.0f, 120.0f, 248.0f, -0.5f);
    glEnd();
    glDisable(GL_DEPTH_TEST);

    /* 4. 50% white blended over the clear colour, upper right bottom strip */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 1.0f, 1.0f, 0.5f);
    glBegin(GL_QUADS);
    glVertex2f(136.0f, 136.0f); glVertex2f(248.0f, 136.0f);
    glVertex2f(248.0f, 184.0f); glVertex2f(136.0f, 184.0f);
    glEnd();
    glDisable(GL_BLEND);

    /* 5. GLSL quad, upper right top strip */
    if (p_glUseProgram) {
        p_glUseProgram(prog);
        glBegin(GL_QUADS);
        glVertex2f(136.0f, 196.0f); glVertex2f(248.0f, 196.0f);
        glVertex2f(248.0f, 248.0f); glVertex2f(136.0f, 248.0f);
        glEnd();
        p_glUseProgram(0);
    }
}

static void check_scene(int have_glsl)
{
    GLfloat z = -1.0f;
    char msg[128];
    int r, g, b;
    float fx, fy, wg, wb, wr;

    /* clear colour */
    check_pixel("clear colour", 124, 124, 0, 0, 64, 2);
    check_pixel("clear colour", 2, 253, 0, 0, 64, 2);

    /* Gouraud: vertex colours near the corners, interpolated centre */
    check_pixel("Gouraud near red vertex", 10, 10, 255, 0, 0, 12);
    check_pixel("Gouraud near green vertex", 116, 10, 255 - 255, 255, 0, 20);
    check_pixel("Gouraud near blue vertex", 10, 116, 0, 0, 255, 20);
    fx = 40.5f; fy = 40.5f;
    wg = (fx - 8.0f) / 112.0f; wb = (fy - 8.0f) / 112.0f; wr = 1.0f - wg - wb;
    r = (int) (wr * 255.0f + 0.5f); g = (int) (wg * 255.0f + 0.5f); b = (int) (wb * 255.0f + 0.5f);
    check_pixel("Gouraud interpolated", 40, 40, r, g, b, 4);
    check_pixel("outside Gouraud triangle", 100, 100, 0, 0, 64, 2);

    /* textured quad quadrants */
    check_pixel("texel red", 150, 22, 255, 0, 0, 2);
    check_pixel("texel green", 234, 22, 0, 255, 0, 2);
    check_pixel("texel blue", 150, 106, 0, 0, 255, 2);
    check_pixel("texel white", 234, 106, 255, 255, 255, 2);

    /* depth test */
    check_pixel("depth: near quad only", 20, 150, 255, 0, 255, 2);
    check_pixel("depth: overlap keeps near quad", 70, 190, 255, 0, 255, 2);
    check_pixel("depth: far quad only", 110, 240, 0, 255, 255, 2);
    glReadPixels(70, 190, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &z);
    wsprintfA(msg, "depth readback at overlap = %d/1000 (expected 250)", (int) (z * 1000.0f + 0.5f));
    check(msg, z > 0.24f && z < 0.26f);

    /* blending */
    check_pixel("alpha blend", 190, 160, 128, 128, 160, 3);

    /* GLSL */
    if (have_glsl)
        check_pixel("GLSL fragment shader", 190, 222, 255, 128, 0, 2);

    check("glGetError() == GL_NO_ERROR", glGetError() == GL_NO_ERROR);

    /* diagnostics (no check): the texture as stored */
    {
        GLubyte t[16];
        int k;
        for (k = 0; k < 16; k++) t[k] = 0xEE;
        glBindTexture(GL_TEXTURE_2D, tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, t);
        logf_("info: texture as stored %d %d %d %d | %d %d %d %d | %d %d %d %d | %d %d %d %d\n",
              t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11], t[12], t[13], t[14], t[15]);
    }
}

/* Log GL strings; returns 1 when the context reports OpenGL 2.0 or later. */
static int log_strings(void)
{
    const char *vendor = (const char *) glGetString(GL_VENDOR);
    const char *renderer = (const char *) glGetString(GL_RENDERER);
    const char *version = (const char *) glGetString(GL_VERSION);
    const char *ext = (const char *) glGetString(GL_EXTENSIONS);
    const char *glsl = 0;
    const char *p;
    int n = 0, len = 0, major = 0;

    logf_("GL_VENDOR     %s\nGL_RENDERER   %s\nGL_VERSION    %s\n",
          vendor ? vendor : "(null)", renderer ? renderer : "(null)", version ? version : "(null)");
    if (version && version[0] >= '0' && version[0] <= '9')
        major = version[0] - '0';
    if (major >= 2) {
        glsl = (const char *) glGetString(GL_SHADING_LANGUAGE_VERSION);
        logf_("GL_SHADING_LANGUAGE_VERSION %s\n", glsl ? glsl : "(null)");
    }
    logf_("GL_EXTENSIONS\n");
    if (ext) {
        for (p = ext; *p; ) {
            const char *s;
            while (*p == ' ') p++;
            if (!*p) break;
            for (s = p; *p && *p != ' '; p++) ;
            log_raw("  ", 2);
            log_raw(s, (int) (p - s));
            log_raw("\n", 1);
            n++;
        }
        for (p = ext; *p; p++) len++;
    }
    logf_("%d extensions, %d characters\n", n, len);
    check("GL_VENDOR is WINVBE (the ICD is in use)", vendor && str_has(vendor, "WINVBE"));
    check("GL_VERSION is 2.1 or later", version && (major > 2 || (major == 2 && version[2] >= '1')));
    check("GL_ARB_multitexture listed", str_has(ext, "GL_ARB_multitexture"));
    check("wglGetProcAddress(glActiveTextureARB)", GETPROC("glActiveTextureARB") != 0);
    check("wglGetProcAddress(glBogusFunctionWVB) is NULL", GETPROC("glBogusFunctionWVB") == 0);
    return major >= 2;
}

/* Everything after the context is current; returns the number of failures. */

/* which renderer build WVBGL32.DLL chose (32-bit: SSE2 or x87, loader.c);
   loads WVBGL32.DLL itself where OPENGL32.DLL has not (e.g. GDI Generic) */
static void log_build(void)
{
#ifdef _WIN64
    logf_("ICD build: x64\n");
#else
    if (!GetModuleHandleA("WVBGLS.DLL") && !GetModuleHandleA("WVBGLX.DLL")) {
        HMODULE m = LoadLibraryA("WVBGL32.DLL");
        FARPROC v = m ? GetProcAddress(m, "DrvValidateVersion") : NULL;
        if (v) ((BOOL (WINAPI *)(ULONG)) v)(2);
    }
    logf_("ICD build: %s\n", GetModuleHandleA("WVBGLS.DLL") ? "WVBGLS.DLL (SSE2)" :
          GetModuleHandleA("WVBGLX.DLL") ? "WVBGLX.DLL (x87)" : "none loaded");
#endif
}

static int run_tests(int secs, DWORD (*ticks)(void), void (*pump)(void))
{
    int have_glsl, frames = 0;
    DWORD t0, t;

    have_glsl = log_strings();
    setup_texture();
    if (have_glsl)
        setup_shader();
    draw_scene();
    glFinish();
    check_scene(have_glsl && p_glUseProgram);
    SWAP();
    /* after a swap (PFD_SWAP_COPY) the back buffer still holds the frame */
    check_pixel("back buffer kept after SwapBuffers", 150, 22, 255, 0, 0, 2);

    t0 = ticks();
    while ((t = ticks() - t0) < (DWORD) secs * 1000) {
        if (pump) pump();
        draw_scene();
        SWAP();
        frames++;
    }
    logf_("%d frames in %lu ms (%lu.%lu per second)\n", frames, (unsigned long) t,
          t ? (unsigned long) ((DWORD) frames * 1000UL / t) : 0UL,
          t ? (unsigned long) (((DWORD) frames * 10000UL / t) % 10) : 0UL);
    log_build();
    logf_("%d checks run\n", nchecks);
    logf_("checks: %d failed\n", nfailed);
    return nfailed;
}

#ifndef ICDTEST_HOST
static DWORD win_ticks(void) { return GetTickCount(); }

static void win_pump(void)
{
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessage(&msg);
}

void WINAPI start(void)
{
    char path[MAX_PATH], *e;
    HINSTANCE hi = GetModuleHandle(NULL);
    const char *cmd = GetCommandLine();
    PIXELFORMATDESCRIPTOR pfd;
    WNDCLASS wc;
    RECT rc;
    HWND hwnd;
    HGLRC glrc;
    int pf, n, secs = 3;

    GetModuleFileName(NULL, path, sizeof(path));
    for (e = path + lstrlen(path); e > path && e[-1] != '\\'; e--) ;
    lstrcpy(e, "ICDTEST.TXT");
    lg = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (*cmd == '"') { cmd++; while (*cmd && *cmd != '"') cmd++; if (*cmd) cmd++; }
    else while (*cmd && *cmd != ' ') cmd++;
    while (*cmd == ' ') cmd++;
    if (*cmd >= '0' && *cmd <= '9') secs = *cmd - '0';

    logf_("ICDTEST - WINVBE OpenGL ICD test\n");
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProc; wc.hInstance = hi; wc.lpszClassName = "ICDTEST";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);
    rc.left = 0; rc.top = 0; rc.right = W; rc.bottom = H;
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    /* topmost: the test shows above the program that started it */
    hwnd = CreateWindowEx(WS_EX_TOPMOST, "ICDTEST", "WINVBE OpenGL ICD test",
                        WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                        40, 40, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, hi, NULL);
    win_pump();
    dc = GetDC(hwnd);
    GetClientRect(hwnd, &rc);
    logf_("window client area %dx%d, desktop %d bpp\n", rc.right, rc.bottom,
          GetDeviceCaps(dc, BITSPIXEL) * GetDeviceCaps(dc, PLANES));

    n = DescribePixelFormat(dc, 1, sizeof(pfd), &pfd);
    logf_("%d pixel formats\n", n);
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 24; pfd.cDepthBits = 24;
    pf = ChoosePixelFormat(dc, &pfd);
    logf_("ChoosePixelFormat %d\n", pf);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) {
        logf_("SetPixelFormat failed %lu\n", GetLastError());
        nfailed++;
        goto out;
    }
    DescribePixelFormat(dc, pf, sizeof(pfd), &pfd);
    logf_("pixel format %d: colour %d (alpha %d), depth %d, stencil %d, accum %d, flags %08lx (%s)\n",
          pf, pfd.cColorBits, pfd.cAlphaBits, pfd.cDepthBits, pfd.cStencilBits, pfd.cAccumBits,
          pfd.dwFlags, (pfd.dwFlags & PFD_GENERIC_FORMAT) ? "Microsoft generic" : "ICD");
    check("pixel format is an ICD format (not PFD_GENERIC_FORMAT)", !(pfd.dwFlags & PFD_GENERIC_FORMAT));
    check("pixel format is double-buffered", (pfd.dwFlags & PFD_DOUBLEBUFFER) != 0);
    glrc = wglCreateContext(dc);
    if (!glrc || !wglMakeCurrent(dc, glrc)) {
        logf_("wglCreateContext/wglMakeCurrent failed %lu\n", GetLastError());
        nfailed++;
        goto out;
    }
    run_tests(secs, win_ticks, win_pump);
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(glrc);
    ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
    CloseHandle(lg);
    ExitProcess(nfailed ? 1 : 0);
out:
    log_build();
    logf_("checks: %d failed\n", nfailed);
    CloseHandle(lg);
    ExitProcess(1);
}
#endif /* !ICDTEST_HOST */
