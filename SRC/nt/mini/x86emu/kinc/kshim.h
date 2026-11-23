/* WINVBE: the C library pieces libx86emu needs, inside a kernel-mode
   miniport (no C runtime). Implemented in ../../emu10.c. */
#ifndef WVB_KSHIM_H
#define WVB_KSHIM_H
#include <stddef.h>
#include <stdarg.h>
void *wvb_calloc(size_t n, size_t sz);
void *wvb_malloc(size_t sz);
void wvb_free(void *p);
int wvb_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
char *strcat(char *d, const char *s);
#define calloc wvb_calloc
#define malloc wvb_malloc
#define free wvb_free
#define vsnprintf wvb_vsnprintf
#endif
