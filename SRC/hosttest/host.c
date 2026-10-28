/* host harness glue: segment table, globals normally from vbe.c/enable.c/util.c */
#include "drv.h"
#include <stdlib.h>
#include <stdarg.h>

#define MAXSEL 4096
static BYTE *selbase[MAXSEL];
static WORD nextsel = 0x10;

WORD host_register(void *p)          /* register a 64K block; returns selector */
{
    WORD s = nextsel; nextsel += 8;
    selbase[s] = p;
    return s;
}
WORD host_alloc(DWORD size, BYTE **out) /* consecutive 64K tiles, returns first selector */
{
    DWORD n = (size + 0xFFFF) >> 16, i;
    BYTE *p = calloc(n ? n : 1, 0x10000);
    WORD first = nextsel;
    for (i = 0; i < n; i++) host_register(p + i * 0x10000);
    if (out) *out = p;
    return first;
}
void *host_mkfp(WORD sel, WORD off)
{
    if (!selbase[sel]) { fprintf(stderr, "bad selector %x\n", sel); abort(); }
    return selbase[sel] + off;
}
WORD host_seg(const void *p)
{
    WORD s;
    for (s = 0x10; s < nextsel; s += 8)
        if ((const BYTE *)p >= selbase[s] && (const BYTE *)p < selbase[s] + 0x10000) return s;
    return 0;
}
WORD host_off(const void *p)
{
    WORD s = host_seg(p);
    if (!s) return (WORD)(uintptr_t)p;
    return (WORD)((const BYTE *)p - selbase[s]);
}

DRVSTATE g;
MODEINFO modes[MAX_MODES];
WORD nmodes;
RMREGS rmr;
void vbe_set_bank(WORD b) { g.curbank = b; }
int int10(RMREGS *r) { return 0; }
void vbe_set_dac(WORD s, WORD c, const BYTE *p) {}
void dbg_puts(const char *s) { fputs(s, stderr); }
void dbg_printf(const char *fmt, ...) { (void)fmt; }
WORD get_ini_int(const char *k, WORD d) { return d; }
void get_ini_str(const char *k, const char *d, char *b, WORD n) { strncpy(b, d, n); }

/* driver pieces that live in enable.c / vbe.c (not built on the host) */
const char *trace_last;
WORD trace_fn(const char *s) { trace_last = s; return 0; }
void win9x_check_screen(void) { }
void svga_mark(SHORT l, SHORT t, SHORT r, SHORT b) { (void)l; (void)t; (void)r; (void)b; }
void svga_flush(void) { }
int  svga_copy(SHORT sx, SHORT sy, SHORT dx, SHORT dy, SHORT w, SHORT h)
{ (void)sx; (void)sy; (void)dx; (void)dy; (void)w; (void)h; return 0; }
int  svga_fill(DWORD color, SHORT x, SHORT y, SHORT w, SHORT h)
{ (void)color; (void)x; (void)y; (void)w; (void)h; return 0; }

/* selector services (protected mode only; the host build runs as "real mode") */
WORD sel_alloc(WORD n) { (void)n; return 0; }
void sel_free(WORD s, WORD n) { (void)s; (void)n; }
DWORD sel_getbase(WORD s) { (void)s; return 0; }
int sel_setbase(WORD s, DWORD b) { (void)s; (void)b; return 0; }
int sel_setlimit(WORD s, DWORD l) { (void)s; (void)l; return 0; }
DWORD seg_limit(WORD s) { (void)s; return 0xFFFF; }
