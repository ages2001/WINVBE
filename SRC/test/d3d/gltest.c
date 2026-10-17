/*
 * GLTEST - OpenGL test (Windows NT 3.5 and later, Windows 95 with OpenGL)
 *   GLTEST [seconds]
 * A 400x300 window, double-buffered pixel format, a rotating shaded
 * triangle (Microsoft's generic OpenGL renders it, GDI shows it through
 * the display driver). Writes GLTEST.TXT.
 */
#include <windows.h>
#include <GL/gl.h>

static HANDLE lg;
void *memset(void *d, int c, size_t n) { char *p = d; while (n--) *p++ = (char)c; return d; }
static void logf_(const char *f, ...)
{
    char b[512]; DWORD n; va_list ap;
    va_start(ap, f); n = wvsprintfA(b, f, ap); va_end(ap);
    WriteFile(lg, b, n, &n, NULL); FlushFileBuffers(lg);
}

void WINAPI start(void)
{
    char path[MAX_PATH], *e;
    HINSTANCE hi = GetModuleHandle(NULL);
    const char *cmd = GetCommandLine();
    PIXELFORMATDESCRIPTOR pfd;
    WNDCLASS wc;
    HWND hwnd;
    HDC dc;
    HGLRC rc;
    MSG msg;
    int pf, secs = 6, frames = 0;
    DWORD t0, t;
    float ang = 0.0f;

    GetModuleFileName(NULL, path, sizeof(path));
    for (e = path + lstrlen(path); e > path && e[-1] != '\\'; e--) ;
    lstrcpy(e, "GLTEST.TXT");
    lg = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (*cmd == '"') { cmd++; while (*cmd && *cmd != '"') cmd++; if (*cmd) cmd++; }
    else while (*cmd && *cmd != ' ') cmd++;
    while (*cmd == ' ') cmd++;
    if (*cmd >= '0' && *cmd <= '9') secs = *cmd - '0';
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProc; wc.hInstance = hi; wc.lpszClassName = "GLTEST";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);
    hwnd = CreateWindow("GLTEST", "OpenGL test", WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                        40, 40, 400, 300, NULL, NULL, hi, NULL);
    dc = GetDC(hwnd);
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 24; pfd.cDepthBits = 16;
    pf = ChoosePixelFormat(dc, &pfd);
    logf_("ChoosePixelFormat %d\n", pf);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) { logf_("SetPixelFormat failed %lu\n", GetLastError()); goto out; }
    DescribePixelFormat(dc, pf, sizeof(pfd), &pfd);
    logf_("pixel format: color %d bits, depth %d, flags %08lx\n", pfd.cColorBits, pfd.cDepthBits, pfd.dwFlags);
    rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) { logf_("wglCreateContext failed %lu\n", GetLastError()); goto out; }
    logf_("GL_VENDOR %s\nGL_RENDERER %s\nGL_VERSION %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER),
          glGetString(GL_VERSION));
    t0 = GetTickCount();
    while ((t = GetTickCount() - t0) < (DWORD)secs * 1000) {
        RECT r;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessage(&msg);
        GetClientRect(hwnd, &r);
        glViewport(0, 0, r.right, r.bottom);
        glClearColor(0.1f, 0.1f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glLoadIdentity();
        glRotatef(ang, 0.0f, 0.0f, 1.0f);
        glBegin(GL_TRIANGLES);
        glColor3f(1.0f, 0.0f, 0.0f); glVertex2f(0.0f, 0.8f);
        glColor3f(0.0f, 1.0f, 0.0f); glVertex2f(-0.7f, -0.5f);
        glColor3f(0.0f, 0.0f, 1.0f); glVertex2f(0.7f, -0.5f);
        glEnd();
        SwapBuffers(dc);
        ang += 3.0f;
        frames++;
    }
    logf_("%d frames in %lu ms (%lu per second), glGetError %u\n", frames, t, t ? (DWORD)frames * 1000UL / t : 0,
          glGetError());
    Sleep(3000);
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
out:
    logf_("done\n");
    CloseHandle(lg);
    ExitProcess(0);
}
