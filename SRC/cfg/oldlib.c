/* WINVBECF for Windows 1.x/2.x: a small sprintf (the one of the C library
   needs the C start-up code).  %d %u %x %X %s %c %%, flags 0 and l, width. */
#include <stdarg.h>
#include <string.h>

int o_sprintf(char *out, const char *fmt, ...)
{
    va_list ap;
    char *o = out;
    va_start(ap, fmt);
    while (*fmt) {
        char tmp[12], *t, *s;
        int width = 0, zero = 0, lng = 0, neg = 0, base = 10, upper = 0, len;
        unsigned long v;
        if (*fmt != '%') { *o++ = *fmt++; continue; }
        fmt++;
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + *fmt++ - '0';
        if (*fmt == 'l') { lng = 1; fmt++; }
        switch (*fmt) {
        case 's':
            s = va_arg(ap, char *);
            if (!s) s = "";
            len = strlen(s);
            while (width-- > len) *o++ = ' ';
            while (*s) *o++ = *s++;
            fmt++;
            continue;
        case 'c':
            *o++ = (char)va_arg(ap, int);
            fmt++;
            continue;
        case 'd':
            if (lng) { long x = va_arg(ap, long); if (x < 0) { neg = 1; x = -x; } v = (unsigned long)x; }
            else { int x = va_arg(ap, int); if (x < 0) { neg = 1; x = -x; } v = (unsigned)x; }
            break;
        case 'X': upper = 1;    /* fall through */
        case 'x': base = 16;    /* fall through */
        case 'u':
            v = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            break;
        case '%':
            *o++ = '%';
            fmt++;
            continue;
        default:
            fmt++;
            continue;
        }
        fmt++;
        t = tmp + sizeof(tmp);
        *--t = 0;
        do {
            int d = (int)(v % base);
            *--t = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
            v /= base;
        } while (v);
        len = strlen(t) + neg;
        if (neg && zero) *o++ = '-';
        while (width-- > len) *o++ = zero ? '0' : ' ';
        if (neg && !zero) *o++ = '-';
        while (*t) *o++ = *t++;
    }
    *o = 0;
    va_end(ap);
    return (int)(o - out);
}
