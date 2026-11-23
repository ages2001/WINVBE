/*
 * WINVBE.SYS - universal VESA BIOS (VBE) video miniport for Windows NT
 *
 * One source for Windows NT 3.1, 3.5, 3.51, 4.0, 2000, XP, 2003 (x86) and
 * XP/2003 x64.  The video BIOS is called through
 *   - VideoPortInt10 on NT 3.1 - 4.0 (x86 only; ES is always 0, so data
 *     buffers live at 0000:4000 in the address space of CSRSS, where the
 *     calls come from: the display driver runs in / is called by CSRSS),
 *   - the Int10 interface of the video port (VideoPortQueryServices) on
 *     Windows 2000 and later (x86 V86 mode, x64 BIOS emulator).
 * The VBE modes with a linear frame buffer are offered to the display
 * driver WINVBE.DLL; refresh rates use VBE 3.0 CRTC timings.
 * The 915PATCH of WINVBE (Intel 830...G45 video BIOS mode table) is built
 * in: it runs before the VBE mode list is read (WinVbe.Patch915).
 */
#include <ntdef.h>
#include <ddk/miniport.h>
#include <ddk/dderror.h>
#include <ddk/video.h>
#include <devioctl.h>
#include <ntddvdeo.h>
#include <stdarg.h>
#include "../common/wvbnt.h"
#include "../../drv/edidx.h"
#ifdef _X86_
#include "emu10.h"
#endif

/* i915core.c works on a BIOS image through plain pointers here */
#include "../../patch/i915core.h"
#include "../../patch/i915chips.h"

#ifndef VIDEO_MEMORY_SPACE_P6CACHE
#define VIDEO_MEMORY_SPACE_P6CACHE 0x04
#endif

/* ntoskrnl (exported since NT 3.1) */
NTSYSAPI BOOLEAN NTAPI MmIsAddressValid(PVOID VirtualAddress);
NTSYSAPI PVOID NTAPI IoGetCurrentProcess(VOID);
#ifdef VGAREPL
NTSYSAPI NTSTATUS NTAPI ZwOpenKey(PHANDLE, ULONG, POBJECT_ATTRIBUTES);
NTSYSAPI NTSTATUS NTAPI ZwSetValueKey(HANDLE, PUNICODE_STRING, ULONG, ULONG, PVOID, ULONG);
NTSYSAPI NTSTATUS NTAPI ZwClose(HANDLE);
#endif

/* ------------------------------------------------------------------ */
/* small runtime                                                       */

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q) return *p - *q;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static inline void outb(USHORT p, UCHAR v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline UCHAR inb(USHORT p) { UCHAR v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outl(USHORT p, ULONG v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline ULONG inl(USHORT p) { ULONG v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outw(USHORT p, USHORT v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(p)); }
static inline USHORT inw(USHORT p) { USHORT v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static USHORT dispi_rd(USHORT i) { outw(0x1CE, i); return inw(0x1CF); }
static void dispi_wr(USHORT i, USHORT v) { outw(0x1CE, i); outw(0x1CF, v); }

/* ------------------------------------------------------------------ */

#define BUFOFF  0x4000          /* NT <= 4: buffer at 0000:4000 (CSRSS) */
#define BUFSIZE 0x400

typedef struct {
    USHORT vbe;
    USHORT w, h;
    UCHAR  bpp, Bpp;
    UCHAR  attr_vga;            /* 1 = VGA compatible (DAC via ports) */
    UCHAR  hzmask;
    ULONG  pitch;
    ULONG  phys;
    UCHAR  rbits, rpos, gbits, gpos, bbits, bpos;
    /* modes without a usable linear frame buffer: the VGA window at A000h,
       switched with VBE 4F05h (window win, granularity and size in KB);
       the display driver draws into a copy in system memory */
    UCHAR  banked, win;
    USHORT gran_kb, winsz_kb;
    UCHAR  planar;              /* VGA mode 12h, 640x480 in 16 colours (VGA.DLL) */
    UCHAR  dispi;               /* not in the BIOS list: set through the Bochs DISPI registers */
} MODE;

typedef struct {
    int    level;               /* 31, 35, 40, 50 */
    int    debug;
    int    gathered;            /* 1 = mode list read, -1 = failed */
    ULONG  scale;               /* WinVbe.Scale: small modes shown in the native mode */
    LONG   vcur;                /* the scaled mode set (-1: none) */
    /* Int10 interface (2000+) */
    VIDEO_PORT_INT10_INTERFACE i10;
    int    have_i10;
    USHORT bseg, boff;          /* buffer for BIOS calls */
    /* BIOS ROM C0000-FFFFF mapped for reading far pointers */
    UCHAR *rom;
    /* VBE */
    USHORT vbever;
    ULONG  memkb;
    UCHAR  caps;
    UCHAR  edid[128];
    int    edid_ok;
    USHORT edid_w, edid_h, edid_hz;
    EDIDTM edtm;                                 /* native timing (base block or extension) */
    UCHAR  edid_minv, edid_maxv, edid_maxh;     /* range limits (0 = none) */
    USHORT edid_maxclk;                          /* MHz */
    char   oem[80];
    ULONG  nmodes;
    MODE   modes[WVB_MAXMODES];
    /* current */
    ULONG  cur;                 /* index into modes */
    ULONG  cur_hz;              /* entry of wvb_hz, 0xFF = BIOS default */
    ULONG  cur_pitch;
    int    mode_set;
    int    uefi_fb;             /* a UEFI VBE shim: the mode is pre-set (GOP),
                                   4F02 cannot change it, so use it as it is */
    UCHAR  dacbits;
    PVOID  fbmap;               /* mapped frame buffer (last map) */
    ULONG  fbsize;
    ULONG  start;               /* display start (byte offset), DirectDraw flip */
    ULONG  bank;                /* window position of a banked mode, -1 = unknown */
    UCHAR  dpms;                /* VBE/PM 4F10h states: 1 standby, 2 suspend, 4 off; 0x80 = asked */
    UCHAR  reposted;            /* video BIOS initialization tried after a failed mode set */
    UCHAR  pci_ok;              /* pcicfg holds the PCI header from the start */
    ULONG  pcicfg[16];
    /* VideoPortGet/SetBusData, looked up at run time: NT 3.1's VIDEOPRT.SYS
       does not have them (an import would keep WINVBE.SYS from loading) */
    ULONG  (NTAPI *getbus)(PVOID, BUS_DATA_TYPE, ULONG, PVOID, ULONG, ULONG);
    ULONG  (NTAPI *setbus)(PVOID, BUS_DATA_TYPE, ULONG, PVOID, ULONG, ULONG);
    ULONG  adapter_power;       /* VIDEO_POWER_STATE of the adapter */
    ULONG  monitor_power;
    ULONG  refresh_cfg;         /* WinVbe.Refresh: 0 = auto */
    ULONG  dac8;
    ULONG  refpol;              /* WinVbe.Refresh: 0 monitor (EDID), 1 all, 2 60 Hz */
    ULONG  access;              /* WinVbe.Access: 0 auto, 1 LFB only, 2 VGA window only */
    WCHAR  patchcmd[80];
    char   patchres[160];
    WVBINFO info;               /* written to the registry */
    /* video BIOS calls through the x86 emulator (emu10.c, NT x86): for a
       BIOS that cannot run in virtual 8086 mode */
    ULONG  int10_mode;          /* WinVbe.Int10: 0 auto, 1 Windows only, 2 emulator */
    void  *emu;
    int    use_emu;
} DEVEXT;

/* ------------------------------------------------------------------ */
/* debug output to port E9h (Bochs/QEMU), WinVbe.Debug = 1             */

static DEVEXT *g_dx;

static void dputs(const char *s)
{
    while (*s) outb(0xE9, (UCHAR)*s++);
}

static char *fmt_num(char *o, char *end, ULONG v, int base, int width, int zero)
{
    char t[12];
    int n = 0;
    do { int d = (int)(v % base); t[n++] = (char)(d < 10 ? '0' + d : 'A' + d - 10); v /= base; } while (v);
    while (width > n && o < end) { *o++ = zero ? '0' : ' '; width--; }
    while (n && o < end) *o++ = t[--n];
    return o;
}

static int vfmt(char *buf, int size, const char *f, va_list ap)
{
    char *o = buf, *end = buf + size - 1;
    while (*f && o < end) {
        int width = 0, zero = 0;
        if (*f != '%') { *o++ = *f++; continue; }
        f++;
        if (*f == '0') { zero = 1; f++; }
        while (*f >= '0' && *f <= '9') width = width * 10 + *f++ - '0';
        if (*f == 'l') f++;
        switch (*f++) {
        case 'u': o = fmt_num(o, end, va_arg(ap, ULONG), 10, width, zero); break;
        case 'd': { LONG v = va_arg(ap, LONG); if (v < 0 && o < end) { *o++ = '-'; v = -v; }
                    o = fmt_num(o, end, (ULONG)v, 10, width, zero); break; }
        case 'x': case 'X': o = fmt_num(o, end, va_arg(ap, ULONG), 16, width, zero); break;
        case 'c': *o++ = (char)va_arg(ap, int); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "";
            while (*s && o < end) *o++ = *s++;
            break;
        }
        case 'S': {
            const WCHAR *s = va_arg(ap, const WCHAR *);
            if (!s) s = L"";
            while (*s && o < end) *o++ = (char)*s++;
            break;
        }
        case '%': *o++ = '%'; break;
        }
    }
    *o = 0;
    return (int)(o - buf);
}

static int sfmt(char *buf, int size, const char *f, ...)
{
    va_list ap;
    int n;
    va_start(ap, f);
    n = vfmt(buf, size, f, ap);
    va_end(ap);
    return n;
}

static void dbg(const char *f, ...)
{
    char b[256];
    va_list ap;
    if (!g_dx || !g_dx->debug) return;
    va_start(ap, f);
    vfmt(b, sizeof(b), f, ap);
    va_end(ap);
    dputs("WVBNT: ");
    dputs(b);
}

/* ------------------------------------------------------------------ */
/* BIOS calls                                                          */

typedef struct { ULONG eax, ebx, ecx, edx, esi, edi; USHORT es; int usebuf; } RM;

/* NT <= 4: is the BIOS memory of CSRSS mapped in the current process? */
static int lowmem_ok(DEVEXT *dx)
{
    VIDEO_X86_BIOS_ARGUMENTS a;
    if (MmIsAddressValid((PVOID)0)) return 1;
    /* the first call maps it (NT 3.1) or attaches to CSRSS (NT 4) */
    VideoPortZeroMemory(&a, sizeof(a));
    a.Eax = 0x4F03;
    VideoPortInt10(dx, &a);
    return MmIsAddressValid((PVOID)0) ? 1 : 0;
}

/* call INT 10h; with r->usebuf, buf[0..inlen) is copied to the buffer
   (ES:DI) before and buf[0..outlen) back after the call */
#ifdef _X86_
static void emu_vdbg(const char *f, ...)
{
    char b[256];
    va_list ap;
    if (!g_dx || !g_dx->debug) return;
    va_start(ap, f);
    vfmt(b, sizeof(b), f, ap);
    va_end(ap);
    dputs("WVBNT: ");
    dputs(b);
}

/* switch the video BIOS calls to the x86 emulator (1 = done) */
static int emu_start(DEVEXT *dx, const char *why)
{
    if (dx->use_emu) return 1;
    if (dx->int10_mode == 1) return 0;
    emu_dbg = emu_vdbg;
    if (!dx->emu) dx->emu = emu_create();
    if (!dx->emu) { dbg("emulator not available (%s)\n", why); return 0; }
    dx->use_emu = 1;
    dbg("video BIOS calls now through the x86 emulator: %s\n", why);
    return 1;
}

static int int10(DEVEXT *dx, RM *r, void *buf, ULONG inlen, ULONG outlen);

/* Intel graphics of Skylake (6th generation) and later: their video BIOS
   reaches its registers in "unreal" mode, which cannot run in virtual
   8086 mode (Windows 2000 stops with 0x0000000A in the attempt) */
static int intel_unreal_bios(DEVEXT *dx)
{
    ULONG id = dx->pcicfg[0], cls = dx->pcicfg[2];
    USHORT ven, dev;
    if (!dx->pci_ok) {
        /* NT 4.0 / 2000 without the PCI header: Intel graphics are always
           at 00:02.0 (configuration mechanism #1) */
        outl(0xCF8, 0x80000000UL | (2UL << 11)); id = inl(0xCFC);
        outl(0xCF8, 0x80000000UL | (2UL << 11) | 8); cls = inl(0xCFC);
    }
    ven = (USHORT)(id & 0xFFFF); dev = (USHORT)(id >> 16);
    if (ven != 0x8086 || (cls >> 24) != 0x03) return 0;
    switch (dev >> 8) {
    case 0x19:              /* Skylake */
    case 0x59:              /* Kaby Lake */
    case 0x3E:              /* Coffee Lake, Whiskey Lake */
    case 0x87:              /* Amber Lake */
    case 0x9B:              /* Comet Lake */
    case 0x5A:              /* Cannon Lake, Apollo Lake */
    case 0x31:              /* Gemini Lake */
    case 0x8A:              /* Ice Lake */
    case 0x45: case 0x4E:   /* Elkhart Lake, Jasper Lake */
    case 0x9A:              /* Tiger Lake */
    case 0x4C:              /* Rocket Lake */
    case 0x46: case 0xA7:   /* Alder Lake, Raptor Lake */
    case 0x7D: case 0x64:   /* Meteor Lake, Lunar Lake */
        return 1;
    }
    return dev == 0x0A84;   /* Broxton */
}

/* WinVbe.Int10 = 0: the first BIOS calls (EDID, VBE information) go
   through the emulator, where nothing can crash; a BIOS that uses
   privileged instructions there (or Intel graphics of Skylake and later)
   stays in the emulator, every other one goes back to Windows' V86 calls */
static void emu_probe(DEVEXT *dx)
{
    UCHAR b[512];
    RM r;
    int intel = intel_unreal_bios(dx), ok;
    if (dx->use_emu || dx->int10_mode != 0) return;
    if (!emu_start(dx, intel ? "Intel graphics of Skylake or later" : "probe")) return;
    emu_clear_priv(dx->emu);
    memset(&r, 0, sizeof(r));
    r.eax = 0x4F15; r.ebx = 1; r.usebuf = 1;
    memset(b, 0, 128);
    int10(dx, &r, b, 128, 128);
    memset(b, 0, sizeof(b));
    memcpy(b, "VBE2", 4);
    memset(&r, 0, sizeof(r));
    r.eax = 0x4F00; r.usebuf = 1;
    ok = int10(dx, &r, b, 512, 512) && (USHORT)r.eax == 0x004F && !memcmp(b, "VESA", 4);
    dbg("emulator probe: VBE %s, privileged instructions %u, Intel unreal BIOS %u\n",
        ok ? "ok" : "failed", emu_priv(dx->emu), intel);
    if (intel || (ok && emu_priv(dx->emu))) {
        dbg("video BIOS calls stay in the x86 emulator\n");
        return;
    }
    dx->use_emu = 0;
}

static int emu_int10(DEVEXT *dx, RM *r, void *buf, ULONG inlen, ULONG outlen)
{
    EMUREGS e;
    memset(&e, 0, sizeof(e));
    e.eax = r->eax; e.ebx = r->ebx; e.ecx = r->ecx; e.edx = r->edx; e.esi = r->esi; e.edi = r->edi;
    e.es = r->es;
    if (r->usebuf) {
        if (inlen > EMU_BUFSIZE || outlen > EMU_BUFSIZE) return 0;
        e.es = EMU_BUFSEG; e.edi = 0;
        if (inlen) emu_write(dx->emu, (ULONG)EMU_BUFSEG << 4, buf, inlen);
    }
    if (!emu_int(dx->emu, 0x10, &e)) return 0;
#ifdef DEVDEBUG
    dbg("emu int10 %x -> ax=%x bx=%x es:di=%x:%x\n", r->eax, e.eax, e.ebx, e.es, e.edi);
#endif
    r->eax = e.eax; r->ebx = e.ebx; r->ecx = e.ecx; r->edx = e.edx; r->esi = e.esi; r->edi = e.edi; r->es = e.es;
    if (r->usebuf && outlen) emu_read(dx->emu, (ULONG)EMU_BUFSEG << 4, buf, outlen);
    return 1;
}
#endif

static int int10(DEVEXT *dx, RM *r, void *buf, ULONG inlen, ULONG outlen)
{
#ifdef _X86_
    if (dx->use_emu) return emu_int10(dx, r, buf, inlen, outlen);
#endif
    if (dx->have_i10) {
        INT10_BIOS_ARGUMENTS a;
        VideoPortZeroMemory(&a, sizeof(a));
        a.Eax = r->eax; a.Ebx = r->ebx; a.Ecx = r->ecx; a.Edx = r->edx;
        a.Esi = r->esi; a.Edi = r->edi;
        if (r->usebuf) {
            a.SegEs = dx->bseg; a.Edi = dx->boff;
#ifdef DEVDEBUG
            dbg("i10 write %x:%x len %u fn %x\n", dx->bseg, dx->boff, inlen, (ULONG)(ULONG_PTR)dx->i10.Int10WriteMemory);
#endif
            if (inlen) dx->i10.Int10WriteMemory(dx->i10.Context, dx->bseg, dx->boff, buf, inlen);
        }
#ifdef DEVDEBUG
        dbg("i10 call ax=%x\n", a.Eax);
#endif
        {
            VP_STATUS st = dx->i10.Int10CallBios(dx->i10.Context, &a);
            if (st != NO_ERROR) { dbg("int10 %x: CallBios error %x\n", r->eax, st); return 0; }
        }
#ifdef DEVDEBUG
        dbg("i10 ret ax=%x\n", a.Eax);
#endif
        r->eax = a.Eax; r->ebx = a.Ebx; r->ecx = a.Ecx; r->edx = a.Edx;
        r->esi = a.Esi; r->edi = a.Edi; r->es = a.SegEs;
        if (r->usebuf && outlen)
            dx->i10.Int10ReadMemory(dx->i10.Context, dx->bseg, dx->boff, buf, outlen);
        return 1;
    }
#ifdef _X86_
    {
        VIDEO_X86_BIOS_ARGUMENTS a;
        VideoPortZeroMemory(&a, sizeof(a));
        a.Eax = r->eax; a.Ebx = r->ebx; a.Ecx = r->ecx; a.Edx = r->edx;
        a.Esi = r->esi; a.Edi = r->edi;
        if (r->usebuf) {
            if (!lowmem_ok(dx)) return 0;
            a.Edi = BUFOFF;     /* ES = 0 */
            if (inlen) memcpy((PVOID)BUFOFF, buf, inlen);
        }
        if (VideoPortInt10(dx, &a) != NO_ERROR) return 0;
        r->eax = a.Eax; r->ebx = a.Ebx; r->ecx = a.Ecx; r->edx = a.Edx;
        r->esi = a.Esi; r->edi = a.Edi; r->es = 0;
        if (r->usebuf && outlen) memcpy(buf, (PVOID)BUFOFF, outlen);
        return 1;
    }
#else
    return 0;
#endif
}

/* read real mode memory seg:off (for far pointers returned by the BIOS) */
static int rm_read(DEVEXT *dx, USHORT seg, USHORT off, void *dst, ULONG len)
{
    ULONG lin = ((ULONG)seg << 4) + off;
#ifdef _X86_
    if (dx->use_emu) return emu_read(dx->emu, lin, dst, len);
#endif
    if (lin >= 0xC0000 && lin + len <= 0x100000 && dx->rom) {
        memcpy(dst, dx->rom + (lin - 0xC0000), len);
        return 1;
    }
    if (lin + len > 0xA0000) return 0;
    if (dx->have_i10)
        return dx->i10.Int10ReadMemory(dx->i10.Context, seg, off, dst, len) == NO_ERROR;
#ifdef _X86_
    if (!MmIsAddressValid((PVOID)(ULONG_PTR)(lin & ~0xFFF))) return 0;
    memcpy(dst, (PVOID)(ULONG_PTR)lin, len);
    return 1;
#else
    return 0;
#endif
}

/* ------------------------------------------------------------------ */
/* registry                                                            */

static VP_STATUS NTAPI reg_cb(PVOID hw, PVOID ctx, PWSTR name, PVOID data, ULONG len)
{
    ULONG *p = ctx;
    if (len >= 4) *p = *(ULONG *)data;
    return NO_ERROR;
}

static ULONG reg_dword(DEVEXT *dx, PWSTR name, ULONG def)
{
    ULONG v = def;
    if (VideoPortGetRegistryParameters(dx, name, FALSE, reg_cb, &v) != NO_ERROR) v = def;
    return v;
}

static VP_STATUS NTAPI reg_str_cb(PVOID hw, PVOID ctx, PWSTR name, PVOID data, ULONG len)
{
    WCHAR *d = ctx, *s = data;
    ULONG n = len / 2, i;
    if (n > 79) n = 79;
    for (i = 0; i < n && s[i]; i++) d[i] = s[i];
    d[i] = 0;
    return NO_ERROR;
}

/* ------------------------------------------------------------------ */
/* EDID                                                                */

static void read_edid(DEVEXT *dx)
{
    RM r;
    UCHAR *e = dx->edid;
    UCHAR ext[128];
    int i, n;
    UCHAR sum = 0;
    memset(&r, 0, sizeof(r));
    r.eax = 0x4F15; r.ebx = 0x0001; r.usebuf = 1;
    memset(e, 0, 128);
    if (!int10(dx, &r, e, 128, 128) || (USHORT)r.eax != 0x004F) return;
    if (e[0] != 0 || e[1] != 0xFF || e[7] != 0) return;
    for (i = 0; i < 128; i++) sum += e[i];
    if (sum) dbg("EDID checksum bad\n");
    dx->edid_ok = 1;
    memset(&dx->edtm, 0, sizeof(dx->edtm));
    edidx_dtd(e + 54, &dx->edtm);
    /* extension blocks: DisplayID / CTA-861 timings beyond 4095 pixels */
    n = e[126] > 3 ? 3 : e[126];
    for (i = 1; i <= n; i++) {
        EDIDTM t;
        memset(&r, 0, sizeof(r));
        r.eax = 0x4F15; r.ebx = 0x0001; r.edx = (ULONG)i; r.usebuf = 1;
        memset(ext, 0, sizeof(ext));
        if (!int10(dx, &r, ext, 128, 128) || (USHORT)r.eax != 0x004F) break;
        memset(&t, 0, sizeof(t));
        if (edidx_ext(ext, &t)) {
            dbg("EDID extension %u (%x): %ux%u@%u\n", i, ext[0], t.ha, t.va, t.hz);
            edidx_merge(&dx->edtm, &t);
        }
    }
    dx->edid_w = dx->edtm.ha;
    dx->edid_h = dx->edtm.va;
    dx->edid_hz = dx->edtm.src ? dx->edtm.hz : 60;
    for (i = 54; i <= 108; i += 18) {
        if (e[i] == 0 && e[i + 1] == 0 && e[i + 3] == 0xFD) {
            dx->edid_minv = e[i + 5]; dx->edid_maxv = e[i + 6];
            dx->edid_maxh = e[i + 8];
            dx->edid_maxclk = e[i + 9] * 10;
        }
    }
    dbg("EDID native %ux%u@%u range v %u-%u h<=%u clk<=%u\n", dx->edid_w, dx->edid_h, dx->edid_hz,
        dx->edid_minv, dx->edid_maxv, dx->edid_maxh, dx->edid_maxclk);
}

/* ------------------------------------------------------------------ */
/* 915PATCH (Intel 830 ... G45 video BIOS mode table)                  */

static ULONG pci_rd32(UCHAR reg) { outl(0xCF8, 0x80000000UL | reg); return inl(0xCFC); }
static UCHAR pci_rd8(UCHAR reg) { outl(0xCF8, 0x80000000UL | (reg & 0xFC)); return inb(0xCFC + (reg & 3)); }
static void pci_wr8(UCHAR reg, UCHAR v) { outl(0xCF8, 0x80000000UL | (reg & 0xFC)); outb(0xCFC + (reg & 3), v); }

static int wtoi(const WCHAR **pp)
{
    const WCHAR *p = *pp;
    int v = 0;
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') v = v * 10 + *p++ - '0';
    *pp = p;
    return v;
}

static int wtohex(const WCHAR **pp)
{
    const WCHAR *p = *pp;
    int v = 0;
    while (*p == ' ') p++;
    for (;; p++) {
        if (*p >= '0' && *p <= '9') v = v * 16 + *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = v * 16 + *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = v * 16 + *p - 'A' + 10;
        else break;
    }
    *pp = p;
    return v;
}

static int wmatch(const WCHAR **pp, const char *w)
{
    const WCHAR *p = *pp;
    while (*p == ' ') p++;
    while (*w) {
        WCHAR c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c != (WCHAR)*w) return 0;
        p++; w++;
    }
    *pp = p;
    return 1;
}

/* one command: AUTO [w h] | mode w h [bpp] */
static void patch_cmd(DEVEXT *dx, VBMAP *m, const WCHAR *p)
{
    char *res = dx->patchres + strlen(dx->patchres);
    int room = (int)(sizeof(dx->patchres) - (res - dx->patchres));
    if (*p == '/') p++;
    if (wmatch(&p, "AUTO")) {
        USHORT x = (USHORT)wtoi(&p), y = (USHORT)wtoi(&p), k;
        static const UCHAR bpps[3] = { 8, 16, 32 };
        int done = 0, had = 0;
        if (!x || !y) {
            if (!dx->edid_ok) { sfmt(res, room, "no EDID; "); return; }
            x = dx->edid_w; y = dx->edid_h;
        }
        for (k = 0; k < 3; k++) {
            VBMODE md, best;
            USHORT j, have = 0;
            best.mode = 0; best.x = best.y = 0;
            for (j = 0; j < m->count; j++) {
                vb_get_mode(m, j, &md);
                if (md.bpp != bpps[k]) continue;
                if (md.x == x && md.y == y) have = 1;
                if ((ULONG)md.x * md.y > (ULONG)best.x * best.y) best = md;
            }
            if (have) had++;
            if (have || !best.mode) continue;
            if (vb_set_mode(m, best.mode, x, y, 0, 0, 0)) done++;
        }
        sfmt(res, room, done ? "%ux%u added to the video BIOS; " : had ? "%ux%u already in the video BIOS; "
                                                                       : "%ux%u: no mode to replace; ", x, y);
    } else {
        int mode = wtohex(&p), x = wtoi(&p), y = wtoi(&p), bpp = wtoi(&p);
        if (x < 320 || y < 200 || x > 4096 || y > 4096) { sfmt(res, room, "bad command; "); return; }
        if (vb_set_mode(m, (u8)mode, (u16)x, (u16)y, (u8)bpp, 0, 0))
            sfmt(res, room, "mode %X %ux%u; ", mode, x, y);
        else
            sfmt(res, room, "mode %X not found; ", mode);
    }
}

static void patch915(DEVEXT *dx)
{
    PHYSICAL_ADDRESS pa;
    UCHAR *bios;
    ULONG id;
    int c = -1, i, pam;
    UCHAR s1 = 0, s2 = 0;
    static VBMAP m;
    const WCHAR *p = dx->patchcmd;
    char *r = dx->patchres;
    int rs = sizeof(dx->patchres);

    r[0] = 0;
    while (*p == ' ') p++;
    if (!*p || wmatch(&p, "OFF")) { sfmt(r, rs, "off"); return; }
    id = pci_rd32(0);
    if ((USHORT)id != 0x8086) { sfmt(r, rs, "no Intel chipset (%04X:%04X)", (USHORT)id, (USHORT)(id >> 16)); return; }
    for (i = 0; i915_chips[i].name; i++)
        if (i915_chips[i].id == (USHORT)(id >> 16)) { c = i; break; }
    if (c < 0) { sfmt(r, rs, "chipset 8086:%04X not supported", (USHORT)(id >> 16)); return; }
    pam = i915_chips[c].pam;
    pa.QuadPart = 0xC0000;
    bios = VideoPortGetDeviceBase(dx, pa, 0x10000, FALSE);
    if (!bios) { sfmt(r, rs, "cannot map the video BIOS"); return; }
    if (bios[0] != 0x55 || bios[1] != 0xAA) { sfmt(r, rs, "no video BIOS"); goto out; }
    m.bios = bios;
    m.size = (ULONG)bios[2] * 512UL;
    if (m.size < 0x1000 || m.size > 0x10000UL) m.size = 0x10000UL;
    {
        ULONG k;
        int intel = 0;
        for (k = 0; k + 5 <= m.size && !intel; k++)
            if ((bios[k] == 'I') && (!memcmp(bios + k, "Intel", 5) || !memcmp(bios + k, "INTEL", 5))) intel = 1;
        if (!intel) { sfmt(r, rs, "not an Intel video BIOS"); goto out; }
    }
    if (!vb_find_table(&m) || !vb_locate(&m)) { sfmt(r, rs, "mode table not found"); goto out; }
    /* unlock the shadow RAM */
    if (pam == PAM_5A) { s1 = pci_rd8(0x5A); pci_wr8(0x5A, 0x33);
#ifdef TEST440FX
        s2 = pci_rd8(0x5B); pci_wr8(0x5B, 0x33);
#endif
    } else { s1 = pci_rd8(0x91); s2 = pci_rd8(0x92); pci_wr8(0x91, 0x33); pci_wr8(0x92, 0x33); }
    {
        UCHAR t = bios[m.table + 1];
        bios[m.table + 1] = (UCHAR)(t ^ 0x5A);
        if (bios[m.table + 1] != (UCHAR)(t ^ 0x5A)) {
            sfmt(r, rs, "shadow RAM cannot be unlocked");
        } else {
            bios[m.table + 1] = t;
            sfmt(r, rs, "Intel %s: ", i915_chips[c].name);
            /* commands separated by ; */
            while (*p) {
                WCHAR cmd[40];
                int n = 0;
                while (*p == ' ' || *p == ';') p++;
                while (*p && *p != ';' && n < 39) cmd[n++] = *p++;
                cmd[n] = 0;
                if (n) patch_cmd(dx, &m, cmd);
            }
            {   /* no "; " at the end */
                size_t l = strlen(r);
                while (l && (r[l - 1] == ' ' || r[l - 1] == ';')) r[--l] = 0;
            }
        }
    }
    if (pam == PAM_5A) { pci_wr8(0x5A, s1);
#ifdef TEST440FX
        pci_wr8(0x5B, s2);
#endif
    } else { pci_wr8(0x91, s1); pci_wr8(0x92, s2); }
out:
    VideoPortFreeDeviceBase(dx, bios);
}

/* ------------------------------------------------------------------ */
/* refresh rates (VBE 3.0 CRTC information block)                      */

#pragma pack(push, 1)
typedef struct {
    USHORT htotal, hss, hse, vtotal, vss, vse;
    UCHAR  flags;
    ULONG  pclk;
    USHORT refresh;
    UCHAR  res[40];
} CRTC;
#pragma pack(pop)

/* VESA CVT 1.2 (standard and reduced blanking), integer math */
static int calc_cvt(CRTC *c, ULONG w, ULONG h, ULONG hz, int reduced)
{
    ULONG vsync, htotal, vtotal, pclk;
    w &= ~7UL;
    if (w * 3 == h * 4) vsync = 4;
    else if (w * 9 == h * 16) vsync = 5;
    else if (w * 10 == h * 16) vsync = 6;
    else if (w * 4 == h * 5 || w * 9 == h * 15) vsync = 7;
    else vsync = 10;
    if (reduced) {
        ULONG hpe_ns = (1000000000UL / hz - 460000UL) / h;
        ULONG vbi = 460000UL / hpe_ns + 1;
        if (vbi < 3 + vsync + 6) vbi = 3 + vsync + 6;
        htotal = w + 160;
        vtotal = h + vbi;
        c->hss = (USHORT)(w + 48); c->hse = (USHORT)(w + 48 + 32);
        c->vss = (USHORT)(h + 3);  c->vse = (USHORT)(h + 3 + vsync);
        c->flags = 0x08;
        if (hz * vtotal > 0xFFFFFFFFUL / htotal) return 0;     /* beyond a 32-bit pixel clock */
        pclk = hz * vtotal * htotal;
        pclk = (pclk / 250000UL) * 250000UL;
    } else {
        ULONG hpe_ns = (1000000000UL / hz - 550000UL) / (h + 3);
        ULONG vsbp = 550000UL / hpe_ns + 1;
        ULONG idc_m, hblank, hsync, khz;
        if (vsbp < vsync + 6) vsbp = vsync + 6;
        vtotal = h + vsbp + 3;
        idc_m = 30000UL - (3UL * hpe_ns) / 10;
        if ((LONG)idc_m < 20000L) idc_m = 20000UL;
        hblank = (w * idc_m / (100000UL - idc_m)) / 16 * 16;
        htotal = w + hblank;
        /* htotal * 10^6 overflows 32 bits beyond 4294 pixels: two steps */
        khz = htotal * 1000UL / hpe_ns * 1000UL + (htotal * 1000UL % hpe_ns) * 1000UL / hpe_ns;
        pclk = (khz / 250UL) * 250000UL;
        hsync = ((htotal * 8 / 100 + 4) / 8) * 8;
        c->hse = (USHORT)(w + hblank / 2);
        c->hss = (USHORT)(c->hse - hsync);
        c->vss = (USHORT)(h + 3); c->vse = (USHORT)(h + 3 + vsync);
        c->flags = 0x04;
    }
    c->htotal = (USHORT)htotal;
    c->vtotal = (USHORT)vtotal;
    c->pclk = pclk;
    c->refresh = (USHORT)(pclk / (htotal * vtotal / 100));
    return 1;
}

static int fill_crtc(DEVEXT *dx, CRTC *c, MODE *md, ULONG hz)
{
    memset(c, 0, sizeof(*c));
    if (dx->edid_ok && dx->edtm.pclk10k && md->w == dx->edtm.ha && md->h == dx->edtm.va && hz == dx->edid_hz) {
        EDIDTM *e = &dx->edtm;
        c->htotal = (USHORT)(e->ha + e->hb); c->hss = (USHORT)(e->ha + e->hso); c->hse = (USHORT)(e->ha + e->hso + e->hsw);
        c->vtotal = (USHORT)(e->va + e->vb); c->vss = (USHORT)(e->va + e->vso); c->vse = (USHORT)(e->va + e->vso + e->vsw);
        if (!e->hpos) c->flags |= 0x04;
        if (!e->vpos) c->flags |= 0x08;
        c->pclk = e->pclk10k * 10000UL;
        c->refresh = (USHORT)(c->pclk / ((ULONG)c->htotal * c->vtotal / 100));
        return 1;
    }
    return calc_cvt(c, md->w, md->h, hz, dx->edid_ok && (dx->edid[20] & 0x80));
}

/* refresh rates that are allowed for a mode */
static UCHAR hz_mask(DEVEXT *dx, MODE *md)
{
    UCHAR mask = 1;             /* 60 Hz (BIOS default without VBE 3.0) */
    int i;
#ifdef ONLY60
    return 1;
#endif
    if (dx->vbever < 0x300 || dx->refpol == 2) return mask;
    for (i = 1; i < WVB_NHZ; i++) {
        ULONG hz = wvb_hz[i];
        CRTC c;
        if (dx->refpol == 1) {          /* all rates: the user knows the monitor */
            mask |= (UCHAR)(1 << i);
            continue;
        }
        if (dx->edid_ok) {
            if (!dx->edid_maxv) {
                /* no range limits: digital panels only get 60 Hz */
                if ((dx->edid[20] & 0x80) || hz > 85) continue;
            } else {
                if (!fill_crtc(dx, &c, md, hz)) continue;
                if (hz < dx->edid_minv || hz > dx->edid_maxv) continue;
                if (dx->edid_maxh && (ULONG)c.vtotal * hz / 1000 > dx->edid_maxh) continue;
                if (dx->edid_maxclk && c.pclk / 1000000UL > dx->edid_maxclk) continue;
            }
        } else if (hz > 85)
            continue;
        mask |= (UCHAR)(1 << i);
    }
    return mask;
}

/* ------------------------------------------------------------------ */
/* VBE mode list                                                       */

static void copy_rmstr(DEVEXT *dx, char *d, int room, ULONG farptr)
{
    char t[64];
    int n;
    if (!farptr || !rm_read(dx, (USHORT)(farptr >> 16), (USHORT)farptr, t, sizeof(t) - 1)) return;
    t[sizeof(t) - 1] = 0;
    n = (int)strlen(d);
    if (n && n < room - 1) d[n++] = ' ';
    {
        int i;
        for (i = 0; t[i] && n < room - 1; i++)
            d[n++] = (t[i] >= 32 && t[i] < 127) ? t[i] : ' ';
        d[n] = 0;
    }
}

static void gather(DEVEXT *dx)
{
    UCHAR vi[512], mi[256];
    USHORT list[256];
    ULONG nlist = 0, i;
    RM r;
    ULONG lp;
    int vbe_ok;

    if (dx->gathered) return;
#ifdef _X86_
    if (!dx->have_i10 && !lowmem_ok(dx)) {
        dbg("gather: BIOS memory not reachable in this process\n");
        return;            /* try again later (from CSRSS) */
    }
#endif
    dx->gathered = -1;
#ifdef DEVDEBUG
    dbg("gather\n");
#endif
#ifdef _X86_
    emu_probe(dx);
#endif
    read_edid(dx);
    patch915(dx);
    dbg("915 patch: %s\n", dx->patchres);

#ifdef _X86_
    if (dx->int10_mode == 2) emu_start(dx, "WinVbe.Int10 = 2");
#endif
    memset(vi, 0, sizeof(vi));
    memcpy(vi, "VBE2", 4);
    memset(&r, 0, sizeof(r));
    r.eax = 0x4F00; r.usebuf = 1;
#ifndef TEST_NOVBE
#define TEST_NOVBE 0
#endif
    vbe_ok = int10(dx, &r, vi, 512, 512) && (USHORT)r.eax == 0x004F && !memcmp(vi, "VESA", 4);
#ifdef _X86_
    /* a video BIOS that does not work in virtual 8086 mode: the same call
       through the emulator */
    if (!vbe_ok && !dx->use_emu && emu_start(dx, "VBE 4F00 failed in V86 mode")) {
        read_edid(dx);
        memset(vi, 0, sizeof(vi));
        memcpy(vi, "VBE2", 4);
        memset(&r, 0, sizeof(r));
        r.eax = 0x4F00; r.usebuf = 1;
        vbe_ok = int10(dx, &r, vi, 512, 512) && (USHORT)r.eax == 0x004F && !memcmp(vi, "VESA", 4);
    }
#endif
    if (!vbe_ok || TEST_NOVBE) {
        dbg("no VBE (ax=%x)\n", r.eax);
        dx->vbever = 0;
        dx->nmodes = 0;
        goto fallback;
    }
    dx->vbever = *(USHORT *)(vi + 4);
    dx->caps = vi[10];
    dx->memkb = *(USHORT *)(vi + 0x12) * 64UL;
    dx->oem[0] = 0;
    copy_rmstr(dx, dx->oem, sizeof(dx->oem), *(ULONG *)(vi + 6));
    if (dx->vbever >= 0x200) {
        copy_rmstr(dx, dx->oem, sizeof(dx->oem), *(ULONG *)(vi + 0x16));
        copy_rmstr(dx, dx->oem, sizeof(dx->oem), *(ULONG *)(vi + 0x1A));
    }
    lp = *(ULONG *)(vi + 0x0E);
    dbg("VBE %x mem=%uK oem='%s' list=%x\n", dx->vbever, dx->memkb, dx->oem, lp);
    /* mode list: inside our buffer or in the ROM */
    {
        ULONG lin = ((lp >> 16) << 4) + (USHORT)lp;
        ULONG blin = dx->have_i10 ? ((ULONG)dx->bseg << 4) + dx->boff : BUFOFF;
        if (lin >= blin && lin < blin + 512) {
            USHORT *s = (USHORT *)(vi + (lin - blin));
            while ((UCHAR *)(s + 1) <= vi + 512 && *s != 0xFFFF && nlist < 256) list[nlist++] = *s++;
        } else {
            USHORT w;
            while (nlist < 256 && rm_read(dx, (USHORT)(lp >> 16), (USHORT)(lp + nlist * 2), &w, 2) && w != 0xFFFF)
                list[nlist++] = w;
        }
    }
    dx->nmodes = 0;
    for (i = 0; i < nlist && dx->nmodes < WVB_MAXMODES; i++) {
        MODE md;
        USHORT attr;
        UCHAR bpp, model;
        ULONG j, pitch;
        memset(mi, 0, sizeof(mi));
        memset(&r, 0, sizeof(r));
        r.eax = 0x4F01; r.ecx = list[i]; r.usebuf = 1;
        if (!int10(dx, &r, mi, 0, 256) || (USHORT)r.eax != 0x004F) continue;
        attr = *(USHORT *)mi;
        bpp = mi[0x19]; model = mi[0x1B];
        if ((attr & 0x11) != 0x11) continue;          /* supported, graphics */
        memset(&md, 0, sizeof(md));
        md.vbe = list[i];
        md.w = *(USHORT *)(mi + 0x12);
        md.h = *(USHORT *)(mi + 0x14);
        md.phys = *(ULONG *)(mi + 0x28);
        md.attr_vga = (attr & 0x20) ? 0 : 1;
        pitch = *(USHORT *)(mi + 0x10);
        if (md.w < 640 || md.h < 480) continue;
        if ((attr & 0x80) && md.phys && dx->access != 2) {
            if (dx->vbever >= 0x300 && *(USHORT *)(mi + 0x32)) pitch = *(USHORT *)(mi + 0x32);
        } else {
            /* no linear frame buffer (VBE 1.2, ISA/VL-Bus cards, or
               WinVbe.Access=2): window A or B at A000h, writable */
            UCHAR wa = mi[2], wb = mi[3];
            USHORT gran = *(USHORT *)(mi + 4), wsz = *(USHORT *)(mi + 6);
            if (dx->access == 1 || (attr & 0x40)) continue;
            if ((wa & 5) == 5 && *(USHORT *)(mi + 8) == 0xA000) md.win = 0;
            else if ((wb & 5) == 5 && *(USHORT *)(mi + 10) == 0xA000) md.win = 1;
            else continue;
            if (wsz > 64) wsz = 64;
            if (!gran || !wsz || gran > wsz || (64 % gran)) continue;
            md.banked = 1;
            md.gran_kb = gran;
            md.winsz_kb = wsz;
            md.phys = 0xA0000;
        }
        if (bpp == 8 && model == 4) {
            md.bpp = 8; md.Bpp = 1;
        } else if (model == 6 && (bpp == 15 || bpp == 16 || bpp == 24 || bpp == 32)) {
            const UCHAR *mk = (dx->vbever >= 0x300 && mi[0x36]) ? mi + 0x36 : mi + 0x1F;
            md.rbits = mk[0]; md.rpos = mk[1]; md.gbits = mk[2]; md.gpos = mk[3];
            md.bbits = mk[4]; md.bpos = mk[5];
            if (bpp == 16 && md.gbits == 5) bpp = 15;
            if (bpp == 15 && !md.rbits) { md.rbits = md.gbits = md.bbits = 5; md.rpos = 10; md.gpos = 5; }
            if (bpp == 16 && !md.rbits) { md.rbits = md.bbits = 5; md.gbits = 6; md.rpos = 11; md.gpos = 5; }
            if (bpp >= 24 && !md.rbits) { md.rbits = md.gbits = md.bbits = 8; md.rpos = 16; md.gpos = 8; }
            md.bpp = bpp; md.Bpp = (UCHAR)((bpp + 7) / 8);
        } else
            continue;
        if (!pitch) pitch = md.w * md.Bpp;
        md.pitch = pitch;
        if (dx->memkb && (pitch * md.h + 1023) / 1024 > dx->memkb) continue;
        for (j = 0; j < dx->nmodes; j++)
            if (dx->modes[j].w == md.w && dx->modes[j].h == md.h && dx->modes[j].bpp == md.bpp) break;
        md.hzmask = hz_mask(dx, &md);
        if (j < dx->nmodes) {
            /* the same mode with a linear frame buffer is better */
            if (!dx->modes[j].banked || md.banked) continue;
            dx->modes[j] = md;
        } else
            dx->modes[dx->nmodes++] = md;
        dbg(" mode %x %ux%ux%u pitch=%u %s=%x gran=%uK hz=%x\n", md.vbe, md.w, md.h, md.bpp, md.pitch,
            md.banked ? "window" : "lfb", md.phys, md.gran_kb, md.hzmask);
    }
    /* Bochs/QEMU/VirtualBox (DISPI): the adapter sets any size, so offer
       the monitor's native size and the common large ones the BIOS list
       stops short of (ultrawide 21:9 and 32:9, 4K, 5K, 8K) */
    {
        static const USHORT big[][2] = {
            { 1920, 1080 }, { 1920, 1200 }, { 2560, 1080 }, { 2560, 1440 }, { 2560, 1600 },
            { 3440, 1440 }, { 3840, 1080 }, { 3840, 1600 }, { 3840, 2160 }, { 4096, 2160 },
            { 5120, 1440 }, { 5120, 2160 }, { 5120, 2880 }, { 7680, 2160 }, { 7680, 4320 }
        };
        static const UCHAR bpps[] = { 8, 16, 24, 32 };
        USHORT id = dispi_rd(0), maxw = 0, maxh = 0;
        ULONG phys = 0, k, b, nb = sizeof(big) / sizeof(big[0]);
        for (k = 0; k < dx->nmodes; k++)
            if (!dx->modes[k].banked && dx->modes[k].phys) { phys = dx->modes[k].phys; break; }
        if (id >= 0xB0C0 && id <= 0xB0CF && phys && dx->access != 2) {
            if (id >= 0xB0C2) {                 /* GETCAPS: the largest size it takes */
                USHORT en = dispi_rd(4);
                dispi_wr(4, (USHORT)(en | 0x02));
                maxw = dispi_rd(1); maxh = dispi_rd(2);
                dispi_wr(4, en);
            }
            if (maxw < 640 || maxh < 480) maxw = maxh = 16384;
            for (k = 0; k <= nb; k++) {
                USHORT w = k < nb ? big[k][0] : dx->edid_w, h = k < nb ? big[k][1] : dx->edid_h;
                if (w < 640 || h < 480 || w > maxw || h > maxh) continue;
                for (b = 0; b < sizeof(bpps) && dx->nmodes < WVB_MAXMODES; b++) {
                    MODE md;
                    ULONG j;
                    for (j = 0; j < dx->nmodes; j++)
                        if (dx->modes[j].w == w && dx->modes[j].h == h &&
                            (dx->modes[j].bpp == bpps[b] || (bpps[b] == 16 && dx->modes[j].bpp == 15))) break;
                    if (j < dx->nmodes) continue;
                    memset(&md, 0, sizeof(md));
                    md.w = w; md.h = h; md.bpp = bpps[b]; md.Bpp = (UCHAR)(bpps[b] / 8);
                    md.pitch = (ULONG)w * md.Bpp;
                    if (dx->memkb && (md.pitch * h + 1023) / 1024 > dx->memkb) continue;
                    md.phys = phys;
                    md.dispi = 1;
                    md.attr_vga = 1;
                    md.hzmask = 1;
                    if (md.bpp == 16) { md.rbits = md.bbits = 5; md.gbits = 6; md.rpos = 11; md.gpos = 5; }
                    if (md.bpp >= 24) { md.rbits = md.gbits = md.bbits = 8; md.rpos = 16; md.gpos = 8; }
                    dx->modes[dx->nmodes++] = md;
                    dbg(" mode DISPI %ux%ux%u\n", w, h, md.bpp);
                }
            }
        }
    }
    /* 15 bpp only where there is no 16 bpp mode of the same size */
    for (i = 0; i < dx->nmodes; i++) {
        ULONG j;
        if (dx->modes[i].bpp != 15) continue;
        for (j = 0; j < dx->nmodes; j++)
            if (dx->modes[j].bpp == 16 && dx->modes[j].w == dx->modes[i].w && dx->modes[j].h == dx->modes[i].h) break;
        if (j < dx->nmodes) {
            for (j = i; j + 1 < dx->nmodes; j++) dx->modes[j] = dx->modes[j + 1];
            dx->nmodes--; i--;
        }
    }
#ifdef VGAREPL
    /* Windows NT 3.x has no display driver for a linear frame buffer
       (VGA256.DLL and VGA64K.DLL need a banked VGA window): the VGA.SYS
       replacement works there like Microsoft's, VGA mode 12h with VGA.DLL */
    if (dx->level < 40) {
        MODE *md = &dx->modes[0];
        memset(md, 0, sizeof(*md));
        md->vbe = 0x12;
        md->w = 640; md->h = 480; md->bpp = 4;
        md->pitch = 80;
        md->planar = 1;
        md->attr_vga = 1;
        md->hzmask = 1;
        md->phys = 0xA0000;
        dx->nmodes = 1;
        dbg("VGA replacement: VGA 640x480x4\n");
    } else
    /* the VGA.SYS replacement offers one linear frame buffer mode at the
       BIOS refresh rate that FRAMEBUF.DLL draws in. The order is the
       preference: the native resolution of the monitor (EDID) in 32 bpp
       first, then 800x600 in 32 bpp, then 640x480 in 16 bpp; when the BIOS
       has none of them, the VGA fallback below (320x200) still starts
       Windows. 24 bpp stands in for a missing 32 bpp mode, 15 bpp for a
       missing 16 bpp one. */
    {
        USHORT want[3][3];
        MODE keep[2];
        ULONG nw = 0, nk = 0, k, j;
        if (dx->edid_ok && dx->edid_w >= 640 && dx->edid_h >= 480 &&
            !(dx->edid_w == 800 && dx->edid_h == 600)) {
            want[nw][0] = dx->edid_w; want[nw][1] = dx->edid_h; want[nw][2] = 32; nw++;
        }
        want[nw][0] = 800; want[nw][1] = 600; want[nw][2] = 32; nw++;
        want[nw][0] = 640; want[nw][1] = 480; want[nw][2] = 16; nw++;
        for (k = 0; k < nw && nk == 0; k++) {
            int best = -1;
            for (j = 0; j < dx->nmodes; j++) {
                MODE *m = &dx->modes[j];
                if (m->banked || m->w != want[k][0] || m->h != want[k][1]) continue;
                if (want[k][2] == 32) {
                    if (m->bpp == 32) { best = (int)j; break; }
                    if (m->bpp == 24 && best < 0) best = (int)j;
                } else {
                    if (m->bpp == 16) { best = (int)j; break; }
                    if (m->bpp == 15 && best < 0) best = (int)j;
                }
            }
            if (best >= 0) {
                keep[0] = dx->modes[best]; keep[0].hzmask = 1; nk = 1;
                dbg("VGA replacement: choice %u, %ux%ux%u\n", k, keep[0].w, keep[0].h, keep[0].bpp);
            }
        }
        /* UEFI: a VBE shim (OVMF, CSM-less firmware) has no VGA behind it,
           so the 320x200 fallback would show nothing. Such firmware lists
           only the pre-set GOP mode, or reports a linear graphics mode as
           the current one (4F03; a real VBE BIOS still is in text mode 3
           here): use that mode as it is. A real VBE BIOS without any of the
           modes above goes to the VGA fallback. */
        if (nk == 0) {
            LONG pick = -1;
            memset(&r, 0, sizeof(r)); r.eax = 0x4F03;
            if (int10(dx, &r, 0, 0, 0) && (USHORT)r.eax == 0x004F)
                for (j = 0; j < dx->nmodes; j++)
                    if (!dx->modes[j].dispi && (dx->modes[j].vbe & 0x1FF) == ((USHORT)r.ebx & 0x1FF)) { pick = (LONG)j; break; }
            if (pick < 0 && dx->nmodes == 1) pick = 0;
            if (pick >= 0 && !dx->modes[pick].banked && dx->modes[pick].bpp >= 8) {
                keep[0] = dx->modes[pick]; keep[0].hzmask = 1; nk = 1;
                dbg("VGA replacement: firmware mode %x %ux%ux%u\n", keep[0].vbe, keep[0].w, keep[0].h, keep[0].bpp);
            }
        }
        /* only the one mode is offered, so Windows cannot fall back to a
           different mode from an older registry setting (a fresh Windows
           2000/XP hardware profile asks for 640x480 in 16 colours); more
           modes need the full WINVBE driver */
#ifdef TESTPITCH
        if (nk) keep[0].pitch += TESTPITCH;     /* test: a mode list line length the BIOS does not set */
#endif
        for (k = 0; k < nk; k++) dx->modes[k] = keep[k];
        dx->nmodes = nk;
        dbg("VGA replacement: %u modes\n", nk);
    }
#endif
fallback:
    /* no VBE graphics mode at all: standard VGA 320x200 in 256 colours,
       so that Windows still starts (instead of "The required system file
       ... is bad or missing" from WINSRV when no display works) */
    if (!dx->nmodes) {
        MODE *md = &dx->modes[0];
        memset(md, 0, sizeof(*md));
        md->vbe = 0x13;
        md->w = 320; md->h = 200; md->bpp = 8; md->Bpp = 1;
        md->attr_vga = 1;
        md->pitch = 320;
        md->hzmask = 1;
#ifdef VGAREPL
        md->banked = 0;                 /* 64000 bytes: linear for FRAMEBUF.DLL */
#else
        md->banked = 1;
#endif
        md->gran_kb = 64; md->winsz_kb = 64;
        md->phys = 0xA0000;
        dx->nmodes = 1;
        dbg("no VBE graphics modes: VGA 320x200x8\n");
    }
    dx->gathered = 1;
    dx->vcur = -1;
    dx->scale = reg_dword(dx, WVB_REG_SCALE, 0);
    if (dx->scale > 2) dx->scale = 0;

    /* information for WINVBECF */
    {
        WVBINFO *f = &dx->info;
        memset(f, 0, sizeof(*f));
        f->size = sizeof(*f);
        f->ntlevel = dx->level;
        f->vbever = dx->vbever;
        f->memkb = dx->memkb;
        memcpy(f->oem, dx->oem, sizeof(f->oem));
        f->edid_ok = (UCHAR)dx->edid_ok;
        memcpy(f->edid, dx->edid, 128);
        f->natw = dx->edid_w; f->nath = dx->edid_h; f->nathz = dx->edid_hz;
        memcpy(f->patch, dx->patchres, sizeof(f->patch));
        f->nmodes = dx->nmodes;
        for (i = 0; i < dx->nmodes; i++) {
            f->modes[i].w = dx->modes[i].w; f->modes[i].h = dx->modes[i].h;
            f->modes[i].bpp = dx->modes[i].bpp; f->modes[i].hzmask = dx->modes[i].hzmask;
            f->modes[i].vbe = dx->modes[i].vbe;
        }
        /* shown by the Display Type dialog (NT 3.5 and later) */
        {
            static WCHAR chip[] = L"VESA VBE x.x", dac[] = L"VGA compatible (6/8 bit)";
            WCHAR adapter[64];
            ULONG mem = dx->memkb * 1024, n;
            chip[9] = (WCHAR)('0' + ((dx->vbever >> 8) & 0xF));
            chip[11] = (WCHAR)('0' + ((dx->vbever >> 4) & 0xF));
            for (n = 0; n < 63 && dx->oem[n]; n++) adapter[n] = (WCHAR)(UCHAR)dx->oem[n];
            adapter[n] = 0;
            VideoPortSetRegistryParameters(dx, L"HardwareInformation.ChipType", chip, sizeof(chip));
            VideoPortSetRegistryParameters(dx, L"HardwareInformation.DacType", dac, sizeof(dac));
            VideoPortSetRegistryParameters(dx, L"HardwareInformation.MemorySize", &mem, sizeof(mem));
            VideoPortSetRegistryParameters(dx, L"HardwareInformation.AdapterString", adapter, (n + 1) * sizeof(WCHAR));
        }
        VideoPortSetRegistryParameters(dx, WVB_REG_INFO, f,
            (ULONG)(sizeof(*f) - sizeof(f->modes) + dx->nmodes * sizeof(WVBMODE)));
    }
}

/* ------------------------------------------------------------------ */
/* mode set                                                            */

static void repost(DEVEXT *dx);
static int set_mode1(DEVEXT *dx, ULONG idx, ULONG hzi, int noclear);

/* PCI header of the adapter (Windows 2000 and later): kept from the start
   and written back when a stand by (S3) has reset it - the adapter stays
   in D0, so nobody else restores its BARs and command register */
static void pci_save(DEVEXT *dx)
{
    if (dx->level < 50 || !dx->getbus || !dx->setbus) return;
    if (dx->getbus(dx, PCIConfiguration, 0, dx->pcicfg, 0, 64) == 64 &&
        (dx->pcicfg[0] & 0xFFFF) != 0xFFFF)
        dx->pci_ok = 1;
}

static int pci_restore(DEVEXT *dx)
{
    ULONG now[16];
    if (!dx->pci_ok) return 0;
    if (dx->getbus(dx, PCIConfiguration, 0, now, 0, 64) != 64) return 0;
    if (!memcmp(&now[4], &dx->pcicfg[4], 6 * 4) && (now[1] & 7) == (dx->pcicfg[1] & 7)) return 0;
    dbg("PCI header restored (BAR0 %x -> %x, command %x -> %x)\n", now[4], dx->pcicfg[4],
        now[1] & 0xFFFF, dx->pcicfg[1] & 0xFFFF);
    dx->setbus(dx, PCIConfiguration, 0, &dx->pcicfg[3], 0x0C, 4);   /* cache line, latency */
    dx->setbus(dx, PCIConfiguration, 0, &dx->pcicfg[4], 0x10, 6 * 4);   /* BARs */
    dx->setbus(dx, PCIConfiguration, 0, &dx->pcicfg[12], 0x30, 4);  /* ROM BAR */
    dx->setbus(dx, PCIConfiguration, 0, &dx->pcicfg[15], 0x3C, 4);  /* interrupt line */
    dx->setbus(dx, PCIConfiguration, 0, &dx->pcicfg[1], 0x04, 2);   /* command */
    return 1;
}

/* Bochs/QEMU/VirtualBox adapters: the mode through the DISPI registers,
   for when the video BIOS is not usable (after stand by in a VM) */
static int dispi_mode(DEVEXT *dx, ULONG idx, ULONG hzi, int noclear)
{
    MODE *md = &dx->modes[idx];
    USHORT id = dispi_rd(0);
    if (md->banked || id < 0xB0C0 || id > 0xB0CF) return 0;
    dispi_wr(4, 0);
    dispi_wr(1, md->w);
    dispi_wr(2, md->h);
    dispi_wr(3, md->bpp);
    dispi_wr(4, 0x41 | (noclear ? 0x80 : 0));       /* enabled, linear frame buffer */
    dispi_wr(6, md->w);
    dispi_wr(8, 0);
    dispi_wr(9, 0);
    if (dispi_rd(1) != md->w || dispi_rd(2) != md->h) return 0;
    /* after a reset the attribute controller has the palette address
       source bit off: the screen would stay blank */
    inb(0x3DA);
    outb(0x3C0, 0x20);
    dx->cur_pitch = (ULONG)dispi_rd(6) * md->Bpp;
    dx->dacbits = 6;
    dx->cur = idx;
    dx->cur_hz = hzi;
    dx->mode_set = 1;
    dx->start = 0;
    dbg("set %ux%ux%u through DISPI\n", md->w, md->h, md->bpp);
    return 1;
}

static int set_mode(DEVEXT *dx, ULONG idx, ULONG hzi, int noclear)
{
    if (dx->modes[idx].dispi)                   /* a size the BIOS does not list */
        return dispi_mode(dx, idx, hzi, noclear);
    if (!set_mode1(dx, idx, hzi, noclear)) {
#ifdef _X86_
        /* the mode set of a BIOS that fails in virtual 8086 mode */
        if (!dx->use_emu && dx->int10_mode == 0 && emu_start(dx, "mode set failed in V86 mode")) {
            if (set_mode1(dx, idx, hzi, noclear)) { dx->reposted = 0; return 1; }
        }
#endif
        /* after stand by the video BIOS may need its initialization
           (the system BIOS does not always run it again) */
        if (!dx->reposted) {
            dx->reposted = 1;
            if (pci_restore(dx) && set_mode1(dx, idx, hzi, noclear)) { dx->reposted = 0; return 1; }
            repost(dx);
            if (set_mode1(dx, idx, hzi, noclear)) { dx->reposted = 0; return 1; }
        }
        return dispi_mode(dx, idx, hzi, noclear);
    }
    dx->reposted = 0;
    return 1;
}

static int set_mode1(DEVEXT *dx, ULONG idx, ULONG hzi, int noclear)
{
    MODE *md = &dx->modes[idx];
    RM r;
    int done = 0;
    ULONG lin = md->banked ? 0 : 0x4000;

    if (md->vbe == 0x13 || md->planar) {        /* VGA fallback, VGA mode 12h */
        memset(&r, 0, sizeof(r));
        r.eax = (noclear ? 0x80 : 0) | md->vbe;
        int10(dx, &r, 0, 0, 0);
        dx->cur_pitch = md->pitch;
        dx->dacbits = 6;
        dx->cur = idx;
        dx->cur_hz = hzi;
        dx->mode_set = 1;
        dx->start = 0;
        dx->bank = 0xFFFFFFFF;
        dbg("set VGA %xh\n", md->vbe);
        return 1;
    }
#ifdef VGAREPL
    if (0) {
#else
    if (hzi < WVB_NHZ && dx->vbever >= 0x300) {
#endif
        CRTC c;
        if (!fill_crtc(dx, &c, md, wvb_hz[hzi])) goto no_crtc;
        memset(&r, 0, sizeof(r));
        r.eax = 0x4F0B; r.ebx = 0; r.ecx = c.pclk; r.edx = md->vbe;
        if (int10(dx, &r, 0, 0, 0) && (USHORT)r.eax == 0x004F && r.ecx) {
            c.pclk = r.ecx;
            c.refresh = (USHORT)(c.pclk / ((ULONG)c.htotal * c.vtotal / 100));
        }
        memset(&r, 0, sizeof(r));
        r.eax = 0x4F02; r.ebx = md->vbe | lin | 0x0800 | (noclear ? 0x8000 : 0); r.usebuf = 1;
        if (int10(dx, &r, &c, sizeof(c), 0) && (USHORT)r.eax == 0x004F) {
            done = 1;
            dbg("set %x %ux%ux%u CRTC %ux%u clk=%u refresh=%u\n", md->vbe, md->w, md->h, md->bpp,
                c.htotal, c.vtotal, c.pclk, c.refresh);
        }
    }
no_crtc:
    if (!done) {
        memset(&r, 0, sizeof(r));
        r.eax = 0x4F02; r.ebx = md->vbe | lin | (noclear ? 0x8000 : 0);
        if (!int10(dx, &r, 0, 0, 0) || (USHORT)r.eax != 0x004F) {
            /* UEFI: OVMF and some firmwares expose the GOP-initialised linear
               framebuffer through a minimal VBE shim that reports one mode and
               cannot set a mode (4F02 fails).  If the current mode (4F03) is
               already the one we want, the framebuffer is live - use it as it
               is instead of failing, so WINVBE runs on a pure-UEFI machine. */
            memset(&r, 0, sizeof(r));
            r.eax = 0x4F03;
            if (int10(dx, &r, 0, 0, 0) && (USHORT)r.eax == 0x004F &&
                ((USHORT)r.ebx & 0x01FF) == (md->vbe & 0x01FF)) {
                dx->uefi_fb = 1;
                dbg("set %x: already current (UEFI framebuffer, no 4F02)\n", md->vbe);
            } else {
                dbg("set %x failed ax=%x\n", md->vbe, r.eax);
                return 0;
            }
        } else {
            dx->uefi_fb = 0;
            dbg("set %x %ux%ux%u\n", md->vbe, md->w, md->h, md->bpp);
        }
    }
    dx->cur_pitch = md->pitch;
    memset(&r, 0, sizeof(r));
    r.eax = 0x4F06; r.ebx = 1;
#ifdef VGAREPL
    /* FRAMEBUF.DLL draws with the line length of the mode list (it never
       asks again after the mode set): when the card uses another one, the
       picture is sheared (diagonal stripes), so the BIOS is told to use
       that length */
    if (int10(dx, &r, 0, 0, 0) && (USHORT)r.eax == 0x004F && (USHORT)r.ebx != md->pitch &&
        (USHORT)r.ebx >= md->w * md->Bpp) {
        ULONG was = (USHORT)r.ebx;
        memset(&r, 0, sizeof(r));
        r.eax = 0x4F06; r.ebx = 2; r.ecx = md->pitch;           /* in bytes (VBE 2.0) */
        if (!int10(dx, &r, 0, 0, 0) || (USHORT)r.eax != 0x004F || (USHORT)r.ebx != md->pitch) {
            memset(&r, 0, sizeof(r));
            r.eax = 0x4F06; r.ebx = 0; r.ecx = md->pitch / md->Bpp;   /* in pixels */
            int10(dx, &r, 0, 0, 0);
        }
        memset(&r, 0, sizeof(r));
        r.eax = 0x4F06; r.ebx = 1;
        if (int10(dx, &r, 0, 0, 0) && (USHORT)r.eax == 0x004F && (USHORT)r.ebx >= md->w * md->Bpp)
            dx->cur_pitch = (USHORT)r.ebx;
        dbg("line length %u, mode list %u, now %u\n", was, md->pitch, dx->cur_pitch);
    }
#else
    if (int10(dx, &r, 0, 0, 0) && (USHORT)r.eax == 0x004F && (USHORT)r.ebx >= md->w * md->Bpp &&
        dx->vbever < 0x300)
        dx->cur_pitch = (USHORT)r.ebx;
#endif
    dx->dacbits = 6;
    if (md->bpp == 8 && (dx->caps & 1) && dx->dac8) {
        memset(&r, 0, sizeof(r));
        r.eax = 0x4F08; r.ebx = 0x0800;
        if (int10(dx, &r, 0, 0, 0) && (USHORT)r.eax == 0x004F && ((r.ebx >> 8) & 0xFF) == 8)
            dx->dacbits = 8;
    }
    dx->cur = idx;
    dx->cur_hz = hzi;
    dx->mode_set = 1;
    dx->start = 0;
    dx->bank = 0xFFFFFFFF;
    return 1;
}

/* banked modes: the position of the VGA window (in granularity units) */
static int set_bank(DEVEXT *dx, ULONG pos)
{
    MODE *md = &dx->modes[dx->cur];
    RM r;
    if (!md->banked) return 0;
    if (pos == dx->bank) return 1;
    if (md->vbe == 0x13) return pos == 0;
    memset(&r, 0, sizeof(r));
    r.eax = 0x4F05; r.ebx = md->win; r.edx = pos;
    if (!int10(dx, &r, 0, 0, 0) || (USHORT)r.eax != 0x004F) {
        dbg("4F05 %x failed ax=%x\n", pos, r.eax);
        dx->bank = 0xFFFFFFFF;
        return 0;
    }
    dx->bank = pos;
    return 1;
}

/* display start: VBE 4F07h, during the vertical retrace (BL = 80h) */
static int set_start(DEVEXT *dx, ULONG off)
{
    MODE *md = &dx->modes[dx->cur];
    RM r;
    ULONG pitch = dx->cur_pitch ? dx->cur_pitch : md->pitch;
    if (off == dx->start) return 1;
    memset(&r, 0, sizeof(r));
    r.eax = 0x4F07; r.ebx = 0x0080;
    r.ecx = (off % pitch) / md->Bpp;
    r.edx = off / pitch;
    if (!int10(dx, &r, 0, 0, 0) || (USHORT)r.eax != 0x004F) {
        memset(&r, 0, sizeof(r));
        r.eax = 0x4F07; r.ebx = 0x0000;
        r.ecx = (off % pitch) / md->Bpp;
        r.edx = off / pitch;
        if (!int10(dx, &r, 0, 0, 0) || (USHORT)r.eax != 0x004F) {
            dbg("4F07 %x failed ax=%x\n", off, r.eax);
            return 0;
        }
    }
    dx->start = off;
    return 1;
}

static void text_mode(DEVEXT *dx)
{
    RM r;
    memset(&r, 0, sizeof(r));
    r.eax = 0x0003;
    int10(dx, &r, 0, 0, 0);
    dx->mode_set = 0;
}

/* ------------------------------------------------------------------ */
/* IOCTLs                                                              */

/* WinVbe.Scale: a mode smaller than the native one of the monitor (EDID)
   is set as the native mode, and WINVBE.DLL scales the desktop into it;
   the index of that native mode, or -1 */
static LONG scaled_native(DEVEXT *dx, ULONG i)
{
    MODE *md = &dx->modes[i];
    ULONG j;
#ifdef VGAREPL
    return -1;
#endif
    if (!dx->scale || !dx->edid_ok || md->banked || md->planar || md->bpp < 8) return -1;
    if (md->w > dx->edid_w || md->h > dx->edid_h || (md->w == dx->edid_w && md->h == dx->edid_h)) return -1;
    for (j = 0; j < dx->nmodes; j++)
        if (dx->modes[j].w == dx->edid_w && dx->modes[j].h == dx->edid_h && dx->modes[j].bpp == md->bpp &&
            !dx->modes[j].banked)
            return (LONG)j;
    return -1;
}

static void fill_info(DEVEXT *dx, VIDEO_MODE_INFORMATION *v, ULONG i, ULONG hzi)
{
    MODE *md = &dx->modes[i];
    LONG nat = scaled_native(dx, i);
    memset(v, 0, sizeof(*v));
    v->Length = sizeof(*v);
    v->ModeIndex = i * 8 + hzi;
    v->VisScreenWidth = md->w;
    v->VisScreenHeight = md->h;
    v->ScreenStride = (dx->mode_set && dx->cur == i) ? dx->cur_pitch : md->pitch;
    v->NumberOfPlanes = 1;
    v->BitsPerPlane = md->bpp;
    v->Frequency = wvb_hz[hzi];
    v->XMillimeter = 320;
    v->YMillimeter = 240;
    v->AttributeFlags = VIDEO_MODE_COLOR | VIDEO_MODE_GRAPHICS;
    v->VideoMemoryBitmapWidth = md->w;
    v->VideoMemoryBitmapHeight = md->h;
    if (md->planar) {
        /* like VGA.SYS: four planes, 64 KB of video memory per plane */
        v->NumberOfPlanes = 4;
        v->BitsPerPlane = 1;
        v->ScreenStride = 80;
        v->VideoMemoryBitmapHeight = 0x10000 / 80;
        v->NumberRedBits = v->NumberGreenBits = v->NumberBlueBits = 6;
        return;
    }
    if (md->banked) {
        /* for WINVBE.DLL: granularity and window size in KB */
        v->AttributeFlags |= VIDEO_MODE_BANKED | VIDEO_MODE_NO_OFF_SCREEN;
        v->DriverSpecificAttributeFlags = WVB_BANKED_FLAG | md->gran_kb | ((ULONG)md->winsz_kb << 16);
    }
    if (nat >= 0) {
        MODE *n = &dx->modes[nat];
        v->VideoMemoryBitmapWidth = n->w;
        v->VideoMemoryBitmapHeight = n->h;
        v->ScreenStride = (dx->mode_set && dx->cur == (ULONG)nat) ? dx->cur_pitch : n->pitch;
        v->DriverSpecificAttributeFlags = WVB_SCALED_FLAG | dx->scale;
    }
    if (md->bpp == 8) {
        UCHAR b = (dx->mode_set && dx->cur == i) ? dx->dacbits : ((dx->caps & 1) && dx->dac8 ? 8 : 6);
        v->NumberRedBits = v->NumberGreenBits = v->NumberBlueBits = b;
        v->AttributeFlags |= VIDEO_MODE_PALETTE_DRIVEN | VIDEO_MODE_MANAGED_PALETTE;
    } else {
        v->NumberRedBits = md->rbits; v->NumberGreenBits = md->gbits; v->NumberBlueBits = md->bbits;
        v->RedMask = ((1UL << md->rbits) - 1) << md->rpos;
        v->GreenMask = ((1UL << md->gbits) - 1) << md->gpos;
        v->BlueMask = ((1UL << md->bbits) - 1) << md->bpos;
    }
}

static ULONG count_modes(DEVEXT *dx)
{
    ULONG i, n = 0;
    int h;
    for (i = 0; i < dx->nmodes; i++)
        for (h = 0; h < WVB_NHZ; h++)
            if (dx->modes[i].hzmask & (1 << h)) n++;
    return n;
}

static void set_palette(DEVEXT *dx, VIDEO_CLUT *clut, ULONG len)
{
    ULONG i, n;
    MODE *md = dx->mode_set ? &dx->modes[dx->cur] : 0;
#ifdef VGAREPL
    int sh = 0;     /* Windows' display drivers scale to NumberRedBits themselves */
#else
    int sh = dx->dacbits == 8 ? 0 : 2;
#endif
    if (len < sizeof(VIDEO_CLUT) - sizeof(VIDEO_CLUTDATA)) return;
    n = clut->NumEntries;
    if (clut->FirstEntry + n > 256) n = 256 - clut->FirstEntry;
    if (md && !md->attr_vga) {
        /* not VGA compatible: VBE function 09h */
        UCHAR b[256 * 4];
        RM r;
        for (i = 0; i < n; i++) {
            b[i * 4 + 0] = clut->LookupTable[i].RgbArray.Blue >> sh;
            b[i * 4 + 1] = clut->LookupTable[i].RgbArray.Green >> sh;
            b[i * 4 + 2] = clut->LookupTable[i].RgbArray.Red >> sh;
            b[i * 4 + 3] = 0;
        }
        memset(&r, 0, sizeof(r));
        r.eax = 0x4F09; r.ebx = 0; r.ecx = n; r.edx = clut->FirstEntry; r.usebuf = 1;
        if (int10(dx, &r, b, n * 4, 0) && (USHORT)r.eax == 0x004F) return;
    }
    outb(0x3C8, (UCHAR)clut->FirstEntry);
    for (i = 0; i < n; i++) {
        outb(0x3C9, clut->LookupTable[i].RgbArray.Red >> sh);
        outb(0x3C9, clut->LookupTable[i].RgbArray.Green >> sh);
        outb(0x3C9, clut->LookupTable[i].RgbArray.Blue >> sh);
    }
}

/* the virtual adapters of Bochs, QEMU, VirtualBox and VMware: video memory
   is ordinary memory there, reading it is fast */
static int has_str(const char *s, const char *w)
{
    for (; *s; s++) {
        const char *a = s, *b = w;
        while (*b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static VP_STATUS NTAPI reg_lut_cb(PVOID hw, PVOID ctx, PWSTR name, PVOID data, ULONG len)
{
    if (len == 768) memcpy(ctx, data, 768);
    return NO_ERROR;
}

static ULONG is_emulator(DEVEXT *dx)
{
    USHORT ven = (USHORT)(dx->pcicfg[0] & 0xFFFF);
    if (ven == 0x1234 || ven == 0x80EE || ven == 0x15AD || ven == 0x1B36 || ven == 0x1AF4) return 1;
    return has_str(dx->oem, "Bochs") || has_str(dx->oem, "SeaBIOS") || has_str(dx->oem, "VirtualBox") ||
           has_str(dx->oem, "VBox") || has_str(dx->oem, "VMware") || has_str(dx->oem, "QEMU") ||
           has_str(dx->oem, "BOCHS") || has_str(dx->oem, "Oracle");
}

static BOOLEAN NTAPI StartIO(PVOID hw, PVIDEO_REQUEST_PACKET rp)
{
    DEVEXT *dx = hw;
    VP_STATUS st = ERROR_INVALID_FUNCTION;
    ULONG info = 0;

    g_dx = dx;
#ifdef DEVDEBUG
    if (rp->IoControlCode != IOCTL_WVB_DEBUG) dbg("ioctl %x in %u out %u\n", rp->IoControlCode, rp->InputBufferLength, rp->OutputBufferLength);
#endif
    switch (rp->IoControlCode) {
    case IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES:
        gather(dx);
        if (rp->OutputBufferLength < sizeof(VIDEO_NUM_MODES)) { st = ERROR_INSUFFICIENT_BUFFER; break; }
        ((VIDEO_NUM_MODES *)rp->OutputBuffer)->NumModes = count_modes(dx);
        ((VIDEO_NUM_MODES *)rp->OutputBuffer)->ModeInformationLength = sizeof(VIDEO_MODE_INFORMATION);
        info = sizeof(VIDEO_NUM_MODES);
        st = NO_ERROR;
        break;
    case IOCTL_VIDEO_QUERY_AVAIL_MODES: {
        ULONG i, n = 0, need;
        int h;
        VIDEO_MODE_INFORMATION *v = rp->OutputBuffer;
        gather(dx);
#ifndef VGAREPL
        dx->scale = reg_dword(dx, WVB_REG_SCALE, 0);        /* WINVBECF changes it at run time */
        if (dx->scale > 2) dx->scale = 0;
#endif
        need = count_modes(dx) * sizeof(VIDEO_MODE_INFORMATION);
        if (rp->OutputBufferLength < need) { st = ERROR_INSUFFICIENT_BUFFER; break; }
        for (i = 0; i < dx->nmodes; i++)
            for (h = 0; h < WVB_NHZ; h++)
                if (dx->modes[i].hzmask & (1 << h)) fill_info(dx, &v[n++], i, h);
        info = need;
        st = NO_ERROR;
        break;
    }
    case IOCTL_VIDEO_QUERY_CURRENT_MODE:
        if (!dx->mode_set) break;
        if (rp->OutputBufferLength < sizeof(VIDEO_MODE_INFORMATION)) { st = ERROR_INSUFFICIENT_BUFFER; break; }
        fill_info(dx, rp->OutputBuffer, dx->vcur >= 0 ? (ULONG)dx->vcur : dx->cur, dx->cur_hz);
        info = sizeof(VIDEO_MODE_INFORMATION);
        st = NO_ERROR;
        break;
    case IOCTL_VIDEO_SET_CURRENT_MODE: {
        ULONG m, i, h;
        if (rp->InputBufferLength < sizeof(VIDEO_MODE)) { st = ERROR_INSUFFICIENT_BUFFER; break; }
        gather(dx);
        m = ((VIDEO_MODE *)rp->InputBuffer)->RequestedMode;
        i = (m & 0x0FFFFFFF) / 8; h = (m & 0x0FFFFFFF) % 8;
        if (i >= dx->nmodes || h >= WVB_NHZ) { st = ERROR_INVALID_PARAMETER; break; }
        {
            LONG nat = scaled_native(dx, i);
            dx->vcur = -1;
            if (nat >= 0) {
                ULONG hh = h;
                if (!(dx->modes[nat].hzmask & (1 << hh)))
                    for (hh = 0; hh < WVB_NHZ && !(dx->modes[nat].hzmask & (1 << hh)); hh++) ;
                if (hh >= WVB_NHZ) hh = 0;
                st = set_mode(dx, (ULONG)nat, hh, (m & VIDEO_MODE_NO_ZERO_MEMORY) != 0) ? NO_ERROR : ERROR_INVALID_PARAMETER;
                if (st == NO_ERROR) { dx->vcur = (LONG)i; dbg("scaled %ux%u in %ux%u\n", dx->modes[i].w, dx->modes[i].h,
                                                               dx->modes[nat].w, dx->modes[nat].h); }
                break;
            }
        }
        st = set_mode(dx, i, h, (m & VIDEO_MODE_NO_ZERO_MEMORY) != 0) ? NO_ERROR : ERROR_INVALID_PARAMETER;
        break;
    }
    case IOCTL_VIDEO_RESET_DEVICE:
        text_mode(dx);
        st = NO_ERROR;
        break;
    case IOCTL_VIDEO_MAP_VIDEO_MEMORY: {
        VIDEO_MEMORY_INFORMATION *mi = rp->OutputBuffer;
        PHYSICAL_ADDRESS pa;
        ULONG len, io, vis;
        PVOID va;
        MODE *md;
        if (rp->InputBufferLength < sizeof(VIDEO_MEMORY) ||
            rp->OutputBufferLength < sizeof(VIDEO_MEMORY_INFORMATION)) { st = ERROR_INSUFFICIENT_BUFFER; break; }
        if (!dx->mode_set) break;
        md = &dx->modes[dx->cur];
        if (md->banked || md->planar) {
            /* the VGA window */
            pa.QuadPart = 0xA0000;
            len = md->planar ? 0x10000 : (ULONG)md->winsz_kb << 10;
            io = 0;
            va = ((VIDEO_MEMORY *)rp->InputBuffer)->RequestedVirtualAddress;
            st = VideoPortMapMemory(dx, pa, &len, &io, &va);
            if (st != NO_ERROR) { dbg("map window failed %x\n", st); break; }
            mi->VideoRamBase = va;
            mi->VideoRamLength = len;
            mi->FrameBufferBase = va;
            mi->FrameBufferLength = len;
            info = sizeof(VIDEO_MEMORY_INFORMATION);
            dx->bank = 0xFFFFFFFF;
            dbg("map window -> %x len %x\n", (ULONG)(ULONG_PTR)va, len);
            break;
        }
        pa.QuadPart = md->phys;
        vis = (dx->cur_pitch * md->h + 0xFFF) & ~0xFFFUL;
        /* NT 4.0 and later: all video memory (DirectDraw off-screen
           surfaces), at most 32 MB; NT 3.x: the visible part */
        len = dx->memkb > 32 * 1024 ? 32UL << 20 : dx->memkb << 10;
        if (dx->level < 40 || len < vis) len = vis;
        for (;;) {
            ULONG l = len;
            io = dx->level >= 40 ? VIDEO_MEMORY_SPACE_P6CACHE : 0;
            va = ((VIDEO_MEMORY *)rp->InputBuffer)->RequestedVirtualAddress;
            st = VideoPortMapMemory(dx, pa, &l, &io, &va);
            if (st != NO_ERROR && io) {
                io = 0; l = len;
                va = ((VIDEO_MEMORY *)rp->InputBuffer)->RequestedVirtualAddress;
                st = VideoPortMapMemory(dx, pa, &l, &io, &va);
            }
            if (st == NO_ERROR || len == vis) { len = l; break; }
            len = len / 2 > vis ? len / 2 : vis;      /* not enough address space */
        }
        if (st != NO_ERROR) { dbg("map %x len %x failed %x\n", md->phys, len, st); break; }
        mi->VideoRamBase = va;
        mi->VideoRamLength = len;
        mi->FrameBufferBase = va;
        mi->FrameBufferLength = vis < len ? vis : len;
        info = sizeof(VIDEO_MEMORY_INFORMATION);
        dbg("map %x -> %x len %x\n", md->phys, (ULONG)(ULONG_PTR)va, len);
        break;
    }
    case IOCTL_VIDEO_UNMAP_VIDEO_MEMORY:
        if (rp->InputBufferLength < sizeof(VIDEO_MEMORY)) { st = ERROR_INSUFFICIENT_BUFFER; break; }
        st = VideoPortUnmapMemory(dx, ((VIDEO_MEMORY *)rp->InputBuffer)->RequestedVirtualAddress, 0);
        break;
    case IOCTL_VIDEO_SHARE_VIDEO_MEMORY: {
        /* frame buffer into a process (DirectDraw) */
        VIDEO_SHARE_MEMORY *sm = rp->InputBuffer;
        VIDEO_SHARE_MEMORY_INFORMATION *si = rp->OutputBuffer;
        PHYSICAL_ADDRESS pa;
        ULONG len, io, total;
        PVOID va;
        if (rp->InputBufferLength < sizeof(VIDEO_SHARE_MEMORY) ||
            rp->OutputBufferLength < sizeof(VIDEO_SHARE_MEMORY_INFORMATION)) { st = ERROR_INSUFFICIENT_BUFFER; break; }
        if (!dx->mode_set || dx->level < 40 || dx->modes[dx->cur].banked) break;
        total = dx->memkb > 32 * 1024 ? 32UL << 20 : dx->memkb << 10;
        if (sm->ViewOffset > total || sm->ViewOffset + sm->ViewSize > total) { st = ERROR_INVALID_PARAMETER; break; }
        pa.QuadPart = dx->modes[dx->cur].phys;
        len = sm->ViewOffset + sm->ViewSize;
        va = sm->ProcessHandle;
        io = VIDEO_MEMORY_SPACE_USER_MODE | VIDEO_MEMORY_SPACE_P6CACHE;
        st = VideoPortMapMemory(dx, pa, &len, &io, &va);
        if (st != NO_ERROR) {
            len = sm->ViewOffset + sm->ViewSize;
            va = sm->ProcessHandle;
            io = VIDEO_MEMORY_SPACE_USER_MODE;
            st = VideoPortMapMemory(dx, pa, &len, &io, &va);
        }
        if (st != NO_ERROR) { dbg("share failed %x\n", st); break; }
        si->SharedViewOffset = sm->ViewOffset;
        si->SharedViewSize = len;
        si->VirtualAddress = va;
        info = sizeof(VIDEO_SHARE_MEMORY_INFORMATION);
        break;
    }
    case IOCTL_VIDEO_UNSHARE_VIDEO_MEMORY: {
        VIDEO_SHARE_MEMORY *sm = rp->InputBuffer;
        if (rp->InputBufferLength < sizeof(VIDEO_SHARE_MEMORY)) { st = ERROR_INSUFFICIENT_BUFFER; break; }
        st = VideoPortUnmapMemory(dx, sm->RequestedVirtualAddress, sm->ProcessHandle);
        break;
    }
    case IOCTL_WVB_SET_START:
        if (rp->InputBufferLength < sizeof(ULONG)) { st = ERROR_INSUFFICIENT_BUFFER; break; }
        if (!dx->mode_set) break;
        st = set_start(dx, *(ULONG *)rp->InputBuffer) ? NO_ERROR : ERROR_INVALID_PARAMETER;
        break;
    case IOCTL_WVB_SET_BANK:
        if (rp->InputBufferLength < sizeof(ULONG)) { st = ERROR_INSUFFICIENT_BUFFER; break; }
        if (!dx->mode_set) break;
        st = set_bank(dx, *(ULONG *)rp->InputBuffer) ? NO_ERROR : ERROR_INVALID_PARAMETER;
        break;
#ifdef VGAREPL
    case IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES: {
        /* VGA.DLL programs the VGA registers itself (3B0h-3DFh) */
        VIDEO_PUBLIC_ACCESS_RANGES *pr = rp->OutputBuffer;
        PHYSICAL_ADDRESS pa;
        ULONG len = 0x30, io = 1;
        PVOID va = NULL;
        if (rp->OutputBufferLength < sizeof(*pr)) { st = ERROR_INSUFFICIENT_BUFFER; break; }
        pa.QuadPart = 0x3B0;
        st = VideoPortMapMemory(dx, pa, &len, &io, &va);
        if (st != NO_ERROR) break;
        pr->InIoSpace = 1;
        pr->MappedInIoSpace = io;
        pr->VirtualAddress = va;
        info = sizeof(*pr);
        break;
    }
    case IOCTL_VIDEO_FREE_PUBLIC_ACCESS_RANGES:
        if (rp->InputBufferLength < sizeof(VIDEO_MEMORY)) { st = ERROR_INSUFFICIENT_BUFFER; break; }
        st = VideoPortUnmapMemory(dx, ((VIDEO_MEMORY *)rp->InputBuffer)->RequestedVirtualAddress, 0);
        break;
    case IOCTL_VIDEO_SET_PALETTE_REGISTERS: {
        /* attribute controller palette (16 colour modes) */
        VIDEO_PALETTE_DATA *pd = rp->InputBuffer;
        ULONG k;
        if (rp->InputBufferLength < FIELD_OFFSET(VIDEO_PALETTE_DATA, Colors) ||
            rp->InputBufferLength < FIELD_OFFSET(VIDEO_PALETTE_DATA, Colors) + pd->NumEntries * sizeof(USHORT) ||
            pd->FirstEntry > 0x10 || pd->FirstEntry + pd->NumEntries > 0x11) { st = ERROR_INVALID_PARAMETER; break; }
        inb(0x3DA);
        for (k = 0; k < pd->NumEntries; k++) {
            outb(0x3C0, (UCHAR)(pd->FirstEntry + k));
            outb(0x3C0, (UCHAR)pd->Colors[k]);
        }
        outb(0x3C0, 0x20);
        st = NO_ERROR;
        break;
    }
    case IOCTL_VIDEO_GET_BANK_SELECT_CODE: {
        /* VGA.DLL of NT 3.x asks; mode 12h is not banked (like VGA.SYS) */
        VIDEO_BANK_SELECT *b = rp->OutputBuffer;
        if (!dx->mode_set || !dx->modes[dx->cur].planar) break;
        if (rp->OutputBufferLength < sizeof(*b)) { st = ERROR_INSUFFICIENT_BUFFER; break; }
        memset(b, 0, sizeof(*b));
        b->Length = b->Size = sizeof(*b);
        b->BankingType = b->PlanarHCBankingType = VideoNotBanked;
        b->BitmapWidthInBytes = 80;
        b->BitmapSize = 0x10000;
        b->Granularity = b->PlanarHCGranularity = 0x10000;
        info = sizeof(*b);
        st = NO_ERROR;
        break;
    }
#endif
    case IOCTL_VIDEO_SET_COLOR_REGISTERS:
        set_palette(dx, rp->InputBuffer, rp->InputBufferLength);
        st = NO_ERROR;
        break;
#ifndef VGAREPL
    case IOCTL_WVB_GET_OPTIONS: {
        WVBOPT *o = rp->OutputBuffer;
        ULONG v;
        if (rp->OutputBufferLength < sizeof(*o)) { st = ERROR_INSUFFICIENT_BUFFER; break; }
        gather(dx);
        memset(o, 0, sizeof(*o));
        o->size = sizeof(*o);
        v = reg_dword(dx, WVB_REG_ROTATE, 0);
        o->rotate = v == 90 || v == 180 || v == 270 ? v : 0;
        o->emulator = is_emulator(dx);
        v = reg_dword(dx, WVB_REG_SHADOW, 0);
        o->shadow = v == 1 || (v == 0 && !o->emulator);
        v = reg_dword(dx, WVB_REG_GAMMA, 100);
        o->gamma = v < 30 ? 30 : v > 300 ? 300 : v;
        v = reg_dword(dx, WVB_REG_BRIGHT, 1000);
        o->bright = v < 900 ? -100 : v > 1100 ? 100 : (long)v - 1000;
        v = reg_dword(dx, WVB_REG_CONTRAST, 100);
        o->contrast = v < 20 ? 20 : v > 300 ? 300 : v;
        v = reg_dword(dx, WVB_REG_SCALE, 0);
        o->scale = v <= 2 ? v : 0;
        for (v = 0; v < 768; v++) o->lut[v / 256][v % 256] = (UCHAR)(v % 256);
        VideoPortGetRegistryParameters(dx, WVB_REG_LUT, FALSE, reg_lut_cb, o->lut);
        for (v = 0; v < 768; v++)
            if (o->lut[v / 256][v % 256] != (UCHAR)(v % 256)) { o->lut_on = 1; break; }
        info = sizeof(*o);
        st = NO_ERROR;
        break;
    }
#endif
    case IOCTL_WVB_DEBUG:
        if (dx->debug && rp->InputBuffer && rp->InputBufferLength) {
            char b[200];
            ULONG n = rp->InputBufferLength < 199 ? rp->InputBufferLength : 199;
            memcpy(b, rp->InputBuffer, n);
            b[n] = 0;
            { char p[24]; sfmt(p, sizeof(p), "WVBDD[%x]: ", (ULONG)(ULONG_PTR)IoGetCurrentProcess() & 0xFFFFF); dputs(p); }
            dputs(b);
        }
        st = NO_ERROR;
        break;
    default:
        dbg("ioctl %x not supported\n", rp->IoControlCode);
        break;
    }
    rp->StatusBlock->Status = st;
    rp->StatusBlock->Information = info;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* initialization                                                      */

static VIDEO_ACCESS_RANGE vga_ranges[] = {
    { { { 0x3B0, 0 } }, 0x0C, 1, 1, 1, 0 },
    { { { 0x3C0, 0 } }, 0x20, 1, 1, 1, 0 },
    { { { 0xA0000, 0 } }, 0x20000, 0, 1, 1, 0 },
};

#ifdef VGAREPL
/* registry key of the service (DriverEntry), "\Device0" is appended */
static WCHAR repl_key[200];

/* InstalledDisplayDrivers of the device key must be REG_MULTI_SZ (NT 4.0
   cannot load a display driver named by the REG_BINARY value that
   VideoPortSetRegistryParameters writes), so the key is written directly:
   FRAMEBUF.DLL (generic frame buffer driver of NT 4.0 and later), VGA.DLL
   on NT 3.x */
static void repl_set_display(DEVEXT *dx)
{
    static const WCHAR fb[] = L"framebuf\0", vga[] = L"vga\0";
    static const WCHAR dev[] = L"\\Device0";
    static WCHAR name[] = L"InstalledDisplayDrivers";
    const WCHAR *dll = dx->level >= 40 ? fb : vga;
    ULONG dlen = dx->level >= 40 ? sizeof(fb) : sizeof(vga);
    WCHAR path[220];
    UNICODE_STRING us, vn;
    OBJECT_ATTRIBUTES oa;
    HANDLE h;
    ULONG n, k;
    NTSTATUS st;

    for (n = 0; repl_key[n] && n < 200; n++) path[n] = repl_key[n];
    if (!n) return;
    for (k = 0; dev[k]; k++) path[n++] = dev[k];
    path[n] = 0;
    us.Buffer = path; us.Length = (USHORT)(n * sizeof(WCHAR)); us.MaximumLength = us.Length + sizeof(WCHAR);
    vn.Buffer = name; vn.Length = sizeof(name) - sizeof(WCHAR); vn.MaximumLength = sizeof(name);
    memset(&oa, 0, sizeof(oa));
    oa.Length = sizeof(oa);
    oa.ObjectName = &us;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    st = ZwOpenKey(&h, 0x0002 /* KEY_SET_VALUE */, &oa);
    if (st >= 0) {
        st = ZwSetValueKey(h, &vn, 0, 7 /* REG_MULTI_SZ */, (PVOID)dll, dlen);
        ZwClose(h);
    }
    dbg("display driver %S: %x\n", dll, st);
}
#endif

static VP_STATUS NTAPI FindAdapter(PVOID hw, PVOID ctx, PWSTR arg, PVIDEO_PORT_CONFIG_INFO ci, PUCHAR again)
{
    DEVEXT *dx = hw;
    PHYSICAL_ADDRESS pa;

    *again = FALSE;
    g_dx = dx;
    dx->level = ctx ? *(int *)ctx : 50;     /* Windows 2000+ (PnP): no HwContext */
    dx->adapter_power = dx->monitor_power = VideoPowerOn;
    dx->vcur = -1;
    dx->debug = (int)reg_dword(dx, WVB_REG_DEBUG, 0);
#ifdef DEVDEBUG
    dx->debug = 1;
#endif
    dx->dac8 = reg_dword(dx, WVB_REG_DAC8, 1);
    dx->refpol = reg_dword(dx, WVB_REG_REFRESH, 0);
    dx->access = reg_dword(dx, WVB_REG_ACCESS, 0);
    dx->int10_mode = reg_dword(dx, WVB_REG_INT10, 0);
#ifdef TEST_ACCESS
    dx->access = TEST_ACCESS;
#endif
    dx->patchcmd[0] = 'A'; dx->patchcmd[1] = 'U'; dx->patchcmd[2] = 'T'; dx->patchcmd[3] = 'O'; dx->patchcmd[4] = 0;
    VideoPortGetRegistryParameters(dx, WVB_REG_PATCH, FALSE, reg_str_cb, dx->patchcmd);
    dbg("FindAdapter level %u cfglen %x\n", dx->level, ci->Length);

    if (VideoPortVerifyAccessRanges(dx, 3, vga_ranges) != NO_ERROR)
        dbg("VGA ranges not claimed\n");

    pa.QuadPart = 0xC0000;
    dx->rom = VideoPortGetDeviceBase(dx, pa, 0x40000, FALSE);

    if (dx->level >= 50 && ci->Length > (ULONG)FIELD_OFFSET(VIDEO_PORT_CONFIG_INFO, VideoPortGetProcAddress) &&
        ci->VideoPortGetProcAddress) {
        typedef VP_STATUS (NTAPI *QS)(PVOID, VIDEO_PORT_SERVICES, PINTERFACE);
        QS qs = (QS)ci->VideoPortGetProcAddress(dx, (PUCHAR)"VideoPortQueryServices");
        dx->getbus = (ULONG (NTAPI *)(PVOID, BUS_DATA_TYPE, ULONG, PVOID, ULONG, ULONG))
            ci->VideoPortGetProcAddress(dx, (PUCHAR)"VideoPortGetBusData");
        dx->setbus = (ULONG (NTAPI *)(PVOID, BUS_DATA_TYPE, ULONG, PVOID, ULONG, ULONG))
            ci->VideoPortGetProcAddress(dx, (PUCHAR)"VideoPortSetBusData");
        pci_save(dx);
        if (qs) {
            dx->i10.Size = sizeof(dx->i10);
            dx->i10.Version = VIDEO_PORT_INT10_INTERFACE_VERSION_1;
            if (qs(dx, VideoPortServicesInt10, (PINTERFACE)&dx->i10) == NO_ERROR) {
                ULONG len = BUFSIZE;
                /* the reference callback of VIDEOPRT.SYS is __stdcall (the
                   MinGW header declares it without a calling convention) */
                ((VOID (NTAPI *)(PVOID))dx->i10.InterfaceReference)(dx->i10.Context);
                if (dx->i10.Int10AllocateBuffer(dx->i10.Context, &dx->bseg, &dx->boff, &len) == NO_ERROR)
                    dx->have_i10 = 1;
            }
        }
        dbg("Int10 interface %u buffer %x:%x\n", dx->have_i10, dx->bseg, dx->boff);
#ifdef DEVDEBUG
        { ULONG k; ULONG *q = (ULONG *)&dx->i10;
          for (k = 0; k < sizeof(dx->i10) / 4; k += 4) dbg(" i10[%u]: %x %x %x %x\n", k, q[k], q[k+1], q[k+2], q[k+3]); }
#endif
    }

#ifdef VGAREPL
    /* the VGA.SYS replacement draws with a display driver of Windows
       itself instead of VGA.DLL (planar 640x480 in 16 colours) */
    repl_set_display(dx);
#endif
    ci->VdmPhysicalVideoMemoryAddress.QuadPart = 0xA0000;
    ci->VdmPhysicalVideoMemoryLength = 0x20000;
#ifdef VGAREPL
    /* NTVDM shares a buffer of this size with the console (as large as the
       one of VGA.SYS: registers and four 64 KB planes); 0 makes every
       16-bit application fail with "The parameter is incorrect" */
    ci->HardwareStateSize = 0x41000;
#else
    ci->HardwareStateSize = 0;
#endif

    /* the modes are read at the first request of the display driver: the
       BIOS can only be called from CSRSS on NT 3.x - 2000, and the x64
       BIOS emulator of XP/2003 x64 is set up when the device is opened */
#ifndef _X86_
    if (!dx->have_i10)
        return ERROR_DEV_NOT_EXIST;
#endif
    return NO_ERROR;
}

static BOOLEAN NTAPI Initialize(PVOID hw)
{
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* power management (Windows 2000 and later): the adapter in D0..D3     */
/* (stand by, hibernate: the display driver sets the mode again when    */
/* the screen comes back) and the monitor through VBE/PM (DPMS)         */

#define MONITOR_UID 0x00001000UL

static UCHAR dpms_caps(DEVEXT *dx)
{
    RM r;
    if (!(dx->dpms & 0x80)) {
        dx->dpms = 0x80;
        memset(&r, 0, sizeof(r));
        r.eax = 0x4F10; r.ebx = 0x0000;          /* report capabilities */
        if (int10(dx, &r, 0, 0, 0) && (USHORT)r.eax == 0x004F)
            dx->dpms |= (UCHAR)((r.ebx >> 8) & 0x07);
        dbg("DPMS caps %x\n", dx->dpms & 7);
    }
    return dx->dpms & 7;
}

static ULONG dpms_bit(ULONG state)
{
    switch (state) {
    case VideoPowerStandBy: return 1;
    case VideoPowerSuspend: return 2;
    case VideoPowerOff: case VideoPowerHibernate: case VideoPowerShutdown: return 4;
    }
    return 0;
}

/* write real mode memory below A0000h */
static int rm_write(DEVEXT *dx, USHORT seg, USHORT off, const void *src, ULONG len)
{
    ULONG lin = ((ULONG)seg << 4) + off;
    if (lin + len > 0xA0000) return 0;
#ifdef _X86_
    if (dx->use_emu) return emu_write(dx->emu, lin, src, len);
#endif
    if (dx->have_i10)
        return dx->i10.Int10WriteMemory(dx->i10.Context, seg, off, (PVOID)src, len) == NO_ERROR;
#ifdef _X86_
    if (!lowmem_ok(dx) || !MmIsAddressValid((PVOID)(ULONG_PTR)(lin & ~0xFFF))) return 0;
    memcpy((PVOID)(ULONG_PTR)lin, src, len);
    return 1;
#else
    return 0;
#endif
}

/* Back from stand by (S3) the graphics card may come up without its video
   BIOS having run (the system BIOS does not always "re-POST" it), so the
   VBE functions are dead.  Run the initialization entry of the video ROM
   (C000:0003) like the system BIOS does at boot: a small stub in the real
   mode buffer is made the INT 10h handler for one call; the ROM sets its
   own INT 10h vector while it initializes. */
static void repost(DEVEXT *dx)
{
    static const UCHAR stub[] = {
        0x60, 0x1E, 0x06,                   /* pusha, push ds, push es */
        0x9A, 0x03, 0x00, 0x00, 0xC0,       /* call far C000:0003 */
        0x07, 0x1F, 0x61, 0xCF              /* pop es, pop ds, popa, iret */
    };
    USHORT sseg, soff;
    ULONG oldvec, vec, now;
    UCHAR hdr[3] = { 0, 0, 0 };
    RM r;
    if (dx->rom) memcpy(hdr, dx->rom, 3);
#ifdef _X86_
    if (dx->use_emu) {                  /* the emulator calls C000:0003 itself */
        EMUREGS e;
        if (hdr[0] != 0x55 || hdr[1] != 0xAA) return;
        memset(&e, 0, sizeof(e));
        e.eax = 0x0010;                 /* AH = bus 0, AL = device 2 function 0 (onboard graphics) */
        emu_call(dx->emu, 0xC000, 0x0003, &e);
        memset(&r, 0, sizeof(r));
        r.eax = 0x4F03;
        int10(dx, &r, 0, 0, 0);
        dbg("repost (emulator): 4F03 ax=%x\n", r.eax);
        return;
    }
#endif
    if (!rm_read(dx, 0, 0x40, &oldvec, 4)) return;
    dbg("repost: int10 %x rom %x %x size %u\n", oldvec, hdr[0], hdr[1], hdr[2]);
    if (hdr[0] != 0x55 || hdr[1] != 0xAA) return;
    if (dx->have_i10) { sseg = dx->bseg; soff = (USHORT)(dx->boff + 0x300); }
    else { sseg = 0; soff = BUFOFF + 0x300; }
    vec = ((ULONG)sseg << 16) | soff;
    if (!rm_write(dx, sseg, soff, stub, sizeof(stub)) || !rm_write(dx, 0, 0x40, &vec, 4)) return;
    memset(&r, 0, sizeof(r));
    int10(dx, &r, 0, 0, 0);
    if (rm_read(dx, 0, 0x40, &now, 4) && now == vec) rm_write(dx, 0, 0x40, &oldvec, 4);
    memset(&r, 0, sizeof(r));
    r.eax = 0x4F03;
    int10(dx, &r, 0, 0, 0);
    dbg("repost: int10 now %x, 4F03 ax=%x\n", now, r.eax);
}

static VP_STATUS NTAPI GetPowerState(PVOID hw, ULONG id, PVIDEO_POWER_MANAGEMENT pm)
{
    DEVEXT *dx = hw;
    ULONG st = pm->PowerState;
    dbg("get power %x %u\n", id, st);
    if (id == DISPLAY_ADAPTER_HW_ID) {
        /* as Microsoft's VGA miniport: the adapter stays powered in stand by
           (the system keeps it in D0); hibernation and shut down are fine */
        return st == VideoPowerOn || st == VideoPowerHibernate || st == VideoPowerShutdown ?
               NO_ERROR : ERROR_INVALID_FUNCTION;
    }
    if (id == MONITOR_UID) {
        if (st == VideoPowerOn || st == VideoPowerHibernate || st == VideoPowerShutdown) return NO_ERROR;
        return (dpms_caps(dx) & dpms_bit(st)) ? NO_ERROR : ERROR_INVALID_FUNCTION;
    }
    return ERROR_INVALID_PARAMETER;
}

static VP_STATUS NTAPI SetPowerState(PVOID hw, ULONG id, PVIDEO_POWER_MANAGEMENT pm)
{
    DEVEXT *dx = hw;
    ULONG st = pm->PowerState;
    RM r;
    dbg("power %x -> %u\n", id, st);
    if (id == DISPLAY_ADAPTER_HW_ID) {
        if (st != VideoPowerOn && dx->adapter_power == VideoPowerOn) {
            /* leaving D0: the video BIOS may be run again (POST) on the way
               back; the mode is set again by the display driver */
            dx->mode_set = 0;
            dx->start = 0;
        }
        if (st == VideoPowerOn) pci_restore(dx);
        dx->adapter_power = st;
        return NO_ERROR;
    }
    if (id == MONITOR_UID) {
        ULONG b = st == VideoPowerOn ? 0 : dpms_bit(st);
        if (b && !(dpms_caps(dx) & b)) return ERROR_INVALID_FUNCTION;
        memset(&r, 0, sizeof(r));
        r.eax = 0x4F10; r.ebx = 0x0001 | (b << 8);  /* set display power state */
        if (dpms_caps(dx) && int10(dx, &r, 0, 0, 0) && (USHORT)r.eax != 0x004F && b)
            return ERROR_INVALID_FUNCTION;
        dx->monitor_power = st;
        return NO_ERROR;
    }
    return ERROR_INVALID_PARAMETER;
}

/* one child: the monitor (EDID as its descriptor); gives Windows a monitor
   device whose power (DPMS) we control */
static VP_STATUS NTAPI GetChild(PVOID hw, PVIDEO_CHILD_ENUM_INFO ci, PVIDEO_CHILD_TYPE type,
                                PUCHAR desc, PULONG uid, PULONG unused)
{
    DEVEXT *dx = hw;
    if (ci->ChildIndex == 0) return VIDEO_ENUM_INVALID_DEVICE;     /* no ACPI children */
    if (ci->ChildIndex != 1) return ERROR_NO_MORE_DEVICES;
    *type = Monitor;
    *uid = MONITOR_UID;
    if (desc && ci->ChildDescriptorSize >= 128) {
        if (dx->gathered > 0 && dx->edid_ok) memcpy(desc, dx->edid, 128);
        else memset(desc, 0, 128);
    }
    return VIDEO_ENUM_MORE_DEVICES;
}

static int lev40 = 40, lev35 = 35, lev31 = 31;

ULONG NTAPI DriverEntry(PVOID ctx1, PVOID ctx2)
{
    VIDEO_HW_INITIALIZATION_DATA d;
    ULONG st;

#ifdef DEVDEBUG
    dputs("WVBNT: DriverEntry\n");
#endif
#ifdef VGAREPL
    {
        UNICODE_STRING *rk = ctx2;
        ULONG n = rk && rk->Buffer ? rk->Length / sizeof(WCHAR) : 0;
        if (n > 190) n = 0;
        memcpy(repl_key, rk ? rk->Buffer : NULL, n * sizeof(WCHAR));
        repl_key[n] = 0;
    }
#endif
    VideoPortZeroMemory(&d, sizeof(d));
    d.HwFindAdapter = FindAdapter;
    d.HwInitialize = Initialize;
    d.HwStartIO = StartIO;
    d.HwDeviceExtensionSize = sizeof(DEVEXT);

#ifdef VGAREPL
    /* the VGA.SYS replacement: initialized like Microsoft's VGA.SYS (the
       VgaSave service is not Plug and Play): no power or child callbacks,
       PCI, ISA, EISA, MCA, then internal */
    d.HwInitDataSize = SIZE_OF_W2K_VIDEO_HW_INITIALIZATION_DATA;
    {
        static const INTERFACE_TYPE bus[5] = { PCIBus, Isa, Eisa, MicroChannel, Internal };
        int k;
        st = 0xC0000059L;
        for (k = 0; k < 5; k++) {
            d.AdapterInterfaceType = bus[k];
            st = VideoPortInitialize(ctx1, ctx2, &d, NULL);
#ifdef DEVDEBUG
            { char b[64]; sfmt(b, sizeof(b), "WVBNT: init50 bus %u: %x\n", (ULONG)bus[k], st); dputs(b); }
#endif
            if (st == 0 || st == 0xC0000059L) break;
        }
    }
#else
    /* Windows 2000 and later: PnP miniport */
    d.HwInitDataSize = SIZE_OF_W2K_VIDEO_HW_INITIALIZATION_DATA;
    d.AdapterInterfaceType = PCIBus;
    d.HwSetPowerState = SetPowerState;
    d.HwGetPowerState = GetPowerState;
    d.HwGetVideoChildDescriptor = GetChild;
    d.HwLegacyResourceList = vga_ranges;
    d.HwLegacyResourceCount = 3;
    st = VideoPortInitialize(ctx1, ctx2, &d, NULL);     /* a PnP miniport must not pass a context */
#ifdef DEVDEBUG
    { char b[64]; sfmt(b, sizeof(b), "WVBNT: init50 %x\n", st); dputs(b); }
#endif
#endif
    if (st != 0xC0000059L) return st;           /* STATUS_REVISION_MISMATCH */

    /* NT 4.0 */
    d.AdapterInterfaceType = Isa;
    d.HwInitDataSize = 0x2C;
    st = VideoPortInitialize(ctx1, ctx2, &d, &lev40);
#ifdef DEVDEBUG
    { char b[64]; sfmt(b, sizeof(b), "WVBNT: init40 %x\n", st); dputs(b); }
#endif
    if (st != 0xC0000059L) return st;
    /* NT 3.5 / 3.51 */
    d.HwInitDataSize = 0x28;
    st = VideoPortInitialize(ctx1, ctx2, &d, &lev35);
    if (st != 0xC0000059L) return st;
    /* NT 3.1 */
    d.HwInitDataSize = 0x20;
    return VideoPortInitialize(ctx1, ctx2, &d, &lev31);
}
