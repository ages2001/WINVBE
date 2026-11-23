/*
 * WINVBE.386 - video BIOS calls of the system VM through an x86 emulator
 * (Windows 3.x 386 enhanced mode, Windows 95/98/ME).
 *
 * Both run the video BIOS in virtual 8086 mode (the display driver's INT 10h
 * goes through DPMI to V86 mode). A video BIOS that needs real mode -
 * "unreal" addressing of its registers above 1 MB, protected mode, MSRs:
 * Intel graphics of Skylake and later - fails there. WINVBE.386 hooks the
 * system VM's V86 INT 10h and runs the BIOS in libx86emu instead (the same
 * emulator as the Windows NT miniport, ../nt/mini/x86emu): the VM's own
 * memory below 1 MB+64 KB (its interrupt vectors, BIOS data, the caller's
 * buffers and stack, the VGA memory and ROMs as mapped for it), memory above
 * that mapped physically on demand, and the real I/O ports.
 *
 * Linked to a flat binary at a fixed address inside the VxD (vxd/build.sh);
 * the VxD calls the jump table at its start (cdecl):
 *   +0  int ve_init(const VECALLS *c)
 *   +5  int ve_int10(unsigned long high_linear, CRS *client)
 *   +10 int ve_priv(void)              privileged instructions seen
 *   +15 void ve_clear_priv(void)
 */
#include "kshim.h"
#include "../nt/mini/x86emu/include/x86emu.h"

typedef unsigned long ULONG;
typedef unsigned short USHORT;
typedef unsigned char UCHAR;

/* VMM services, called through wrappers in winvbe.asm */
typedef struct {
    void *(*alloc)(ULONG size);             /* _HeapAllocate, zeroed */
    void (*free)(void *p);                  /* _HeapFree */
    void *(*map)(ULONG phys, ULONG size);   /* _MapPhysToLinear (permanent) */
    void (*dbg)(const char *s);             /* debug output (or NULL) */
} VECALLS;

/* Client_Reg_Struc of the VMM */
typedef struct {
    ULONG edi, esi, ebp, res0, ebx, edx, ecx, eax, error, eip;
    USHORT cs, res1;
    ULONG eflags, esp;
    USHORT ss, res2, es, res3, ds, res4, fs, res5, gs, res6;
} CRS;

static VECALLS vc;
static x86emu_t *emu;
static ULONG hl;                    /* high linear address of the VM's memory */
static int priv, fault;

#define NWIN 32                     /* 64 KB windows above 1 MB (mappings are permanent) */
static struct { ULONG base; UCHAR *va; } win[NWIN];
static int nwin;

/* ------------------------------------------------------------------ */
/* the C library pieces libx86emu needs                                */

void *memcpy(void *d, const void *s, size_t n)
{
    UCHAR *p = d; const UCHAR *q = s;
    while (n--) *p++ = *q++;
    return d;
}

void *memset(void *d, int c, size_t n)
{
    UCHAR *p = d;
    while (n--) *p++ = (UCHAR)c;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const UCHAR *p = a, *q = b;
    for (; n; n--, p++, q++) if (*p != *q) return *p - *q;
    return 0;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

char *strcat(char *d, const char *s)
{
    char *r = d;
    while (*d) d++;
    while ((*d++ = *s++)) ;
    return r;
}

void *wvb_malloc(size_t sz) { return vc.alloc(sz ? sz : 1); }
void *wvb_calloc(size_t n, size_t sz) { return vc.alloc(n * sz ? n * sz : 1); }   /* zeroed */
void wvb_free(void *p) { if (p) vc.free(p); }
int wvb_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    (void)fmt; (void)ap;
    if (n) buf[0] = 0;
    return 0;
}

static void dbg(const char *s) { if (vc.dbg) vc.dbg(s); }

static void dbghex(const char *s, ULONG v)
{
    char b[16];
    int i;
    if (!vc.dbg) return;
    vc.dbg(s);
    for (i = 0; i < 8; i++) b[i] = "0123456789ABCDEF"[(v >> (28 - i * 4)) & 15];
    b[8] = ' '; b[9] = 0;
    vc.dbg(b);
}

/* ------------------------------------------------------------------ */

static UCHAR *where(ULONG a, ULONG n)
{
    ULONG b;
    int i;
    if (a + n <= 0x110000) return (UCHAR *)(hl + a);     /* the VM's V86 memory */
    if (a < 0x110000) return 0;
    b = a & ~0xFFFFUL;
    if ((a & 0xFFFF) + n > 0x10000) return 0;
    for (i = 0; i < nwin; i++)
        if (win[i].base == b) return win[i].va + (a - b);
    if (nwin == NWIN) { dbg("WVBEMU: too many mappings\r\n"); return 0; }
    win[nwin].va = vc.map(b, 0x10000);
    if (!win[nwin].va || win[nwin].va == (UCHAR *)-1) { win[nwin].va = 0; return 0; }
    win[nwin].base = b;
    return win[nwin++].va + (a - b);
}

static ULONG rd(ULONG a, ULONG n)
{
    UCHAR *p = where(a, n);
    ULONG v = 0, i;
    if (!p) {
        if (n == 1) return 0xFF;
        for (i = 0; i < n; i++) v |= rd(a + i, 1) << (i * 8);
        return v;
    }
    return n == 1 ? *(volatile UCHAR *)p : n == 2 ? *(volatile USHORT *)p : *(volatile ULONG *)p;
}

static void wr(ULONG a, ULONG v, ULONG n)
{
    UCHAR *p = where(a, n);
    ULONG i;
    if (!p) {
        if (n > 1) for (i = 0; i < n; i++) wr(a + i, (v >> (i * 8)) & 0xFF, 1);
        return;
    }
    if (n == 1) *(volatile UCHAR *)p = (UCHAR)v;
    else if (n == 2) *(volatile USHORT *)p = (USHORT)v;
    else *(volatile ULONG *)p = v;
}

static inline void outb_(USHORT p, UCHAR v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline UCHAR inb_(USHORT p) { UCHAR v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outw_(USHORT p, USHORT v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(p)); }
static inline USHORT inw_(USHORT p) { USHORT v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outl_(USHORT p, ULONG v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline ULONG inl_(USHORT p) { ULONG v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }

static unsigned memio(x86emu_t *e, u32 addr, u32 *val, unsigned type)
{
    unsigned bits = type & 0xFF, n = bits == X86EMU_MEMIO_16 ? 2 : bits == X86EMU_MEMIO_32 ? 4 : 1;
    (void)e;
    switch (type & ~0xFF) {
    case X86EMU_MEMIO_R: case X86EMU_MEMIO_X: *val = rd(addr, n); break;
    case X86EMU_MEMIO_W: wr(addr, *val, n); break;
    case X86EMU_MEMIO_I: *val = n == 1 ? inb_((USHORT)addr) : n == 2 ? inw_((USHORT)addr) : inl_((USHORT)addr); break;
    case X86EMU_MEMIO_O:
        if (n == 1) outb_((USHORT)addr, (UCHAR)*val);
        else if (n == 2) outw_((USHORT)addr, (USHORT)*val);
        else outl_((USHORT)addr, *val);
        break;
    }
    return 0;
}

static int intr(x86emu_t *e, u8 num, unsigned type)
{
    if ((type & 0xFF) == INTR_TYPE_FAULT) {
        ULONG a = e->x86.R_CS_BASE + e->x86.R_EIP;
        dbghex("WVBEMU: exception", num);
        dbghex("at", ((ULONG)e->x86.R_CS << 16) | (e->x86.R_EIP & 0xFFFF));
        dbghex("code", rd(a, 1) | (rd(a + 1, 1) << 8) | (rd(a + 2, 1) << 16) | (rd(a + 3, 1) << 24));
        dbghex("eax", e->x86.R_EAX);
        dbg("\r\n");
        fault = 1;
        x86emu_stop(e);
        return 1;
    }
    return 0;
}

/* before each instruction: the end of the call (the return address
   0000:0000 we pushed) and instructions that fail in V86 mode */
static int code_check(x86emu_t *e)
{
    ULONG a = e->x86.R_CS_BASE + e->x86.R_EIP;
    UCHAR b0 = 0, b1, b2;
    int i;
    if (e->x86.R_CS == 0 && e->x86.R_EIP == 0) return 1;        /* returned */
    if (priv) return 0;
    if (e->x86.R_CR0 & 1) { priv = 1; return 0; }
    for (i = 0; i < 3; i++) {
        b0 = (UCHAR)rd(a, 1);
        if (b0 != 0x66 && b0 != 0x67 && b0 != 0x2E && b0 != 0x3E && b0 != 0x26 &&
            b0 != 0x36 && b0 != 0x64 && b0 != 0x65) break;
        a++;
    }
    if (b0 != 0x0F) return 0;
    b1 = (UCHAR)rd(a + 1, 1);
    b2 = (UCHAR)rd(a + 2, 1);
    if (b1 == 0x20 || b1 == 0x22 || b1 == 0x30 || b1 == 0x32 ||
        (b1 == 0x01 && ((b2 >> 3) & 7) >= 2 && ((b2 >> 3) & 7) <= 3)) {
        dbg("WVBEMU: privileged instruction in the video BIOS\r\n");
        priv = 1;
    }
    return 0;
}

int ve_init(const VECALLS *c)
{
    vc = *c;
    if (emu) return 1;
    emu = x86emu_new(X86EMU_PERM_RWX, X86EMU_PERM_RW);
    if (!emu) return 0;
    x86emu_set_memio_handler(emu, memio);
    x86emu_set_intr_handler(emu, intr);
    x86emu_set_code_handler(emu, code_check);
    dbg("WVBEMU: x86 emulator ready\r\n");
    return 1;
}

int ve_priv(void) { return priv; }
void ve_clear_priv(void) { priv = 0; }

/* The video BIOS's own INT 10h entry. The VM's vector often points to a
   hook of DOS or Windows (pushf; call far C000:xxxx) in which the VMM has
   placed V86 break points (ARPL, #UD in the emulator): follow far calls
   and jumps - immediate, or through a pointer in the hook's segment - to
   the ROM (C000h-DFFFh). */
static int in_rom(USHORT seg) { return seg >= 0xC000 && seg < 0xE000; }

static ULONG vbios_entry(void)
{
    USHORT seg = (USHORT)rd(0x42, 2), off = (USHORT)rd(0x40, 2);
    int depth, i;
    for (depth = 0; depth < 4 && !in_rom(seg); depth++) {
        ULONG base = (ULONG)seg << 4;
        USHORT ns = 0, no = 0;
        for (i = 0; i < 64 && !ns; i++) {
            ULONG a = base + (USHORT)(off + i);
            UCHAR op = (UCHAR)rd(a, 1), m = (UCHAR)rd(a + 1, 1);
            if (op == 0x9A || op == 0xEA) {                     /* call/jmp far ptr16:16 */
                no = (USHORT)rd(a + 1, 2); ns = (USHORT)rd(a + 3, 2);
            } else if (op == 0x2E && (UCHAR)rd(a + 1, 1) == 0xFF) {   /* cs: call/jmp far [disp16] */
                m = (UCHAR)rd(a + 2, 1);
                if (m == 0x1E || m == 0x2E) {
                    USHORT d = (USHORT)rd(a + 3, 2);
                    no = (USHORT)rd(base + d, 2); ns = (USHORT)rd(base + d + 2, 2);
                }
            } else if (op == 0xFF && (m == 0x1E || m == 0x2E)) {      /* call/jmp far [disp16] (DS = CS in hooks) */
                USHORT d = (USHORT)rd(a + 2, 2);
                no = (USHORT)rd(base + d, 2); ns = (USHORT)rd(base + d + 2, 2);
            }
            if (ns && (ns < 0x40 || ns >= 0xF000)) ns = 0;      /* not a code address of interest */
        }
        if (!ns) break;
        seg = ns; off = no;
    }
    if (!in_rom(seg)) return 0;
    dbghex("WVBEMU: video BIOS INT 10h at", ((ULONG)seg << 16) | off);
    dbg("\r\n");
    return ((ULONG)seg << 16) | off;
}

static ULONG entry10;

/* the VM's INT 10h with its registers and stack; 1 = done (client updated) */
int ve_int10(ULONG high_linear, CRS *c)
{
    x86emu_t *e = emu;
    USHORT ip, cs, sp;
    ULONG ss;
    if (!e) return 0;
    hl = high_linear;
    if (!entry10) entry10 = vbios_entry();
    if (entry10) { ip = (USHORT)entry10; cs = (USHORT)(entry10 >> 16); }
    else { ip = (USHORT)rd(0x40, 2); cs = (USHORT)rd(0x42, 2); }
    e->x86.R_EAX = c->eax; e->x86.R_EBX = c->ebx; e->x86.R_ECX = c->ecx; e->x86.R_EDX = c->edx;
    e->x86.R_ESI = c->esi; e->x86.R_EDI = c->edi; e->x86.R_EBP = c->ebp;
    e->x86.R_EFLG = (c->eflags & 0x0DD5) | 0x0202;
    e->x86.R_CR0 = 0;
    x86emu_set_seg_register(e, e->x86.R_ES_SEL, c->es);
    x86emu_set_seg_register(e, e->x86.R_DS_SEL, c->ds);
    x86emu_set_seg_register(e, e->x86.R_FS_SEL, c->fs);
    x86emu_set_seg_register(e, e->x86.R_GS_SEL, c->gs);
    x86emu_set_seg_register(e, e->x86.R_SS_SEL, c->ss);
    /* the IRET frame of the INT on the VM's stack: flags, 0000:0000 */
    sp = (USHORT)c->esp;
    ss = (ULONG)c->ss << 4;
    sp -= 2; wr(ss + sp, c->eflags & 0xFFFF, 2);
    sp -= 2; wr(ss + sp, 0, 2);
    sp -= 2; wr(ss + sp, 0, 2);
    e->x86.R_ESP = sp;
    x86emu_set_seg_register(e, e->x86.R_CS_SEL, cs);
    e->x86.R_EIP = ip;
    fault = 0;
    e->max_instr = e->x86.R_TSC + 200000000ULL;
    x86emu_run(e, X86EMU_RUN_LOOP | X86EMU_RUN_MAX_INSTR);
    if (fault || e->x86.R_CS != 0 || e->x86.R_EIP != 0) {
        dbg("WVBEMU: the video BIOS call did not return\r\n");
        return 0;
    }
    c->eax = e->x86.R_EAX; c->ebx = e->x86.R_EBX; c->ecx = e->x86.R_ECX; c->edx = e->x86.R_EDX;
    c->esi = e->x86.R_ESI; c->edi = e->x86.R_EDI; c->ebp = e->x86.R_EBP;
    c->eflags = (c->eflags & ~0x0DD5UL) | (e->x86.R_EFLG & 0x0DD5);
    c->es = e->x86.R_ES; c->ds = e->x86.R_DS; c->fs = e->x86.R_FS; c->gs = e->x86.R_GS;
    return 1;
}
