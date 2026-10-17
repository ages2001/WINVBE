/*
 * 915PATCH - add resolutions to the video BIOS of Intel integrated graphics
 *            (830 ... G45 / Atom), in the spirit of Linux' 915resolution.
 *
 *   915PATCH /LIST                          show chipset, BIOS type and modes
 *   915PATCH /AUTO                          monitor native resolution (EDID)
 *   915PATCH /AUTO x y                      given resolution
 *   915PATCH mode x y [bpp [htotal vtotal]] replace one Intel mode (hex)
 *   915PATCH /DUMP [file]                   save the video BIOS (VBIOS.ROM)
 *   915PATCH /CHECK                         no output; exit code 0 when this
 *                                           PC has a patchable Intel BIOS
 *   /LOG  with any of them: also write 915PATCH.LOG next to 915PATCH.EXE
 *         (WINVBECF shows the result from it; /AUTO always does, unless
 *         /NOLOG);  /Q  no screen output
 *   /TYPE:n (n = 1..4) with any of them     layout of the BIOS, as
 *                                           915resolution -t (4 = GM45
 *                                           class, EDID timings)
 *
 * Run it from DOS before Windows (e.g. in AUTOEXEC.BAT): the video BIOS is
 * in shadow RAM, the change is lost at the next reset.  On other hardware
 * the program changes nothing and ends with an error (exit code 2).
 *
 * Built with VXDINIT the same code is the real mode initialization of
 * 915PATCH.VXD (vxdinit.c), which Windows runs while it starts (SYSTEM.INI
 * [386Enh] device=), for Windows ME and others without a DOS prompt.
 */
#ifndef VXDINIT
#include <stdio.h>
#include <stdarg.h>
static void say(const char *fmt, ...);
#else
void say(const char *fmt, ...);     /* vxdinit.c: into the log */
#endif
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <dos.h>
#include <i86.h>
#include "i915core.h"

#include "i915chips.h"
#define chips i915_chips

/* ---- PCI configuration mechanism #1 (needs a 386) ---- */
static void outpd(u16 port, u32 v);
#pragma aux outpd = ".386" "mov ax,cx" "shl eax,16" "mov ax,bx" "out dx,eax" parm [dx] [cx bx] modify [ax];
static u32 inpd(u16 port);
#pragma aux inpd = ".386" "in eax,dx" "mov edx,eax" "shr edx,16" parm [dx] value [dx ax];
/* FLAGS after trying to set bits 12-15 (8086: always set, 286: always clear) */
static u16 flags_hi(void);
#pragma aux flags_hi = "pushf" "pop ax" "or ax,0F000h" "push ax" "popf" "pushf" "pop ax" \
    "push ax" "and ax,0FFFh" "push ax" "popf" "pop ax" value [ax];
static u32 pci_rd32(u8 reg)
{
    outpd(0xCF8, 0x80000000UL | reg);
    return inpd(0xCFC);
}
static u8 pci_rd8(u8 reg)
{
    outpd(0xCF8, 0x80000000UL | (reg & 0xFC));
    return (u8)inp(0xCFC + (reg & 3));
}
static void pci_wr8(u8 reg, u8 v)
{
    outpd(0xCF8, 0x80000000UL | (reg & 0xFC));
    outp(0xCFC + (reg & 3), v);
}

static int is_386(void)
{
    u16 f = flags_hi() & 0xF000;
    return f != 0 && f != 0xF000;
}

static u8 save1, save2;
static int pam;

static void unlock(void)
{
    if (pam == PAM_5A) {
        save1 = pci_rd8(0x5A);
        pci_wr8(0x5A, 0x33);
#ifdef TEST440FX
        save2 = pci_rd8(0x5B);
        pci_wr8(0x5B, 0x33);
#endif
    } else if (pam == PAM_90) {
        save1 = pci_rd8(0x91);
        save2 = pci_rd8(0x92);
        pci_wr8(0x91, 0x33);
        pci_wr8(0x92, 0x33);
    }
}

static void relock(void)
{
    if (pam == PAM_5A) {
        pci_wr8(0x5A, save1);
#ifdef TEST440FX
        pci_wr8(0x5B, save2);
#endif
    }
    else if (pam == PAM_90) { pci_wr8(0x91, save1); pci_wr8(0x92, save2); }
}

static int find_str(u8 __far *p, u32 n, const char *s)
{
    u32 i, l = strlen(s);
    for (i = 0; i + l <= n; i++)
        if (p[i] == (u8)s[0] && !_fmemcmp(p + i, s, (size_t)l)) return 1;
    return 0;
}

/* INT 10h AX=4F15h BL=1: EDID block 0 into ES:DI */
static u16 ddc_read(u8 __far *p);
#pragma aux ddc_read = \
    "push bp" \
    "mov ax,4F15h" \
    "mov bx,1" \
    "xor cx,cx" \
    "xor dx,dx" \
    "int 10h" \
    "pop bp" \
    parm [es di] value [ax] modify [bx cx dx si];

/* hexadecimal number (Intel mode) */
static u16 hexval(const char *s)
{
    u16 v = 0;
    for (; *s; s++) {
        char c = *s;
        if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = v * 16 + (c - 'A' + 10);
        else break;
    }
    return v;
}

/* EDID preferred timing via VBE/DDC (INT 10h 4F15h) */
static int edid_native(u16 *x, u16 *y)
{
    static u8 buf[128];
    if (ddc_read((u8 __far *)buf) != 0x004F) return 0;
    if (buf[0] != 0 || buf[1] != 0xFF) return 0;
    *x = buf[56] | ((buf[58] & 0xF0) << 4);
    *y = buf[59] | ((buf[61] & 0xF0) << 4);
    return *x >= 320 && *y >= 200;
}

#ifndef VXDINIT
static int windows_running(void)
{
    union REGS r;
    r.x.ax = 0x1600;
    int86(0x2F, &r, &r);
    if (r.h.al != 0 && r.h.al != 0x80) return 1;      /* enhanced mode */
    r.x.ax = 0x4680;
    int86(0x2F, &r, &r);
    return r.x.ax == 0;                                 /* real/standard */
}
#endif

/* three columns: a table has up to 36 modes and a screen 25 lines */
static void list(VBMAP *m)
{
    u16 i;
    VBMODE md;
    say("Mode Resolution  Bits   Mode Resolution  Bits   Mode Resolution  Bits\n");
    for (i = 0; i < m->count; i++) {
        vb_get_mode(m, i, &md);
        if (md.x) say(" %02X  %4u x %-4u %2u", md.mode, md.x, md.y, md.bpp);
        else say(" %02X  (empty)     %2u", md.mode, md.bpp);
        say(i % 3 == 2 || i + 1 == m->count ? "\n" : "   ");
    }
}

#ifndef VXDINIT
/* the candidate tables, for a BIOS whose layout is not known */
static void dump_table(VBMAP *m)
{
    u32 p = 0x40;
    u16 n = 0;
    while (n < 6 && vb_find_next(m, p)) {
        u16 i, c = m->count < 6 ? m->count : 6;
        say("Rejected table at C000:%04X (%u entries), mode bits block:", m->table, m->count);
        for (i = 0; i < c; i++) {
            u8 __far *e = m->bios + m->table + i * 5;
            say(" %02X/%u/%04X", e[0], e[1], e[2] | (e[3] << 8));
        }
        say("\n");
        p = (u32)m->table + 1;
        n++;
    }
}

static int dump_bios(u8 __far *bios, u32 size, const char *name)
{
    FILE *f = fopen(name, "wb");
    u32 o;
    static u8 buf[512];
    if (!f) { say("Error: cannot create %s.\n", name); return 1; }
    for (o = 0; o < size; o += sizeof(buf)) {
        _fmemcpy(buf, bios + (u16)o, sizeof(buf));
        if (fwrite(buf, 1, sizeof(buf), f) != sizeof(buf)) { fclose(f); say("Error: write error.\n"); return 1; }
    }
    fclose(f);
    say("Video BIOS (%lu KB) saved to %s.\n", size / 1024, name);
    return 0;
}
#endif

static void usage(void)
{
#ifdef VXDINIT
    say("Usage: Patch=/LIST | /AUTO [width height] | mode width height [bits [htotal vtotal]] [/TYPE:n]\n");
#else
    say("Usage: 915PATCH /LIST\n"
           "       915PATCH /AUTO [width height]\n"
           "       915PATCH mode width height [bits [htotal vtotal]]   (mode in hex)\n"
           "       915PATCH /DUMP [file]       save the video BIOS for a bug report\n"
           "Add /TYPE:1 ... /TYPE:4 to force the BIOS layout (see /LIST).\n");
#endif
}

int patch_run(int argc, char **argv)
{
    static VBMAP m;
    u32 id;
    int i, c = -1, rc = 0;
    u8 __far *bios = (u8 __far *)MK_FP(0xC000, 0);
    int force = 0, j;

    /* /TYPE:n anywhere on the command line */
    for (i = j = 1; i < argc; i++) {
        if ((argv[i][0] == '/' || argv[i][0] == '-') && !strnicmp(argv[i] + 1, "TYPE", 4)) {
            force = atoi(argv[i] + 5 + (argv[i][5] == ':' || argv[i][5] == '='));
            if (force < 1 || force > 4) { say("Error: /TYPE:1, /TYPE:2, /TYPE:3 or /TYPE:4.\n"); return 1; }
        } else
            argv[j++] = argv[i];
    }
    argc = j;
    if (argc < 2) { usage(); return 1; }
#ifndef VXDINIT
    if (!is_386()) { say("Error: a 386 or newer CPU is needed.\n"); return 2; }
    if (windows_running()) {
        say("Error: run 915PATCH from DOS, before Windows is started.\n"
            "Windows ME (no MS-DOS mode): use 915PATCH.VXD, see README.TXT.\n");
        return 2;
    }
#endif

#ifndef VXDINIT
    if (!stricmp(argv[1], "/DUMP") || !stricmp(argv[1], "-DUMP")) {
        u32 n = (u32)bios[2] * 512UL;
        if (bios[0] != 0x55 || bios[1] != 0xAA) { say("Error: no video BIOS at C000h.\n"); return 2; }
        if (n < 0x1000 || n > 0x10000UL) n = 0x10000UL;
        return dump_bios(bios, n, argc >= 3 ? argv[2] : "VBIOS.ROM");
    }
#endif
    id = pci_rd32(0);
    if ((u16)id != 0x8086) {
        say("Error: no Intel host bridge (PCI 0:0.0 = %04X:%04X).\n"
               "This program only works with Intel integrated graphics (830 ... G45, Atom).\n",
               (u16)id, (u16)(id >> 16));
        return 2;
    }
    for (i = 0; chips[i].name; i++)
        if (chips[i].id == (u16)(id >> 16)) { c = i; break; }
    if (c < 0) {
        say("Error: chipset 8086:%04X is not supported.\n"
               "Supported: Intel 830/845G/855GM/865G/915/945/946/965/G3x/G4x/Atom graphics.\n",
               (u16)(id >> 16));
        return 2;
    }
    pam = chips[c].pam;
    say("Chipset   : Intel %s (8086:%04X)\n", chips[c].name, (u16)(id >> 16));

    if (bios[0] != 0x55 || bios[1] != 0xAA) { say("Error: no video BIOS at C000h.\n"); return 2; }
    m.bios = bios;
    m.size = (u32)bios[2] * 512UL;
    if (m.size < 0x1000 || m.size > 0x10000UL) m.size = 0x10000UL;
    if (!find_str(bios, m.size, "Intel") && !find_str(bios, m.size, "INTEL")) {
        say("Error: the video BIOS is not an Intel BIOS (separate graphics card?).\n");
        return 2;
    }
    if (!vb_find_table(&m)) {
        say("Error: mode table not found in the video BIOS.\n");
#ifndef VXDINIT
        say("Please send the file of 915PATCH /DUMP with a bug report.\n");
#endif
        return 2;
    }
    if (force ? !vb_force_type(&m, force) : !vb_locate(&m)) {
        if (force) say("Error: no mode table of type %d in this video BIOS.\n", force);
        else say("Error: no known mode table in this video BIOS.\n");
#ifndef VXDINIT
        dump_table(&m);
#endif
        say("This video BIOS may not have a patchable mode table (newer Intel BIOSes\n"
            "set the panel resolution from their VBT); nothing was changed.\n");
#ifndef VXDINIT
        say("Please send the file of 915PATCH /DUMP with a bug report.\n");
#endif
        return 2;
    }
    say("Video BIOS: %lu KB, mode table at C000:%04X, %u modes, %s%s\n\n",
           m.size / 1024, m.table, m.count, vb_type_name(m.type), force ? " (forced)" : "");

    if (!stricmp(argv[1], "/LIST") || !stricmp(argv[1], "-LIST") || !stricmp(argv[1], "/L")) {
        list(&m);
        return 0;
    }

    unlock();
    /* the shadow RAM must be writable now */
    {
        u8 t = bios[m.table + 1];
        bios[m.table + 1] = (u8)(t ^ 0x5A);
        if (bios[m.table + 1] != (u8)(t ^ 0x5A)) {
            relock();
            say("Error: the video BIOS shadow RAM cannot be unlocked on this board.\n");
            return 2;
        }
        bios[m.table + 1] = t;
    }

    if (!stricmp(argv[1], "/AUTO") || !stricmp(argv[1], "-AUTO")) {
        u16 x = 0, y = 0, k;
        static const u8 bpps[3] = { 8, 16, 32 };
        if (argc >= 4) { x = (u16)atoi(argv[2]); y = (u16)atoi(argv[3]); }
        else if (!edid_native(&x, &y)) {
            relock();
            say("Error: the monitor resolution cannot be read (no EDID); give it: /AUTO 1280 800\n");
            return 1;
        }
        say("Wanted    : %u x %u\n", x, y);
        for (k = 0; k < 3; k++) {
            VBMODE md, best;
            u16 j, have = 0;
            best.mode = 0; best.x = best.y = 0;
            for (j = 0; j < m.count; j++) {
                vb_get_mode(&m, j, &md);
                if (md.bpp != bpps[k]) continue;
                if (md.x == x && md.y == y) have = 1;
                if ((u32)md.x * md.y > (u32)best.x * best.y) best = md;
            }
            if (have) { say("  %2u bits : already available\n", bpps[k]); continue; }
            if (!best.mode) continue;
            if (vb_set_mode(&m, best.mode, x, y, 0, 0, 0))
                say("  %2u bits : mode %02X %u x %u -> %u x %u\n", bpps[k], best.mode, best.x, best.y, x, y);
        }
    } else {
        u8 mode = (u8)hexval(argv[1]);
        u16 x, y, ht = 0, vt = 0;
        u8 bpp = 0;
        if (argc < 4) { relock(); usage(); return 1; }
        x = (u16)atoi(argv[2]);
        y = (u16)atoi(argv[3]);
        if (argc >= 5) bpp = (u8)atoi(argv[4]);
        if (argc >= 7) { ht = (u16)atoi(argv[5]); vt = (u16)atoi(argv[6]); }
        if (x < 320 || y < 200 || x > 4096 || y > 4096) { relock(); say("Error: bad resolution.\n"); return 1; }
        if (!vb_set_mode(&m, mode, x, y, bpp, ht, vt)) {
            relock();
            say("Error: mode %02X is not in the table (see 915PATCH /LIST).\n", mode);
            return 1;
        }
        say("Mode %02X is now %u x %u.\n", mode, x, y);
    }
    relock();
    say("\nDone. The new resolution is available until the next reset.\n");
    return rc;
}

#ifndef VXDINIT
static int quiet;
static FILE *logf;

static void say(const char *fmt, ...)
{
    va_list ap;
    if (!quiet) { va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
    if (logf) { va_start(ap, fmt); vfprintf(logf, fmt, ap); va_end(ap); }
}

int main(int argc, char **argv)
{
    int i, j, check = 0, log = 0, nolog = 0, rc;
    /* /CHECK, /LOG, /NOLOG, /Q anywhere on the command line; /AUTO always
       writes the log (WINVBECF shows the result from it) */
    for (i = j = 1; i < argc; i++) {
        char *a = argv[i];
        if ((a[0] == '/' || a[0] == '-') && !stricmp(a + 1, "CHECK")) { check = 1; quiet = 1; }
        else if ((a[0] == '/' || a[0] == '-') && !stricmp(a + 1, "LOG")) log = 1;
        else if ((a[0] == '/' || a[0] == '-') && !stricmp(a + 1, "NOLOG")) nolog = 1;
        else if ((a[0] == '/' || a[0] == '-') && !stricmp(a + 1, "Q")) quiet = 1;
        else argv[j++] = a;
    }
    argc = j;
    if (check) { argv[1] = "/LIST"; argc = 2; }
    if (argc > 1 && (argv[1][0] == '/' || argv[1][0] == '-') && !stricmp(argv[1] + 1, "AUTO")) log = 1;
    if (log && !check && !nolog) {
        static char path[128];
        char *p;
        strncpy(path, argv[0], sizeof(path) - 16);
        path[sizeof(path) - 16] = 0;
        p = strrchr(path, '\\');
        if (p) p[1] = 0; else path[0] = 0;
        strcat(path, "915PATCH.LOG");
        logf = fopen(path, "w");
    }
    say("915PATCH - WINVBE v1.0 video BIOS mode patcher for Intel graphics\n\n");
    rc = patch_run(argc, argv);
    if (logf) fclose(logf);
    return rc;
}
#endif
