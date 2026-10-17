/*
 * WINVBE - VESA BIOS Extensions / VGA / Bochs-DISPI hardware layer.
 *
 * Works in Windows 3.0 real mode (direct INT 10h), standard mode and
 * 386 enhanced mode (DPMI 0300h "simulate real mode interrupt").
 */
#include "drv.h"
#include <conio.h>

extern WORD  __far __pascal GetModuleHandle(LPSTR);
extern DWORD __far __pascal GetProcAddress(WORD, LPSTR);
extern DWORD __far __pascal GlobalDosAlloc(DWORD);

RMREGS   rmr;
#pragma aux rmr "*"
MODEINFO modes[MAX_MODES];
WORD     nmodes;

static WORD sel_c000, sel_e000, sel_f000;
static WORD scratch_sel;
static WORD dpmi_selinc = 8;

/* real mode stack switch info used by entry.asm */
WORD rm_stack_seg, rm_stack_top;
#pragma aux rm_stack_seg "*"
#pragma aux rm_stack_top "*"

/* ---------------- DPMI helpers ---------------------------------------- */

static int dpmi_simint(BYTE intno, RMREGS FAR *r);
#pragma aux dpmi_simint = \
    "push es" \
    "mov es,dx" \
    "mov ax,0300h" \
    "xor bh,bh" \
    "xor cx,cx" \
    "int 31h" \
    "sbb ax,ax" \
    "pop es" \
    parm [bl] [dx di] value [ax] modify [bx cx];

static WORD dpmi_alloc_sel(WORD n);
#pragma aux dpmi_alloc_sel = \
    "xor ax,ax" \
    "int 31h" \
    "jnc ok" \
    "xor ax,ax" \
    "ok:" \
    parm [cx] value [ax];

WORD sel_alloc(WORD count)
{
    if (!g.pmode) return 0;
    return dpmi_alloc_sel(count);
}

static void dpmi_free_sel(WORD s);
#pragma aux dpmi_free_sel = "mov ax,0001h" "int 31h" parm [bx] modify [ax];

void sel_free(WORD sel, WORD count)
{
    WORD i;
    if (!sel) return;
    for (i = 0; i < count; i++)
        dpmi_free_sel(sel + i * dpmi_selinc);
}

static int dpmi_setbase(WORD sel, WORD hi, WORD lo);
#pragma aux dpmi_setbase = "mov ax,0007h" "int 31h" "sbb ax,ax" parm [bx] [cx] [dx] value [ax];
static int dpmi_setlimit(WORD sel, WORD hi, WORD lo);
#pragma aux dpmi_setlimit = "mov ax,0008h" "int 31h" "sbb ax,ax" parm [bx] [cx] [dx] value [ax];
static DWORD dpmi_getbase(WORD sel);
#pragma aux dpmi_getbase = "mov ax,0006h" "int 31h" "jnc ok" "xor cx,cx" "xor dx,dx" "ok:" "mov ax,dx" "mov dx,cx" \
    parm [bx] value [dx ax] modify [cx];
static WORD dpmi_selinc_get(void);
#pragma aux dpmi_selinc_get = "mov ax,0003h" "int 31h" "jnc ok" "mov ax,8" "ok:" value [ax];

static int dpmi_getdesc(WORD sel, BYTE FAR *buf);
#pragma aux dpmi_getdesc = "mov ax,000Bh" "int 31h" "sbb ax,ax" parm [bx] [es di] value [ax];
static int dpmi_setdesc(WORD sel, BYTE FAR *buf);
#pragma aux dpmi_setdesc = "mov ax,000Ch" "int 31h" "sbb ax,ax" parm [bx] [es di] value [ax];

int sel_setbase(WORD sel, DWORD base)
{
    BYTE d[8], r[8];
    if (!dpmi_setbase(sel, HIWORD(base), LOWORD(base)) && dpmi_getbase(sel) == base)
        return 1;
    /* 286 DOS extender (Windows 3.0 standard mode): write the raw descriptor
       including base bits 24..31, which a 386+ CPU honours */
    if (dpmi_getdesc(sel, (BYTE FAR *)d)) return 0;
    d[2] = (BYTE)base; d[3] = (BYTE)(base >> 8); d[4] = (BYTE)(base >> 16); d[7] = (BYTE)(base >> 24);
    if (dpmi_setdesc(sel, (BYTE FAR *)d)) return 0;
    if (dpmi_getdesc(sel, (BYTE FAR *)r)) return 0;
    if (r[2] != d[2] || r[3] != d[3] || r[4] != d[4] || r[7] != d[7]) {
        DBG("setdesc readback mismatch %x %x\n", r[4], r[7]);
        return 0;
    }
    return 1;
}

int sel_setlimit(WORD sel, DWORD limit)
{
    return !dpmi_setlimit(sel, HIWORD(limit), LOWORD(limit));
}

/* DPMI 0800h: map physical region; returns linear address or 0 */
static DWORD dpmi_map(WORD bhi, WORD blo, WORD shi, WORD slo);
#pragma aux dpmi_map = \
    "mov ax,0800h" \
    "int 31h" \
    "jnc ok" \
    "xor bx,bx" \
    "xor cx,cx" \
    "ok:" \
    "mov ax,cx" \
    "mov dx,bx" \
    parm [bx] [cx] [si] [di] value [dx ax];

DWORD map_phys(DWORD phys, DWORD size)
{
    if (!g.pmode) return 0;
    if (phys < 0x100000UL) return phys;
    return dpmi_map(HIWORD(phys), LOWORD(phys), HIWORD(size), LOWORD(size));
}

/* ---------------- shadow frame buffer ---------------------------------- */

/* DPMI 0501h/0600h: size bytes of locked linear memory, 64 KB tile
   selectors over it (the cursor reads it at interrupt time) */
int shadow_alloc(DWORD size)
{
    WORD shi, slo, lhi = 0, llo = 0, hhi = 0, hlo = 0, ok = 0, tiles, i;
    size = (size + 0xFFFFUL) & 0xFFFF0000UL;
    if (g.shsel && g.shsize >= size) return 1;
    shadow_free();
    if (!g.pmode || !g.cpu386 || !size) return 0;
    shi = HIWORD(size); slo = LOWORD(size);
    _asm {
        push si
        push di
        mov bx, shi
        mov cx, slo
        mov ax, 0501h
        int 31h
        jc a_fail
        mov lhi, bx
        mov llo, cx
        mov hhi, si
        mov hlo, di
        mov ok, 1
    a_fail:
        pop di
        pop si
    }
    if (!ok) { DBG("shadow: no memory for %lu bytes\n", size); return 0; }
    g.shlin = MAKELONG(llo, lhi);
    g.shhandle = MAKELONG(hlo, hhi);
    g.shsize = size;
    ok = 0;
    _asm {
        push si
        push di
        mov bx, lhi
        mov cx, llo
        mov si, shi
        mov di, slo
        mov ax, 0600h
        int 31h
        pop di
        pop si
        jc l_fail
        mov ok, 1
    l_fail:
    }
    if (!ok) DBG("shadow: lock failed\n");
    tiles = (WORD)(size >> 16);
    g.shsel = sel_alloc(tiles);
    if (g.shsel) {
        for (i = 0; i < tiles; i++) {
            WORD sl = g.shsel + i * dpmi_selinc;
            if (!sel_setlimit(sl, 0xFFFFUL) || !sel_setbase(sl, g.shlin + ((DWORD)i << 16))) {
                sel_free(g.shsel, tiles);
                g.shsel = 0;
                break;
            }
        }
    }
    if (!g.shsel) { shadow_free(); return 0; }
    g.shtiles = tiles;
    g.lfbinc = dpmi_selinc;
    DBG("shadow: %lu bytes at %lx\n", size, g.shlin);
    return 1;
}

void shadow_free(void)
{
    WORD hhi = HIWORD(g.shhandle), hlo = LOWORD(g.shhandle);
    WORD lhi = HIWORD(g.shlin), llo = LOWORD(g.shlin);
    WORD shi = HIWORD(g.shsize), slo = LOWORD(g.shsize);
    g.shadow = 0;
    if (g.shsel) { sel_free(g.shsel, g.shtiles); g.shsel = 0; g.shtiles = 0; }
    if (!g.shhandle) return;
    _asm {
        push si
        push di
        mov bx, lhi
        mov cx, llo
        mov si, shi
        mov di, slo
        mov ax, 0601h
        int 31h
        mov si, hhi
        mov di, hlo
        mov ax, 0502h
        int 31h
        pop di
        pop si
    }
    g.shhandle = 0; g.shlin = 0; g.shsize = 0;
}

/* WINVBE.386: write combining for the frame buffer mapping (INT 2Fh
   AX=B57Fh, reflected to the VxD's V86 hook). Returns 0 = not done (no
   VxD, no PAT/MTRR), 1 = PAT, 2 = MTRR, 3 = was already on */
WORD vxd_wc(DWORD lin, DWORD size, DWORD phys)
{
    WORD r = 0;
    _asm {
        push si
        push di
        mov edx, lin
        mov ecx, size
        mov esi, phys
        mov bx, 1
        mov ax, 0B57Fh
        int 2Fh
        mov r, ax
        pop di
        pop si
    }
    return r == 0xB57F ? 0 : r;
}

/* ---------------- interrupt 10h --------------------------------------- */

int int10(RMREGS *r)
{
    if (g.pmode) {
        r->ss = rm_stack_seg;
        r->sp = rm_stack_top;
        r->flags = 0x3202 & 0x0202;
        r->fs = r->gs = 0;
        return !dpmi_simint(0x10, (RMREGS FAR *)r);
    }
    /* real mode: entry.asm executes INT 10h from the global rmr */
    if (r != &rmr) rmr = *r;
    rm_int10();
    if (r != &rmr) *r = rmr;
    return 1;
}

static void rclear(void)
{
    memset(&rmr, 0, sizeof(rmr));
}

LPBYTE rm_ptr(WORD seg, WORD off)
{
    DWORD lin;
    if (!g.pmode)
        return (LPBYTE)MK_FP(seg, off);
    if (seg == g.dosbuf_seg)
        return (LPBYTE)MK_FP(g.dosbuf_sel, off);
    lin = ((DWORD)seg << 4) + off;
    if (sel_c000 && lin >= 0xC0000UL && lin < 0xD0000UL)
        return (LPBYTE)MK_FP(sel_c000, (WORD)(lin - 0xC0000UL));
    if (sel_e000 && lin >= 0xE0000UL && lin < 0xF0000UL)
        return (LPBYTE)MK_FP(sel_e000, (WORD)(lin - 0xE0000UL));
    if (sel_f000 && lin >= 0xF0000UL && lin < 0x100000UL)
        return (LPBYTE)MK_FP(sel_f000, (WORD)(lin - 0xF0000UL));
    if (!scratch_sel) {
        scratch_sel = sel_alloc(1);
        if (!scratch_sel) return NULL;
        sel_setlimit(scratch_sel, 0xFFFFUL);
    }
    sel_setbase(scratch_sel, lin);
    return (LPBYTE)MK_FP(scratch_sel, 0);
}

/* ---------------- environment ----------------------------------------- */

static WORD kconst(WORD hk, WORD ord)
{
    DWORD p = GetProcAddress(hk, (LPSTR)MK_FP(0, ord));
    return LOWORD(p);
}

void env_init(void)
{
    WORD hk = GetModuleHandle((LPSTR)"KERNEL");
#ifndef OLDWIN
    DWORD db;
    g.winflags = kconst(hk, 178);
#else
    g.winflags = 0;             /* Windows 1.x/2.x: real (or V86) mode only */
#endif
    {
        DWORD (__far __pascal *gv)(void) = (DWORD (__far __pascal *)(void))GetProcAddress(hk, (LPSTR)MK_FP(0, 3));
        g.winver = gv ? (WORD)gv() : 3;
        g.dibwords = ((g.winver >> 8) & 0xFF) < 10;
    }
    g.pmode    = (g.winflags & 0x0001) ? 1 : 0;
    g.enhanced = (g.winflags & 0x0020) ? 1 : 0;
    g.cpu386   = (g.winflags & 0x000C) ? 1 : 0;   /* 386 or 486 */
#ifdef OLDWIN
    g.ahincr   = 0x1000;        /* real mode huge pointer increment */
#else
    g.ahincr   = kconst(hk, 114);
#endif
#ifdef OLDWIN
    g.a000sel  = 0xA000;        /* no __A000H in 1.x (asking for it is fatal) */
#else
    g.a000sel  = kconst(hk, 174);
#endif
    if (g.pmode) {
        sel_c000 = kconst(hk, 195);
        sel_e000 = kconst(hk, 190);
        sel_f000 = kconst(hk, 194);
        dpmi_selinc = dpmi_selinc_get();
    } else {
        g.a000sel = 0xA000;
    }
#ifdef OLDWIN
    /* Windows 1.x/2.x run in real mode: a paragraph aligned buffer inside
       our fixed data segment serves as the real mode transfer buffer */
    {
        static BYTE rmbuf[4096 + 16];
        LPBYTE p = (LPBYTE)rmbuf;
        WORD seg = FP_SEG(p) + ((FP_OFF(p) + 15) >> 4);
        g.dosbuf_sel = seg;
        g.dosbuf_seg = seg;
    }
#else
    db = GlobalDosAlloc(4096);
    g.dosbuf_sel = LOWORD(db);
    g.dosbuf_seg = HIWORD(db);
#endif
    rm_stack_seg = g.dosbuf_seg;
    rm_stack_top = 4094;
    DBG("WINVBE: winver=%x winflags=%x pm=%u enh=%u ahincr=%x a000=%x dosbuf=%x:%x\n",
        g.winver, g.winflags, g.pmode, g.enhanced, g.ahincr, g.a000sel, g.dosbuf_sel, g.dosbuf_seg);
}

/* ---------------- Bochs / QEMU DISPI ---------------------------------- */

#define DISPI_IDX 0x01CE
#define DISPI_DAT 0x01CF

static WORD dispi_rd(WORD i) { outpw(DISPI_IDX, i); return inpw(DISPI_DAT); }
static void dispi_wr(WORD i, WORD v) { outpw(DISPI_IDX, i); outpw(DISPI_DAT, v); }

static WORD dispi_id;
static DWORD dispi_lfb;
static WORD dispi_mem64k;

static void outpd_(WORD port, DWORD v);
#pragma aux outpd_ = "mov ax,cx" "shl eax,16" "mov ax,bx" "out dx,eax" parm [dx] [cx bx] modify [ax];
static DWORD inpd_(WORD port);
#pragma aux inpd_ = "in eax,dx" "mov edx,eax" "shr edx,16" parm [dx] value [dx ax];
#define outpd outpd_
#define inpd inpd_

static DWORD pci_read(BYTE bus, BYTE dev, BYTE fn, BYTE reg)
{
    DWORD a = 0x80000000UL | ((DWORD)bus << 16) | ((DWORD)dev << 11) | ((DWORD)fn << 8) | (reg & 0xFC);
    outpd(0xCF8, a);
    return inpd(0xCFC);
}

static BYTE pci_present(void)
{
    static BYTE known = 0, ok = 0;
    if (!known) {
        DWORD old = inpd(0xCF8);
        outpd(0xCF8, 0x80000000UL);
        ok = (inpd(0xCF8) == 0x80000000UL);
        outpd(0xCF8, old);
        known = 1;
    }
    return ok;
}

/*
 * Is the VBE linear frame buffer address reachable?
 * Looks at every PCI display controller. Returns
 *   1  address lies in a memory BAR below 4 GB of a display controller
 *   0  display controllers exist but none has a 32-bit reachable BAR there
 *      (typical when "Above 4G decoding"/Resizable BAR moved the BAR above
 *      4 GB and the BIOS reports the truncated low 32 bits)
 *  -1  no PCI or no PCI display controller found (cannot tell)
 * BARs are only read, never sized, so the running device is not disturbed.
 */
static int lfb_pci_check(DWORD phys)
{
    WORD bus, dev, fn, nfn;
    BYTE found = 0;
    if (!pci_present()) return -1;
    for (bus = 0; bus < 256; bus++) {
        for (dev = 0; dev < 32; dev++) {
            DWORD id = pci_read((BYTE)bus, (BYTE)dev, 0, 0);
            if ((WORD)id == 0xFFFF || (WORD)id == 0) continue;
            nfn = (pci_read((BYTE)bus, (BYTE)dev, 0, 0x0C) & 0x00800000UL) ? 8 : 1;
            for (fn = 0; fn < nfn; fn++) {
                DWORD cls;
                BYTE reg;
                if (fn && (WORD)pci_read((BYTE)bus, (BYTE)dev, (BYTE)fn, 0) == 0xFFFF) continue;
                cls = pci_read((BYTE)bus, (BYTE)dev, (BYTE)fn, 0x08) >> 16;   /* class:subclass */
                if ((cls >> 8) != 0x03 && cls != 0x0001) continue;          /* display / old VGA */
                if ((pci_read((BYTE)bus, (BYTE)dev, (BYTE)fn, 0x0C) & 0x007F0000UL) != 0) continue;
                found = 1;
                for (reg = 0x10; reg <= 0x24; reg += 4) {
                    DWORD v = pci_read((BYTE)bus, (BYTE)dev, (BYTE)fn, reg), hi = 0, base;
                    if (v & 1) continue;                                     /* I/O BAR */
                    if (((v >> 1) & 3) == 2 && reg < 0x24) {
                        reg += 4;
                        hi = pci_read((BYTE)bus, (BYTE)dev, (BYTE)fn, reg);
                    }
                    base = v & 0xFFFFFFF0UL;
                    DBG("PCI %u:%u.%u class %x BAR %x: %lx:%08lx\n", bus, dev, fn, (WORD)cls, reg,
                        hi, base);
                    if (hi || !base) continue;
                    if (phys >= base && phys - base < 0x20000000UL) return 1;
                }
            }
        }
    }
    return found ? 0 : -1;
}

static void dispi_probe(void)
{
    BYTE dev;
    dispi_id = dispi_rd(0);
    if (dispi_id < 0xB0C0 || dispi_id > 0xB0CF) {
        dispi_id = 0;
        return;
    }
    dispi_mem64k = (dispi_id >= 0xB0C5) ? dispi_rd(10) : 64;
    if (!dispi_mem64k) dispi_mem64k = 64;
    /* find LFB via PCI: QEMU stdvga 1234:1111, VirtualBox 80EE:BEEF */
    for (dev = 0; dev < 32; dev++) {
        DWORD id = pci_read(0, dev, 0, 0);
        if (id == 0x11111234UL || id == 0xBEEF80EEUL) {
            dispi_lfb = pci_read(0, dev, 0, 0x10) & 0xFFFFFFF0UL;
            break;
        }
    }
    DBG("DISPI id=%x mem=%uK lfb=%lx\n", dispi_id, dispi_mem64k * 64, dispi_lfb);
}

/* ---------------- VMware SVGA II ----------------------------------------- */
/* VMware, VirtualBox (VMSVGA) and QEMU -vga vmware: any resolution in the
   host's colour depth (32 bit), screen-to-screen copies and solid fills by
   the host (RECT_COPY / RECT_FILL), screen updates through the command
   FIFO (SVGA_CMD_UPDATE). */
#define SVGA_ID_2               0x90000002UL
#define SVGA_REG_ID             0
#define SVGA_REG_ENABLE         1
#define SVGA_REG_WIDTH          2
#define SVGA_REG_HEIGHT         3
#define SVGA_REG_MAX_WIDTH      4
#define SVGA_REG_MAX_HEIGHT     5
#define SVGA_REG_BITS_PER_PIXEL 7
#define SVGA_REG_RED_MASK       9
#define SVGA_REG_GREEN_MASK     10
#define SVGA_REG_BLUE_MASK      11
#define SVGA_REG_BYTES_PER_LINE 12
#define SVGA_REG_FB_START       13
#define SVGA_REG_FB_OFFSET      14
#define SVGA_REG_VRAM_SIZE      15
#define SVGA_REG_CAPABILITIES   17
#define SVGA_REG_MEM_START      18
#define SVGA_REG_MEM_SIZE       19
#define SVGA_REG_CONFIG_DONE    20
#define SVGA_REG_SYNC           21
#define SVGA_REG_BUSY           22
#define SVGA_REG_GUEST_ID       23
#define SVGA_REG_HOST_BPP       28
#define SVGA_CAP_RECT_FILL      0x0001
#define SVGA_CAP_RECT_COPY      0x0002
#define SVGA_CMD_UPDATE         1
#define SVGA_CMD_RECT_FILL      2
#define SVGA_CMD_RECT_COPY      3
#define FIFO_MIN 0
#define FIFO_MAX 1
#define FIFO_NEXT 2
#define FIFO_STOP 3

static WORD  svga_port;             /* index port, 0 = no SVGA II device */
static DWORD svga_fbphys, svga_fifophys, svga_fifosize, svga_vram, svga_caps;
static DWORD svga_rmask, svga_gmask, svga_bmask;
static WORD  svga_maxw, svga_maxh, svga_hostbpp;
static WORD  svga_fifosel;
static DWORD svga_fmin, svga_fmax, svga_fnext;
static BYTE  svga_fifo_on, svga_pending, svga_dirty, svga_accel;
static BYTE  svga_verified;         /* the host draws with the geometry of our mode */
static WORD  svga_retry;
static SHORT svga_dl, svga_dt, svga_dr, svga_db;

static DWORD svga_rd(WORD i) { outpd(svga_port, i); return inpd(svga_port + 1); }

/* a virtual machine's display (Bochs/QEMU/VirtualBox DISPI, VMware SVGA
   II), whose screen switches WINVBE.386 handles */
int vbe_virtual_hw(void)
{
#ifdef NODISPIFIX
    return 0;
#endif
    return dispi_id != 0 || svga_port != 0;
}
static void svga_wr(WORD i, DWORD v) { outpd(svga_port, i); outpd(svga_port + 1, v); }

static void svga_probe(void)
{
    BYTE dev;
    DWORD b0;
    svga_port = 0;
    /* Windows 9x: only when asked for (VMware=1) - the SVGA frame buffer
       starts in the VGA memory the VDD keeps for full-screen DOS boxes */
    if (!pci_present() || !get_ini_int("VMware", g.win9x ? 0 : 1)) return;
    for (dev = 0; dev < 32; dev++)
        if (pci_read(0, dev, 0, 0) == 0x040515ADUL) break;
    if (dev == 32) return;
    b0 = pci_read(0, dev, 0, 0x10);
    if (!(b0 & 1)) return;
    svga_port = (WORD)(b0 & 0xFFFC);
    svga_wr(SVGA_REG_ID, SVGA_ID_2);
    if (svga_rd(SVGA_REG_ID) != SVGA_ID_2) { svga_port = 0; return; }
    svga_fbphys = svga_rd(SVGA_REG_FB_START);
    svga_vram = svga_rd(SVGA_REG_VRAM_SIZE);
    svga_fifophys = svga_rd(SVGA_REG_MEM_START);
    svga_fifosize = svga_rd(SVGA_REG_MEM_SIZE);
    svga_caps = svga_rd(SVGA_REG_CAPABILITIES);
    svga_maxw = (WORD)svga_rd(SVGA_REG_MAX_WIDTH);
    svga_maxh = (WORD)svga_rd(SVGA_REG_MAX_HEIGHT);
    svga_hostbpp = (WORD)svga_rd(SVGA_REG_HOST_BPP);
    if (!svga_hostbpp) svga_hostbpp = 32;
    if (svga_hostbpp < 24 || !svga_fbphys || svga_fifosize < 0x4000) { svga_port = 0; return; }
    DBG("SVGA II port=%x fb=%lx vram=%lx fifo=%lx/%lx caps=%lx max=%ux%u hostbpp=%u\n", svga_port,
        svga_fbphys, svga_vram, svga_fifophys, svga_fifosize, svga_caps, svga_maxw, svga_maxh, svga_hostbpp);
}

static void set_svga(MODEINFO *m, WORD w, WORD h)
{
    memset(m, 0, sizeof(*m));
    m->mode = 0xFFFE; m->xres = w; m->yres = h; m->bpp = 32; m->Bpp = 4; m->pf = PF_32;
    m->pitch = m->lpitch = w * 4;
    m->memmodel = 6;
    m->physbase = svga_fbphys;
    m->attr = 0x9B | 0x80;
    m->rsize = 8; m->rpos = 16; m->gsize = 8; m->gpos = 8; m->bsize = 8;
}

/* wait until the host has done every command in the FIFO */
void svga_sync(void)
{
    WORD n;
    if (!svga_fifo_on) return;
    svga_wr(SVGA_REG_SYNC, 1);
    for (n = 0; n < 60000U && svga_rd(SVGA_REG_BUSY); n++) ;
    svga_pending = 0;
}

static void svga_cmd(WORD n, DWORD a0, DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6)
{
    DWORD __far *f = (DWORD __far *)MK_FP(svga_fifosel, 0);
    DWORD arg[7];
    WORD i;
    arg[0] = a0; arg[1] = a1; arg[2] = a2; arg[3] = a3; arg[4] = a4; arg[5] = a5; arg[6] = a6;
    for (;;) {     /* room for the command (and one free dword)? */
        DWORD stop = f[FIFO_STOP], used;
        used = svga_fnext >= stop ? svga_fnext - stop : (svga_fmax - svga_fmin) - (stop - svga_fnext);
        if (used + (DWORD)n * 4 + 4 < svga_fmax - svga_fmin) break;
        svga_sync();
    }
    for (i = 0; i < n; i++) {
        f[(WORD)(svga_fnext >> 2)] = arg[i];
        svga_fnext += 4;
        if (svga_fnext >= svga_fmax) svga_fnext = svga_fmin;
    }
    f[FIFO_NEXT] = svga_fnext;
}

/* the CPU is about to draw into (l,t)-(r,b): accelerated commands must be
   done first; the area is sent to the host with the next svga_flush */
void svga_mark(SHORT l, SHORT t, SHORT r, SHORT b)
{
    if (!g.svga) return;
    if (svga_pending) svga_sync();
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > (SHORT)g.xres) r = g.xres;
    if (b > (SHORT)g.yres) b = g.yres;
    if (r <= l || b <= t) return;
    if (!svga_dirty) { svga_dl = l; svga_dt = t; svga_dr = r; svga_db = b; svga_dirty = 1; return; }
    if (l < svga_dl) svga_dl = l;
    if (t < svga_dt) svga_dt = t;
    if (r > svga_dr) svga_dr = r;
    if (b > svga_db) svga_db = b;
}

void svga_flush(void)
{
    if (!g.svga || !svga_dirty) return;
    svga_dirty = 0;
    if (!svga_fifo_on || !g.enabled || g.in_background) return;
    svga_cmd(5, SVGA_CMD_UPDATE, svga_dl, svga_dt, svga_dr - svga_dl, svga_db - svga_dt, 0, 0);
}

/* The host may still use the geometry of the previous mode for a while
   after a mode change (QEMU until its next display refresh): a filled test
   pixel in the bottom right corner must show up where the CPU sees it. */
static int svga_verify(void)
{
    DWORD save, got, test = 0x00A55A3CUL;
    WORD x = g.xres - 1, y = g.yres - 1;
    if (svga_verified) return 1;
    if (svga_retry) { svga_retry--; return 0; }
    svga_sync();
    scr_read(y, x, 1, (BYTE *)&save);
    svga_cmd(6, SVGA_CMD_RECT_FILL, test, x, y, 1, 1, 0);
    svga_sync();
    scr_read(y, x, 1, (BYTE *)&got);
    scr_write(y, x, 1, (BYTE *)&save);
    if ((got & 0xFFFFFFUL) == test) { svga_verified = 1; DBG("SVGA II accel verified\n"); return 1; }
    svga_retry = 32;
    return 0;
}

/* screen to screen copy by the host; 0 = not possible, do it by CPU */
int svga_copy(SHORT sx, SHORT sy, SHORT dx, SHORT dy, SHORT w, SHORT h)
{
    if (!g.svga || !svga_fifo_on || !svga_accel || !(svga_caps & SVGA_CAP_RECT_COPY) || g.in_background) return 0;
    if ((svga_caps & SVGA_CAP_RECT_FILL) && !svga_verify()) return 0;   /* no fill: cannot test */
    svga_flush();
    svga_cmd(7, SVGA_CMD_RECT_COPY, sx, sy, dx, dy, w, h);
    svga_pending = 1;
    svga_sync();        /* the CPU may draw on it right after */
    return 1;
}

int svga_fill(DWORD color, SHORT x, SHORT y, SHORT w, SHORT h)
{
    if (!g.svga || !svga_fifo_on || !svga_accel || !(svga_caps & SVGA_CAP_RECT_FILL) || g.in_background) return 0;
    if (!svga_verify()) return 0;
    svga_flush();
    svga_cmd(6, SVGA_CMD_RECT_FILL, color, x, y, w, h, 0);
    svga_pending = 1;
    svga_sync();
    return 1;
}

static int svga_set(void)
{
    DWORD __far *f;
    if (svga_fifo_on) svga_sync();
    svga_fifo_on = 0;
    svga_wr(SVGA_REG_WIDTH, g.mi.xres);
    svga_wr(SVGA_REG_HEIGHT, g.mi.yres);
    svga_wr(SVGA_REG_BITS_PER_PIXEL, 32);
    svga_wr(SVGA_REG_ENABLE, 1);
    g.pitch = (WORD)svga_rd(SVGA_REG_BYTES_PER_LINE);
    if (g.pitch < g.mi.xres * 4) g.pitch = g.mi.xres * 4;
    svga_rmask = svga_rd(SVGA_REG_RED_MASK);
    svga_gmask = svga_rd(SVGA_REG_GREEN_MASK);
    svga_bmask = svga_rd(SVGA_REG_BLUE_MASK);
    g.dacbits = 8;
    svga_accel = g.ini_vmaccel;
    if (!g.ini_vmfifo) return 1;      /* host traces the frame buffer */
    if (!svga_fifosel) {
        DWORD lin = map_phys(svga_fifophys, 0x10000UL);
        WORD s;
        if (!lin) return 1;
        s = sel_alloc(1);
        if (!s) return 1;
        if (!sel_setlimit(s, 0xFFFFUL) || !sel_setbase(s, lin)) { sel_free(s, 1); return 1; }
        svga_fifosel = s;
    }
    f = (DWORD __far *)MK_FP(svga_fifosel, 0);
    svga_fmin = 16;
    svga_fmax = svga_fifosize > 0x10000UL ? 0x10000UL : svga_fifosize;
    svga_fnext = svga_fmin;
    f[FIFO_MIN] = svga_fmin;
    f[FIFO_MAX] = svga_fmax;
    f[FIFO_NEXT] = svga_fmin;
    f[FIFO_STOP] = svga_fmin;
    svga_wr(SVGA_REG_CONFIG_DONE, 1);
    svga_fifo_on = 1;
    svga_pending = svga_dirty = 0;
    svga_verified = 0;
    svga_retry = 0;
    return 1;
}

/* leave the SVGA mode (the VGA takes the screen again) */
void svga_off(void)
{
    if (!svga_port || !g.svga) return;
    if (svga_fifo_on) svga_sync();
    svga_wr(SVGA_REG_ENABLE, 0);
    svga_verified = 0;
}

/* the screen was away (DOS box): check the host's geometry again before
   the next accelerated command */
void svga_reverify(void)
{
    svga_verified = 0;
    svga_retry = 0;
}

/* back from a DOS box: SVGA mode still configured, only switched off */
int svga_repair(void)
{
    if (!svga_port || !g.svga) return 0;
    if (svga_rd(SVGA_REG_ENABLE)) return 0;
    svga_wr(SVGA_REG_ENABLE, 1);
    svga_verified = 0;          /* the host sets up its screen again */
    svga_retry = 0;
    return 1;
}

/* ---------------- VBE probing ------------------------------------------ */

static void add_mode(WORD num, LPBYTE mi)
{
    MODEINFO *m;
    WORD attr = *(LPWORD)mi;
    BYTE bpp = mi[0x19];
    BYTE model = mi[0x1B];
    if (nmodes >= MAX_MODES) return;
    if (!(attr & 0x01)) return;              /* not supported */
    if (!(attr & 0x10)) return;              /* text mode */
    if (!(attr & 0x08)) return;              /* mono */
    if (model != 4 && model != 6) return;    /* packed / direct only */
    if (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32) return;
    if (model == 4 && bpp != 8) return;
    m = &modes[nmodes];
    memset(m, 0, sizeof(*m));
    m->mode = num;
    m->attr = attr;
    m->winAattr = mi[2];
    m->winBattr = mi[3];
    m->wingran = *(LPWORD)(mi + 4);
    m->winsize = *(LPWORD)(mi + 6);
    m->winAseg = *(LPWORD)(mi + 8);
    m->winBseg = *(LPWORD)(mi + 10);
    m->winfunc = *(LPDWORD)(mi + 12);
    m->pitch = *(LPWORD)(mi + 16);
    m->xres = *(LPWORD)(mi + 18);
    m->yres = *(LPWORD)(mi + 20);
    m->bpp = bpp;
    m->memmodel = model;
    m->pages = mi[0x1D];
    if (model == 6) {
        m->rsize = mi[0x1F]; m->rpos = mi[0x20];
        m->gsize = mi[0x21]; m->gpos = mi[0x22];
        m->bsize = mi[0x23]; m->bpos = mi[0x24];
        if (bpp == 16 && m->gsize == 5) m->bpp = bpp = 15;
        if (bpp == 15 && m->gsize == 6) m->bpp = bpp = 16;
    } else if (bpp == 15 || bpp == 16) {
        /* VBE 1.2 BIOS without masks */
    }
    if (bpp == 15 && !m->rsize) { m->rsize = 5; m->rpos = 10; m->gsize = 5; m->gpos = 5; m->bsize = 5; m->bpos = 0; }
    if (bpp == 16 && !m->rsize) { m->rsize = 5; m->rpos = 11; m->gsize = 6; m->gpos = 5; m->bsize = 5; m->bpos = 0; }
    if (bpp >= 24 && !m->rsize) { m->rsize = 8; m->rpos = 16; m->gsize = 8; m->gpos = 8; m->bsize = 8; m->bpos = 0; }
    if (g.vbever >= 0x200 && (attr & 0x80))
        m->physbase = *(LPDWORD)(mi + 0x28);
    m->lpitch = m->pitch;
    if (g.vbever >= 0x300 && *(LPWORD)(mi + 0x32))
        m->lpitch = *(LPWORD)(mi + 0x32);
    m->Bpp = (bpp + 7) >> 3;
    if (m->Bpp == 2) m->pf = (bpp == 15) ? PF_555 : PF_565;
    else if (m->Bpp == 1) m->pf = PF_8;
    else if (m->Bpp == 3) m->pf = PF_24;
    else m->pf = PF_32;
    if (!m->pitch) m->pitch = m->xres * m->Bpp;
    if (!m->lpitch) m->lpitch = m->pitch;
    nmodes++;
}

static void read_edid(void)
{
    LPBYTE p;
    WORD i;
    BYTE sum = 0;
    rclear();
    rmr.eax = 0x4F15; rmr.ebx = 0;
    if (!int10(&rmr) || (WORD)rmr.eax != 0x004F) {
        DBG("EDID: DDC not supported (ax=%x)\n", (WORD)rmr.eax);
        return;
    }
    rclear();
    rmr.eax = 0x4F15; rmr.ebx = 1; rmr.ecx = 0; rmr.edx = 0;
    rmr.es = g.dosbuf_seg; rmr.edi = 0x200;
    p = rm_ptr(g.dosbuf_seg, 0x200);
    far_memset(p, 0, 128);
    if (!int10(&rmr) || (WORD)rmr.eax != 0x004F) {
        DBG("EDID: read failed (ax=%x)\n", (WORD)rmr.eax);
        return;
    }
    for (i = 0; i < 128; i++) { g.edid[i] = p[i]; sum += p[i]; }
    if (g.edid[0] != 0 || g.edid[1] != 0xFF || g.edid[7] != 0) {
        DBG("EDID: bad header\n");
        return;
    }
    g.have_edid = 1;
    /* preferred (first) detailed timing */
    {
        BYTE *d = g.edid + 54;
        WORD clk = d[0] | (d[1] << 8);
        if (clk) {
            WORD ha = d[2] | ((d[4] & 0xF0) << 4);
            WORD hb = d[3] | ((d[4] & 0x0F) << 8);
            WORD va = d[5] | ((d[7] & 0xF0) << 4);
            WORD vb = d[6] | ((d[7] & 0x0F) << 8);
            DWORD tot = (DWORD)(ha + hb) * (va + vb);
            g.edid_w = ha;
            g.edid_h = va;
            if (tot) g.edid_hz = (WORD)(((DWORD)clk * 10000UL + tot / 2) / tot);
        }
    }
    DBG("EDID: ok sum=%x preferred %ux%u@%u\n", sum, g.edid_w, g.edid_h, g.edid_hz);
}

int vbe_probe(void)
{
    LPBYTE info, mi;
    LPWORD ml;
    WORD list[160];
    WORD n = 0, i;

    nmodes = 0;
    g.vbever = 0;
    dispi_probe();
    svga_probe();
    if (!g.dosbuf_seg)
        return 0;
    info = rm_ptr(g.dosbuf_seg, 0);
    far_memset(info, 0, 512);
    info[0] = 'V'; info[1] = 'B'; info[2] = 'E'; info[3] = '2';
    rclear();
    rmr.eax = 0x4F00; rmr.es = g.dosbuf_seg; rmr.edi = 0;
    if (!int10(&rmr) || (WORD)rmr.eax != 0x004F ||
        info[0] != 'V' || info[1] != 'E' || info[2] != 'S' || info[3] != 'A') {
        DBG("VBE: not present (ax=%x)\n", (WORD)rmr.eax);
        return 0;
    }
    g.vbever = *(LPWORD)(info + 4);
    g.vbecaps_lo = *(LPWORD)(info + 10);
    g.totalmem64k = *(LPWORD)(info + 18);
    {
        LPSTR oem = (LPSTR)rm_ptr(*(LPWORD)(info + 8), *(LPWORD)(info + 6));
        for (i = 0; i < 63 && oem && oem[i]; i++) g.oemstr[i] = oem[i];
        g.oemstr[i] = 0;
    }
    /* copy the mode list before reusing the buffer */
    info = rm_ptr(g.dosbuf_seg, 0);
    ml = (LPWORD)rm_ptr(*(LPWORD)(info + 16), *(LPWORD)(info + 14));
    while (ml && n < 160 && ml[n] != 0xFFFF)
        { list[n] = ml[n]; n++; }
    DBG("VBE %x mem=%uK caps=%x modes=%u oem='%s'\n", g.vbever, g.totalmem64k * 64, g.vbecaps_lo, n, g.oemstr);

    for (i = 0; i < n; i++) {
        mi = rm_ptr(g.dosbuf_seg, 0x200);
        far_memset(mi, 0, 256);
        rclear();
        rmr.eax = 0x4F01; rmr.ecx = list[i]; rmr.es = g.dosbuf_seg; rmr.edi = 0x200;
        if (!int10(&rmr) || (WORD)rmr.eax != 0x004F)
            continue;
        mi = rm_ptr(g.dosbuf_seg, 0x200);
        add_mode(list[i], mi);
    }
    for (i = 0; i < nmodes; i++)
        DBG(" mode %x: %ux%ux%u pitch=%u attr=%x win=%x/%x gran=%u size=%u seg=%x lfb=%lx\n",
            modes[i].mode, modes[i].xres, modes[i].yres, modes[i].bpp, modes[i].pitch, modes[i].attr,
            modes[i].winAattr, modes[i].winBattr, modes[i].wingran, modes[i].winsize, modes[i].winAseg,
            modes[i].physbase);
    read_edid();
    return 1;
}

/* ---------------- mode selection ---------------------------------------- */

static BYTE mode_banked_ok(MODEINFO *m)
{
    if (m->attr & 0x40) return 0;                    /* no windowed mode */
    if (!(m->winAattr & 0x01)) return 0;             /* window A missing */
    if (!m->wingran || !m->winsize) return 0;
    if (m->winsize < 64 && (m->winsize & (m->winsize - 1))) return 0;
    if (m->wingran > m->winsize) return 0;
    return 1;
}

static BYTE mode_lfb_ok(MODEINFO *m)
{
    return g.pmode && (m->attr & 0x80) && m->physbase;
}

static void set_vga13(MODEINFO *m)
{
    memset(m, 0, sizeof(*m));
    m->mode = 0x13; m->xres = 320; m->yres = 200; m->bpp = 8; m->Bpp = 1; m->pf = PF_8;
    m->pitch = m->lpitch = 320; m->memmodel = 4;
}

static void set_dispi(MODEINFO *m, WORD w, WORD h, WORD bpp)
{
    memset(m, 0, sizeof(*m));
    if (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32) bpp = 16;
    w &= ~7;                              /* DISPI hardware needs multiples of 8 */
    m->mode = 0xFFFF; m->xres = w; m->yres = h; m->bpp = (BYTE)bpp;
    m->Bpp = (BYTE)((bpp + 7) >> 3);
    m->pf = bpp == 8 ? PF_8 : bpp == 15 ? PF_555 : bpp == 16 ? PF_565 : bpp == 24 ? PF_24 : PF_32;
    m->pitch = m->lpitch = w * m->Bpp;
    m->memmodel = bpp == 8 ? 4 : 6;
    m->physbase = dispi_lfb;
    m->attr = 0x9B | (dispi_lfb ? 0x80 : 0);
    m->winAattr = 7; m->wingran = 64; m->winsize = 64; m->winAseg = 0xA000;
    if (bpp == 15) { m->rsize = 5; m->rpos = 10; m->gsize = 5; m->gpos = 5; m->bsize = 5; }
    if (bpp == 16) { m->rsize = 5; m->rpos = 11; m->gsize = 6; m->gpos = 5; m->bsize = 5; }
    if (bpp >= 24) { m->rsize = 8; m->rpos = 16; m->gsize = 8; m->gpos = 8; m->bsize = 8; }
}

/*
 * choose the mode from SYSTEM.INI [WINVBE] settings:
 *   Mode=0x117   explicit VBE mode
 *   Width/Height/Depth  desired (0 = auto: EDID preferred, else 1024x768)
 *   Access=0 auto, 1 LFB only, 2 banked only
 *   Dispi=0/1    allow Bochs/QEMU/VirtualBox DISPI custom resolutions
 */
int vbe_select_mode(void)
{
    WORD i, best = 0xFFFF;
    DWORD bestscore = 0xFFFFFFFFUL;
    WORD w = g.req_w, h = g.req_h, d = g.req_bpp;

    if (!w || !h) {
        if (g.have_edid && g.edid_w >= 640 && g.edid_h >= 400) { w = g.edid_w; h = g.edid_h; }
        else { w = 1024; h = 768; }
    }
    if (!d) d = 16;

    /* explicit mode number */
    if (g.req_mode == 0x13) { set_vga13(&g.mi); return 1; }
    if (g.req_mode) {
        for (i = 0; i < nmodes; i++)
            if ((modes[i].mode & 0x1FF) == (g.req_mode & 0x1FF)) { best = i; break; }
    }

    if (best == 0xFFFF) {
        for (i = 0; i < nmodes; i++) {
            MODEINFO *m = &modes[i];
            DWORD score;
            BYTE lin = mode_lfb_ok(m), ban = mode_banked_ok(m);
            if (!lin && !ban) continue;
            if (g.req_access == 1 && !lin) continue;
            if (g.req_access == 2 && !ban) continue;
            if (m->xres > 4096 || m->yres > 4096) continue;
            if ((DWORD)m->lpitch * m->yres > (DWORD)g.totalmem64k * 65536UL && g.totalmem64k) continue;
            score = (DWORD)(m->xres > w ? m->xres - w : w - m->xres) * 4 +
                    (DWORD)(m->yres > h ? m->yres - h : h - m->yres) * 4;
            /* depth difference: big penalty */
            {
                WORD dd = m->bpp > d ? m->bpp - d : d - m->bpp;
                score += (DWORD)dd * 64;
                if (d >= 24 && m->bpp == 32 && d == 24) score -= 63;   /* 32 ok for 24 */
                if (d == 32 && m->bpp == 24) score += 0;
            }
            if (!lin) score += 1;     /* prefer linear */
            if (score < bestscore) { bestscore = score; best = i; }
        }
    }

    /* VMware SVGA II: any resolution, 32 bits per pixel */
    if (svga_port && g.req_access != 2 && !g.req_mode && (d == 32 || d == 24) && w >= 320 && h >= 200 && (!svga_maxw || w <= svga_maxw) &&
        (!svga_maxh || h <= svga_maxh) && (DWORD)w * 4 * h <= svga_vram) {
        set_svga(&g.mi, w, h);
        DBG("select: SVGA II %ux%ux32\n", w, h);
        return 1;
    }

    /* custom DISPI resolution if allowed and VBE has no exact match */
    if (dispi_id && g.use_dispi && (best == 0xFFFF || modes[best].xres != w || modes[best].yres != h ||
                                     (modes[best].bpp != d && !(d == 24 && modes[best].bpp == 32)))) {
        DWORD need = (DWORD)w * ((d + 7) >> 3) * h;
        if (need <= (DWORD)dispi_mem64k * 65536UL && w >= 320 && h >= 200 && w <= 4096 && h <= 4096) {
            set_dispi(&g.mi, w, h, d);
            DBG("select: DISPI %ux%ux%u\n", w, h, d);
            return 1;
        }
    }

    if (best == 0xFFFF) {
        DBG("select: no VBE mode, using VGA 13h\n");
        set_vga13(&g.mi);
        return 1;
    }
    g.mi = modes[best];
    DBG("select: mode %x %ux%ux%u (wanted %ux%ux%u)\n", g.mi.mode, g.mi.xres, g.mi.yres, g.mi.bpp, w, h, d);
    return 1;
}

/* ---------------- refresh rate (VBE 3.0 CRTC) -------------------------- */

typedef struct {
    WORD htotal, hss, hse, vtotal, vss, vse;
    BYTE flags;
    DWORD pclk;
    WORD refresh;
    BYTE res[40];
} CRTCINFO;

/* VESA CVT 1.2 timing (standard and reduced blanking), integer math */
static void calc_cvt(CRTCINFO *c, WORD w, WORD h, WORD hz, BYTE reduced)
{
    WORD vsync;
    DWORD htotal, vtotal, pclk;
    w &= ~7;
    if ((DWORD)w * 3 == (DWORD)h * 4) vsync = 4;
    else if ((DWORD)w * 9 == (DWORD)h * 16) vsync = 5;
    else if ((DWORD)w * 10 == (DWORD)h * 16) vsync = 6;
    else if ((DWORD)w * 4 == (DWORD)h * 5 || (DWORD)w * 9 == (DWORD)h * 15) vsync = 7;
    else vsync = 10;
    if (reduced) {
        DWORD hpe_ns = (1000000000UL / hz - 460000UL) / h;
        WORD vbi = (WORD)(460000UL / hpe_ns) + 1;
        if (vbi < 3 + vsync + 6) vbi = 3 + vsync + 6;
        htotal = w + 160;
        vtotal = h + vbi;
        c->hss = w + 48; c->hse = w + 48 + 32;
        c->vss = h + 3;  c->vse = h + 3 + vsync;
        c->flags = 0x08;                     /* +hsync, -vsync */
        pclk = (DWORD)hz * vtotal * htotal;
        pclk = (pclk / 250000UL) * 250000UL;
    } else {
        DWORD hpe_ns = (1000000000UL / hz - 550000UL) / (h + 3);
        WORD vsbp = (WORD)(550000UL / hpe_ns) + 1;
        DWORD idc_m, hblank, hsync, khz;
        if (vsbp < vsync + 6) vsbp = vsync + 6;
        vtotal = h + vsbp + 3;
        idc_m = 30000UL - (3UL * hpe_ns) / 10;
        if ((long)idc_m < 20000L) idc_m = 20000UL;
        hblank = ((DWORD)w * idc_m / (100000UL - idc_m)) / 16 * 16;
        htotal = w + hblank;
        khz = htotal * 1000000UL / hpe_ns;              /* kHz */
        pclk = (khz / 250UL) * 250000UL;
        hsync = ((htotal * 8 / 100 + 4) / 8) * 8;
        c->hse = w + (WORD)(hblank / 2);                /* end of sync = start of back porch */
        c->hss = c->hse - (WORD)hsync;
        c->vss = h + 3; c->vse = h + 3 + vsync;
        c->flags = 0x04;                     /* -hsync, +vsync */
    }
    c->htotal = (WORD)htotal;
    c->vtotal = (WORD)vtotal;
    c->pclk = pclk;
    c->refresh = (WORD)(pclk / (htotal * vtotal / 100));
}

static int fill_crtc(CRTCINFO *c)
{
    WORD hz = g.req_hz;
    memset(c, 0, sizeof(*c));
    /* exact EDID native timing if refresh matches native */
    if (g.have_edid && g.mi.xres == g.edid_w && g.mi.yres == g.edid_h &&
        (hz == 0 || hz == g.edid_hz || hz == 1)) {
        BYTE *d = g.edid + 54;
        WORD clk = d[0] | (d[1] << 8);
        WORD ha = d[2] | ((d[4] & 0xF0) << 4), hb = d[3] | ((d[4] & 0x0F) << 8);
        WORD va = d[5] | ((d[7] & 0xF0) << 4), vb = d[6] | ((d[7] & 0x0F) << 8);
        WORD hso = d[8] | ((d[11] & 0xC0) << 2), hsw = d[9] | ((d[11] & 0x30) << 4);
        WORD vso = (d[10] >> 4) | ((d[11] & 0x0C) << 2), vsw = (d[10] & 0x0F) | ((d[11] & 0x03) << 4);
        if (hz == 0) return 0;
        c->htotal = ha + hb; c->hss = ha + hso; c->hse = ha + hso + hsw;
        c->vtotal = va + vb; c->vss = va + vso; c->vse = va + vso + vsw;
        c->flags = 0;
        if ((d[17] & 0x18) == 0x18) {
            if (!(d[17] & 0x02)) c->flags |= 0x04;
            if (!(d[17] & 0x04)) c->flags |= 0x08;
        }
        c->pclk = (DWORD)clk * 10000UL;
        c->refresh = (WORD)(c->pclk / ((DWORD)c->htotal * c->vtotal / 100));
        return 1;
    }
    if (hz < 50 || hz > 240) return 0;
    calc_cvt(c, g.mi.xres, g.mi.yres, hz,
             (BYTE)(g.ini_timing == 2 ? (g.have_edid && (g.edid[20] & 0x80)) : g.ini_timing == 1));
    return 1;
}

/* ---------------- mode set ------------------------------------------- */

#define VGA_RESERVE 0x40000UL     /* VGA planes 0-3 (text, font) */

/* Start the visible screen behind the first 256 KB of video memory. The
   VGA text and font planes live there: a full-screen MS-DOS box whose
   state the VDD keeps (Windows 9x keeps the font plane in place) would
   otherwise find Windows' pixels in its font after an Alt+Tab. */
static void vga_reserve(void)
{
    DWORD lines, need;
    g.fbofs = 0;
    if (g.svga) { g.fbofs = svga_rd(SVGA_REG_FB_OFFSET); return; }
    if (g.access == ACC_VGA13 || !g.pitch || !g.ini_vgareserve) return;
    lines = (VGA_RESERVE + g.pitch - 1) / g.pitch;
    need = (lines + g.yres) * g.pitch;
    if (!g.totalmem64k || need > (DWORD)g.totalmem64k * 65536UL) return;
    if (g.access == ACC_LFB && (DWORD)g.lfbtiles * 65536UL < need) return;
    if (g.mi.mode != 0xFFFF) {
        rclear(); rmr.eax = 0x4F07; rmr.ebx = 0; rmr.ecx = 0; rmr.edx = lines;
        if (!int10(&rmr) || (WORD)rmr.eax != 0x004F) return;
    }
    if (dispi_id) {         /* Bochs/QEMU/VirtualBox: check (or set) it directly */
        if (dispi_rd(9) != (WORD)lines) dispi_wr(9, (WORD)lines);
        DBG("display start: line %u (DISPI y offset %u)\n", (WORD)lines, dispi_rd(9));
        if (dispi_rd(9) != (WORD)lines) { dispi_wr(9, 0); return; }
    }
    g.fbofs = lines * g.pitch;
}

/* selector (or real mode segment) for a VBE window segment */
static WORD win_sel(WORD seg)
{
    static WORD cache_seg, cache_sel;
    WORD s;
    if (!seg) seg = 0xA000;
    if (!g.pmode) return seg;
    if (seg == 0xA000) return g.a000sel;
    if (seg == cache_seg && cache_sel) return cache_sel;
    s = sel_alloc(1);
    if (!s) return g.a000sel;
    sel_setlimit(s, 0xFFFFUL);
    sel_setbase(s, (DWORD)seg << 4);
    cache_seg = seg; cache_sel = s;
    return s;
}

static int setup_lfb(void)
{
    DWORD lin, size = g.fbsize;
    WORD tiles, i;
    if (!g.pmode || !g.mi.physbase) return 0;
    /* room for the screen moved behind the VGA planes (vga_reserve) */
    size += VGA_RESERVE + 0x10000UL;
    if (g.totalmem64k && size > (DWORD)g.totalmem64k * 65536UL) size = (DWORD)g.totalmem64k * 65536UL;
    if (size < g.fbsize) size = g.fbsize;
    size = (size + 0xFFFFUL) & 0xFFFF0000UL;
    if (g.lfbsel && g.lfblin) return 1;      /* already mapped (re-enable) */
    if (g.lfb_state == 2) return 0;
    {   /* SYSTEM.INI LfbAddr=0xE0000000 overrides the address reported by the BIOS */
        char b[16];
        get_ini_str("LfbAddr", "", b, sizeof(b));
        if (b[0]) {
            DWORD a = 0;
            char *p = b;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
            for (; *p; p++) {
                char c = *p;
                if (c >= '0' && c <= '9') a = (a << 4) | (c - '0');
                else if (c >= 'a' && c <= 'f') a = (a << 4) | (c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') a = (a << 4) | (c - 'A' + 10);
                else break;
            }
            if (a) g.mi.physbase = a;
        }
    }
    if (g.lfb_check && g.mi.mode != 0xFFFF && lfb_pci_check(g.mi.physbase) == 0) {
        DBG("LFB: %lx is not in a display BAR below 4 GB - using the VGA window\n", g.mi.physbase);
        g.lfb_state = 2;
        return 0;
    }
    lin = map_phys(g.mi.physbase, size);
    DBG("LFB: phys=%lx size=%lx lin=%lx\n", g.mi.physbase, size, lin);
    if (!lin) return 0;
    tiles = (WORD)(size >> 16);
    g.lfbsel = sel_alloc(tiles);
    if (!g.lfbsel) return 0;
    for (i = 0; i < tiles; i++) {
        WORD s = g.lfbsel + i * dpmi_selinc;
        if (!sel_setlimit(s, 0xFFFFUL) || !sel_setbase(s, lin + ((DWORD)i << 16))) {
            DBG("LFB: selector setup failed at tile %u\n", i);
            sel_free(g.lfbsel, tiles);
            g.lfbsel = 0;
            return 0;
        }
    }
    g.lfbtiles = tiles;
    g.lfblin = lin;
    g.lfbinc = dpmi_selinc;
    /* write combining: the CPU merges frame buffer writes into bursts
       (PCI/AGP/PCIe writes become several times faster) */
    if (g.ini_wc && g.cpu386 && (g.enhanced || g.win9x)) {
        g.wc = (BYTE)vxd_wc(lin, size, g.mi.physbase);
        DBG("LFB write combining: %u\n", g.wc);
    }
    return 1;
}

static void dispi_set(void)
{
    dispi_wr(4, 0);
    dispi_wr(1, g.mi.xres);
    dispi_wr(2, g.mi.yres);
    dispi_wr(3, g.mi.bpp);
    dispi_wr(6, g.mi.xres);
    dispi_wr(8, 0);
    dispi_wr(9, 0);
    dispi_wr(4, 0x01 | (g.access == ACC_LFB ? 0x40 : 0) | (g.dacbits == 8 ? 0x20 : 0));
    g.pitch = dispi_rd(6) * g.Bpp;
    if (!g.pitch) g.pitch = g.mi.xres * g.Bpp;
}

/*
 * Cross-check the linear frame buffer with the VGA window at A000h after the
 * mode is set: a pattern written through bank 0 of the window must appear at
 * offset 0 of the LFB. The LFB is only read, never written, so an invalid
 * address cannot corrupt memory. If the window itself does not work in this
 * mode the test is inconclusive and the LFB is kept.
 * returns 1 = LFB ok / unknown, 0 = LFB does not show video memory
 */
static int lfb_verify(void)
{
    static const BYTE pat[8] = { 0x5A, 0xC3, 0x96, 0x0F, 0xA5, 0x3C, 0x69, 0xF0 };
    BYTE save[8];
    LPBYTE win, lfb;
    WORD i;
    int same_win = 1, same_lfb = 1;
    if (!g.lfb_check || g.mi.mode == 0xFFFF) return 1;
    if (!mode_banked_ok(&g.mi) || g.mi.winAseg != 0xA000 || (g.mi.winAattr & 7) != 7) return 1;
    rclear(); rmr.eax = 0x4F05; rmr.ebx = 0; rmr.edx = 0;
    if (!int10(&rmr) || (WORD)rmr.eax != 0x004F) return 1;
    win = (LPBYTE)MK_FP(g.pmode ? g.a000sel : 0xA000, 0);
    lfb = (LPBYTE)MK_FP(g.lfbsel, 0);
    for (i = 0; i < 8; i++) save[i] = win[i];
    for (i = 0; i < 8; i++) win[i] = pat[i];
    for (i = 0; i < 8; i++) if (win[i] != pat[i]) same_win = 0;
    for (i = 0; i < 8; i++) if (lfb[i] != pat[i]) same_lfb = 0;
    for (i = 0; i < 8; i++) win[i] = save[i];
    DBG("LFB check: window %s, lfb %s\n", same_win ? "ok" : "n/a", same_lfb ? "ok" : "mismatch");
    if (!same_win) return 1;
    return same_lfb;
}

int vbe_set_mode(void)
{
    CRTCINFO FAR *c;
    WORD m = g.mi.mode;
    BYTE want_lfb;

    g.xres = g.mi.xres; g.yres = g.mi.yres;
    g.bpp = g.mi.bpp; g.Bpp = g.mi.Bpp; g.pf = g.mi.pf;
    g.palettized = (g.Bpp == 1);
    g.curbank = 0xFFFF;

    if (m == 0x13) {
        rclear(); rmr.eax = 0x0013; int10(&rmr);
        g.access = ACC_VGA13; g.pitch = 320; g.fbsize = 64000UL;
        g.winsel = g.a000sel; g.dacbits = 6;
        return 1;
    }

    want_lfb = mode_lfb_ok(&g.mi) && g.req_access != 2;
    if (!mode_banked_ok(&g.mi) && !want_lfb) return 0;
    g.fbsize = (DWORD)g.mi.lpitch * g.yres;
    if ((DWORD)g.mi.pitch * g.yres > g.fbsize) g.fbsize = (DWORD)g.mi.pitch * g.yres;
    if (want_lfb && !setup_lfb()) want_lfb = 0;
    if (!want_lfb && !mode_banked_ok(&g.mi)) return 0;
    g.access = want_lfb ? ACC_LFB : ACC_BANKED;

    g.svga = 0;
    if (m == 0xFFFE) {
        if (g.access != ACC_LFB) return 0;
        g.svga = 1;
        svga_set();
    } else if (m == 0xFFFF) {
        /* Bochs DISPI custom mode: put VGA in a graphics state first */
        rclear(); rmr.eax = 0x0013; int10(&rmr);
        g.dacbits = 6;
        dispi_set();
    } else {
        if (g.access == ACC_LFB && !g.lfb_state) {
            /* first time: set the mode with the VGA window and check that the
               LFB address really shows the same video memory */
            g.lfb_state = 1;
            rclear(); rmr.eax = 0x4F02; rmr.ebx = m;
            if (int10(&rmr) && (WORD)rmr.eax == 0x004F && !lfb_verify()) {
                DBG("LFB at %lx does not show video memory - using the VGA window\n", g.mi.physbase);
                g.lfb_state = 2;
                g.access = ACC_BANKED;
            }
        }
        g.refresh_set = 0;
        c = (CRTCINFO FAR *)rm_ptr(g.dosbuf_seg, 0x300);
        if (g.vbever >= 0x300 && g.req_hz) {
            CRTCINFO ci;
            if (fill_crtc(&ci)) {
                /* ask BIOS for closest pixel clock */
                rclear(); rmr.eax = 0x4F0B; rmr.ebx = 0; rmr.ecx = ci.pclk; rmr.edx = m;
                if (int10(&rmr) && (WORD)rmr.eax == 0x004F && rmr.ecx) {
                    ci.pclk = rmr.ecx;
                    ci.refresh = (WORD)(ci.pclk / ((DWORD)ci.htotal * ci.vtotal / 100));
                }
                far_memcpy(c, &ci, sizeof(ci));
                rclear();
                rmr.eax = 0x4F02; rmr.ebx = m | 0x0800 | (g.access == ACC_LFB ? 0x4000 : 0);
                rmr.es = g.dosbuf_seg; rmr.edi = 0x300;
                if (int10(&rmr) && (WORD)rmr.eax == 0x004F) {
                    g.refresh_set = 1;
                    g.refresh_cur = ci.refresh;
                    DBG("modeset %x with CRTC %ux%u pclk=%lu refresh=%u.%02u\n", m, ci.htotal, ci.vtotal,
                        ci.pclk, ci.refresh / 100, ci.refresh % 100);
                }
            }
        }
        if (!g.refresh_set) {
            rclear();
            rmr.eax = 0x4F02; rmr.ebx = m | (g.access == ACC_LFB ? 0x4000 : 0);
            if (!int10(&rmr) || (WORD)rmr.eax != 0x004F) {
                DBG("modeset %x failed ax=%x\n", m, (WORD)rmr.eax);
                if (g.access == ACC_LFB && mode_banked_ok(&g.mi)) {
                    g.access = ACC_BANKED;
                    rclear(); rmr.eax = 0x4F02; rmr.ebx = m;
                    if (!int10(&rmr) || (WORD)rmr.eax != 0x004F) return 0;
                } else
                    return 0;
            }
        }
        /* actual pitch */
        rclear(); rmr.eax = 0x4F06; rmr.ebx = 1;
        if (int10(&rmr) && (WORD)rmr.eax == 0x004F && (WORD)rmr.ebx)
            g.pitch = (WORD)rmr.ebx;
        else
            g.pitch = (g.access == ACC_LFB) ? g.mi.lpitch : g.mi.pitch;
        if (g.access == ACC_LFB && g.vbever >= 0x300) g.pitch = g.mi.lpitch;
        /* DAC width */
        g.dacbits = 6;
        if (g.palettized && (g.vbecaps_lo & 1) && g.ini_dac8) {
            rclear(); rmr.eax = 0x4F08; rmr.ebx = 0x0800;
            if (int10(&rmr) && (WORD)rmr.eax == 0x004F)
                g.dacbits = (BYTE)(rmr.ebx >> 8);
            if (g.dacbits < 6 || g.dacbits > 8) g.dacbits = 6;
        }
    }

    if (g.access == ACC_BANKED) {
        WORD wkb = g.mi.winsize >= 64 ? 64 : g.mi.winsize;
        BYTE ra = (g.mi.winAattr & 0x03) == 0x03, wa = (g.mi.winAattr & 0x05) == 0x05;
        BYTE rb = (g.mi.winBattr & 0x03) == 0x03, wb = (g.mi.winBattr & 0x05) == 0x05;
        WORD rseg = g.mi.winAseg, wseg = g.mi.winAseg;
        /* separate read and write windows (window A write-only or
           read-only): VBE 1.2 cards of the Tseng/ATI generation */
        if (!ra && rb && g.mi.winBseg) rseg = g.mi.winBseg;
        if (!wa && wb && g.mi.winBseg) wseg = g.mi.winBseg;
        g.winsel = win_sel(wseg);
        g.winrsel = rseg == wseg ? g.winsel : win_sel(rseg);
        /* window size: the bank changes every winsize KB (32 KB windows
           exist, e.g. two windows sharing A000h-AFFFh) */
        g.wshift = 10;
        while ((1U << (g.wshift - 10)) < wkb && g.wshift < 16) g.wshift++;
        g.bankmul = (WORD)(1U << (g.wshift - 10)) / g.mi.wingran;
        if (!g.bankmul) g.bankmul = 1;
        g.use_winfunc = (!g.pmode && g.mi.winfunc) ? 1 : 0;
        g.winread_ok = ra;
        g.winwrite_ok = wa;
        DBG("window: A %x/%x B %x/%x size %u KB gran %u KB, read %x write %x\n", g.mi.winAseg, g.mi.winAattr,
            g.mi.winBseg, g.mi.winBattr, wkb, g.mi.wingran, rseg, wseg);
    }
    vga_reserve();
    DBG("mode set: %ux%ux%u pitch=%u access=%u dac=%u fb=%lx ofs=%lx\n", g.xres, g.yres, g.bpp, g.pitch, g.access,
        g.dacbits, g.fbsize, g.fbofs);
    return 1;
}

/* Windows 9x: an MS-DOS box runs the video BIOS on the real hardware (the
   VGA VDD only virtualizes the standard VGA registers), which switches the
   Bochs/QEMU/VirtualBox DISPI interface off. The DISPI registers of our
   mode are remembered and written back - with the "keep video memory" flag -
   when Windows owns the screen again (VGA graphics mode) but DISPI is off. */
static WORD dispi_save[10];
static BYTE dispi_saved;

void vbe_dispi_snapshot(void)
{
    WORD i;
    dispi_saved = 0;
    if (!dispi_id) return;
    for (i = 1; i < 10; i++) dispi_save[i] = dispi_rd(i);
    dispi_saved = (dispi_save[4] & 1) != 0;
}

int vbe_dispi_repair(void)
{
    BYTE old, gr6;
    WORD i;
#ifdef NODISPIFIX
    return 0;                               /* test: behave as on real hardware */
#endif
    if (!dispi_saved || (dispi_rd(4) & 1)) return 0;
    old = inp(0x3CE); outp(0x3CE, 6); gr6 = inp(0x3CF); outp(0x3CE, old);
    if (!(gr6 & 1)) return 0;               /* text mode: a full-screen DOS box */
    for (i = 1; i < 4; i++) dispi_wr(i, dispi_save[i]);
    dispi_wr(4, dispi_save[4] | 0x80);      /* VBE_DISPI_NOCLEARMEM */
    /* enabling resets the display start: virtual size and offsets after it */
    for (i = 5; i < 10; i++) if (i != 7) dispi_wr(i, dispi_save[i]);
    g.curbank = 0xFFFF;
    if (g.palettized) {
        BYTE shift = 8 - g.dacbits;
        outp(0x3C8, 0);
        for (i = 0; i < 256; i++) {
            outp(0x3C9, g.pal[i * 4 + 0] >> shift);
            outp(0x3C9, g.pal[i * 4 + 1] >> shift);
            outp(0x3C9, g.pal[i * 4 + 2] >> shift);
        }
    }
    return 1;
}

void vbe_text_mode(void)
{
    svga_off();
    if (g.mi.mode == 0xFFFF)
        dispi_wr(4, 0);
    if (g.dacbits == 8) {
        rclear(); rmr.eax = 0x4F08; rmr.ebx = 0x0600; int10(&rmr);
    }
    rclear();
    rmr.eax = 0x0003;
    int10(&rmr);
    /* the video BIOS ends its attribute controller set-up with the index
       0x20 written, i.e. the flip-flop in data state; whoever restores a
       DOS screen next (the standard mode grabber) expects the index state
       and would blank the screen (palette address source off) */
    inp(0x3DA);
    inp(0x3BA);
    DBG("text mode: DISPI %x\n", dispi_id ? dispi_rd(4) : 0xFFFF);
}

/* Windows 3.x enhanced mode, INT 2Fh 4001h: the VDD is about to give the
   screen to a DOS box and only restores the standard VGA registers, so the
   extended (VBE) mode must be left without disturbing the VDD: no memory
   clear, no BIOS data area change of the DOS box (mode 83h, no clear). */
void vbe_leave_bg(void)
{
#ifndef TESTBG10
    if (dispi_id && (dispi_rd(4) & 1)) {  /* Bochs/QEMU DISPI: just switch it off */
        dispi_wr(4, 0);
        return;
    }
#endif
    if (g.dacbits == 8) {
        rclear(); rmr.eax = 0x4F08; rmr.ebx = 0x0600; int10(&rmr);
    }
    rclear();
    rmr.eax = 0x0083;
    int10(&rmr);
}

/* Windows 3.x enhanced mode: when Windows gets the screen back from a
   full-screen DOS box, the standard VDD (VDDVGA) reloads the VGA registers
   and the DAC it saved long ago - it knows nothing of the VBE mode. A
   snapshot of the standard registers (and some DAC entries) taken after
   our mode set tells whether that happened. */
static BYTE vga_snap[64];
static BYTE vga_snap_ok;

static void vga_read(BYTE *p)
{
    WORD i, crt = (inp(0x3CC) & 1) ? 0x3D4 : 0x3B4;
    BYTE o;
    *p++ = inp(0x3CC);
    o = inp(0x3C4); for (i = 1; i < 5; i++) { outp(0x3C4, i); *p++ = inp(0x3C5); } outp(0x3C4, o);
    o = inp(0x3CE); for (i = 5; i < 7; i++) { outp(0x3CE, i); *p++ = inp(0x3CF); } outp(0x3CE, o);
    o = inp(crt);
    for (i = 0; i < 0x19; i++) { outp(crt, i); *p++ = inp(crt + 1); }
    outp(crt, o);
    inp(crt + 6); outp(0x3C0, 0x30); *p++ = inp(0x3C1); inp(crt + 6); outp(0x3C0, 0x20); inp(crt + 6);
    if (g.palettized) {
        static const BYTE idx[4] = { 7, 8, 248, 255 };
        for (i = 0; i < 4; i++) {
            outp(0x3C7, idx[i]); *p++ = inp(0x3C9); *p++ = inp(0x3C9); *p++ = inp(0x3C9);
        }
    }
}

void vbe_dispi_off(void)
{
    if (dispi_id && dispi_saved && (dispi_rd(4) & 1)) dispi_wr(4, 0);
}

void vga_snapshot(void)
{
    vga_read(vga_snap);
    vga_snap_ok = 1;
}

/* write the standard VGA registers of the snapshot back (what the VDD
   overwrote) - no BIOS call, the extended/VBE state is left alone */
int vga_restore(void)
{
    BYTE *p = vga_snap;
    WORD i, crt;
    if (!vga_snap_ok) return 0;
    outp(0x3C2, p[0]);
    crt = (p[0] & 1) ? 0x3D4 : 0x3B4;
    outpw(0x3C4, 0x0100);                       /* synchronous reset */
    for (i = 1; i < 5; i++) outpw(0x3C4, i | ((WORD)p[i] << 8));
    outpw(0x3C4, 0x0300);
    for (i = 0; i < 2; i++) outpw(0x3CE, (5 + i) | ((WORD)p[5 + i] << 8));
    outp(crt, 0x11); outp(crt + 1, inp(crt + 1) & 0x7F);   /* unlock CR0-7 */
    for (i = 0; i < 0x19; i++) if (i != 0x11) outpw(crt, i | ((WORD)p[7 + i] << 8));
    outpw(crt, 0x11 | ((WORD)p[7 + 0x11] << 8));
    inp(crt + 6); outp(0x3C0, 0x30); outp(0x3C0, p[7 + 0x19]); inp(crt + 6); outp(0x3C0, 0x20); inp(crt + 6);
    if (dispi_id && !(dispi_rd(4) & 1) && dispi_saved) vbe_dispi_repair();
    if (g.palettized) {
        BYTE shift = 8 - g.dacbits;
        outp(0x3C8, 0);
        for (i = 0; i < 256; i++) {
            outp(0x3C9, g.pal[i * 4 + 0] >> shift);
            outp(0x3C9, g.pal[i * 4 + 1] >> shift);
            outp(0x3C9, g.pal[i * 4 + 2] >> shift);
        }
    }
    return !vga_changed();
}

int vga_changed(void)
{
    BYTE now[64];
    WORD i;
    if (!vga_snap_ok) return 0;
    memset(now, 0, sizeof(now));
    vga_read(now);
    for (i = 0; i < sizeof(now); i++)
        if (now[i] != vga_snap[i]) {
            WORD j;
            for (j = i; j < sizeof(now); j++)
                if (now[j] != vga_snap[j]) DBG("vga[%u]: %x -> %x\n", j, vga_snap[j], now[j]);
            return 1;
        }
    if (dispi_id && !(dispi_rd(4) & 1)) return 1;
    return 0;
}

/* ---------------- banking ------------------------------------------- */

void vbe_set_bank(WORD bank)
{
    WORD pos;
    if (bank == g.curbank) return;
    g.curbank = bank;
    pos = bank * g.bankmul;
    if (g.mi.mode == 0xFFFF) {
        dispi_wr(5, bank);
        return;
    }
    if (g.use_winfunc) {
        rm_winfunc(pos);
        return;
    }
    rclear();
    rmr.eax = 0x4F05; rmr.ebx = 0; rmr.edx = pos;
    int10(&rmr);
    if (!(g.winread_ok && g.winwrite_ok) && (g.mi.winBattr & 1)) {
        rclear();
        rmr.eax = 0x4F05; rmr.ebx = 1; rmr.edx = pos;
        int10(&rmr);
    }
}

/* ---------------- DAC -------------------------------------------- */

void vbe_set_dac(WORD start, WORD count, const BYTE *rgbx)
{
    WORD i;
    BYTE shift = 8 - g.dacbits;
    if (!g.palettized) return;
    if (g.mi.mode == 0x13 || g.mi.mode == 0xFFFF || !(g.vbecaps_lo & 0x02)) {
        /* VGA compatible DAC */
        outp(0x3C8, (BYTE)start);
        for (i = 0; i < count; i++) {
            outp(0x3C9, rgbx[i * 4 + 0] >> shift);
            outp(0x3C9, rgbx[i * 4 + 1] >> shift);
            outp(0x3C9, rgbx[i * 4 + 2] >> shift);
        }
        return;
    }
    /* non-VGA DAC: VBE 4F09, entries B,G,R,align */
    {
        LPBYTE p = rm_ptr(g.dosbuf_seg, 0x400);
        for (i = 0; i < count; i++) {
            p[i * 4 + 0] = rgbx[i * 4 + 2] >> shift;
            p[i * 4 + 1] = rgbx[i * 4 + 1] >> shift;
            p[i * 4 + 2] = rgbx[i * 4 + 0] >> shift;
            p[i * 4 + 3] = 0;
        }
        rclear();
        rmr.eax = 0x4F09; rmr.ebx = 0; rmr.ecx = count; rmr.edx = start;
        rmr.es = g.dosbuf_seg; rmr.edi = 0x400;
        int10(&rmr);
    }
}

/* ---------------- power management -------------------------------- */
#ifndef OLDWIN
static BYTE pm_caps, pm_probed;

/* VBE/PM 4F10h BL=0: BH = power states the BIOS can set (bit 0 stand by,
   bit 1 suspend, bit 2 off); bit 7 = VBE/PM present */
WORD vbe_pm_caps(void)
{
    if (!pm_probed && g.vbever) {
        pm_probed = 1;
        rclear(); rmr.eax = 0x4F10; rmr.ebx = 0;
        if (int10(&rmr) && (WORD)rmr.eax == 0x004F)
            pm_caps = 0x80 | ((BYTE)(rmr.ebx >> 8) & 0x07);
        DBG("VBE/PM: %x\n", pm_caps);
    }
    return pm_caps;
}

/* monitor power: 0 on, 1 stand by, 2 suspend, 4 off.  The picture is blanked
   with the sequencer "screen off" bit and the attribute controller (every
   VGA compatible adapter, also emulated ones); VBE/PM signals the monitor
   to save power (DPMS). */
void vbe_dpms(WORD state)
{
    BYTE caps = (BYTE)vbe_pm_caps(), b = (BYTE)state, idx, sr1;
    if (state && (caps & 0x80)) {
        if (b >= 4 && !(caps & 4)) b = 2;
        if (b == 2 && !(caps & 2)) b = 1;
        if (b == 1 && !(caps & 1)) b = (caps & 2) ? 2 : (caps & 4) ? 4 : 0;
    }
    if (!state || b) {
        idx = inp(0x3C4);
        outp(0x3C4, 1); sr1 = inp(0x3C5);
        outp(0x3C5, state ? (sr1 | 0x20) : (sr1 & ~0x20));
        outp(0x3C4, idx);
        /* attribute controller palette address source: off = blank */
        inp(0x3DA);
        outp(0x3C0, state ? 0x00 : 0x20);
        inp(0x3DA);
    }
    if ((caps & 0x80) && (!state || b)) {
        rclear(); rmr.eax = 0x4F10; rmr.ebx = 0x0001 | ((WORD)b << 8);
        int10(&rmr);
        DBG("DPMS %u -> %x (ax=%x)\n", state, b, (WORD)rmr.eax);
    }
    g.dpms = (BYTE)state;
}

static int dpmi_callfar(RMREGS FAR *r);
#pragma aux dpmi_callfar = \
    "push es" \
    "mov es,dx" \
    "mov ax,0301h" \
    "xor bh,bh" \
    "xor cx,cx" \
    "int 31h" \
    "sbb ax,ax" \
    "pop es" \
    parm [dx di] value [ax] modify [bx cx];

/* after a resume: does the adapter still show our mode?  Then no BIOS
   mode set is done - in Windows 9x a mode set through the BIOS in the
   system VM upsets the text of windowed MS-DOS boxes */
int vbe_mode_intact(void)
{
    BYTE idx, gr6;
    if (g.svga)
        return svga_rd(SVGA_REG_ENABLE) && (WORD)svga_rd(SVGA_REG_WIDTH) == g.xres &&
               (WORD)svga_rd(SVGA_REG_HEIGHT) == g.yres;
    idx = inp(0x3CE); outp(0x3CE, 6); gr6 = inp(0x3CF); outp(0x3CE, idx);
    DBG("mode check: GR6 %x DISPI %x %ux%u\n", gr6, dispi_id ? dispi_rd(4) : 0, dispi_id ? dispi_rd(1) : 0,
        dispi_id ? dispi_rd(2) : 0);
    if (!(gr6 & 1)) return 0;                       /* text mode */
    if (dispi_id && (dispi_rd(4) & 1))
        return dispi_rd(1) == g.xres && dispi_rd(2) == g.yres;
    if (dispi_id && g.mi.mode == 0xFFFF) return 0;
    if (g.mi.mode == 0x13) return 1;
    rclear(); rmr.eax = 0x4F03;
    if (!int10(&rmr) || (WORD)rmr.eax != 0x004F) return 0;
    return ((WORD)rmr.ebx & 0x1FF) == (g.mi.mode & 0x1FF);
}

/* the video BIOS data of the BIOS data area (40:49-40:66, 40:84-40:8A and
   40:A8-40:BF: mode, columns, rows, cursor, save pointer, VBE extensions of
   video BIOSes), which DOS boxes share with Windows: a mode set of Windows'
   own after a resume must not change what a windowed DOS box uses */
static const BYTE bda_rng[3][2] = { { 0x49, 0x1E }, { 0x84, 7 }, { 0xA8, 0x18 } };
static BYTE bda_save[0x1E + 7 + 0x18];

static void bda_copy(int save)
{
    LPBYTE p = rm_ptr(0x40, 0);
    WORD i, j, n = 0;
    if (!p) return;
    for (i = 0; i < 3; i++)
        for (j = 0; j < bda_rng[i][1]; j++, n++) {
            if (save) bda_save[n] = p[bda_rng[i][0] + j];
            else p[bda_rng[i][0] + j] = bda_save[n];
        }
}
void vbe_bda_save(void) { bda_copy(1); }
void vbe_bda_restore(void) { bda_copy(0); }

/* the PCI display controller the video BIOS belongs to (VGA decoding on):
   AX value for its initialisation entry (bus << 8 | device << 3 | fn) */
static WORD vga_bdf(void)
{
    WORD bus, dev;
    if (!pci_present()) return 0;
    for (bus = 0; bus < 256; bus++)
        for (dev = 0; dev < 32; dev++) {
            DWORD id = pci_read((BYTE)bus, (BYTE)dev, 0, 0), cls, cmd;
            if ((WORD)id == 0xFFFF || (WORD)id == 0) continue;
            cls = pci_read((BYTE)bus, (BYTE)dev, 0, 0x08) >> 16;
            cmd = pci_read((BYTE)bus, (BYTE)dev, 0, 0x04);
            if ((cls == 0x0300 || cls == 0x0001) && (cmd & 3)) return (bus << 8) | (dev << 3);
        }
    return 0;
}

/* run the video BIOS initialisation again (C000:0003, as the system BIOS
   does at power on): after a resume that lost the adapter state */
int vbe_repost(void)
{
    LPBYTE rom;
    if (!g.pmode || !g.ini_repost) return 0;
    rom = rm_ptr(0xC000, 0);
    if (!rom || rom[0] != 0x55 || rom[1] != 0xAA) return 0;
    rclear();
    rmr.eax = vga_bdf();
    rmr.cs = 0xC000; rmr.ip = 3;
    rmr.ss = rm_stack_seg; rmr.sp = rm_stack_top;
    rmr.flags = 0x0202;
    if (dpmi_callfar((RMREGS FAR *)&rmr)) return 0;
    DBG("video BIOS initialised again (AX=%x)\n", (WORD)vga_bdf());
    return 1;
}
#endif

/* map resources for the selected mode before GDI asks for GDIINFO */
int vbe_prepare(void)
{
    if (g.mi.mode == 0x13) return 1;
    g.fbsize = (DWORD)g.mi.lpitch * g.mi.yres;
    if ((DWORD)g.mi.pitch * g.mi.yres > g.fbsize) g.fbsize = (DWORD)g.mi.pitch * g.mi.yres;
    if (g.req_access != 2 && mode_lfb_ok(&g.mi) && setup_lfb())
        return 1;
    return mode_banked_ok(&g.mi);
}
