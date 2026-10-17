/* WINVBE - VBE / EDID / DISPI query (Win16 real+protected mode, and DOS) */
#include <string.h>
#include <i86.h>
#include <conio.h>
#ifdef __WINDOWS__
#include <windows.h>
#endif
#include "vbeq.h"

typedef struct {
    QDWORD edi, esi, ebp, res, ebx, edx, ecx, eax;
    QWORD flags, es, ds, fs, gs, ip, cs, sp, ss;
} RMR;

static QWORD buf_seg;           /* real mode segment of 2K buffer */
static unsigned char __far *buf; /* far pointer to it */
static int pmode;
static QWORD scratch;

#ifdef __WINDOWS__
static int dpmi_simint(RMR __far *r);
#pragma aux dpmi_simint = "push es" "mov es,dx" "mov ax,0300h" "mov bx,0010h" "xor cx,cx" "int 31h" \
    "sbb ax,ax" "pop es" parm [dx di] value [ax] modify [bx cx];
static QWORD dpmi_alloc(void);
#pragma aux dpmi_alloc = "xor ax,ax" "mov cx,1" "int 31h" "jnc ok" "xor ax,ax" "ok:" value [ax] modify [cx];
static void dpmi_base(QWORD sel, QWORD hi, QWORD lo);
#pragma aux dpmi_base = "mov ax,0007h" "int 31h" parm [bx] [cx] [dx] modify [ax];
static void dpmi_limit(QWORD sel);
#pragma aux dpmi_limit = "mov ax,0008h" "xor cx,cx" "mov dx,0FFFFh" "int 31h" parm [bx] modify [ax cx dx];
#endif

#ifdef OLDWIN
void __far __pascal OLDINT10(QWORD __far *r);
#endif

static int int10(RMR *r)
{
#ifdef __WINDOWS__
    if (pmode) {
        r->ss = buf_seg; r->sp = 0xFFE; r->flags = 0x202;
        r->fs = r->gs = 0;
        return !dpmi_simint((RMR __far *)r);
    }
#endif
#ifdef OLDWIN
    {   /* Windows 1.x/2.x: no C library start-up, INT 10h in cfgold.asm */
        QWORD v[7];
        v[0] = (QWORD)r->eax; v[1] = (QWORD)r->ebx; v[2] = (QWORD)r->ecx; v[3] = (QWORD)r->edx;
        v[4] = (QWORD)r->esi; v[5] = (QWORD)r->edi; v[6] = r->es;
        OLDINT10((QWORD __far *)v);
        r->eax = v[0]; r->ebx = v[1]; r->ecx = v[2]; r->edx = v[3]; r->esi = v[4]; r->edi = v[5];
        r->es = v[6];
        return 1;
    }
#else
    {
        union REGS in;
        struct SREGS sr;
        segread(&sr);
        in.w.ax = (QWORD)r->eax; in.w.bx = (QWORD)r->ebx; in.w.cx = (QWORD)r->ecx;
        in.w.dx = (QWORD)r->edx; in.w.di = (QWORD)r->edi; in.w.si = (QWORD)r->esi;
        sr.es = r->es;
        int86x(0x10, &in, &in, &sr);
        r->eax = in.w.ax; r->ebx = in.w.bx; r->ecx = in.w.cx; r->edx = in.w.dx; r->edi = in.w.di;
        r->es = sr.es;
        return 1;
    }
#endif
}

/* far pointer to real mode seg:off */
static unsigned char __far *rmp(QWORD seg, QWORD off)
{
    if (seg == buf_seg) return buf + off;
#ifdef __WINDOWS__
    if (pmode) {
        if (!scratch) { scratch = dpmi_alloc(); if (scratch) dpmi_limit(scratch); }
        if (!scratch) return 0;
        {
            QDWORD lin = ((QDWORD)seg << 4) + off;
            dpmi_base(scratch, (QWORD)(lin >> 16), (QWORD)lin);
        }
        return (unsigned char __far *)MK_FP(scratch, 0);
    }
#endif
    return (unsigned char __far *)MK_FP(seg, off);
}

static void fcopystr(char __far *d, unsigned char __far *s, int n)
{
    int i;
    for (i = 0; i < n - 1 && s && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

static void rclear(RMR *r) { memset(r, 0, sizeof(*r)); }

/* Windows 1.x/2.x (OLDWIN) run in real mode: a buffer in the data segment */
#if !defined(__WINDOWS__) || defined(OLDWIN)
static unsigned char dosbuf[2048 + 16];
#endif

static int buffers(void)
{
#if defined(__WINDOWS__) && !defined(OLDWIN)
    QDWORD r;
    pmode = (GetWinFlags() & WF_PMODE) ? 1 : 0;
    r = GlobalDosAlloc(4096);
    if (!r) return 0;
    buf = (unsigned char __far *)MK_FP(LOWORD(r), 0);
    buf_seg = HIWORD(r);
    return 1;
#else
    unsigned char __far *p = (unsigned char __far *)dosbuf;
    QDWORD lin = ((QDWORD)FP_SEG(p) << 4) + FP_OFF(p) + 15;
    buf_seg = (QWORD)(lin >> 4);
    buf = (unsigned char __far *)MK_FP(buf_seg, 0);
    pmode = 0;
    return 1;
#endif
}

static void parse_edid(VQINFO __far *vi)
{
    QBYTE *e = vi->edid;
    int i;
    QWORD m = (e[8] << 8) | e[9];
    vi->mfg[0] = (char)('@' + ((m >> 10) & 31));
    vi->mfg[1] = (char)('@' + ((m >> 5) & 31));
    vi->mfg[2] = (char)('@' + (m & 31));
    vi->mfg[3] = 0;
    vi->digital = (e[20] & 0x80) ? 1 : 0;
    for (i = 0; i < 4; i++) {
        QBYTE *d = e + 54 + i * 18;
        if (d[0] || d[1]) {
            if (i == 0) {
                QWORD clk = d[0] | (d[1] << 8);
                QWORD ha = d[2] | ((d[4] & 0xF0) << 4), hb = d[3] | ((d[4] & 0x0F) << 8);
                QWORD va = d[5] | ((d[7] & 0xF0) << 4), vb = d[6] | ((d[7] & 0x0F) << 8);
                QDWORD tot = (QDWORD)(ha + hb) * (va + vb);
                vi->natw = ha; vi->nath = va;
                if (tot) vi->nathz = (QWORD)(((QDWORD)clk * 10000UL + tot / 2) / tot);
            }
            continue;
        }
        if (d[3] == 0xFC) {
            int k;
            for (k = 0; k < 13 && d[5 + k] != 0x0A; k++) vi->monname[k] = d[5 + k];
            vi->monname[k] = 0;
            while (k > 0 && vi->monname[k - 1] == ' ') vi->monname[--k] = 0;
        } else if (d[3] == 0xFD) {
            vi->range_ok = 1;
            vi->vmin = d[5]; vi->vmax = d[6]; vi->hmin = d[7]; vi->hmax = d[8];
            vi->maxclk = d[9] * 10;
        }
    }
}

static QWORD dispi_rd(QWORD i) { outpw(0x1CE, i); return inpw(0x1CF); }

int vq_query(VQINFO __far *vi)
{
    RMR r;
    unsigned char __far *p;
    QWORD list[VQ_MAXMODES];
    QWORD n = 0, i;
    _fmemset(vi, 0, sizeof(VQINFO));
    if (!buffers()) return 0;
    /* Bochs DISPI */
    vi->dispi_id = dispi_rd(0);
    if (vi->dispi_id < 0xB0C0 || vi->dispi_id > 0xB0CF) vi->dispi_id = 0;
    else vi->dispi_memkb = vi->dispi_id >= 0xB0C5 ? dispi_rd(10) * 64 : 4096;

    _fmemset(buf, 0, 512);
    _fmemcpy(buf, "VBE2", 4);
    rclear(&r); r.eax = 0x4F00; r.es = buf_seg; r.edi = 0;
    if (!int10(&r) || (QWORD)r.eax != 0x004F || _fmemcmp(buf, "VESA", 4))
        return 0;
    vi->ver = *(QWORD __far *)(buf + 4);
    vi->memkb = (QDWORD)*(QWORD __far *)(buf + 18) * 64;
    {
        QWORD oo = *(QWORD __far *)(buf + 6), os = *(QWORD __far *)(buf + 8);
        QWORD vo = *(QWORD __far *)(buf + 22), vs = *(QWORD __far *)(buf + 24);
        QWORD po = *(QWORD __far *)(buf + 26), ps = *(QWORD __far *)(buf + 28);
        QWORD lo = *(QWORD __far *)(buf + 14), ls = *(QWORD __far *)(buf + 16);
        fcopystr(vi->oem, rmp(os, oo), sizeof(vi->oem));
        if (vi->ver >= 0x200) {
            if (vs) fcopystr(vi->vendor, rmp(vs, vo), sizeof(vi->vendor));
            if (ps) fcopystr(vi->product, rmp(ps, po), sizeof(vi->product));
        }
        p = rmp(ls, lo);
        while (p && n < VQ_MAXMODES && ((QWORD __far *)p)[n] != 0xFFFF) {
            list[n] = ((QWORD __far *)p)[n];
            n++;
            p = rmp(ls, lo);          /* scratch selector may move */
        }
    }
    for (i = 0; i < n; i++) {
        QWORD attr;
        QBYTE bpp, model;
        VQMODE __far *m;
        _fmemset(buf + 0x200, 0, 256);
        rclear(&r); r.eax = 0x4F01; r.ecx = list[i]; r.es = buf_seg; r.edi = 0x200;
        if (!int10(&r) || (QWORD)r.eax != 0x004F) continue;
        p = buf + 0x200;
        attr = *(QWORD __far *)p;
        bpp = p[0x19]; model = p[0x1B];
        if (!(attr & 1) || !(attr & 0x10) || !(attr & 0x08)) continue;
        if (model != 4 && model != 6) continue;
        if (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32) continue;
        if (model == 6 && bpp == 16 && p[0x21] == 5) bpp = 15;
        if (list[i] == 0x13) continue;
        m = &vi->modes[vi->nmodes++];
        m->mode = list[i];
        m->w = *(QWORD __far *)(p + 18);
        m->h = *(QWORD __far *)(p + 20);
        m->bpp = bpp;
        m->pitch = *(QWORD __far *)(p + 16);
        m->banked = !(attr & 0x40) && (p[2] & 1);
        m->linear = (vi->ver >= 0x200 && (attr & 0x80)) ? 1 : 0;
        if (m->linear) m->lfb = *(QDWORD __far *)(p + 0x28);
    }
    /* EDID */
    rclear(&r); r.eax = 0x4F15; r.ebx = 0;
    if (int10(&r) && (QWORD)r.eax == 0x004F) {
        _fmemset(buf + 0x400, 0, 128);
        rclear(&r); r.eax = 0x4F15; r.ebx = 1; r.es = buf_seg; r.edi = 0x400;
        if (int10(&r) && (QWORD)r.eax == 0x004F && buf[0x401] == 0xFF && buf[0x400] == 0) {
            _fmemcpy(vi->edid, buf + 0x400, 128);
            vi->edid_ok = 1;
            parse_edid(vi);
        }
    }
    return 1;
}

static int mcmp(VQMODE __far *a, VQMODE __far *b)
{
    if (a->w != b->w) return a->w < b->w ? -1 : 1;
    if (a->h != b->h) return a->h < b->h ? -1 : 1;
    if (a->bpp != b->bpp) return a->bpp < b->bpp ? -1 : 1;
    return 0;
}

void vq_sort(VQINFO __far *vi)
{
    QWORD i, j;
    for (i = 1; i < vi->nmodes; i++) {
        VQMODE t = vi->modes[i];
        j = i;
        while (j > 0 && mcmp(&vi->modes[j - 1], (VQMODE __far *)&t) > 0) {
            vi->modes[j] = vi->modes[j - 1];
            j--;
        }
        vi->modes[j] = t;
    }
}
