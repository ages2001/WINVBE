/*
 * WINVBE - utilities: debug output, INI access, misc math
 */
#include "drv.h"
#include <stdarg.h>
#include <conio.h>

#ifdef OLDWIN
/* Windows 1.x/2.x: settings live in WIN.INI [WINVBE]; GetProfileString is
   resolved at run time so that only KERNEL.47/50 are imported */
extern WORD  __far __pascal GetModuleHandle(LPSTR);
extern DWORD __far __pascal GetProcAddress(WORD, LPSTR);
typedef int (__far __pascal *PGETPROFSTR)(LPSTR app, LPSTR key, LPSTR def, LPSTR ret, int size);
static PGETPROFSTR pGetProfileString;
#else
extern int  __far __pascal GetPrivateProfileInt(LPSTR app, LPSTR key, int def, LPSTR file);
extern int  __far __pascal GetPrivateProfileString(LPSTR app, LPSTR key, LPSTR def, LPSTR ret, int size, LPSTR file);
#endif

static char ini_app[]  = "WINVBE";
static char ini_file[] = "SYSTEM.INI";
WORD dbg_port = 0xE9;

static void dbg_putc(char c)
{
    if (dbg_port == 0xE9) {
        outp(0xE9, c);
    } else {
        WORD n = 0;
        while (!(inp(dbg_port + 5) & 0x20) && --n)
            ;
        outp(dbg_port, c);
    }
}

void dbg_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            dbg_putc('\r');
        dbg_putc(*s++);
    }
}

static char *utoa_(char *end, DWORD v, WORD base, WORD width, char pad)
{
    static const char dig[] = "0123456789ABCDEF";
    char *p = end;
    *p = 0;
    do {
        *--p = dig[(WORD)(v % base)];
        v /= base;
        if (width) width--;
    } while (v);
    while (width) { *--p = pad; width--; }
    return p;
}

void dbg_printf(const char *fmt, ...)
{
    char out[160];
    char num[24];
    char *o = out, *e = out + sizeof(out) - 1;
    va_list ap;
    va_start(ap, fmt);
    while (*fmt && o < e) {
        char c = *fmt++;
        if (c != '%') { *o++ = c; continue; }
        {
            WORD width = 0, lng = 0;
            char pad = ' ';
            const char *s;
            DWORD v;
            if (*fmt == '0') { pad = '0'; fmt++; }
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
            if (*fmt == 'l') { lng = 1; fmt++; }
            c = *fmt++;
            switch (c) {
            case 'd':
                if (lng) { LONG l = va_arg(ap, LONG); if (l < 0) { *o++ = '-'; l = -l; } v = l; }
                else { SHORT i = va_arg(ap, SHORT); if (i < 0) { *o++ = '-'; i = -i; } v = (WORD)i; }
                s = utoa_(num + 23, v, 10, width, pad);
                break;
            case 'u':
                v = lng ? va_arg(ap, DWORD) : va_arg(ap, WORD);
                s = utoa_(num + 23, v, 10, width, pad);
                break;
            case 'x': case 'X':
                v = lng ? va_arg(ap, DWORD) : va_arg(ap, WORD);
                s = utoa_(num + 23, v, 16, width, pad);
                break;
            case 'p': {
                LPVOID p = va_arg(ap, LPVOID);
                char *q;
                utoa_(num + 23, FP_OFF(p), 16, 4, '0');
                num[18] = ':';
                s = utoa_(num + 18, FP_SEG(p), 16, 4, '0');
                break; }
            case 's':
                s = va_arg(ap, char *);
                break;
            case 'S': {   /* far string */
                LPSTR fs = va_arg(ap, LPSTR);
                while (*fs && o < e) *o++ = *fs++;
                s = "";
                break; }
            case 'c':
                num[0] = (char)va_arg(ap, int); num[1] = 0; s = num;
                break;
            default:
                num[0] = c; num[1] = 0; s = num;
            }
            while (*s && o < e) *o++ = *s++;
        }
    }
    *o = 0;
    va_end(ap);
    dbg_puts(out);
}

WORD get_ini_int(const char *key, WORD def)
{
    char buf[16];
    char *p = buf;
    WORD v = 0, base = 10;
    get_ini_str(key, "", buf, sizeof(buf));
    if (!buf[0])
        return def;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    for (; *p; p++) {
        char c = *p;
        WORD d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if ((c == 'h' || c == 'H') && base == 10) {
            /* reparse as hex */
            WORD w = 0; char *q;
            for (q = buf; q < p; q++) {
                char cc = *q;
                w <<= 4;
                if (cc >= '0' && cc <= '9') w += cc - '0';
                else if (cc >= 'a' && cc <= 'f') w += cc - 'a' + 10;
                else if (cc >= 'A' && cc <= 'F') w += cc - 'A' + 10;
            }
            return w;
        }
        else break;
        v = v * base + d;
    }
    return v;
}

void get_ini_str(const char *key, const char *def, char *buf, WORD n)
{
#ifdef OLDWIN
    if (!pGetProfileString)
        pGetProfileString = (PGETPROFSTR)GetProcAddress(GetModuleHandle((LPSTR)"KERNEL"), (LPSTR)MK_FP(0, 58));
    if (pGetProfileString)
        pGetProfileString((LPSTR)ini_app, (LPSTR)key, (LPSTR)def, (LPSTR)buf, n);
    else {
        WORD i;
        for (i = 0; def[i] && i + 1 < n; i++) buf[i] = def[i];
        buf[i] = 0;
    }
    (void)ini_file;
#else
    GetPrivateProfileString((LPSTR)ini_app, (LPSTR)key, (LPSTR)def, (LPSTR)buf, n, (LPSTR)ini_file);
#endif
}

DWORD isqrt32(DWORD v)
{
    DWORD r = 0, b = 1UL << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}
