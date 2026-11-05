/*
 * WINVBECF - configuration program for the WINVBE display driver
 * Windows 3.0 compatible (real, standard and 386 enhanced mode).
 *
 * Built with OLDWIN it runs on Windows 1.x/2.x: settings in WIN.INI
 * [WINVBE], list boxes instead of combo boxes (Windows 1.x/2.x have none),
 * own start-up code (cfgold.asm) and only the functions of Windows 1.01.
 * The driver itself is linked into WIN100/WIN200.BIN by INSTALL.EXE.
 */
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "vbeq.h"
#include "winvbecf.h"
#if defined(OLDWIN) && defined(MARK)
#include <conio.h>
#define DBG(c) outp(0xE9, c)
#else
#define DBG(c)
#endif
#ifdef OLDWIN
int o_sprintf(char *out, const char *fmt, ...);
#define sprintf o_sprintf
#endif


#define WINVBE_GETINFO 0x7701
#define EW_RESTART    0x42

typedef struct {
    WORD  size, version, mode, width, height, bpp, pitch, access, vbever, refresh, dpi, memkb;
    char  oem[64];
} WINVBEINFO;

static HANDLE hInst;
#ifdef OLDWIN
/* USER.2: ends the Windows session (Windows 1.x/2.x, no parameters) */
void FAR PASCAL OldExitWindows(void);
#define ini_get_int(key, def)   GetProfileInt(sect, key, def)
#define ini_put_str(key, val)   WriteProfileString(sect, key, val)
/* list boxes take the place of the combo boxes */
#define XB_ADD   LB_ADDSTRING
#define XB_RESET LB_RESETCONTENT
#define XB_GET   LB_GETCURSEL
#define XB_SET   LB_SETCURSEL
static int depth_map[8];          /* no LB_SETITEMDATA on Windows 1.x/2.x */
#else
#define ini_get_int(key, def)   GetPrivateProfileInt(sect, key, def, sysini)
#define ini_put_str(key, val)   WritePrivateProfileString(sect, key, val, sysini)
#define XB_ADD   CB_ADDSTRING
#define XB_RESET CB_RESETCONTENT
#define XB_GET   CB_GETCURSEL
#define XB_SET   CB_SETCURSEL
#endif
static VQINFO FAR *vi;
#ifndef OLDWIN
static char sysini[] = "SYSTEM.INI";

/* Windows 95/98/ME: the display settings belong to the Display control panel
   (registry HKEY_CURRENT_CONFIG\Display\Settings) and the modes it offers to
   the MODES key of the display adapter. KERNEL has the 16-bit registry
   functions only on these versions. */
typedef LONG (FAR PASCAL *PREGOPENKEY)(DWORD, LPCSTR, DWORD FAR *);
typedef LONG (FAR PASCAL *PREGCLOSEKEY)(DWORD);
typedef LONG (FAR PASCAL *PREGENUMKEY)(DWORD, DWORD, LPSTR, DWORD);
typedef LONG (FAR PASCAL *PREGDELETEKEY)(DWORD, LPCSTR);
typedef LONG (FAR PASCAL *PREGQUERYVALUEEX)(DWORD, LPCSTR, DWORD FAR *, DWORD FAR *, LPBYTE, DWORD FAR *);
typedef LONG (FAR PASCAL *PREGSETVALUEEX)(DWORD, LPCSTR, DWORD, DWORD, LPBYTE, DWORD);
#define W9_HKLM 0x80000002UL
#define W9_HKCC 0x80000005UL
static int win9x;
static PREGOPENKEY r_open, r_create;
static PREGCLOSEKEY r_close;
static PREGENUMKEY r_enum;
static PREGDELETEKEY r_delete;
static PREGQUERYVALUEEX r_query;
static PREGSETVALUEEX r_set;

static void win9x_init(void)
{
    HMODULE k = GetModuleHandle("KERNEL");
    r_open = (PREGOPENKEY)GetProcAddress(k, "REGOPENKEY");
    r_create = (PREGOPENKEY)GetProcAddress(k, "REGCREATEKEY");
    r_close = (PREGCLOSEKEY)GetProcAddress(k, "REGCLOSEKEY");
    r_enum = (PREGENUMKEY)GetProcAddress(k, "REGENUMKEY");
    r_delete = (PREGDELETEKEY)GetProcAddress(k, "REGDELETEKEY");
    r_query = (PREGQUERYVALUEEX)GetProcAddress(k, "REGQUERYVALUEEX");
    r_set = (PREGSETVALUEEX)GetProcAddress(k, "REGSETVALUEEX");
    win9x = r_open && r_create && r_close && r_enum && r_delete && r_query && r_set;
}

static int reg_get(DWORD root, LPCSTR key, LPCSTR name, char *buf, int n)
{
    DWORD hk, type, len = n - 1;
    LONG r;
    buf[0] = 0;
    if (r_open(root, key, &hk)) return 0;
    r = r_query(hk, name, NULL, &type, (LPBYTE)buf, &len);
    r_close(hk);
    if (r) return 0;
    buf[len < (DWORD)n ? (int)len : n - 1] = 0;
    return 1;
}

static void reg_put(DWORD root, LPCSTR key, LPCSTR name, char *val)
{
    DWORD hk;
    if (r_create(root, key, &hk)) return;
    r_set(hk, name, 0, 1 /* REG_SZ */, (LPBYTE)val, strlen(val) + 1);
    r_close(hk);
}

/* the display class key of the adapter that uses WINVBE.DRV */
static int winvbe_class_key(char *path)
{
    DWORD i;
    char sub[16], drv[32];
    for (i = 0; i < 32; i++) {
        DWORD hk;
        if (r_open(W9_HKLM, "System\\CurrentControlSet\\Services\\Class\\Display", &hk)) return 0;
        if (r_enum(hk, i, sub, sizeof(sub))) { r_close(hk); return 0; }
        r_close(hk);
        sprintf(path, "System\\CurrentControlSet\\Services\\Class\\Display\\%s", sub);
        {
            char k2[96];
            sprintf(k2, "%s\\DEFAULT", path);
            if (reg_get(W9_HKLM, k2, "drv", drv, sizeof(drv)) && !stricmp(drv, "winvbe.drv")) return 1;
        }
    }
    return 0;
}

#endif
static char sect[] = "WINVBE";

/* list box entries: resolution table */
typedef struct { WORD w, h; WORD depths; } RES;   /* depths: bit0=8 1=15 2=16 3=24 4=32 */
static RES res[96];
static int nres;
static int can_refresh;          /* VBE 3.0: refresh rate can be set */
static int refresh_vals[] = { 0, 1, 56, 60, 65, 70, 72, 75, 85, 90, 100, 120 };
#define NREFRESH (sizeof(refresh_vals) / sizeof(refresh_vals[0]))
static int depth_vals[] = { 0, 8, 15, 16, 24, 32 };
static char *depth_names[] = { "Automatic", "256 colors", "32K (15 bit)", "64K (16 bit)", "16M (24 bit)",
                               "16M (32 bit)" };

static int depth_bit(int bpp)
{
    switch (bpp) { case 8: return 1; case 15: return 2; case 16: return 4; case 24: return 8; case 32: return 16; }
    return 0;
}

static void build_res(void)
{
    WORD i;
    int k;
    nres = 0;
    for (i = 0; i < vi->nmodes; i++) {
        VQMODE FAR *m = &vi->modes[i];
        for (k = 0; k < nres; k++)
            if (res[k].w == m->w && res[k].h == m->h) break;
        if (k == nres) {
            if (nres >= 96) continue;
            res[nres].w = m->w; res[nres].h = m->h; res[nres].depths = 0;
            nres++;
        }
        res[k].depths |= depth_bit(m->bpp);
    }
}

static int ini_int(char *key, int def)
{
    return ini_get_int(key, def);
}

static void ini_put_int(char *key, int v)
{
    char b[16];
    sprintf(b, "%d", v);
    ini_put_str(key, b);
}

#ifndef OLDWIN
static int file_exists(char *name)
{
    char path[160];
    OFSTRUCT of;
    GetSystemDirectory(path, sizeof(path));
    strcat(path, "\\"); strcat(path, name);
    if (OpenFile(path, &of, OF_EXIST) != -1) return 1;
    GetWindowsDirectory(path, sizeof(path));
    strcat(path, "\\"); strcat(path, name);
    return OpenFile(path, &of, OF_EXIST) != -1;
}

/* copy a file with the Win16 file API; returns 0 on failure */
static int copy_file(char *src, char *dst)
{
    static char buf[4096];
    OFSTRUCT os, od;
    HFILE i, o;
    UINT n;
    int ok = 1;
    i = OpenFile(src, &os, OF_READ | OF_SHARE_DENY_NONE);
    if (i == HFILE_ERROR) return 0;
    o = OpenFile(dst, &od, OF_CREATE | OF_WRITE);
    if (o == HFILE_ERROR) { _lclose(i); return 0; }
    while ((n = _lread(i, buf, sizeof(buf))) > 0 && n != (UINT)-1)
        if (_lwrite(o, buf, n) != n) { ok = 0; break; }
    _lclose(i); _lclose(o);
    return ok;
}

/* copy WINVBE's own 120 dpi fonts from the program's folder to SYSTEM */
static void copy_own_fonts(void)
{
    static char *wf[] = { "WVBSYS.FON", "WVBFIX.FON", "WVBOEM.FON" };
    char me[160], src[160], dst[160];
    char *p;
    OFSTRUCT of;
    int k;
    GetModuleFileName(hInst, me, sizeof(me));
    p = strrchr(me, '\\');
    if (p) p[1] = 0; else me[0] = 0;
    for (k = 0; k < 3; k++) {
        GetSystemDirectory(dst, sizeof(dst)); strcat(dst, "\\"); strcat(dst, wf[k]);
        strcpy(src, me); strcat(src, wf[k]);
        if (stricmp(src, dst) && OpenFile(dst, &of, OF_EXIST) == HFILE_ERROR &&
            OpenFile(src, &of, OF_EXIST) != HFILE_ERROR)
            copy_file(src, dst);
    }
}

/* When run from the distribution folder, install WINVBE.DRV into the Windows
   SYSTEM directory and this program into the Windows directory. A driver that
   is currently loaded is never overwritten. */
static int install_files(HWND dlg, int active)
{
    char me[160], src[160], dst[160];
    char *p;
    OFSTRUCT of;
    GetModuleFileName(hInst, me, sizeof(me));
    p = strrchr(me, '\\');
    if (p) p[1] = 0; else me[0] = 0;
    GetSystemDirectory(dst, sizeof(dst)); strcat(dst, "\\WINVBE.DRV");
    strcpy(src, me); strcat(src, "WINVBE.DRV");
    if (stricmp(src, dst) && OpenFile(src, &of, OF_EXIST) != HFILE_ERROR &&
        (!active || OpenFile(dst, &of, OF_EXIST) == HFILE_ERROR)) {
        if (!copy_file(src, dst)) {
            MessageBox(dlg, "Could not copy WINVBE.DRV to the Windows SYSTEM directory.", "WINVBE", MB_OK | MB_ICONSTOP);
            return 0;
        }
    }
    if (OpenFile(dst, &of, OF_EXIST) == HFILE_ERROR) {
        MessageBox(dlg, "WINVBE.DRV is not installed. Run WINVBECF.EXE from the WINVBE folder "
                        "(where WINVBE.DRV is), or use INSTALL.EXE from DOS.", "WINVBE", MB_OK | MB_ICONSTOP);
        return 0;
    }
    GetWindowsDirectory(dst, sizeof(dst)); strcat(dst, "\\WINVBECF.EXE");
    strcpy(src, me); strcat(src, "WINVBECF.EXE");
    if (stricmp(src, dst) && OpenFile(dst, &of, OF_EXIST) == HFILE_ERROR)
        copy_file(src, dst);
    return 1;
}

#endif

#ifndef OLDWIN
/* MODES\<bpp>\<w>,<h> for every mode of the card (and the custom size) */
static void write_modes(int cw, int ch)
{
    char cls[80], k[128];
    int i, b;
    static int bits[] = { 8, 15, 16, 24, 32 };
    if (!winvbe_class_key(cls)) return;
    sprintf(k, "%s\\MODES", cls);
    r_delete(W9_HKLM, k);           /* Windows 9x deletes the whole subtree */
    for (i = 0; i < nres; i++)
        for (b = 0; b < 5; b++)
            if (res[i].depths & (1 << b) && bits[b] != 15) {
                DWORD hk;
                sprintf(k, "%s\\MODES\\%d\\%u,%u", cls, bits[b], res[i].w, res[i].h);
                if (!r_create(W9_HKLM, k, &hk)) r_close(hk);
            }
    if (cw && ch)
        for (b = 0; b < 5; b++)
            if (bits[b] != 15) {
                DWORD hk;
                sprintf(k, "%s\\MODES\\%d\\%d,%d", cls, bits[b], cw, ch);
                if (!r_create(W9_HKLM, k, &hk)) r_close(hk);
            }
}
#endif

static void fill_depths(HWND dlg, int sel_bpp)
{
    HWND cb = GetDlgItem(dlg, IDC_DEPTH);
    int idx = (int)SendMessage(GetDlgItem(dlg, IDC_MODES), LB_GETCURSEL, 0, 0L);
    WORD mask = 0x1F;
    int i, cur = 0, n = 0;
    if (idx >= 2 && idx - 2 < nres) mask = res[idx - 2].depths;
    if (idx == 1) mask = 1;                                   /* VGA 320x200 */
    if (IsDlgButtonChecked(dlg, IDC_CUSTOM)) mask = vi->dispi_id ? 0x1F : mask;
    SendMessage(cb, XB_RESET, 0, 0L);
    for (i = 0; i < 6; i++) {
        if (i && !(mask & (1 << (i - 1)))) continue;
        SendMessage(cb, XB_ADD, 0, (LONG)(LPSTR)depth_names[i]);
#ifdef OLDWIN
        depth_map[n] = depth_vals[i];
#else
        SendMessage(cb, CB_SETITEMDATA, n, (LONG)depth_vals[i]);
#endif
        if (depth_vals[i] == sel_bpp) cur = n;
        n++;
    }
    SendMessage(cb, XB_SET, cur, 0L);
}

static int cur_depth(HWND dlg)
{
    HWND cb = GetDlgItem(dlg, IDC_DEPTH);
    int i = (int)SendMessage(cb, XB_GET, 0, 0L);
    if (i < 0) return 0;
#ifdef OLDWIN
    return i < 8 ? depth_map[i] : 0;
#else
    return (int)SendMessage(cb, CB_GETITEMDATA, i, 0L);
#endif
}


/* ---------------- 915PATCH (Intel integrated graphics) ---------------- */
#ifndef OLDWIN
#include "../patch/i915core.h"
#include "../patch/i915chips.h"

static void p_outpd(WORD port, DWORD v);
#pragma aux p_outpd = ".386" "mov ax,cx" "shl eax,16" "mov ax,bx" "out dx,eax" parm [dx] [cx bx] modify [ax];
static DWORD p_inpd(WORD port);
#pragma aux p_inpd = ".386" "in eax,dx" "mov edx,eax" "shr edx,16" parm [dx] value [dx ax];

/* Intel chipset with a video BIOS 915PATCH can patch?  chip = its name */
static int i915_check(char *chip)
{
    static VBMAP m;
    DWORD id;
    WORD sel, i;
    BYTE FAR *bios;
    int c = -1;
    chip[0] = 0;
    if (!(GetWinFlags() & (WF_CPU386 | WF_CPU486))) return 0;
    p_outpd(0xCF8, 0x80000000UL);
    id = p_inpd(0xCFC);
    if ((WORD)id != 0x8086) return 0;
    for (i = 0; i915_chips[i].name; i++)
        if (i915_chips[i].id == (WORD)(id >> 16)) { c = i; break; }
    if (c < 0) return 0;
    strcpy(chip, i915_chips[c].name);
    sel = LOWORD(GetProcAddress(GetModuleHandle("KERNEL"), MAKEINTRESOURCE(195)));   /* __C000H */
    if (!sel) return 0;
    bios = (BYTE FAR *)MAKELP(sel, 0);
    if (bios[0] != 0x55 || bios[1] != 0xAA) return 0;
    m.bios = bios;
    m.size = (DWORD)bios[2] * 512UL;
    if (m.size < 0x1000 || m.size > 0x10000UL) m.size = 0x10000UL;
    for (i = 0; i + 5 < (WORD)(m.size - 1); i++)
        if (bios[i] == 'I' && (!_fmemcmp(bios + i, "Intel", 5) || !_fmemcmp(bios + i, "INTEL", 5))) break;
    if (i + 5 >= (WORD)(m.size - 1)) return 0;
    return vb_locate(&m);
}

/* a whole file into a buffer (at most n - 1 bytes) */
static int read_text(char *path, char *buf, int n)
{
    OFSTRUCT of;
    HFILE h = OpenFile(path, &of, OF_READ | OF_SHARE_DENY_NONE);
    UINT got;
    buf[0] = 0;
    if (h == HFILE_ERROR) return -1;
    got = _lread(h, buf, n - 1);
    _lclose(h);
    if (got == (UINT)-1) got = 0;
    buf[got] = 0;
    return (int)got;
}

static char *find_ci(char *s, char *w)
{
    int l = strlen(w);
    for (; *s; s++) if (!strnicmp(s, w, l)) return s;
    return NULL;
}

static char ini_buf[16384];

/* SYSTEM.INI [386Enh] has device=<vxd> */
static int ini_has_device(char *vxd)
{
    char path[160], *p, *e;
    GetWindowsDirectory(path, sizeof(path));
    strcat(path, "\\SYSTEM.INI");
    WritePrivateProfileString(NULL, NULL, NULL, "SYSTEM.INI");     /* flush the cache */
    if (read_text(path, ini_buf, sizeof(ini_buf)) <= 0) return 0;
    p = find_ci(ini_buf, "[386Enh]");
    if (!p) return 0;
    e = strchr(p + 1, '[');
    for (p = strchr(p, '\n'); p && (!e || p < e); p = strchr(p + 1, '\n')) {
        char *l = p + 1;
        while (*l == ' ' || *l == '\t') l++;
        if (!strnicmp(l, "device", 6)) {
            l += 6;
            while (*l == ' ' || *l == '\t') l++;
            if (*l == '=') {
                l++;
                while (*l == ' ' || *l == '\t') l++;
                if (!strnicmp(l, vxd, strlen(vxd))) return 1;
            }
        }
    }
    return 0;
}

/* a device=<vxd> line right after [386Enh] (backup SYSTEM.WVB) */
static int ini_add_device(char *vxd)
{
    char path[160], bak[160], line[40], *p;
    OFSTRUCT of;
    HFILE h;
    int n, ok;
    GetWindowsDirectory(path, sizeof(path));
    strcpy(bak, path);
    strcat(path, "\\SYSTEM.INI");
    strcat(bak, "\\SYSTEM.WVB");
    WritePrivateProfileString(NULL, NULL, NULL, "SYSTEM.INI");
    n = read_text(path, ini_buf, sizeof(ini_buf) - 64);
    if (n <= 0 || n >= (int)sizeof(ini_buf) - 65) return 0;
    p = find_ci(ini_buf, "[386Enh]");
    if (!p || !(p = strchr(p, '\n'))) return 0;
    p++;
    copy_file(path, bak);
    sprintf(line, "device=%s\r\n", vxd);
    memmove(p + strlen(line), p, n - (p - ini_buf) + 1);
    memcpy(p, line, strlen(line));
    h = OpenFile(path, &of, OF_CREATE | OF_WRITE);
    if (h == HFILE_ERROR) return 0;
    ok = _lwrite(h, ini_buf, n + strlen(line)) == (UINT)(n + strlen(line));
    _lclose(h);
    WritePrivateProfileString(NULL, NULL, NULL, "SYSTEM.INI");
    return ok;
}

/* WINVBECF /915 - run once after the installation with Device Manager
   (Windows 95/98/ME): 915PATCH.VXD into SYSTEM.INI when this PC has a
   patchable Intel video BIOS, else its file is removed again; no message
   unless there is something to restart for */
static void setup915(void)
{
    char chip[40], vxd[160], m[300];
    OFSTRUCT of;
    GetSystemDirectory(vxd, sizeof(vxd));
    strcat(vxd, "\\915PATCH.VXD");
    if (!i915_check(chip)) {
        if (!ini_has_device("915patch.vxd")) OpenFile(vxd, &of, OF_DELETE);
        return;
    }
    if (OpenFile(vxd, &of, OF_EXIST) == HFILE_ERROR || ini_has_device("915patch.vxd")) return;
    if (!ini_add_device("915patch.vxd")) return;
    sprintf(m, "Intel %s graphics: from the next start of Windows on, 915PATCH.VXD adds "
               "the resolution of your monitor to the video BIOS.\n\nRestart the computer now?", chip);
    if (MessageBox(NULL, m, "WINVBE", MB_YESNO | MB_ICONQUESTION) == IDYES)
        ExitWindows(0x43 /* EW_REBOOTSYSTEM */, 0);
}

/* the result of the last 915PATCH run (915PATCH.LOG of the Windows
   directory, written by 915PATCH.VXD or by 915PATCH /AUTO in
   AUTOEXEC.BAT) as one line; "" when 915PATCH is not used */
static void summary915(char *out)
{
    static char log[2048];
    char path[160], chip[40], want[24], *p, *q;
    int added = 0, have = 0, modes = 0;
    out[0] = chip[0] = want[0] = 0;
    GetWindowsDirectory(path, sizeof(path));
    strcat(path, "\\915PATCH.LOG");
    if (read_text(path, log, sizeof(log)) <= 0) {
        if (win9x && ini_has_device("915patch.vxd"))
            strcpy(out, "915 patch: active after the next restart of Windows");
        return;
    }
    if ((p = strstr(log, "Chipset   : ")) != NULL) {
        int i = 0;
        p += 12;
        while (*p && *p != '(' && *p != '\r' && *p != '\n' && i < 38) chip[i++] = *p++;
        while (i && chip[i - 1] == ' ') i--;
        chip[i] = 0;
    }
    if ((p = strstr(log, "Wanted    : ")) != NULL) {
        unsigned x = atoi(p + 12), y = 0;
        if ((q = strchr(p + 12, 'x')) != NULL) y = atoi(q + 1);
        sprintf(want, "%ux%u", x, y);
    }
    for (p = log; (p = strstr(p, " -> ")) != NULL; p++) added++;
    for (p = log; (p = strstr(p, "already available")) != NULL; p++) have++;
    for (p = log; (p = strstr(p, " is now ")) != NULL; p++) modes++;
    if (!added && !have && !modes && (p = strstr(log, "Error: ")) != NULL) {
        int i = 0;
        char *o = out + sprintf(out, "915 patch: ");
        p += 7;
        while (*p && *p != '\r' && *p != '\n' && i < 90) { *o++ = *p++; i++; }
        *o = 0;
        return;
    }
    if (!chip[0]) return;
    if (want[0])
        sprintf(out, "915 patch: %s, %s %s", chip, want,
                added ? "added to the video BIOS" : "already in the video BIOS");
    else
        sprintf(out, "915 patch: %s, %d mode%s changed", chip, modes, modes == 1 ? "" : "s");
}
#endif

static void show_info(HWND dlg)
{
    char b[560], *p = b;
    p += sprintf(p, "VBE %u.%u, %lu KB video memory, %u usable modes\n", vi->ver >> 8, vi->ver & 0xFF,
                 vi->memkb, vi->nmodes);
    if (vi->vendor[0] || vi->product[0])
        p += sprintf(p, "%s %s\n", vi->vendor, vi->product);
    else
        p += sprintf(p, "%s\n", vi->oem);
    if (vi->edid_ok) {
        p += sprintf(p, "Monitor: %s (%s), %s, native %ux%u @ %u Hz", vi->monname[0] ? vi->monname : "unnamed",
                     vi->mfg, vi->digital ? "digital/LCD" : "analog", vi->natw, vi->nath, vi->nathz);
        if (vi->range_ok)
            p += sprintf(p, "\nRange: %u-%u Hz vertical, %u-%u kHz horizontal, %u MHz max", vi->vmin, vi->vmax,
                         vi->hmin, vi->hmax, vi->maxclk);
    } else
        p += sprintf(p, "Monitor: no EDID information (DDC not available)");
#ifndef OLDWIN
    {
        char s9[128];
        summary915(s9);
        if (s9[0]) p += sprintf(p, "\n%s", s9);
    }
#endif
    if (vi->dispi_id)
        p += sprintf(p, "\nBochs/QEMU/VirtualBox adapter: any resolution available");
    SetDlgItemText(dlg, IDC_INFO, b);
}

static void show_current(HWND dlg)
{
    HDC dc = GetDC(NULL);
    WINVBEINFO wi;
    WORD esc = WINVBE_GETINFO;
    char b[160];
    memset(&wi, 0, sizeof(wi));
    if (Escape(dc, QUERYESCSUPPORT, sizeof(WORD), (LPSTR)&esc, NULL) > 0 &&
        Escape(dc, WINVBE_GETINFO, 0, NULL, (LPSTR)&wi) > 0) {
        char rr[24];
        if (wi.refresh) sprintf(rr, "%u.%02u Hz", wi.refresh / 100, wi.refresh % 100);
        else strcpy(rr, "BIOS refresh");
        /* access: 1 LFB, 2 window, 3 VGA; bit 7 LFB unusable, 6 write
           combining, 5 shadow frame buffer, 4 DIB engine (Windows 9x) */
        sprintf(b, "Active: %ux%u, %u bit, mode %Xh, %s%s%s%s, %s", wi.width, wi.height, wi.bpp, wi.mode,
                (wi.access & 0x0F) == 1 ? ((wi.access & 0x10) ? "LFB" : "linear frame buffer") :
                (wi.access & 0x0F) == 2 ? ((wi.access & 0x80) ? "VGA window (LFB not usable)" : "VGA window A000h")
                                        : "VGA",
                (wi.access & 0x40) ? " + WC" : "", (wi.access & 0x20) ? " + shadow" : "",
                (wi.access & 0x10) ? " + DIB engine" : "", rr);
    } else
        strcpy(b, "Not active (Windows 95/98/ME: WIN9X\\WINVBE.INF in Device Manager)");
    ReleaseDC(NULL, dc);
    SetDlgItemText(dlg, IDC_CURRENT, b);
}

static void update_enables(HWND dlg)
{
    BOOL custom = IsDlgButtonChecked(dlg, IDC_CUSTOM);
    EnableWindow(GetDlgItem(dlg, IDC_CUSTW), custom);
    EnableWindow(GetDlgItem(dlg, IDC_CUSTH), custom);
    EnableWindow(GetDlgItem(dlg, IDC_MODES), !custom);
    EnableWindow(GetDlgItem(dlg, IDC_REFRESH), can_refresh);
    EnableWindow(GetDlgItem(dlg, IDC_TIMING), can_refresh);
}

static void init_dialog(HWND dlg)
{
    char b[64];
    int i, w, h, d, sel = 0, hz, acc;
#ifndef OLDWIN
    int dpi;
#endif
    HWND lb = GetDlgItem(dlg, IDC_MODES);
    HWND cb;
    show_info(dlg);
    DBG('1');
    show_current(dlg);
    DBG('2');
    build_res();
    DBG('3');
    w = ini_int("Width", 0); h = ini_int("Height", 0); d = ini_int("Depth", 0);
    if (ini_int("Mode", 0) == 0x13 || ini_int("Forcevga", 0)) sel = 1;
#ifndef OLDWIN
    if (win9x) {                    /* Windows 9x: the Display control panel's setting */
        char v[24];
        if (reg_get(W9_HKCC, "Display\\Settings", "Resolution", v, sizeof(v))) {
            int a = atoi(v), c = strchr(v, ',') ? atoi(strchr(v, ',') + 1) : 0;
            if (a >= 320 && c >= 200 && (a != w || c != h)) { w = a; h = c; sel = 0; }
        }
        if (reg_get(W9_HKCC, "Display\\Settings", "BitsPerPixel", v, sizeof(v))) {
            int a = atoi(v);
            if (a == 8 || a == 15 || a == 16 || a == 24 || a == 32) d = a;
        }
    }
#endif

    SendMessage(lb, LB_ADDSTRING, 0, (LONG)(LPSTR)"Automatic (monitor native)");
    SendMessage(lb, LB_ADDSTRING, 0, (LONG)(LPSTR)"320 x 200 (standard VGA)");
    for (i = 0; i < nres; i++) {
        char *tag = "";
        if (vi->edid_ok && res[i].w == vi->natw && res[i].h == vi->nath) tag = "  (native)";
        sprintf(b, "%u x %u%s", res[i].w, res[i].h, tag);
        SendMessage(lb, LB_ADDSTRING, 0, (LONG)(LPSTR)b);
        if (w == res[i].w && h == res[i].h && !sel) sel = i + 2;
    }
    if (w && h && !sel && vi->dispi_id) {
        CheckDlgButton(dlg, IDC_CUSTOM, 1);
    }
    SendMessage(lb, LB_SETCURSEL, sel, 0L);
    SetDlgItemInt(dlg, IDC_CUSTW, w ? w : 1024, FALSE);
    SetDlgItemInt(dlg, IDC_CUSTH, h ? h : 768, FALSE);
    if (vi->dispi_id)
        SetDlgItemText(dlg, IDC_DETAILS, "Custom size: emulator adapter");
    else {
        SetDlgItemText(dlg, IDC_DETAILS, "Custom size needs an emulator");
        EnableWindow(GetDlgItem(dlg, IDC_CUSTOM), FALSE);
    }
    fill_depths(dlg, d);

    cb = GetDlgItem(dlg, IDC_REFRESH);
    hz = ini_int("Refresh", 0);
    can_refresh = vi->ver >= 0x300;
    for (i = 0; i < NREFRESH && can_refresh; i++) {
        int v = refresh_vals[i];
        if (v == 0) strcpy(b, "BIOS default");
        else if (v == 1) strcpy(b, "Monitor native");
        else {
            sprintf(b, "%d Hz", v);
            if (vi->range_ok && (v > vi->vmax || v < vi->vmin)) strcat(b, " (!)");
        }
        SendMessage(cb, XB_ADD, 0, (LONG)(LPSTR)b);
        if (v == hz) SendMessage(cb, XB_SET, i, 0L);
    }
    if (!can_refresh)       /* VBE 1.x/2.x BIOS (also VirtualBox, Bochs): no CRTC timing control */
        SendMessage(cb, XB_ADD, 0, (LONG)(LPSTR)(vi->dispi_id ? "Fixed (virtual)" : "Fixed (needs VBE 3)"));
    if (SendMessage(cb, XB_GET, 0, 0L) < 0) SendMessage(cb, XB_SET, 0, 0L);

    cb = GetDlgItem(dlg, IDC_TIMING);
    if (can_refresh) {
        SendMessage(cb, XB_ADD, 0, (LONG)(LPSTR)"CRT (VESA CVT)");
        SendMessage(cb, XB_ADD, 0, (LONG)(LPSTR)"LCD (CVT-RB)");
        i = ini_int("Timing", vi->edid_ok && vi->digital ? 1 : 0);
        SendMessage(cb, XB_SET, i ? 1 : 0, 0L);
    } else {
        SendMessage(cb, XB_ADD, 0, (LONG)(LPSTR)"Not used");
        SendMessage(cb, XB_SET, 0, 0L);
    }

    cb = GetDlgItem(dlg, IDC_ACCESS);
    SendMessage(cb, XB_ADD, 0, (LONG)(LPSTR)"Automatic");
    acc = ini_int("Access", 0);
#ifdef OLDWIN
    /* Windows 1.x/2.x run in real mode: no linear frame buffer */
    SendMessage(cb, XB_ADD, 0, (LONG)(LPSTR)"VGA window");
    SendMessage(cb, XB_SET, acc == 2 ? 1 : 0, 0L);
    CheckDlgButton(dlg, IDC_DEBUG, ini_int("Debug", 0) ? 1 : 0);
    update_enables(dlg);
}
#else
    SendMessage(cb, XB_ADD, 0, (LONG)(LPSTR)"Linear (LFB)");
    SendMessage(cb, XB_ADD, 0, (LONG)(LPSTR)"VGA window");
    SendMessage(cb, XB_SET, acc <= 2 ? acc : 0, 0L);

    dpi = ini_int("DPI", 0);
    if (!dpi) {                     /* same rule as the driver: follow the system font */
        char f[20];
        GetPrivateProfileString("boot", "fonts.fon", "", f, sizeof(f), sysini);
        dpi = (strstr(f, "8514") || strstr(f, "wvb") || strstr(f, "WVB")) ? 120 : 96;
    }
    CheckRadioButton(dlg, IDC_SMALL, IDC_LARGE, dpi >= 110 ? IDC_LARGE : IDC_SMALL);
    CheckDlgButton(dlg, IDC_DEBUG, ini_int("Debug", 0) ? 1 : 0);
    GetPrivateProfileString("boot", "display.drv", "", b, sizeof(b), sysini);
    CheckDlgButton(dlg, IDC_ENABLE, !stricmp(b, "winvbe.drv"));
    if (win9x) {
        /* Windows 9x: the driver is installed in Device Manager and the font
           size is chosen in Display Properties */
        char cls[80];
        GetPrivateProfileString("boot", "fonts.fon", "", b, sizeof(b), sysini);
        CheckRadioButton(dlg, IDC_SMALL, IDC_LARGE, strstr(b, "8514") ? IDC_LARGE : IDC_SMALL);
        EnableWindow(GetDlgItem(dlg, IDC_SMALL), FALSE);
        EnableWindow(GetDlgItem(dlg, IDC_LARGE), FALSE);
        CheckDlgButton(dlg, IDC_ENABLE, winvbe_class_key(cls));
        EnableWindow(GetDlgItem(dlg, IDC_ENABLE), FALSE);
        SetDlgItemText(dlg, IDC_ENABLE, "Installed in Device Manager");
    }
    update_enables(dlg);
}
#endif

#ifndef OLDWIN
/* Windows 9x: the mode save_settings wrote for the Display control panel */
static int live_w, live_h, live_bpp;

/* Windows 95/98/ME: USER32's ChangeDisplaySettingsA (through KERNEL's 16-to-32
   bit calls: LoadLibraryEx32W, GetProcAddress32W, CallProc32W) with
   CDS_UPDATEREGISTRY - the new resolution and colour depth at once, as
   games and DirectDraw change them. 0 = done, 1 = Windows wants a restart,
   -1 = not possible here */
static int change_display_9x(int w, int h, int bpp)
{
    typedef DWORD (FAR PASCAL *PLL32)(LPCSTR, DWORD, DWORD);
    typedef DWORD (FAR PASCAL *PGPA32)(DWORD, LPCSTR);
    typedef DWORD (FAR PASCAL *PCP32_2)(DWORD, DWORD, DWORD, DWORD, DWORD);
    static BYTE dm[156];                    /* DEVMODEA of Windows 95 */
    HMODULE k = GetModuleHandle("KERNEL");
    PLL32 ll = (PLL32)GetProcAddress(k, MAKEINTRESOURCE(513));
    PGPA32 gpa = (PGPA32)GetProcAddress(k, MAKEINTRESOURCE(515));
    PCP32_2 cp = (PCP32_2)GetProcAddress(k, MAKEINTRESOURCE(517));
    DWORD u32, fn, r;
    if (!ll || !gpa || !cp || w <= 0 || h <= 0 || bpp <= 0) return -1;
    u32 = ll("USER32.DLL", 0, 0);
    if (!u32) return -1;
    fn = gpa(u32, "ChangeDisplaySettingsA");
    if (!fn) return -1;
    memset(dm, 0, sizeof(dm));
    *(WORD *)(dm + 32) = 0x0400;            /* dmSpecVersion */
    *(WORD *)(dm + 36) = sizeof(dm);        /* dmSize */
    *(DWORD *)(dm + 40) = 0x00040000UL | 0x00080000UL | 0x00100000UL;  /* BITSPERPEL, PELSWIDTH, PELSHEIGHT */
    *(DWORD *)(dm + 104) = (DWORD)bpp;
    *(DWORD *)(dm + 108) = (DWORD)w;
    *(DWORD *)(dm + 112) = (DWORD)h;
    /* the DEVMODE (parameter 1 of 2, bit 1) is a 16:16 pointer to convert */
    r = cp((DWORD)(LPVOID)dm, 1 /* CDS_UPDATEREGISTRY */, fn, 2, 2);
    return r == 0 ? 0 : 1;
}

/* the [WINVBE] keys save_settings writes that the driver reads only when
   Windows starts (everything but the mode) */
static char *const start_keys[] = { "Mode", "Refresh", "Timing", "Access", "Debug" };
static const int start_defs[] = { 0, 0, 2, 0, 0 };     /* the driver's defaults */
#define NSTART (sizeof(start_keys) / sizeof(start_keys[0]))
static int start_vals[NSTART];

/* as the driver reads them (a missing key is its default; Mode=0x13 counts
   as a start setting of its own) */
static void snap_start_keys(int *vals)
{
    int i;
    for (i = 0; i < (int)NSTART; i++) {
        char b[16];
        GetPrivateProfileString(sect, start_keys[i], "", b, sizeof(b), sysini);
        vals[i] = !b[0] ? start_defs[i] : (b[0] == '0' && (b[1] == 'x' || b[1] == 'X')) ? (int)strtol(b, NULL, 16)
                                                                                        : atoi(b);
    }
    if (vals[2] == 2)                   /* Timing: automatic = by the EDID */
        vals[2] = vi && vi->edid_ok && vi->digital ? 1 : 0;
}
#endif

static BOOL save_settings(HWND dlg)
{
    int sel = (int)SendMessage(GetDlgItem(dlg, IDC_MODES), LB_GETCURSEL, 0, 0L);
    int w = 0, h = 0, d = cur_depth(dlg);
    int r = (int)SendMessage(GetDlgItem(dlg, IDC_REFRESH), XB_GET, 0, 0L);
#ifndef OLDWIN
    int large;
    char *fs, *ff, *fo;
#endif

    if (IsDlgButtonChecked(dlg, IDC_CUSTOM)) {
        BOOL ok1, ok2;
        w = GetDlgItemInt(dlg, IDC_CUSTW, &ok1, FALSE);
        h = GetDlgItemInt(dlg, IDC_CUSTH, &ok2, FALSE);
        if (!ok1 || !ok2 || w < 320 || h < 200 || w > 4096 || h > 4096) {
            MessageBox(dlg, "Custom size must be between 320x200 and 4096x4096.", "WINVBE", MB_OK | MB_ICONEXCLAMATION);
            return FALSE;
        }
    } else if (sel >= 2 && sel - 2 < nres) {
        w = res[sel - 2].w; h = res[sel - 2].h;
    }
    ini_put_int("Width", w);
    ini_put_int("Height", h);
    ini_put_int("Depth", d);
    ini_put_str("Mode", sel == 1 ? "0x13" : "0");
    if (can_refresh) {
        ini_put_int("Refresh", r > 0 ? refresh_vals[r] : 0);
        ini_put_int("Timing", (int)SendMessage(GetDlgItem(dlg, IDC_TIMING), XB_GET, 0, 0L));
    }
#ifdef OLDWIN
    ini_put_int("Access", SendMessage(GetDlgItem(dlg, IDC_ACCESS), XB_GET, 0, 0L) == 1 ? 2 : 0);
    ini_put_int("Debug", IsDlgButtonChecked(dlg, IDC_DEBUG) ? 1 : 0);
    return TRUE;
#else
    ini_put_int("Access", (int)SendMessage(GetDlgItem(dlg, IDC_ACCESS), XB_GET, 0, 0L));
    ini_put_int("Debug", IsDlgButtonChecked(dlg, IDC_DEBUG) ? 1 : 0);

    if (win9x) {
        /* the same setting for the Display control panel, which Windows 9x
           hands to the driver; "Automatic" is written as the size it gives */
        char v[24];
        int rw = w, rh = h, rd = d ? d : 16;
        if (sel == 1) { rw = 320; rh = 200; rd = 8; }
        if (!rw || !rh) {
            rw = vi->edid_ok && vi->natw ? vi->natw : 1024;
            rh = vi->edid_ok && vi->nath ? vi->nath : 768;
        }
        sprintf(v, "%d,%d", rw, rh);
        live_w = rw; live_h = rh; live_bpp = rd == 15 ? 16 : rd;
        reg_put(W9_HKCC, "Display\\Settings", "Resolution", v);
        sprintf(v, "%d", rd == 15 ? 16 : rd);
        reg_put(W9_HKCC, "Display\\Settings", "BitsPerPixel", v);
        write_modes(IsDlgButtonChecked(dlg, IDC_CUSTOM) ? w : 0, IsDlgButtonChecked(dlg, IDC_CUSTOM) ? h : 0);
        return TRUE;
    }

    large = IsDlgButtonChecked(dlg, IDC_LARGE);
    copy_own_fonts();
    {
        int ms = file_exists("8514SYS.FON") && file_exists("8514FIX.FON") && file_exists("8514OEM.FON");
        int own = file_exists("WVBSYS.FON") && file_exists("WVBFIX.FON") && file_exists("WVBOEM.FON");
        if (large && !ms && !own) {
            MessageBox(dlg, "The large fonts (WVBSYS.FON, WVBFIX.FON, WVBOEM.FON) were not found. Run "
                            "WINVBECF.EXE from the WINVBE folder or install WINVBE with INSTALL.EXE. "
                            "Small fonts will be used.", "WINVBE", MB_OK | MB_ICONEXCLAMATION);
            large = 0;
        }
        /* Microsoft's 8514 fonts when Windows has them, else WINVBE's own */
        if (large && ms) { fs = "8514sys.fon"; ff = "8514fix.fon"; fo = "8514oem.fon"; }
        else if (large) { fs = "wvbsys.fon"; ff = "wvbfix.fon"; fo = "wvboem.fon"; }
        else { fs = "vgasys.fon"; ff = "vgafix.fon"; fo = "vgaoem.fon"; }
    }
    ini_put_int("DPI", large ? 120 : 96);
    if (IsDlgButtonChecked(dlg, IDC_ENABLE)) {
        char cur[32];
        GetPrivateProfileString("boot", "display.drv", "", cur, sizeof(cur), sysini);
        if (!install_files(dlg, !stricmp(cur, "winvbe.drv"))) return FALSE;
        WritePrivateProfileString("boot", "fonts.fon", fs, sysini);
        WritePrivateProfileString("boot", "fixedfon.fon", ff, sysini);
        WritePrivateProfileString("boot", "oemfonts.fon", fo, sysini);
        WritePrivateProfileString("boot", "display.drv", "winvbe.drv", sysini);
        /* Windows 3.1 names the VGA grabbers *.2GR / *.3GR, Windows 3.0 *.GR2 / *.GR3 */
        WritePrivateProfileString("boot", "286grabber",
                                  file_exists("VGACOLOR.2GR") ? "vgacolor.2gr" : "vgacolor.gr2", sysini);
        WritePrivateProfileString("boot", "386grabber",
                                  file_exists("VGA.3GR") ? "vga.3gr" : "vga.gr3", sysini);
        WritePrivateProfileString("386Enh", "display", "*vddvga", sysini);
        WritePrivateProfileString("boot.description", "display.drv", "WINVBE Universal VESA/VGA/SVGA", sysini);
    } else {
        char b[32];
        GetPrivateProfileString("boot", "display.drv", "", b, sizeof(b), sysini);
        if (!stricmp(b, "winvbe.drv")) {
            WritePrivateProfileString("boot", "display.drv", "vga.drv", sysini);
            WritePrivateProfileString("boot", "fonts.fon", "vgasys.fon", sysini);
            WritePrivateProfileString("boot", "fixedfon.fon", "vgafix.fon", sysini);
            WritePrivateProfileString("boot", "oemfonts.fon", "vgaoem.fon", sysini);
            WritePrivateProfileString("boot.description", "display.drv", "VGA", sysini);
        }
    }
    return TRUE;
#endif
}

BOOL FAR PASCAL __export AboutProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_INITDIALOG) return TRUE;
    if (msg == WM_COMMAND && (wp == IDOK || wp == IDCANCEL)) { EndDialog(dlg, 0); return TRUE; }
    return FALSE;
}

/* ---------------- WINVBE Mouse (WINVBECF /MOUSE) ----------------
 * Pointer speed, double-click speed and swapped buttons as Windows keeps
 * them (WIN.INI [windows]; SystemParametersInfo from Windows 3.1 on), and
 * the color and size of the pointer WINVBE draws ([WINVBE] CursorColor,
 * CursorSize; the driver takes them at once through WINVBE_CURSOR).
 */
#define WINVBE_CURSOR 0x7703
static const int ms_th1[5] = { 0, 10, 6, 4, 2 }, ms_th2[5] = { 0, 0, 10, 6, 4 }, ms_acc[5] = { 0, 1, 1, 2, 2 };
static const char *ms_names[5] = { "Slowest", "Slow", "Normal", "Fast", "Fastest" };
static const int md_ms[3] = { 900, 500, 300 };
static const char *md_names[3] = { "Slow", "Normal", "Fast" };
static const char *mc_names[8] = { "Standard (white)", "Black", "Red", "Green", "Blue", "Yellow", "Magenta", "Cyan" };
#ifndef OLDWIN
typedef BOOL (FAR PASCAL *PSPI)(UINT, UINT, LPVOID, UINT);
static PSPI get_spi(void)
{
    HMODULE hu = GetModuleHandle("USER");
    return hu ? (PSPI)GetProcAddress(hu, "SystemParametersInfo") : NULL;
}
#endif

static void mouse_list(HWND dlg, int id, const char **names, int n, int sel)
{
    int i;
    SendDlgItemMessage(dlg, id, LB_RESETCONTENT, 0, 0L);
    for (i = 0; i < n; i++) SendDlgItemMessage(dlg, id, LB_ADDSTRING, 0, (LPARAM)(LPSTR)names[i]);
    SendDlgItemMessage(dlg, id, LB_SETCURSEL, sel, 0L);
}

static int driver_cursor_esc(void)
{
    HDC dc = GetDC(NULL);
    WORD esc = WINVBE_CURSOR;
    int r = Escape(dc, QUERYESCSUPPORT, sizeof(WORD), (LPSTR)&esc, NULL) > 0;
    ReleaseDC(NULL, dc);
    return r;
}

static void mouse_init(HWND dlg)
{
    int th1, th2, acc, i, best = 2, d, bd = 32767, dbl;
    th1 = GetProfileInt("windows", "MouseThreshold1", 6);
    th2 = GetProfileInt("windows", "MouseThreshold2", 10);
    acc = GetProfileInt("windows", "MouseSpeed", 1);
#ifndef OLDWIN
    {
        PSPI spi = get_spi();
        int m[3];
        if (spi && spi(3 /* SPI_GETMOUSE */, 0, (LPVOID)m, 0)) { th1 = m[0]; th2 = m[1]; acc = m[2]; }
    }
#endif
    for (i = 0; i < 5; i++) {
        d = (acc - ms_acc[i]) * 8;
        if (d < 0) d = -d;
        d += (th1 > ms_th1[i] ? th1 - ms_th1[i] : ms_th1[i] - th1);
        if (d < bd) { bd = d; best = i; }
    }
    mouse_list(dlg, IDC_MSPEED, ms_names, 5, best);
    dbl = GetProfileInt("windows", "DoubleClickSpeed", 452);
    mouse_list(dlg, IDC_MDBL, md_names, 3, dbl >= 700 ? 0 : dbl >= 400 ? 1 : 2);
    i = ini_int("CursorColor", 0);
    mouse_list(dlg, IDC_MCOLOR, mc_names, 8, i >= 0 && i < 8 ? i : 0);
    CheckDlgButton(dlg, IDC_MSWAP, GetProfileInt("windows", "SwapMouseButtons", 0) != 0);
    CheckDlgButton(dlg, IDC_MLARGE, ini_int("CursorSize", 1) == 2);
#ifdef OLDWIN
    /* the driver of Windows 1.x/2.x keeps no buffer for a large pointer */
    CheckDlgButton(dlg, IDC_MLARGE, 0);
    EnableWindow(GetDlgItem(dlg, IDC_MLARGE), FALSE);
    SetDlgItemText(dlg, IDC_MNOTE, driver_cursor_esc() ?
                   "The pointer color applies to the pointer WINVBE draws." :
                   "WINVBE is not the active display driver: the pointer color takes effect with it.");
#else
    SetDlgItemText(dlg, IDC_MNOTE, driver_cursor_esc() ?
                   "The pointer color and size apply to the pointer WINVBE draws." :
                   "WINVBE is not the active display driver: the pointer color and size take effect with it.");
#endif
}

static void mouse_save(HWND dlg)
{
    char b[16];
    int sp = (int)SendDlgItemMessage(dlg, IDC_MSPEED, LB_GETCURSEL, 0, 0L);
    int db = (int)SendDlgItemMessage(dlg, IDC_MDBL, LB_GETCURSEL, 0, 0L);
    int co = (int)SendDlgItemMessage(dlg, IDC_MCOLOR, LB_GETCURSEL, 0, 0L);
    int sw = IsDlgButtonChecked(dlg, IDC_MSWAP);
    int lg = IsDlgButtonChecked(dlg, IDC_MLARGE);
    if (sp < 0 || sp > 4) sp = 2;
    if (db < 0 || db > 2) db = 1;
    if (co < 0 || co > 7) co = 0;
    sprintf(b, "%d", ms_th1[sp]); WriteProfileString("windows", "MouseThreshold1", b);
    sprintf(b, "%d", ms_th2[sp]); WriteProfileString("windows", "MouseThreshold2", b);
    sprintf(b, "%d", ms_acc[sp]); WriteProfileString("windows", "MouseSpeed", b);
    sprintf(b, "%d", md_ms[db]); WriteProfileString("windows", "DoubleClickSpeed", b);
    WriteProfileString("windows", "SwapMouseButtons", sw ? "1" : "0");
#ifndef OLDWIN
    {
        PSPI spi = get_spi();
        int m[3];
        m[0] = ms_th1[sp]; m[1] = ms_th2[sp]; m[2] = ms_acc[sp];
        if (spi) spi(4 /* SPI_SETMOUSE */, 0, (LPVOID)m, 0);
        SetDoubleClickTime(md_ms[db]);
        SwapMouseButton(sw);
    }
#endif
    ini_put_int("CursorColor", co);
    ini_put_int("CursorSize", lg ? 2 : 1);
    if (driver_cursor_esc()) {
        HDC dc = GetDC(NULL);
        WORD a[2];
        a[0] = co; a[1] = lg ? 2 : 1;
        Escape(dc, WINVBE_CURSOR, sizeof(a), (LPSTR)a, NULL);
        ReleaseDC(NULL, dc);
    }
}

BOOL FAR PASCAL __export MouseProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        mouse_init(dlg);
        return TRUE;
    case WM_COMMAND:
        switch (wp) {
#ifdef OLDWIN
        case IDC_MSWAP:
        case IDC_MLARGE:
            CheckDlgButton(dlg, wp, !IsDlgButtonChecked(dlg, wp));
            return TRUE;
#endif
        case IDOK:
            mouse_save(dlg);
#ifdef OLDWIN
            MessageBox(dlg, "Saved. The speeds and the buttons take effect when Windows is started again.",
                       "WINVBE Mouse", MB_OK | MB_ICONINFORMATION);
#endif
            EndDialog(dlg, 1);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void mouse_dialog(HWND parent)
{
    FARPROC fp = MakeProcInstance((FARPROC)MouseProc, hInst);
    DialogBox(hInst, MAKEINTRESOURCE(IDD_MOUSE), parent, (DLGPROC)fp);
    FreeProcInstance(fp);
}

#ifndef OLDWIN
/* ---------------- WINVBE Colors (gamma, brightness, contrast) ----------
 * [WINVBE] Gamma (1/100), Brightness (-100 .. 100), Contrast (percent).
 * The driver takes them at once through WINVBE_COLOR: in 256 colours
 * through the palette, in high and true colour when it draws itself with
 * the shadow frame buffer; with the DIB engine of Windows 9x in high or
 * true colour they take effect after a restart (WINVBE then draws without
 * the DIB engine, and DirectDraw has no HAL).
 */
#define WINVBE_COLOR 0x7704
static int c_gamma, c_bright, c_contr, c0_gamma, c0_bright, c0_contr, c_live;
/* [WINVBE] Rotate (0, 90, 180, 270) and Scale (0 off, 1 aspect kept, 2 full
   screen): the desktop turned on the mode, or a smaller resolution shown on
   the native mode of the panel; both at the next start of Windows */
static int c_rot, c_scale, c0_rot, c0_scale;
static const char *c_rot_names[4] = { "No", "90 degrees clockwise", "180 degrees", "270 degrees (90 counter-clockwise)" };
static const char *c_scale_names[3] = { "As the video BIOS sets them", "Scaled to the native mode (aspect kept)",
                                        "Scaled to the native mode (full screen)" };

/* the driver's curve (drv/surf.c), for the preview when the screen does
   not show it yet */
static long c_lg2(unsigned long v)
{
    long r = 0;
    int k;
    while (v >= 0x20000UL) { v >>= 1; r += 0x10000L; }
    while (v < 0x10000UL) { v <<= 1; r -= 0x10000L; }
    v >>= 1;
    for (k = 15; k >= 0; k--) {
        v = (v * v) >> 15;
        if (v >= 0x10000UL) { v >>= 1; r += 1L << k; }
    }
    return r;
}

static unsigned long c_ex2(long e)
{
    static const unsigned t[15] = { 46341, 38968, 35734, 34219, 33486, 33125, 32946, 32857,
                                    32813, 32790, 32779, 32774, 32771, 32769, 32769 };
    long ip = e >> 16;
    unsigned f = (unsigned)(e & 0xFFFF);
    unsigned long r = 0x8000UL;
    int k;
    for (k = 0; k < 15; k++)
        if (f & (0x8000U >> k)) r = (r * t[k]) >> 15;
    r <<= 1;
    if (ip < -31) return 0;
    return ip < 0 ? r >> (int)-ip : r;
}

static int c_curve(int i)
{
    long y;
    if (!i) y = 0;
    else if (c_gamma == 100) y = ((unsigned long)i << 16) / 255;
    else y = (long)c_ex2(c_lg2(((unsigned long)i << 16) / 255) * 100 / (long)c_gamma);
    y = (y - 32768L) * (long)c_contr / 100 + 32768L + (long)c_bright * 65536L / 200;
    if (y < 0) y = 0;
    if (y > 65536L) y = 65536L;
    return (int)((y * 255 + 32768L) >> 16);
}

/* 1 applied, 2 saved for later, 0 WINVBE not active */
static int c_send(void)
{
    HDC dc = GetDC(NULL);
    WORD esc = WINVBE_COLOR, a[3];
    int r = 0;
    if (Escape(dc, QUERYESCSUPPORT, sizeof(WORD), (LPSTR)&esc, NULL) > 0) {
        a[0] = c_gamma; a[1] = c_bright + 1000; a[2] = c_contr;
        r = Escape(dc, WINVBE_COLOR, sizeof(a), (LPSTR)a, NULL);
    }
    ReleaseDC(NULL, dc);
    return r;
}

static void c_show(HWND dlg)
{
    char b[16];
    sprintf(b, "%d.%02d", c_gamma / 100, c_gamma % 100); SetDlgItemText(dlg, IDC_CGAMMAV, b);
    sprintf(b, "%+d", c_bright); SetDlgItemText(dlg, IDC_CBRIGHTV, b);
    sprintf(b, "%d %%", c_contr); SetDlgItemText(dlg, IDC_CCONTRV, b);
    SetScrollPos(GetDlgItem(dlg, IDC_CGAMMA), SB_CTL, c_gamma, TRUE);
    SetScrollPos(GetDlgItem(dlg, IDC_CBRIGHT), SB_CTL, c_bright, TRUE);
    SetScrollPos(GetDlgItem(dlg, IDC_CCONTR), SB_CTL, c_contr, TRUE);
    SetDlgItemText(dlg, IDC_CNOTE,
        c_live == 1 ? "The screen shows the settings as they are changed." :
        c_live == 2 ? "In this color depth the settings take effect when Windows is started again "
                      "(on Windows 95/98/ME WINVBE then draws without the DIB engine, and DirectDraw "
                      "has no acceleration). The preview shows them." :
                      "WINVBE is not the active display driver: the settings take effect with it. "
                      "The preview shows them.");
    InvalidateRect(GetDlgItem(dlg, IDC_CPREVIEW), NULL, FALSE);
}

static void c_preview(DRAWITEMSTRUCT FAR *di)
{
    static const BYTE bars[6][3] = { {255,0,0}, {0,255,0}, {0,0,255}, {255,255,0}, {0,255,255}, {255,0,255} };
    RECT r = di->rcItem, c;
    int w = r.right - r.left, h = r.bottom - r.top, i;
    for (i = 0; i < 16; i++) {
        HBRUSH br;
        int g = i * 17;
        if (c_live != 1) g = c_curve(g);
        c.left = r.left + w * i / 16; c.right = r.left + w * (i + 1) / 16;
        c.top = r.top; c.bottom = r.top + h / 2;
        br = CreateSolidBrush(RGB(g, g, g));
        FillRect(di->hDC, &c, br);
        DeleteObject(br);
    }
    for (i = 0; i < 6; i++) {
        HBRUSH br;
        int k[3], j;
        for (j = 0; j < 3; j++) k[j] = c_live != 1 ? c_curve(bars[i][j]) : bars[i][j];
        c.left = r.left + w * i / 6; c.right = r.left + w * (i + 1) / 6;
        c.top = r.top + h / 2; c.bottom = r.bottom;
        br = CreateSolidBrush(RGB(k[0], k[1], k[2]));
        FillRect(di->hDC, &c, br);
        DeleteObject(br);
    }
}

static void c_scroll(HWND dlg, HWND bar, WPARAM code, int pos)
{
    int id = GetDlgCtrlID(bar), *v, lo, hi, step;
    if (id == IDC_CGAMMA) { v = &c_gamma; lo = 30; hi = 300; step = 5; }
    else if (id == IDC_CBRIGHT) { v = &c_bright; lo = -100; hi = 100; step = 5; }
    else if (id == IDC_CCONTR) { v = &c_contr; lo = 20; hi = 300; step = 5; }
    else return;
    switch (code) {
    case SB_LINEUP: *v -= 1; break;
    case SB_LINEDOWN: *v += 1; break;
    case SB_PAGEUP: *v -= step * 4; break;
    case SB_PAGEDOWN: *v += step * 4; break;
    case SB_THUMBTRACK: case SB_THUMBPOSITION: *v = pos; break;
    case SB_TOP: *v = lo; break;
    case SB_BOTTOM: *v = hi; break;
    case SB_ENDSCROLL: c_live = c_send(); c_show(dlg); return;
    default: return;
    }
    if (*v < lo) *v = lo;
    if (*v > hi) *v = hi;
    /* the screen follows when the thumb is let go (a redraw of the whole
       screen in high colour) */
    if (code != SB_THUMBTRACK) c_live = c_send();
    c_show(dlg);
}

BOOL FAR PASCAL __export ColorProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        c0_gamma = c_gamma = ini_int("Gamma", 100);
        c0_bright = c_bright = ini_int("Brightness", 0);
        c0_contr = c_contr = ini_int("Contrast", 100);
        if (c_gamma < 30 || c_gamma > 300) c_gamma = 100;
        if (c_bright < -100 || c_bright > 100) c_bright = 0;
        if (c_contr < 20 || c_contr > 300) c_contr = 100;
        {
            int i, r = ini_int("Rotate", 0);
            c0_rot = c_rot = r == 90 ? 1 : r == 180 ? 2 : r == 270 ? 3 : 0;
            c0_scale = c_scale = ini_int("Scale", 0);
            if (c_scale < 0 || c_scale > 2) c0_scale = c_scale = 0;
            for (i = 0; i < 4; i++) SendDlgItemMessage(dlg, IDC_CROTATE, CB_ADDSTRING, 0, (LPARAM)(LPSTR)c_rot_names[i]);
            for (i = 0; i < 3; i++) SendDlgItemMessage(dlg, IDC_CSCALE, CB_ADDSTRING, 0, (LPARAM)(LPSTR)c_scale_names[i]);
            SendDlgItemMessage(dlg, IDC_CROTATE, CB_SETCURSEL, c_rot, 0);
            SendDlgItemMessage(dlg, IDC_CSCALE, CB_SETCURSEL, c_scale, 0);
        }
        SetScrollRange(GetDlgItem(dlg, IDC_CGAMMA), SB_CTL, 30, 300, FALSE);
        SetScrollRange(GetDlgItem(dlg, IDC_CBRIGHT), SB_CTL, -100, 100, FALSE);
        SetScrollRange(GetDlgItem(dlg, IDC_CCONTR), SB_CTL, 20, 300, FALSE);
        c_live = c_send();
        c_show(dlg);
        return TRUE;
    case WM_HSCROLL:
        c_scroll(dlg, (HWND)HIWORD(lp), wp, (int)LOWORD(lp));
        return TRUE;
    case WM_DRAWITEM:
        if (wp == IDC_CPREVIEW) { c_preview((DRAWITEMSTRUCT FAR *)lp); return TRUE; }
        break;
    case WM_COMMAND:
        switch (wp) {
        case IDC_CDEFAULT:
            c_gamma = 100; c_bright = 0; c_contr = 100;
            c_rot = c_scale = 0;
            SendDlgItemMessage(dlg, IDC_CROTATE, CB_SETCURSEL, 0, 0);
            SendDlgItemMessage(dlg, IDC_CSCALE, CB_SETCURSEL, 0, 0);
            c_live = c_send();
            c_show(dlg);
            return TRUE;
        case IDOK:
            ini_put_int("Gamma", c_gamma);
            ini_put_int("Brightness", c_bright);
            ini_put_int("Contrast", c_contr);
            c_rot = (int)SendDlgItemMessage(dlg, IDC_CROTATE, CB_GETCURSEL, 0, 0);
            c_scale = (int)SendDlgItemMessage(dlg, IDC_CSCALE, CB_GETCURSEL, 0, 0);
            if (c_rot < 0) c_rot = 0;
            if (c_scale < 0) c_scale = 0;
            if (c_rot != c0_rot || c_scale != c0_scale) {
                ini_put_int("Rotate", c_rot * 90);
                ini_put_int("Scale", c_scale);
                if (MessageBox(dlg, "Turning and scaling take effect when Windows is started again.\n\n"
                                    "Restart Windows now?", "WINVBE", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    EndDialog(dlg, 1);
                    ExitWindows(EW_RESTART, 0);
                    return TRUE;
                }
            }
            EndDialog(dlg, 1);
            return TRUE;
        case IDCANCEL:
            c_gamma = c0_gamma; c_bright = c0_bright; c_contr = c0_contr;
            c_send();
            EndDialog(dlg, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void color_dialog(HWND parent)
{
    FARPROC fp = MakeProcInstance((FARPROC)ColorProc, hInst);
    DialogBox(hInst, MAKEINTRESOURCE(IDD_COLOR), parent, (DLGPROC)fp);
    FreeProcInstance(fp);
}
#endif

#ifndef OLDWIN
/* ---------------- shortcuts (WINVBECF /SETUP, /UNINSTALL) ----------------
 * A Program Manager group "WINVBE" (Windows 3.x; the Start menu folder of
 * that name on Windows 95/98/ME, whose shell takes the same DDE commands)
 * with WINVBE Display Settings, WINVBE Mouse and - only where there is no
 * Add/Remove Programs, i.e. Windows 3.x - Uninstall WINVBE. INSTALL.EXE
 * puts "WINVBECF /SETUP" in WIN.INI run= (it removes itself), the Windows
 * 9x INF in RunOnce.
 */
#include <dde.h>
static HWND dde_server;
static BYTE dde_acked;

LRESULT FAR PASCAL __export DdeWndProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_DDE_ACK) {
        if (!dde_server) {
            dde_server = (HWND)wp;
            GlobalDeleteAtom(LOWORD(lp));
            GlobalDeleteAtom(HIWORD(lp));
        } else {
            dde_acked = 1;
            if (HIWORD(lp)) GlobalFree((HGLOBAL)HIWORD(lp));
        }
        return 0;
    }
    return DefWindowProc(w, m, wp, lp);
}

static void dde_wait(HWND w, BYTE *flag, DWORD ms)
{
    DWORD t = GetTickCount();
    MSG msg;
    while (!*flag && GetTickCount() - t < ms) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        Yield();
    }
    (void)w;
}

/* the commands to Program Manager ([CreateGroup(...)][AddItem(...)]...) */
static int progman_exec(const char *cmds)
{
    WNDCLASS wc;
    HWND w;
    ATOM a1, a2;
    HGLOBAL hc;
    LPSTR p;
    BYTE got = 0;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DdeWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "WinvbeDde";
    RegisterClass(&wc);
    w = CreateWindow("WinvbeDde", "", WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!w) return 0;
    dde_server = NULL; dde_acked = 0;
    a1 = GlobalAddAtom("PROGMAN"); a2 = GlobalAddAtom("PROGMAN");
    SendMessage((HWND)0xFFFF, WM_DDE_INITIATE, (WPARAM)w, MAKELPARAM(a1, a2));
    GlobalDeleteAtom(a1); GlobalDeleteAtom(a2);
    if (!dde_server && !win9x) {
        /* Program Manager not running yet: start it, then ask again */
        WinExec("PROGMAN.EXE", SW_SHOWMINNOACTIVE);
        a1 = GlobalAddAtom("PROGMAN"); a2 = GlobalAddAtom("PROGMAN");
        SendMessage((HWND)0xFFFF, WM_DDE_INITIATE, (WPARAM)w, MAKELPARAM(a1, a2));
        GlobalDeleteAtom(a1); GlobalDeleteAtom(a2);
    }
    if (dde_server) {
        hc = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, lstrlen(cmds) + 1);
        if (hc) {
            p = GlobalLock(hc);
            lstrcpy(p, cmds);
            GlobalUnlock(hc);
            if (PostMessage(dde_server, WM_DDE_EXECUTE, (WPARAM)w, MAKELPARAM(0, hc))) {
                dde_wait(w, &dde_acked, 10000);
                got = dde_acked;
            } else
                GlobalFree(hc);
        }
        PostMessage(dde_server, WM_DDE_TERMINATE, (WPARAM)w, 0L);
    }
    DestroyWindow(w);
    return got;
}

static void my_path(char *out, int n)
{
    GetModuleFileName(hInst, out, n);
}

static void shortcuts_setup(void)
{
    static char cmds[1024];
    char me[144];
    /* Windows 3.0's Program Manager does not wrap icon titles: short ones */
    int w30 = (GetVersion() & 0xFFFF) == 0x0003;
    my_path(me, sizeof(me));
    if (!win9x)     /* Program Manager would add a second set of items */
        progman_exec("[DeleteGroup(WINVBE)]");
    /* Windows 3.0's Program Manager saves a group file of odd size with a
       wrong checksum ("invalid or damaged" at the next start). The path
       of WINVBECF is in the group six times, so only the fixed text
       decides: " /DISPLAY" (ignored) makes the size even */
    sprintf(cmds, "[CreateGroup(WINVBE)][ShowGroup(WINVBE,1)]"
                  "[AddItem(%s%s,%s)]"
                  "[AddItem(%s /MOUSE,%s,%s,0)]", me, w30 ? " /DISPLAY" : "",
                  w30 ? "Display" : "WINVBE Display Settings",
                  me, w30 ? "Mouse" : "WINVBE Mouse", me);
    if (!win9x)
        sprintf(cmds + strlen(cmds), "[AddItem(%s /UNINSTALL,%s,%s,1)]", me,
                w30 ? "Uninstall" : "Uninstall WINVBE", me);
    progman_exec(cmds);
}

/* WIN.INI run=: take out "...WINVBECF.EXE /SETUP" */
static void run_entry_remove(void)
{
    char r[256], o[256], *p, *q;
    GetProfileString("windows", "run", "", r, sizeof(r));
    o[0] = 0;
    for (p = r; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        q = p;
        while (*q && *q != ' ') q++;
        if (!(q - p >= 12 && !strnicmp(q - 12, "WINVBECF.EXE", 12)) && !(q - p >= 8 && !strnicmp(q - 8, "WINVBECF", 8))) {
            if (o[0]) strcat(o, " ");
            strncat(o, p, q - p);
        }
        p = q;
    }
    WriteProfileString("windows", "run", o);
}

static int uninstall(void)
{
    char b[32];
    if (MessageBox(NULL, "Remove WINVBE? Windows will use its standard VGA display driver again.",
                   "Uninstall WINVBE", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
    GetPrivateProfileString("boot", "display.drv", "", b, sizeof(b), sysini);
    if (!stricmp(b, "winvbe.drv")) {
        WritePrivateProfileString("boot", "display.drv", "vga.drv", sysini);
        WritePrivateProfileString("boot", "fonts.fon", "vgasys.fon", sysini);
        WritePrivateProfileString("boot", "fixedfon.fon", "vgafix.fon", sysini);
        WritePrivateProfileString("boot", "oemfonts.fon", "vgaoem.fon", sysini);
        WritePrivateProfileString("boot.description", "display.drv", "VGA", sysini);
    }
    progman_exec("[DeleteGroup(WINVBE)]");
    if (MessageBox(NULL, "WINVBE was removed. Windows must be restarted for the VGA driver.\n\nRestart Windows now?",
                   "Uninstall WINVBE", MB_YESNO | MB_ICONQUESTION) == IDYES)
        ExitWindows(EW_RESTART, 0);
    return 1;
}
#endif

BOOL FAR PASCAL __export MainProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        DBG('F');
        init_dialog(dlg);
        DBG('G');
        return TRUE;
    case WM_COMMAND:
        switch (wp) {
        case IDC_MODES:
            if (HIWORD(lp) == LBN_SELCHANGE) fill_depths(dlg, cur_depth(dlg));
            return TRUE;
#ifdef OLDWIN
        case IDC_DEBUG:     /* plain check boxes on Windows 1.x */
            CheckDlgButton(dlg, IDC_DEBUG, !IsDlgButtonChecked(dlg, IDC_DEBUG));
            return TRUE;
#endif
        case IDC_CUSTOM:
#ifdef OLDWIN
            CheckDlgButton(dlg, IDC_CUSTOM, !IsDlgButtonChecked(dlg, IDC_CUSTOM));
#endif
            update_enables(dlg);
            fill_depths(dlg, cur_depth(dlg));
            return TRUE;
        case IDC_MOUSE:
            mouse_dialog(dlg);
            return TRUE;
#ifndef OLDWIN
        case IDC_COLORS:
            color_dialog(dlg);
            return TRUE;
#endif
        case IDC_ABOUT: {
            FARPROC fp = MakeProcInstance((FARPROC)AboutProc, hInst);
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUT), dlg, (DLGPROC)fp);
            FreeProcInstance(fp);
            return TRUE;
        }
        case IDOK:
#ifndef OLDWIN
            if (win9x) snap_start_keys(start_vals);
#endif
            if (!save_settings(dlg)) return TRUE;
#ifndef OLDWIN
            /* Windows 9x: only the mode changed - set it now, no restart */
            if (win9x) {
                int now[NSTART];
                int i, same = 1;
                snap_start_keys(now);
                for (i = 0; i < (int)NSTART; i++)
                    /* the timing formula only counts with a refresh rate */
                    if (now[i] != start_vals[i] && !(i == 2 && !now[1] && !start_vals[1])) same = 0;
                if (same && now[0] != 0x13 && change_display_9x(live_w, live_h, live_bpp) == 0) {
                    EndDialog(dlg, 1);
                    return TRUE;
                }
            }
#endif
#ifdef OLDWIN
            if (MessageBox(dlg, "Settings saved. They take effect when Windows is started again.\n\n"
                                "End the Windows session now?", "WINVBE", MB_YESNO | MB_ICONQUESTION) == IDYES) {
#else
            if (MessageBox(dlg, win9x ? "Settings saved. The computer must be restarted for them to take effect.\n\n"
                                        "Restart the computer now?" :
                                "Settings saved. Windows must be restarted for them to take effect.\n\n"
                                "Restart Windows now?", "WINVBE", MB_YESNO | MB_ICONQUESTION) == IDYES) {
#endif
                EndDialog(dlg, 2);
                return TRUE;
            }
            EndDialog(dlg, 1);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

int PASCAL WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    FARPROC fp;
    int r;
    HANDLE hmem;
    hInst = hi;
    DBG('C');
#ifndef OLDWIN
    win9x_init();
    {
        char FAR *c = cmd;
        while (*c == ' ') c++;
        if ((*c == '/' || *c == '-') && !_fstrnicmp(c + 1, "915", 3)) {
            if (win9x) setup915();
            return 0;
        }
        /* started by WIN.INI run= after INSTALL.EXE: the group, once */
        if (!win9x && !*c) {
            char r[256];
            int inrun, pending;
            GetProfileString("windows", "run", "", r, sizeof(r));
            strupr(r);
            inrun = strstr(r, "WINVBECF") != NULL;
            pending = GetPrivateProfileInt("WINVBE", "Shortcuts", 0, sysini) != 0;
            if (inrun || pending) {
                if (inrun) run_entry_remove();
                WritePrivateProfileString("WINVBE", "Shortcuts", NULL, sysini);
                shortcuts_setup();
                /* started from run= (another run= program can have taken
                   the entry out already): done; started by hand: the
                   settings follow */
                if (inrun || GetTickCount() < 180000UL) return 0;
            }
        }
        if ((*c == '/' || *c == '-') && !_fstrnicmp(c + 1, "SETUP", 5)) {
            if (!win9x) {
                run_entry_remove();
                WritePrivateProfileString("WINVBE", "Shortcuts", NULL, sysini);
            }
            else if (!FindWindow("Shell_TrayWnd", NULL)) {
                /* RunOnce of the INF runs before the shell is up, and the
                   shell waits for it: start a copy that waits for the
                   shell (/SETUPW), and return at once */
                MSG msg;
                DWORD t;
                if ((c[6] | 0x20) != 'w') {
                    char me[160];
                    my_path(me, sizeof(me) - 10);
                    strcat(me, " /SETUPW");
                    WinExec(me, SW_HIDE);
                    return 0;
                }
                SetTimer(NULL, 0, 500, NULL);
                t = GetTickCount();
                while (!FindWindow("Shell_TrayWnd", NULL) && GetTickCount() - t < 300000UL
                       && GetMessage(&msg, NULL, 0, 0))
                    DispatchMessage(&msg);
                if (!FindWindow("Shell_TrayWnd", NULL)) return 0;
                t = GetTickCount();
                while (GetTickCount() - t < 5000 && GetMessage(&msg, NULL, 0, 0))
                    DispatchMessage(&msg);
            }
            shortcuts_setup();
            return 0;
        }
        if ((*c == '/' || *c == '-') && !_fstrnicmp(c + 1, "UNINSTALL", 9)) {
            if (!win9x) uninstall();
            return 0;
        }
    }
#endif
    {
        char FAR *c = cmd;
        while (*c == ' ') c++;
        if ((*c == '/' || *c == '-') && (c[1] | 0x20) == 'm' && (c[2] | 0x20) == 'o') {
            mouse_dialog(NULL);             /* WINVBECF /MOUSE: WINVBE Mouse */
            return 0;
        }
#ifndef OLDWIN
        if ((*c == '/' || *c == '-') && (c[1] | 0x20) == 'c' && (c[2] | 0x20) == 'o') {
            color_dialog(NULL);             /* WINVBECF /COLORS: WINVBE Colors */
            return 0;
        }
#endif
    }
    hmem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(VQINFO));
    if (!hmem) return 0;
    vi = (VQINFO FAR *)GlobalLock(hmem);
    if (!vq_query(vi)) {
        MessageBox(NULL, "No VESA BIOS Extensions (VBE) were found on this graphics card.\n"
                         "WINVBE can still run in standard VGA 320x200x256 mode.", "WINVBE", MB_OK | MB_ICONINFORMATION);
    }
    DBG('D');
    vq_sort(vi);
    DBG('E');
    fp = MakeProcInstance((FARPROC)MainProc, hi);
    r = DialogBox(hi, MAKEINTRESOURCE(IDD_MAIN), NULL, (DLGPROC)fp);
    FreeProcInstance(fp);
    GlobalUnlock(hmem);
    GlobalFree(hmem);
    if (r == 2)
#ifdef OLDWIN
        OldExitWindows();
#else
        /* Windows 9x loads a Windows 3.1 style display driver only at boot */
        ExitWindows(win9x ? 0x43 /* EW_REBOOTSYSTEM */ : EW_RESTART, 0);
#endif
    return 0;
}
