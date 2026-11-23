/*
 * WINVBE - video BIOS calls through an x86 emulator (Windows NT x86).
 * See emu10.h. Built with libx86emu (x86emu/, MIT/X11 style license).
 */
#include <ntdef.h>
#include "x86emu/include/x86emu.h"
#include "emu10.h"

/* ntoskrnl.exe (all present in NT 3.1; the third argument of MmMapIoSpace
   was a BOOLEAN "cache enable" there, the same values) */
typedef enum { WvbNonPagedPool = 0 } WVB_POOL;
NTSYSAPI PVOID NTAPI ExAllocatePool(WVB_POOL type, SIZE_T size);
NTSYSAPI VOID NTAPI ExFreePool(PVOID p);
NTSYSAPI PVOID NTAPI MmMapIoSpace(PHYSICAL_ADDRESS pa, SIZE_T size, int cache);
NTSYSAPI VOID NTAPI MmUnmapIoSpace(PVOID va, SIZE_T size);

void (*emu_dbg)(const char *fmt, ...);
#define DBG(...) do { if (emu_dbg) emu_dbg(__VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ */
/* what libx86emu needs of a C library                                 */

void *wvb_malloc(size_t sz)
{
    size_t *p = ExAllocatePool(WvbNonPagedPool, sz + 8);
    if (!p) return NULL;
    p[0] = sz;
    return (char *)p + 8;
}

void *wvb_calloc(size_t n, size_t sz)
{
    void *p = wvb_malloc(n * sz);
    if (p) memset(p, 0, n * sz);
    return p;
}

void wvb_free(void *p)
{
    if (p) ExFreePool((char *)p - 8);
}

/* the emulator's own log (disassembly) is not kept */
int wvb_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    (void)fmt; (void)ap;
    if (n) buf[0] = 0;
    return 0;
}

char *strcat(char *d, const char *s)
{
    char *r = d;
    while (*d) d++;
    while ((*d++ = *s++)) ;
    return r;
}

/* ------------------------------------------------------------------ */
/* the emulated machine                                                */

#define LOWSIZE 0xA0000UL
#define NWIN    8               /* mapped 64 KB windows above 1 MB */
#define STUB    0x0600          /* 0000:0600: HLT, where a call returns to */
#define STKSEG  0x1000          /* stack 1000:FFF0 */

typedef struct {
    x86emu_t *e;
    UCHAR *low;                 /* 0 - 9FFFF, private */
    UCHAR *vga;                 /* A0000 - BFFFF, physical */
    UCHAR *rom;                 /* C0000 - FFFFF, physical */
    struct { ULONG base; UCHAR *va; } win[NWIN];
    ULONG next;
    int fault;
    int priv;                   /* privileged instructions seen */
} EMU;

static UCHAR *map_phys(ULONG pa, ULONG size)
{
    PHYSICAL_ADDRESS p;
    p.QuadPart = pa;
    return MmMapIoSpace(p, size, 0);      /* MmNonCached */
}

/* the host address of emulated memory, NULL if none; *dev: device memory
   (accessed with the size the BIOS uses, registers need it) */
static UCHAR *where(EMU *m, ULONG a, ULONG n, int *dev)
{
    ULONG b, i;
    *dev = 0;
    if (a + n <= LOWSIZE) return m->low + a;
    *dev = 1;
    if (a >= 0xA0000 && a + n <= 0xC0000) return m->vga ? m->vga + (a - 0xA0000) : NULL;
    if (a >= 0xC0000 && a + n <= 0x100000) return m->rom ? m->rom + (a - 0xC0000) : NULL;
    if (a < 0x100000) return NULL;
    b = a & ~0xFFFFUL;
    if ((a & 0xFFFF) + n > 0x10000) return NULL;        /* split by the caller */
    for (i = 0; i < NWIN; i++)
        if (m->win[i].va && m->win[i].base == b) return m->win[i].va + (a - b);
    i = m->next++ % NWIN;
    if (m->win[i].va) MmUnmapIoSpace(m->win[i].va, 0x10000);
    m->win[i].base = b;
    m->win[i].va = map_phys(b, 0x10000);
    if (!m->win[i].va) { DBG("emu: cannot map %x\n", b); return NULL; }
    return m->win[i].va + (a - b);
}

static ULONG mem_rd(EMU *m, ULONG a, ULONG n)
{
    int dev;
    UCHAR *p = where(m, a, n, &dev);
    if (!p) {
        ULONG v = 0, i;
        if (n == 1) return 0xFF;
        for (i = 0; i < n; i++) v |= mem_rd(m, a + i, 1) << (i * 8);
        return v;
    }
    switch (n) {
    case 1: return *(volatile UCHAR *)p;
    case 2: return *(volatile USHORT *)p;
    default: return *(volatile ULONG *)p;
    }
}

static void mem_wr(EMU *m, ULONG a, ULONG v, ULONG n)
{
    int dev;
    UCHAR *p = where(m, a, n, &dev);
    if (!p) {
        ULONG i;
        if (n == 1) return;
        for (i = 0; i < n; i++) mem_wr(m, a + i, (v >> (i * 8)) & 0xFF, 1);
        return;
    }
    switch (n) {
    case 1: *(volatile UCHAR *)p = (UCHAR)v; break;
    case 2: *(volatile USHORT *)p = (USHORT)v; break;
    default: *(volatile ULONG *)p = v; break;
    }
}

static inline void outb_(USHORT p, UCHAR v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline UCHAR inb_(USHORT p) { UCHAR v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outw_(USHORT p, USHORT v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(p)); }
static inline USHORT inw_(USHORT p) { USHORT v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outl_(USHORT p, ULONG v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline ULONG inl_(USHORT p) { ULONG v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }

static unsigned memio(x86emu_t *e, u32 addr, u32 *val, unsigned type)
{
    EMU *m = e->_private;
    unsigned bits = type & 0xFF, n = bits == X86EMU_MEMIO_16 ? 2 : bits == X86EMU_MEMIO_32 ? 4 : 1;
    switch (type & ~0xFF) {
    case X86EMU_MEMIO_R:
    case X86EMU_MEMIO_X:
        *val = mem_rd(m, addr, n);
        break;
    case X86EMU_MEMIO_W:
        mem_wr(m, addr, *val, n);
        break;
    case X86EMU_MEMIO_I:
        *val = n == 1 ? inb_((USHORT)addr) : n == 2 ? inw_((USHORT)addr) : inl_((USHORT)addr);
        break;
    case X86EMU_MEMIO_O:
        if (n == 1) outb_((USHORT)addr, (UCHAR)*val);
        else if (n == 2) outw_((USHORT)addr, (USHORT)*val);
        else outl_((USHORT)addr, *val);
        break;
    }
    return 0;
}

/* software interrupts go through the vector table (0 = not handled
   here); an exception (#UD, #GP...) ends the call */
static int intr(x86emu_t *e, u8 num, unsigned type)
{
    EMU *m = e->_private;
    if ((type & 0xFF) == INTR_TYPE_FAULT) {
        DBG("emu: exception %x at %x:%x\n", num, e->x86.R_CS, e->x86.R_EIP);
        m->fault = 1;
        x86emu_stop(e);
        return 1;
    }
    return 0;
}

/* before each instruction: note the ones a BIOS can only run in real
   mode, not in virtual 8086 mode (they #GP there) */
static int code_check(x86emu_t *e)
{
    EMU *m = e->_private;
    ULONG a = e->x86.R_CS_BASE + e->x86.R_EIP;
    UCHAR b0, b1, b2;
    int i;
    if (m->priv) return 0;
    if (e->x86.R_CR0 & 1) { m->priv = 1; return 0; }        /* protected mode */
    for (i = 0; i < 3; i++) {                               /* prefixes */
        b0 = (UCHAR)mem_rd(m, a, 1);
        if (b0 != 0x66 && b0 != 0x67 && b0 != 0x2E && b0 != 0x3E && b0 != 0x26 &&
            b0 != 0x36 && b0 != 0x64 && b0 != 0x65) break;
        a++;
    }
    if (b0 != 0x0F) return 0;
    b1 = (UCHAR)mem_rd(m, a + 1, 1);
    b2 = (UCHAR)mem_rd(m, a + 2, 1);
    if (b1 == 0x20 || b1 == 0x22 ||                         /* MOV r32, CRn / MOV CRn, r32 */
        b1 == 0x30 || b1 == 0x32 ||                         /* WRMSR / RDMSR */
        (b1 == 0x01 && ((b2 >> 3) & 7) >= 2 && ((b2 >> 3) & 7) <= 3)) {  /* LGDT / LIDT */
        DBG("emu: privileged instruction 0F %x at %x:%x\n", b1, e->x86.R_CS, e->x86.R_EIP);
        m->priv = 1;
    }
    return 0;
}

int emu_priv(void *h) { return h ? ((EMU *)h)->priv : 0; }
void emu_clear_priv(void *h) { if (h) ((EMU *)h)->priv = 0; }

void emu_destroy(void *h)
{
    EMU *m = h;
    ULONG i;
    if (!m) return;
    if (m->e) x86emu_done(m->e);
    for (i = 0; i < NWIN; i++)
        if (m->win[i].va) MmUnmapIoSpace(m->win[i].va, 0x10000);
    if (m->vga) MmUnmapIoSpace(m->vga, 0x20000);
    if (m->rom) MmUnmapIoSpace(m->rom, 0x40000);
    if (m->low) ExFreePool(m->low);
    ExFreePool(m);
}

void *emu_create(void)
{
    EMU *m = ExAllocatePool(WvbNonPagedPool, sizeof(EMU));
    UCHAR *p;
    ULONG ebda;
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));
    m->low = ExAllocatePool(WvbNonPagedPool, LOWSIZE);
    if (!m->low) goto fail;
    memset(m->low, 0, LOWSIZE);
    /* the interrupt vectors and the BIOS data area as the BIOS left them
       (Windows NT does not use the first physical page) */
    p = map_phys(0, 0x1000);
    if (!p) goto fail;
    memcpy(m->low, p, 0x1000);
    MmUnmapIoSpace(p, 0x1000);
    /* the extended BIOS data area at the top of conventional memory */
    ebda = (ULONG)(m->low[0x40E] | (m->low[0x40F] << 8)) << 4;
    if (ebda >= 0x80000 && ebda < LOWSIZE) {
        p = map_phys(ebda, LOWSIZE - ebda);
        if (p) { memcpy(m->low + ebda, p, LOWSIZE - ebda); MmUnmapIoSpace(p, LOWSIZE - ebda); }
    }
    m->vga = map_phys(0xA0000, 0x20000);
    m->rom = map_phys(0xC0000, 0x40000);
    if (!m->rom) goto fail;
    m->e = x86emu_new(X86EMU_PERM_RWX, X86EMU_PERM_RW);
    if (!m->e) goto fail;
    m->e->_private = m;
    x86emu_set_memio_handler(m->e, memio);
    x86emu_set_intr_handler(m->e, intr);
    x86emu_set_code_handler(m->e, code_check);
    m->low[STUB] = 0xF4;                       /* HLT */
    DBG("emu: ready (EBDA %x, INT 10h %x:%x)\n", ebda,
        m->low[0x42] | (m->low[0x43] << 8), m->low[0x40] | (m->low[0x41] << 8));
    return m;
fail:
    DBG("emu: not available\n");
    emu_destroy(m);
    return NULL;
}

static void push16(EMU *m, USHORT v)
{
    x86emu_t *e = m->e;
    e->x86.R_SP -= 2;
    mem_wr(m, ((ULONG)STKSEG << 4) + e->x86.R_SP, v, 2);
}

/* run from cs:ip until the HLT at 0000:STUB */
static int run(EMU *m, USHORT cs, USHORT ip, EMUREGS *r, int as_int)
{
    x86emu_t *e = m->e;
    e->x86.R_EAX = r->eax; e->x86.R_EBX = r->ebx; e->x86.R_ECX = r->ecx;
    e->x86.R_EDX = r->edx; e->x86.R_ESI = r->esi; e->x86.R_EDI = r->edi;
    e->x86.R_EBP = 0;
    e->x86.R_EFLG = 0x0202;
    x86emu_set_seg_register(e, e->x86.R_ES_SEL, r->es);
    x86emu_set_seg_register(e, e->x86.R_DS_SEL, r->ds);
    x86emu_set_seg_register(e, e->x86.R_FS_SEL, 0);
    x86emu_set_seg_register(e, e->x86.R_GS_SEL, 0);
    x86emu_set_seg_register(e, e->x86.R_SS_SEL, STKSEG);
    e->x86.R_ESP = 0xFFF0;
    if (as_int) push16(m, 0x0202);             /* IRET frame */
    push16(m, 0);                              /* return to 0000:STUB */
    push16(m, STUB);
    x86emu_set_seg_register(e, e->x86.R_CS_SEL, cs);
    e->x86.R_EIP = ip;
    m->fault = 0;
    /* a mode set waits for the vertical retrace and polls: enough for
       that, not for an endless loop */
    e->max_instr = e->x86.R_TSC + 200000000ULL;
    x86emu_run(e, X86EMU_RUN_LOOP | X86EMU_RUN_MAX_INSTR);
    if (m->fault || e->x86.R_CS != 0 || (USHORT)e->x86.R_EIP != STUB + 1) {
        DBG("emu: call %x:%x (ax=%x) did not return: %x:%x%s\n", cs, ip, r->eax,
            e->x86.R_CS, e->x86.R_EIP, m->fault ? " (exception)" : "");
        return 0;
    }
    r->eax = e->x86.R_EAX; r->ebx = e->x86.R_EBX; r->ecx = e->x86.R_ECX;
    r->edx = e->x86.R_EDX; r->esi = e->x86.R_ESI; r->edi = e->x86.R_EDI;
    r->es = e->x86.R_ES; r->ds = e->x86.R_DS;
    return 1;
}

int emu_int(void *h, unsigned char n, EMUREGS *r)
{
    EMU *m = h;
    USHORT ip, cs;
    if (!m) return 0;
    ip = (USHORT)(m->low[n * 4] | (m->low[n * 4 + 1] << 8));
    cs = (USHORT)(m->low[n * 4 + 2] | (m->low[n * 4 + 3] << 8));
    return run(m, cs, ip, r, 1);
}

int emu_call(void *h, unsigned short cs, unsigned short ip, EMUREGS *r)
{
    if (!h) return 0;
    return run(h, cs, ip, r, 0);
}

int emu_write(void *h, unsigned long lin, const void *src, unsigned long len)
{
    EMU *m = h;
    const UCHAR *s = src;
    if (!m || lin + len > 0x100000) return 0;
    while (len--) mem_wr(m, lin++, *s++, 1);
    return 1;
}

int emu_read(void *h, unsigned long lin, void *dst, unsigned long len)
{
    EMU *m = h;
    UCHAR *d = dst;
    if (!m || lin + len > 0x100000) return 0;
    while (len--) *d++ = (UCHAR)mem_rd(m, lin++, 1);
    return 1;
}
