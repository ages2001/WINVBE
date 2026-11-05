/*
 * compat.c - C runtime helpers for WVBGL32.DLL.
 *
 * The DLL links against the plain msvcrt.dll of Windows 95 (VC6
 * redistributable), 98, ME, 2000 and XP without mingw-w64's libmingwex.
 * mingw-w64's headers redirect a few C99 functions to libmingwex wrappers;
 * this file supplies them on top of functions every msvcrt.dll exports.
 */

#include <stdarg.h>
#include <stddef.h>

/* msvcrt.dll exports, declared here to avoid mingw-w64's header redirects. */
__declspec(dllimport) int __cdecl _vsnprintf(char *, size_t, const char *, va_list);
__declspec(dllimport) double __cdecl strtod(const char *, char **);
__declspec(dllimport) void *__cdecl malloc(size_t);
__declspec(dllimport) void __cdecl free(void *);

/* C99 vsnprintf: always NUL-terminates (n > 0) and returns the length the
 * complete output would have.  msvcrt's _vsnprintf returns -1 on truncation
 * and msvcrt 6.0 has no _vscprintf, so measure with a temporary buffer. */
int __cdecl __ms_vsnprintf(char *d, size_t n, const char *fmt, va_list ap)
{
    size_t cap;
    int r;

    if (n > 0 && d) {
        r = _vsnprintf(d, n, fmt, ap);
        if (r >= 0 && (size_t) r < n)
            return r;
        d[n - 1] = '\0';
    }
    for (cap = n >= 256 ? n * 2 : 512; cap <= (1u << 24); cap *= 2) {
        char *tmp = (char *) malloc(cap);
        if (!tmp)
            return -1;
        r = _vsnprintf(tmp, cap, fmt, ap);
        free(tmp);
        if (r >= 0 && (size_t) r < cap)
            return r;
    }
    return -1;
}

int __cdecl wvb_vscprintf(const char *fmt, va_list ap)
{
    return __ms_vsnprintf(NULL, 0, fmt, ap);
}

/* mingw-w64 inlines vsnprintf(NULL, 0, ...) as _vscprintf(), which only
 * msvcrt.dll 7.0 (XP) and later export: satisfy that import here. */
#ifdef _WIN64
#define WVB_IMP_VSCPRINTF "__imp__vscprintf"    /* no leading underscore on x64 */
#else
#define WVB_IMP_VSCPRINTF "__imp___vscprintf"
#endif
int (__cdecl *wvb_imp_vscprintf)(const char *, va_list) __asm__(WVB_IMP_VSCPRINTF) = wvb_vscprintf;

/* libmingwex's C99 strtod; msvcrt's is good enough for Mesa's parsers. */
double __cdecl __strtod(const char *s, char **end)
{
    return strtod(s, end);
}

/* frexp: mingw-w64's libmsvcrt.a links a static copy built for i686 (it
 * uses CMOV); this one is compiled for the Pentium like the rest. */
double __cdecl frexp(double x, int *e)
{
    union { double d; unsigned long long u; } v;
    int ex;

    v.d = x;
    ex = (int) ((v.u >> 52) & 0x7ff);
    if (ex == 0) {                      /* zero or subnormal */
        if (x == 0.0) {
            *e = 0;
            return x;
        }
        v.d = x * 18014398509481984.0;  /* 2^54 */
        ex = (int) ((v.u >> 52) & 0x7ff) - 54;
    }
    else if (ex == 0x7ff) {             /* inf or NaN */
        *e = 0;
        return x;
    }
    *e = ex - 1022;
    v.u = (v.u & 0x800fffffffffffffULL) | 0x3fe0000000000000ULL;
    return v.d;
}

/* Math functions on the x87 FPU.  mingw-w64's msvcrt import library has
 * no call thunks for msvcrt's sin, cos, exp, log, pow and ldexp (its
 * libmingwex normally provides them), so the linker reached them through
 * auto-import pseudo-relocations - which only mingw's start-up code
 * resolves, and this DLL has none: the calls jumped into the import table.
 * These versions need nothing but the FPU (any 387 / Pentium). */
double __cdecl sin(double x)
{
    double r;
    __asm__("fsin" : "=t"(r) : "0"(x));
    return r;
}

double __cdecl cos(double x)
{
    double r;
    __asm__("fcos" : "=t"(r) : "0"(x));
    return r;
}

/* ln x = ln 2 * log2 x */
double __cdecl log(double x)
{
    double r;
    __asm__("fldln2\n\tfxch\n\tfyl2x" : "=t"(r) : "0"(x));
    return r;
}

/* e^x = 2^(x log2 e): 2^frac with f2xm1, the integer part with fscale */
double __cdecl exp(double x)
{
    double r;
    if (x != x) return x;
    if (x > 709.78) return 1.0e308 * 10.0;     /* +inf */
    if (x < -745.2) return 0.0;
    __asm__("fldl2e\n\t"
            "fmulp\n\t"                         /* t = x log2 e */
            "fld %%st(0)\n\t"
            "frndint\n\t"                       /* n, t */
            "fxch\n\t"                          /* t, n */
            "fsub %%st(1), %%st(0)\n\t"         /* t - n, n */
            "f2xm1\n\t"
            "fld1\n\t"
            "faddp\n\t"                         /* 2^(t - n), n */
            "fscale\n\t"
            "fstp %%st(1)"
            : "=t"(r) : "0"(x));
    return r;
}

double __cdecl ldexp(double x, int n)
{
    union { double d; unsigned long long u; } p;
    while (n > 1023) { x *= 8.98846567431158e307; n -= 1023; }      /* 2^1023 */
    while (n < -1022) { x *= 2.2250738585072014e-308; n += 1022; }   /* 2^-1022 */
    p.u = (unsigned long long)(n + 1023) << 52;
    return x * p.d;
}

double __cdecl pow(double x, double y)
{
    double r;
    long long iy;
    if (y == 0.0) return 1.0;
    if (x == 1.0) return 1.0;
    if (x == 0.0) return y > 0.0 ? 0.0 : 1.0e308 * 10.0;
    if (x > 0.0) return exp(y * log(x));
    /* a negative base: integer exponents only */
    iy = (long long)y;
    if ((double)iy != y) return 0.0 / 0.0;
    r = exp(y * log(-x));
    return (iy & 1) ? -r : r;
}
