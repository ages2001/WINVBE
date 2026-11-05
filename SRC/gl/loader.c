/*
 * loader.c - WVBGL32.DLL on 32-bit Windows: the OpenGL ICD entry points of
 * WINVBE, which pick one of two builds of the renderer at run time:
 *
 *   WVBGLS.DLL  compiled for SSE2 (Pentium 4, Pentium M, Athlon 64 and
 *               later), used when the CPU has SSE2 and the operating system
 *               lets programs use it;
 *   WVBGLX.DLL  compiled for the x87 FPU and the Pentium (every CPU).
 *
 * "The operating system lets programs use it" is tested, not assumed from
 * the Windows version: an SSE2 instruction is executed under a structured
 * exception handler. It runs where the OS (Windows 98, 2000, XP) or a driver
 * such as WINSIMD (Windows 95, NT 3.x, NT 4.0) has set CR4.OSFXSR and so
 * saves the XMM registers on task switches; elsewhere it raises an invalid
 * opcode exception and WVBGLX.DLL is used. If WVBGLS.DLL cannot be loaded,
 * WVBGLX.DLL is used as well. Environment variable WVBGL_X87=1 forces the
 * x87 build (comparisons).
 *
 * The chosen DLL is loaded on the first call of any entry point (not in
 * DllMain), and its functions are called directly: what DrvGetProcAddress
 * and DrvSetContext return comes from that DLL, so OpenGL calls do not pass
 * through this one. No C runtime; imports from KERNEL32 only.
 */

#include <windows.h>

static CRITICAL_SECTION lk;
static HINSTANCE self;
static volatile LONG state;             /* 0 not tried, 1 loaded, -1 failed */
static HMODULE impl;

/* ---- SSE2: CPU and operating system ------------------------------------ */

/* CPUID leaf 1 EDX: bit 24 FXSR, 25 SSE, 26 SSE2 (CPUID itself only where
   EFLAGS.ID can be toggled: not on a 386 or an early 486) */
static int cpu_sse2(void)
{
    DWORD a, b, d;
    __asm__ volatile ("pushfl\n\tpopl %0\n\tmovl %0, %1\n\txorl $0x200000, %0\n\t"
                      "pushl %0\n\tpopfl\n\tpushfl\n\tpopl %0\n\tpushl %1\n\tpopfl"
                      : "=&r"(a), "=&r"(b));
    if (!((a ^ b) & 0x200000))
        return 0;
    __asm__ volatile ("cpuid" : "=a"(a), "=d"(d) : "a"(0) : "ebx", "ecx");
    if (a < 1)
        return 0;
    __asm__ volatile ("cpuid" : "=a"(a), "=d"(d) : "a"(1) : "ebx", "ecx");
    return (d & 0x07000000UL) == 0x07000000UL;
}

/* wvb_probe_sse2(): 1 if PXOR xmm0, xmm0 runs, 0 if it raised an exception.
   Its own SEH frame on the stack (FS:[0]); the handler resumes at
   wvb_probe_resume with EAX = 0. */
extern int wvb_probe_sse2(void);
extern char wvb_probe_resume[];

EXCEPTION_DISPOSITION __cdecl wvb_probe_handler(EXCEPTION_RECORD *er, void *frame, CONTEXT *ctx, void *dc)
{
    (void) frame; (void) dc;
    if (er->ExceptionFlags & 6)         /* unwinding */
        return ExceptionContinueSearch;
    ctx->Eax = 0;
    ctx->Eip = (DWORD) wvb_probe_resume;
    return ExceptionContinueExecution;
}

__asm__(
    ".text\n"
    ".globl _wvb_probe_sse2\n"
    "_wvb_probe_sse2:\n"
    "    pushl $_wvb_probe_handler\n"
    "    pushl %fs:0\n"
    "    movl %esp, %fs:0\n"
    "    movl $1, %eax\n"
    "    pxor %xmm0, %xmm0\n"           /* #UD without CR4.OSFXSR */
    "    addsd %xmm0, %xmm0\n"
    ".globl _wvb_probe_resume\n"
    "_wvb_probe_resume:\n"
    "    popl %fs:0\n"
    "    addl $4, %esp\n"
    "    ret\n");

static int use_sse2(void)
{
    char v[8];
    if (GetEnvironmentVariableA("WVBGL_X87", v, sizeof(v)) && v[0] == '1')
        return 0;
    return cpu_sse2() && wvb_probe_sse2();
}

/* ---- loading -------------------------------------------------------------- */

static HMODULE load_beside(const char *name)
{
    char path[MAX_PATH + 16];
    DWORD n = GetModuleFileNameA(self, path, MAX_PATH), i;
    while (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/' && path[n - 1] != ':')
        n--;
    for (i = 0; name[i]; i++)
        path[n + i] = name[i];
    path[n + i] = 0;
    return LoadLibraryA(path);
}

#define ENTRIES(X) \
    X(DrvCopyContext, 3) X(DrvCreateContext, 1) X(DrvCreateLayerContext, 2) X(DrvDeleteContext, 1) \
    X(DrvDescribeLayerPlane, 5) X(DrvDescribePixelFormat, 4) X(DrvGetLayerPaletteEntries, 5) \
    X(DrvGetProcAddress, 1) X(DrvRealizeLayerPalette, 3) X(DrvReleaseContext, 1) \
    X(DrvSetCallbackProcs, 2) X(DrvSetContext, 3) X(DrvSetLayerPaletteEntries, 5) \
    X(DrvSetPixelFormat, 2) X(DrvShareLists, 2) X(DrvSwapBuffers, 1) X(DrvSwapLayerBuffers, 2) \
    X(DrvValidateVersion, 1)

#define PTR(n, a) static FARPROC p_##n;
ENTRIES(PTR)

static int load(void)
{
    if (state)
        return state > 0;
    EnterCriticalSection(&lk);
    if (!state) {
        HMODULE m = NULL;
        if (use_sse2())
            m = load_beside("WVBGLS.DLL");
        if (!m)
            m = load_beside("WVBGLX.DLL");
        if (m) {
#define GET(n, a) p_##n = GetProcAddress(m, #n);
            ENTRIES(GET)
            impl = m;
        }
        state = m && p_DrvValidateVersion && p_DrvSetContext ? 1 : -1;
    }
    LeaveCriticalSection(&lk);
    return state > 0;
}

/* stdcall entry points by argument count: every argument is 32 bits */
#define A1 DWORD a
#define A2 DWORD a, DWORD b
#define A3 DWORD a, DWORD b, DWORD c
#define A4 DWORD a, DWORD b, DWORD c, DWORD d
#define A5 DWORD a, DWORD b, DWORD c, DWORD d, DWORD e
#define V1 a
#define V2 a, b
#define V3 a, b, c
#define V4 a, b, c, d
#define V5 a, b, c, d, e
#define STUB(n, k) \
    DWORD APIENTRY n(A##k) \
    { \
        if (!load() || !p_##n) return 0; \
        return ((DWORD (APIENTRY *)(A##k)) p_##n)(V##k); \
    }
ENTRIES(STUB)

BOOL WINAPI WvbLoaderEntry(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void) reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        /* DisableThreadLibraryCalls is not on NT 3.1's KERNEL32: resolve it at
           run time so the DLL still loads there (it is only an optimisation). */
        HMODULE k = GetModuleHandleA("KERNEL32.DLL");
        BOOL (WINAPI *dtlc)(HMODULE) = k ? (BOOL (WINAPI *)(HMODULE))
            GetProcAddress(k, "DisableThreadLibraryCalls") : NULL;
        self = hinst;
        if (dtlc)
            dtlc(hinst);
        InitializeCriticalSection(&lk);
    }
    /* the renderer DLL stays loaded until the process ends (no FreeLibrary
       in DllMain) */
    return TRUE;
}
