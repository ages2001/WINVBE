/*
 * WINVBECF for Windows NT - settings program of the WINVBE display driver.
 *
 * Windows NT 3.1, 3.5, 3.51, 4.0, 2000, XP and Server 2003 (x86) and the x64
 * editions. Win32 ANSI API only, no C runtime (NT 3.1 has no MSVCRT.DLL).
 *
 *  - shows the adapter, the monitor (EDID) and the 915 patch result, from
 *    the WinVbe.Info value the miniport writes into its device key;
 *  - resolution, colors and refresh rate: ChangeDisplaySettings (NT 3.51
 *    and later) or DefaultSettings.* in the device key (NT 3.1 / 3.5);
 *  - miniport options (refresh rates offered, 8-bit DAC, debug log, 915
 *    patch command): device key, effective after a restart;
 *  - "Install driver": NT 3.x / 4.0 copy the files and write the service
 *    keys themselves, 2000 and later use WINVBE.INF through Plug and Play.
 */
#include <windows.h>
#include "cfgres.h"

#pragma pack(push, 1)
typedef struct { unsigned short w, h; unsigned char bpp, hzmask; unsigned short vbe; } WVBMODE;
#define WVB_MAXMODES 200
typedef struct {
    unsigned long  size, ntlevel;
    unsigned short vbever, pad0;
    unsigned long  memkb;
    char           oem[80];
    unsigned char  edid_ok, pad1[3];
    unsigned char  edid[128];
    char           patch[160];
    unsigned long  nmodes;
    WVBMODE        modes[WVB_MAXMODES];
} WVBINFO;
#pragma pack(pop)

static const unsigned short hz_tab[6] = { 60, 70, 72, 75, 85, 100 };

#define V_INFO    "WinVbe.Info"
#define V_REFRESH "WinVbe.Refresh"
#define V_PATCH   "WinVbe.Patch915"
#define V_DAC8    "WinVbe.Dac8"
#define V_DEBUG   "WinVbe.Debug"

static HINSTANCE hinst;
static int ntmaj, ntmin;
static char exedir[MAX_PATH];
static char devkey[300];            /* device key below HKLM, "" = none */
static WVBINFO info;
static int have_info;
static int cur_w, cur_h, cur_bpp, cur_hz;
static int sel_w, sel_h;            /* selected resolution */
static int o_rates, o_dac8, o_debug;
static char o_patch[160];

typedef LONG (WINAPI *PCDS)(DEVMODEA *, DWORD);
static PCDS pChangeDS;

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

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int shas(const char *s, const char *w)
{ int i, n = slen(w); for (; *s; s++) { for (i = 0; i < n && s[i] == w[i]; i++) ; if (i == n) return 1; } return 0; }
static void scpy(char *d, const char *s, int max) { int i = 0; while (s[i] && i < max - 1) { d[i] = s[i]; i++; } d[i] = 0; }
static void scat(char *d, const char *s, int max) { int n = slen(d); scpy(d + n, s, max - n); }
static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int sieq(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return lower(*a) == lower(*b);
}
static int sipre(const char *s, const char *pre)      /* s starts with pre */
{
    while (*pre) { if (lower(*s) != lower(*pre)) return 0; s++; pre++; }
    return 1;
}
static const char *sistr(const char *s, const char *w)
{
    for (; *s; s++) if (sipre(s, w)) return s;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* registry                                                            */

static DWORD reg_dword(HKEY k, const char *name, DWORD def)
{
    DWORD v, t, n = sizeof(v);
    if (RegQueryValueExA(k, name, NULL, &t, (BYTE *)&v, &n) || t != REG_DWORD) return def;
    return v;
}

static void reg_set_dword(HKEY k, const char *name, DWORD v)
{
    RegSetValueExA(k, name, 0, REG_DWORD, (BYTE *)&v, sizeof(v));
}

static int reg_str(HKEY k, const char *name, char *buf, DWORD size)
{
    DWORD t, n = size;
    buf[0] = 0;
    if (RegQueryValueExA(k, name, NULL, &t, (BYTE *)buf, &n)) return 0;
    if (n >= size) n = size - 1;
    buf[n] = 0;
    return 1;
}

static HKEY open_dev(REGSAM sam)
{
    HKEY k;
    if (!devkey[0] || RegOpenKeyExA(HKEY_LOCAL_MACHINE, devkey, 0, sam, &k)) return NULL;
    return k;
}

/* the device key of the active WINVBE miniport, from HARDWARE\DEVICEMAP\VIDEO */
static int find_device(void)
{
    HKEY m, k;
    int i;
    devkey[0] = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\VIDEO", 0, KEY_READ, &m)) return 0;
    for (i = 0; i < 16 && !devkey[0]; i++) {
        char name[32], path[300], drv[64];
        const char *p;
        wsprintfA(name, "\\Device\\Video%d", i);
        if (!reg_str(m, name, path, sizeof(path))) continue;
        p = path;
        if (sipre(p, "\\REGISTRY\\Machine\\")) p += 18;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, p, 0, KEY_READ, &k)) continue;
        drv[0] = 0;
        reg_str(k, "InstalledDisplayDrivers", drv, sizeof(drv));
        {
            DWORD t, n = 0;
            if (!RegQueryValueExA(k, V_INFO, NULL, &t, NULL, &n) || sieq(drv, "winvbe"))
                scpy(devkey, p, sizeof(devkey));
        }
        RegCloseKey(k);
    }
    RegCloseKey(m);
    return devkey[0] != 0;
}

static void load_info(void)
{
    HKEY k = open_dev(KEY_READ);
    DWORD t, n = sizeof(info);
    have_info = 0;
    memset(&info, 0, sizeof(info));
    o_rates = 0; o_dac8 = 1; o_debug = 0; scpy(o_patch, "AUTO", sizeof(o_patch));
    if (!k) return;
    if (!RegQueryValueExA(k, V_INFO, NULL, &t, (BYTE *)&info, &n) && n >= 16 && info.nmodes <= WVB_MAXMODES)
        have_info = 1;
    o_rates = (int)reg_dword(k, V_REFRESH, 0);
    o_dac8 = (int)reg_dword(k, V_DAC8, 1);
    o_debug = (int)reg_dword(k, V_DEBUG, 0);
    if (!reg_str(k, V_PATCH, o_patch, sizeof(o_patch))) scpy(o_patch, "AUTO", sizeof(o_patch));
    RegCloseKey(k);
}

/* ------------------------------------------------------------------ */
/* dialog                                                              */

static void status(HWND d, const char *s) { SetDlgItemTextA(d, IDC_STATUS, s); }

static const char *depth_name(int bpp)
{
    switch (bpp) {
    case 1: return "2 colors";
    case 4: return "16 colors";
    case 8: return "256 colors";
    case 15: return "32K colors (15 bit)";
    case 16: return "64K colors (16 bit)";
    case 24: return "16M colors (24 bit)";
    default: return "16M colors (32 bit)";
    }
}

static void fill_info(HWND d)
{
    char b[600], t[160];
    if (!devkey[0]) {
        SetDlgItemTextA(d, IDC_INFO, "WINVBE is not the active display driver.\r\n"
                        "Use \"Install driver\" (Windows NT 3.x / 4.0), or install WINVBE.INF "
                        "with Device Manager (Windows 2000 and later), and restart.");
        return;
    }
    if (!have_info) {
        SetDlgItemTextA(d, IDC_INFO, "WINVBE is installed, but the miniport has not reported the "
                        "adapter yet. Restart Windows.");
        return;
    }
    wsprintfA(b, "%s\r\nVBE %d.%d, %lu KB video memory, %lu modes.\r\n", info.oem, info.vbever >> 8,
              info.vbever & 0xFF, info.memkb, info.nmodes);
    if (info.edid_ok) {
        const unsigned char *e = info.edid;
        int w = e[56] | ((e[58] & 0xF0) << 4), h = e[59] | ((e[61] & 0xF0) << 4);
        char mon[20];
        int i, j;
        mon[0] = 0;
        for (i = 54; i <= 108; i += 18)
            if (e[i] == 0 && e[i + 1] == 0 && e[i + 3] == 0xFC) {
                for (j = 0; j < 13 && e[i + 5 + j] != 0x0A; j++) mon[j] = (char)e[i + 5 + j];
                mon[j] = 0;
            }
        wsprintfA(t, "Monitor: %s, native %dx%d.\r\n", mon[0] ? mon : "(EDID)", w, h);
    } else
        scpy(t, "Monitor: no EDID (refresh rates up to 85 Hz).\r\n", sizeof(t));
    scat(b, t, sizeof(b));
    scat(b, "915 patch: ", sizeof(b));
    scat(b, info.patch[0] ? info.patch : "-", sizeof(b));
    SetDlgItemTextA(d, IDC_INFO, b);
}

static void current_mode(HWND d)
{
    HDC dc = GetDC(NULL);
    char b[120];
    cur_w = GetDeviceCaps(dc, HORZRES);
    cur_h = GetDeviceCaps(dc, VERTRES);
    cur_bpp = GetDeviceCaps(dc, BITSPIXEL) * GetDeviceCaps(dc, PLANES);
    cur_hz = (ntmaj >= 4) ? GetDeviceCaps(dc, 116 /* VREFRESH */) : 0;
    ReleaseDC(NULL, dc);
    if (cur_hz > 1)
        wsprintfA(b, "Current: %dx%d, %s, %d Hz", cur_w, cur_h, depth_name(cur_bpp), cur_hz);
    else
        wsprintfA(b, "Current: %dx%d, %s", cur_w, cur_h, depth_name(cur_bpp));
    SetDlgItemTextA(d, IDC_CURRENT, b);
}

static int sel_bpp(HWND d)
{
    LRESULT i = SendDlgItemMessageA(d, IDC_DEPTH, CB_GETCURSEL, 0, 0);
    return i < 0 ? 0 : (int)SendDlgItemMessageA(d, IDC_DEPTH, CB_GETITEMDATA, i, 0);
}

static int sel_hz(HWND d)
{
    LRESULT i = SendDlgItemMessageA(d, IDC_REFRESH, CB_GETCURSEL, 0, 0);
    return i < 0 ? 0 : (int)SendDlgItemMessageA(d, IDC_REFRESH, CB_GETITEMDATA, i, 0);
}

static void fill_refresh(HWND d, int want)
{
    unsigned mask = 0;
    unsigned long i;
    int bpp = sel_bpp(d), k, pick = -1;
    SendDlgItemMessageA(d, IDC_REFRESH, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < info.nmodes; i++)
        if (info.modes[i].w == sel_w && info.modes[i].h == sel_h && info.modes[i].bpp == bpp)
            mask |= info.modes[i].hzmask;
    if (!mask) mask = 1;
    for (k = 0; k < 6; k++)
        if (mask & (1u << k)) {
            char b[16];
            LRESULT n;
            wsprintfA(b, "%d Hz", hz_tab[k]);
            n = SendDlgItemMessageA(d, IDC_REFRESH, CB_ADDSTRING, 0, (LPARAM)b);
            SendDlgItemMessageA(d, IDC_REFRESH, CB_SETITEMDATA, n, hz_tab[k]);
            if (hz_tab[k] == want || pick < 0) pick = (int)n;
        }
    SendDlgItemMessageA(d, IDC_REFRESH, CB_SETCURSEL, pick < 0 ? 0 : pick, 0);
}

static void fill_depth(HWND d, int want_bpp, int want_hz)
{
    static const int order[5] = { 8, 15, 16, 24, 32 };
    int k, pick = -1;
    SendDlgItemMessageA(d, IDC_DEPTH, CB_RESETCONTENT, 0, 0);
    for (k = 0; k < 5; k++) {
        unsigned long i;
        for (i = 0; i < info.nmodes; i++)
            if (info.modes[i].w == sel_w && info.modes[i].h == sel_h && info.modes[i].bpp == order[k]) {
                LRESULT n = SendDlgItemMessageA(d, IDC_DEPTH, CB_ADDSTRING, 0, (LPARAM)depth_name(order[k]));
                SendDlgItemMessageA(d, IDC_DEPTH, CB_SETITEMDATA, n, order[k]);
                if (order[k] == want_bpp) pick = (int)n;
                break;
            }
    }
    if (pick < 0) pick = (int)SendDlgItemMessageA(d, IDC_DEPTH, CB_GETCOUNT, 0, 0) - 1;
    SendDlgItemMessageA(d, IDC_DEPTH, CB_SETCURSEL, pick < 0 ? 0 : pick, 0);
    fill_refresh(d, want_hz);
}

/* resolutions sorted by width, height (unique) */
static void fill_modes(HWND d)
{
    unsigned long i, j;
    int pick = -1;
    SendDlgItemMessageA(d, IDC_MODES, LB_RESETCONTENT, 0, 0);
    for (;;) {
        int bw = 0, bh = 0;
        unsigned long cnt = (unsigned long)SendDlgItemMessageA(d, IDC_MODES, LB_GETCOUNT, 0, 0);
        int lw = 0, lh = 0;
        if (cnt) {
            DWORD v = (DWORD)SendDlgItemMessageA(d, IDC_MODES, LB_GETITEMDATA, cnt - 1, 0);
            lw = LOWORD(v); lh = HIWORD(v);
        }
        for (i = 0; i < info.nmodes; i++) {
            int w = info.modes[i].w, h = info.modes[i].h;
            if (w < lw || (w == lw && h <= lh)) continue;
            if (!bw || w < bw || (w == bw && h < bh)) { bw = w; bh = h; }
        }
        if (!bw) break;
        {
            char b[24];
            LRESULT n;
            wsprintfA(b, "%d x %d", bw, bh);
            n = SendDlgItemMessageA(d, IDC_MODES, LB_ADDSTRING, 0, (LPARAM)b);
            SendDlgItemMessageA(d, IDC_MODES, LB_SETITEMDATA, n, MAKELONG(bw, bh));
            if (bw == cur_w && bh == cur_h) pick = (int)n;
        }
        (void)j;
    }
    if (pick < 0) pick = 0;
    SendDlgItemMessageA(d, IDC_MODES, LB_SETCURSEL, pick, 0);
    {
        DWORD v = (DWORD)SendDlgItemMessageA(d, IDC_MODES, LB_GETITEMDATA, pick, 0);
        sel_w = LOWORD(v); sel_h = HIWORD(v);
    }
    fill_depth(d, cur_bpp, cur_hz);
}

static void fill_options(HWND d)
{
    static const char *rates[3] = { "Monitor (EDID)", "All rates", "60 Hz only" };
    int i;
    for (i = 0; i < 3; i++) SendDlgItemMessageA(d, IDC_RATES, CB_ADDSTRING, 0, (LPARAM)rates[i]);
    SendDlgItemMessageA(d, IDC_RATES, CB_SETCURSEL, (o_rates >= 0 && o_rates < 3) ? o_rates : 0, 0);
    CheckDlgButton(d, IDC_DAC8, o_dac8 ? 1 : 0);
    CheckDlgButton(d, IDC_DEBUG, o_debug ? 1 : 0);
    SetDlgItemTextA(d, IDC_PATCH, o_patch);
}

/* ------------------------------------------------------------------ */
/* restart                                                             */

static void restart(HWND d)
{
    HANDLE tok;
    if (MessageBoxA(d, "The new settings take effect after a restart.\r\nRestart Windows now?",
                    "WINVBE", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        TOKEN_PRIVILEGES tp;
        tp.PrivilegeCount = 1;
        if (LookupPrivilegeValueA(NULL, "SeShutdownPrivilege", &tp.Privileges[0].Luid)) {
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
        }
        CloseHandle(tok);
    }
    ExitWindowsEx(EWX_REBOOT, 0);
}

/* ------------------------------------------------------------------ */
/* apply                                                               */

static void nt3_setup_video(int w, int h, int bpp);

static int apply(HWND d)
{
    int reboot = 0, w = sel_w, h = sel_h, bpp = sel_bpp(d), hz = sel_hz(d);
    HKEY k = open_dev(KEY_READ | KEY_WRITE);
    if (!k) {
        MessageBoxA(d, "WINVBE is not the active display driver (or no administrator rights).", "WINVBE",
                    MB_OK | MB_ICONEXCLAMATION);
        return 0;
    }
    {   /* miniport options: read at start-up */
        int rates = (int)SendDlgItemMessageA(d, IDC_RATES, CB_GETCURSEL, 0, 0);
        int dac8 = IsDlgButtonChecked(d, IDC_DAC8) ? 1 : 0;
        int dbg = IsDlgButtonChecked(d, IDC_DEBUG) ? 1 : 0;
        char patch[160];
        GetDlgItemTextA(d, IDC_PATCH, patch, sizeof(patch));
        if (rates < 0) rates = 0;
        if (rates != o_rates) { reg_set_dword(k, V_REFRESH, rates); reboot = 1; }
        if (dac8 != o_dac8) { reg_set_dword(k, V_DAC8, dac8); reboot = 1; }
        if (dbg != o_debug) { reg_set_dword(k, V_DEBUG, dbg); reboot = 1; }
        if (!sieq(patch, o_patch)) {
            RegSetValueExA(k, V_PATCH, 0, REG_SZ, (BYTE *)patch, slen(patch) + 1);
            reboot = 1;
        }
    }
    if (w && h && bpp && (w != cur_w || h != cur_h || bpp != cur_bpp || (hz && cur_hz > 1 && hz != cur_hz))) {
        if (pChangeDS) {
            DEVMODEA dm;
            LONG r;
            memset(&dm, 0, sizeof(dm));
            /* NT 3.51 knows the DEVMODE up to dmDisplayFrequency */
            dm.dmSize = (WORD)(ntmaj >= 4 ? sizeof(dm) : 0x9C);
            dm.dmSpecVersion = DM_SPECVERSION;
            dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | (hz ? DM_DISPLAYFREQUENCY : 0);
            dm.dmPelsWidth = w; dm.dmPelsHeight = h; dm.dmBitsPerPel = bpp; dm.dmDisplayFrequency = hz;
            r = pChangeDS(&dm, CDS_UPDATEREGISTRY);
            if (r == DISP_CHANGE_RESTART)
                reboot = 1;
            else if (r != DISP_CHANGE_SUCCESSFUL) {
                char b[100];
                wsprintfA(b, "The mode could not be set (error %ld).", r);
                MessageBoxA(d, b, "WINVBE", MB_OK | MB_ICONEXCLAMATION);
                RegCloseKey(k);
                return 0;
            }
        } else {
            /* NT 3.1 / 3.5: the display driver reads these at start-up */
            reg_set_dword(k, "DefaultSettings.XResolution", w);
            reg_set_dword(k, "DefaultSettings.YResolution", h);
            reg_set_dword(k, "DefaultSettings.BitsPerPel", bpp);
            reg_set_dword(k, "DefaultSettings.VRefresh", hz ? hz : 60);
            reboot = 1;
        }
        nt3_setup_video(w, h, bpp);
    }
    RegCloseKey(k);
    if (reboot) restart(d);
    return 1;
}

/* ------------------------------------------------------------------ */
/* installation                                                        */

/* does the file contain the string (case-insensitive)? - tells the NT 3.x
   display DLL (imports WINSRV.DLL) from the NT 4.0+ one (WIN32K.SYS) */
static int file_has(const char *fn, const char *what)
{
    HANDLE f = CreateFileA(fn, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    static char buf[65536];
    DWORD n = 0, i;
    int wl = slen(what), found = 0;
    if (f == INVALID_HANDLE_VALUE) return -1;
    ReadFile(f, buf, sizeof(buf) - 1, &n, NULL);
    CloseHandle(f);
    for (i = 0; i + wl <= n && !found; i++) {
        int j;
        for (j = 0; j < wl && lower(buf[i + j]) == lower(what[j]); j++) ;
        if (j == wl) found = 1;
    }
    return found;
}

/* CopyFile keeps the attributes of the source: files copied from a CD are
   read-only, and a read-only driver file can then not be replaced later -
   neither by WINVBECF nor by NT Setup (OEMSETUP.INF) */
static int copy_rw(const char *src, const char *dst)
{
    SetFileAttributesA(dst, FILE_ATTRIBUTE_NORMAL);
    if (!CopyFileA(src, dst, FALSE)) return 0;
    SetFileAttributesA(dst, FILE_ATTRIBUTE_NORMAL);
    return 1;
}

/* copy, or replace at the next restart when the target is in use; the
   temporary name is a valid 8.3 name (NT 3.1 FAT has no long names):
   winvbe.dll -> winvbe.~dl */
static int put_file(const char *src, const char *dst)
{
    char tmp[MAX_PATH];
    int i, dot = -1;
    if (copy_rw(src, dst)) return 1;
    scpy(tmp, dst, sizeof(tmp));
    for (i = 0; tmp[i]; i++)
        if (tmp[i] == '.') dot = i;
        else if (tmp[i] == '\\') dot = -1;
    if (dot < 0 || dot + 3 >= MAX_PATH) return 0;
    tmp[dot + 1] = '~';
    tmp[dot + 2] = dst[dot + 1] ? dst[dot + 1] : '_';
    tmp[dot + 3] = dst[dot + 1] && dst[dot + 2] ? dst[dot + 2] : '_';
    tmp[dot + 4] = 0;
    if (!copy_rw(src, tmp)) return 0;
    return MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_DELAY_UNTIL_REBOOT) != 0;
}

static HKEY mk(HKEY root, const char *path)
{
    HKEY k;
    DWORD disp;
    if (RegCreateKeyExA(root, path, 0, "", 0, KEY_ALL_ACCESS, NULL, &k, &disp)) return NULL;
    return k;
}

/* NT 3.1: VGA stays loaded (full screen MS-DOS) but must come after us in
   the Video group, so that WINVBE gets \Device\Video0: a new tag just
   before the tag of VGA in GroupOrderList\Video */
static void nt31_tag(HKEY svc)
{
    HKEY vga, g;
    DWORD t[64], n = sizeof(t), ty, sz = 4, vtag = 0, tag = 0, i, j;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\Vga", 0, KEY_ALL_ACCESS, &vga))
        return;
    reg_set_dword(vga, "Start", 1);
    RegQueryValueExA(vga, "Tag", NULL, &ty, (BYTE *)&vtag, &sz);
    RegCloseKey(vga);
    if (reg_dword(svc, "Tag", 0) > 1) return;          /* already done (1 = VIDEOPRT's own) */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\GroupOrderList", 0,
                      KEY_ALL_ACCESS, &g))
        return;
    if (!RegQueryValueExA(g, "Video", NULL, &ty, (BYTE *)t, &n) && n >= 4 && t[0] < 60) {
        for (i = 1; i <= t[0]; i++) if (t[i] > tag) tag = t[i];
        tag++;
        for (i = 1; i <= t[0] && t[i] != vtag; i++) ;
        for (j = t[0]; j >= i; j--) t[j + 1] = t[j];
        t[i] = tag; t[0]++;
        RegSetValueExA(g, "Video", 0, REG_BINARY, (BYTE *)t, (t[0] + 1) * 4);
        reg_set_dword(svc, "Tag", tag);
    }
    RegCloseKey(g);
}

/* the display miniport in use now (if any): disabled like NT Setup does
   when the display type is changed - except VGA / VgaSave and WINVBE */
static void disable_old(void)
{
    HKEY m, k;
    char path[300], svc[64], key[120];
    const char *p, *q;
    int i;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\VIDEO", 0, KEY_READ, &m)) return;
    i = reg_str(m, "\\Device\\Video0", path, sizeof(path));
    RegCloseKey(m);
    if (!i || !(p = sistr(path, "\\Services\\"))) return;
    p += 10;
    for (q = p, i = 0; *q && *q != '\\' && i < 63; q++) svc[i++] = *q;
    svc[i] = 0;
    if (sieq(svc, "vga") || sieq(svc, "vgasave") || sieq(svc, "winvbe") || !svc[0]) return;
    wsprintfA(key, "SYSTEM\\CurrentControlSet\\Services\\%s", svc);
    if (!RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_ALL_ACCESS, &k)) {
        reg_set_dword(k, "Start", 4);
        RegCloseKey(k);
    }
}

/* Windows NT 3.x Setup (Options > Change System Settings) shows the
   display option named in Control\Setup "Video" - the option names of
   OEMSETUP.INF are "WINVBE <w>x<h>x<bpp>"; it finds them in the copies of
   OEM display INFs in SYSTEM32 (OEMVIO*.INF) */
static void nt3_setup_video(int w, int h, int bpp)
{
    HKEY k;
    char v[40], old[40];
    if (ntmaj != 3 || !w || !h || !bpp) return;
    wsprintfA(v, "WINVBE %dx%dx%d", w, h, bpp);
    k = mk(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Setup");
    if (!k) return;
    if (!reg_str(k, "Video", old, sizeof(old)) || !sieq(old, v))
        RegSetValueExA(k, "Video", 0, REG_SZ, (BYTE *)v, slen(v) + 1);
    RegCloseKey(k);
}

/* the mode in the registry of the active WINVBE (takes effect at the next
   start) -> Control\Setup "Video" */
static void nt3_setup_sync(void)
{
    HKEY k;
    if (ntmaj != 3 || !(k = open_dev(KEY_READ))) return;
    nt3_setup_video((int)reg_dword(k, "DefaultSettings.XResolution", 0),
                    (int)reg_dword(k, "DefaultSettings.YResolution", 0),
                    (int)reg_dword(k, "DefaultSettings.BitsPerPel", 0));
    RegCloseKey(k);
}

/* OEMSETUP.INF (folder of WINVBECF.EXE) as SYSTEM32\OEMVIOn.INF, where
   Setup looks for the names of OEM display options: the copy that already
   holds WINVBE, else the first free number */
static void nt3_copy_inf(void)
{
    char src[MAX_PATH], dst[MAX_PATH], sys[MAX_PATH];
    int i, free_n = -1;
    scpy(src, exedir, sizeof(src)); scat(src, "OEMSETUP.INF", sizeof(src));
    if (GetFileAttributesA(src) == 0xFFFFFFFF) return;
    GetSystemDirectoryA(sys, sizeof(sys));
    for (i = 0; i < 100; i++) {
        wsprintfA(dst, "%s\\OEMVIO%d.INF", sys, i);
        if (GetFileAttributesA(dst) == 0xFFFFFFFF) { if (free_n < 0) free_n = i; continue; }
        if (file_has(dst, "WINVBE") == 1) { copy_rw(src, dst); return; }
    }
    if (free_n < 0) return;
    wsprintfA(dst, "%s\\OEMVIO%d.INF", sys, free_n);
    copy_rw(src, dst);
}

static int install_nt3_4(HWND d)
{
    char src[MAX_PATH], dst[MAX_PATH], sys[MAX_PATH];
    HKEY s, dev;
    int has_srv;
    static const char *files[3][2] = {
        { "WINVBE.SYS", "\\drivers\\winvbe.sys" }, { "WINVBE.DLL", "\\winvbe.dll" }, { "WINVBECF.EXE", "\\winvbecf.exe" } };
    int i, fresh;

    scpy(src, exedir, sizeof(src)); scat(src, "WINVBE.DLL", sizeof(src));
    has_srv = file_has(src, "winsrv.dll");
    if (has_srv < 0) {
        MessageBoxA(d, "WINVBE.SYS and WINVBE.DLL must be in the folder of WINVBECF.EXE.", "WINVBE", MB_OK | MB_ICONSTOP);
        return 0;
    }
    if ((ntmaj == 3) != (has_srv == 1)) {
        MessageBoxA(d, ntmaj == 3 ? "This WINVBE.DLL is for Windows NT 4.0 - XP. Use the files of NT\\I386\\NT3X."
                                  : "This WINVBE.DLL is for Windows NT 3.x. Use the files of NT\\I386\\NT4-XP.",
                    "WINVBE", MB_OK | MB_ICONSTOP);
        return 0;
    }
    GetSystemDirectoryA(sys, sizeof(sys));
    for (i = 0; i < 3; i++) {
        scpy(src, exedir, sizeof(src)); scat(src, files[i][0], sizeof(src));
        scpy(dst, sys, sizeof(dst)); scat(dst, files[i][1], sizeof(dst));
        if (GetFileAttributesA(src) == 0xFFFFFFFF && i == 2) continue;      /* settings program optional */
        if (!sieq(src, dst) && !put_file(src, dst)) {
            char b[MAX_PATH + 40];
            wsprintfA(b, "Cannot copy %s (administrator rights?).", files[i][0]);
            MessageBoxA(d, b, "WINVBE", MB_OK | MB_ICONSTOP);
            return 0;
        }
    }
    disable_old();
    s = mk(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\winvbe");
    if (!s) { MessageBoxA(d, "Cannot write the registry (administrator rights?).", "WINVBE", MB_OK | MB_ICONSTOP); return 0; }
    reg_set_dword(s, "Type", 1);
    reg_set_dword(s, "Start", 1);
    reg_set_dword(s, "ErrorControl", 1);
    RegSetValueExA(s, "Group", 0, REG_SZ, (BYTE *)"Video", 6);
    RegSetValueExA(s, "ImagePath", 0, REG_EXPAND_SZ, (BYTE *)"\\SystemRoot\\System32\\drivers\\winvbe.sys", 40);
    dev = mk(s, "Device0");
    if (dev) {
        fresh = reg_dword(dev, "DefaultSettings.XResolution", 0) == 0;
        RegSetValueExA(dev, "InstalledDisplayDrivers", 0, REG_MULTI_SZ, (BYTE *)"winvbe\0", 8);
        reg_set_dword(dev, "VgaCompatible", 0);
        if (fresh) {
            reg_set_dword(dev, "DefaultSettings.XResolution", 800);
            reg_set_dword(dev, "DefaultSettings.YResolution", 600);
            reg_set_dword(dev, "DefaultSettings.BitsPerPel", 16);
            reg_set_dword(dev, "DefaultSettings.VRefresh", 60);
            reg_set_dword(dev, "DefaultSettings.Interlaced", 0);
            reg_set_dword(dev, "DefaultSettings.XPanning", 0);
            reg_set_dword(dev, "DefaultSettings.YPanning", 0);
        }
        RegCloseKey(dev);
    }
    if (ntmaj == 3 && ntmin == 10) nt31_tag(s);
    RegCloseKey(s);
    if (ntmaj == 3) {
        HKEY k2;
        nt3_copy_inf();
        if (!RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\winvbe\\Device0", 0, KEY_READ, &k2)) {
            nt3_setup_video((int)reg_dword(k2, "DefaultSettings.XResolution", 800),
                            (int)reg_dword(k2, "DefaultSettings.YResolution", 600),
                            (int)reg_dword(k2, "DefaultSettings.BitsPerPel", 16));
            RegCloseKey(k2);
        }
    }
    s = mk(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\EventLog\\System\\winvbe");
    if (s) {
        static const char msg[] = "%SystemRoot%\\System32\\IoLogMsg.dll;%SystemRoot%\\System32\\drivers\\winvbe.sys";
        RegSetValueExA(s, "EventMessageFile", 0, REG_EXPAND_SZ, (BYTE *)msg, sizeof(msg));
        reg_set_dword(s, "TypesSupported", 7);
        RegCloseKey(s);
    }
    status(d, "WINVBE installed (800x600, 64K colors). Restart Windows.");
    restart(d);
    return 1;
}

/* Windows 2000 and later: Plug and Play with WINVBE.INF */
typedef BOOL (WINAPI *PUPDATE)(HWND, LPCSTR, LPCSTR, DWORD, PBOOL);
typedef void *(WINAPI *PGETCLASSDEVS)(const GUID *, PCSTR, HWND, DWORD);
typedef BOOL (WINAPI *PENUMDEV)(void *, DWORD, void *);
typedef BOOL (WINAPI *PGETPROP)(void *, void *, DWORD, PDWORD, PBYTE, DWORD, PDWORD);
typedef BOOL (WINAPI *PDESTROY)(void *);
typedef struct { DWORD cbSize; GUID ClassGuid; DWORD DevInst; ULONG_PTR Reserved; } DEVINFODATA;

static int install_pnp(HWND d)
{
    static const GUID disp = { 0x4d36e968, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };
    HMODULE nd = LoadLibraryA("newdev.dll"), sa = LoadLibraryA("setupapi.dll");
    PUPDATE upd = nd ? (PUPDATE)GetProcAddress(nd, "UpdateDriverForPlugAndPlayDevicesA") : NULL;
    PGETCLASSDEVS gcd = sa ? (PGETCLASSDEVS)GetProcAddress(sa, "SetupDiGetClassDevsA") : NULL;
    PENUMDEV en = sa ? (PENUMDEV)GetProcAddress(sa, "SetupDiEnumDeviceInfo") : NULL;
    PGETPROP gp = sa ? (PGETPROP)GetProcAddress(sa, "SetupDiGetDeviceRegistryPropertyA") : NULL;
    PDESTROY ds = sa ? (PDESTROY)GetProcAddress(sa, "SetupDiDestroyDeviceInfoList") : NULL;
    char inf[MAX_PATH], hw[512];
    void *h;
    DEVINFODATA di;
    DWORD i;
    BOOL rb = FALSE;
    int done = 0;
    DWORD err = 0;
    scpy(inf, exedir, sizeof(inf)); scat(inf, "WINVBE.INF", sizeof(inf));
    if (GetFileAttributesA(inf) == 0xFFFFFFFF) {
        MessageBoxA(d, "WINVBE.INF must be in the folder of WINVBECF.EXE.", "WINVBE", MB_OK | MB_ICONSTOP);
        return 0;
    }
    if (!upd || !gcd || !en || !gp || !ds) {
        MessageBoxA(d, "Install WINVBE.INF with Device Manager: Display adapters > Update Driver > "
                    "Have Disk.", "WINVBE", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    /* the INF lists the class compatible IDs of display controllers
       (PCI\CC_0300); every present display adapter is updated by its own
       first hardware ID (Windows 2000 matches only those), then by the
       class IDs as a fallback */
    status(d, "Installing - confirm the driver signing question with \"Continue Anyway\"...");
    h = gcd(&disp, NULL, NULL, 2 /* DIGCF_PRESENT */);
    di.cbSize = sizeof(di);
    for (i = 0; h != INVALID_HANDLE_VALUE && en(h, i, &di); i++) {
        memset(hw, 0, sizeof(hw));
        if (!gp(h, &di, 1 /* SPDRP_HARDWAREID */, NULL, (PBYTE)hw, sizeof(hw) - 2, NULL) || !hw[0]) continue;
        if (upd(d, hw, inf, 1 /* INSTALLFLAG_FORCE */, &rb)) done = 1;
        else if (!err) err = GetLastError();
    }
    if (h != INVALID_HANDLE_VALUE) ds(h);
    if (!done && (upd(d, "PCI\\CC_0300", inf, 1, &rb) || upd(d, "PCI\\CC_0001", inf, 1, &rb)))
        done = 1;
    if (!done) {
        char m[96];
        if (!err) err = GetLastError();
        wsprintfA(m, "Installation failed or cancelled (error %lX).", err);
        status(d, m);
        return 0;
    }
    status(d, "WINVBE installed.");
    if (rb) restart(d);
    return 1;
}

/* ------------------------------------------------------------------ */

static INT_PTR CALLBACK about_proc(HWND d, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    if (msg == WM_INITDIALOG) return TRUE;
    if (msg == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) { EndDialog(d, 0); return TRUE; }
    return FALSE;
}

static INT_PTR CALLBACK main_proc(HWND d, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        HICON ic = LoadIconA(hinst, MAKEINTRESOURCEA(IDI_APP));
        SendMessageA(d, WM_SETICON, 1, (LPARAM)ic);
        fill_info(d);
        current_mode(d);
        if (have_info) fill_modes(d);
        fill_options(d);
        if (!have_info) {
            EnableWindow(GetDlgItem(d, IDC_MODES), FALSE);
            EnableWindow(GetDlgItem(d, IDC_DEPTH), FALSE);
            EnableWindow(GetDlgItem(d, IDC_REFRESH), FALSE);
        }
        if (!devkey[0]) {
            EnableWindow(GetDlgItem(d, IDC_RATES), FALSE);
            EnableWindow(GetDlgItem(d, IDC_DAC8), FALSE);
            EnableWindow(GetDlgItem(d, IDC_DEBUG), FALSE);
            EnableWindow(GetDlgItem(d, IDC_PATCH), FALSE);
        }
        if (have_info && info.patch[0] && (shas(info.patch, "not supported") ||
                                           shas(info.patch, "no Intel"))) {
            /* no Intel 915-family chipset: nothing to patch, the adapter
               box above already says so */
            ShowWindow(GetDlgItem(d, IDC_PATCHLBL), SW_HIDE);
            ShowWindow(GetDlgItem(d, IDC_PATCH), SW_HIDE);
        }
        if (ntmaj == 3 && ntmin < 51)
            status(d, "Windows NT 3.1 / 3.5: a new mode takes effect after a restart.");
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_MODES:
            if (HIWORD(wp) == LBN_SELCHANGE) {
                LRESULT i = SendDlgItemMessageA(d, IDC_MODES, LB_GETCURSEL, 0, 0);
                DWORD v = (DWORD)SendDlgItemMessageA(d, IDC_MODES, LB_GETITEMDATA, i, 0);
                int b = sel_bpp(d), hz = sel_hz(d);
                sel_w = LOWORD(v); sel_h = HIWORD(v);
                fill_depth(d, b, hz);
            }
            return TRUE;
        case IDC_DEPTH:
            if (HIWORD(wp) == CBN_SELCHANGE) fill_refresh(d, sel_hz(d));
            return TRUE;
        case IDC_INSTALL:
            if (ntmaj >= 5) install_pnp(d); else install_nt3_4(d);
            return TRUE;
        case IDC_ABOUT:
            DialogBoxParamA(hinst, MAKEINTRESOURCEA(IDD_ABOUT), d, about_proc, 0);
            return TRUE;
        case IDOK:
            if (!devkey[0]) { EndDialog(d, 0); return TRUE; }
            if (apply(d)) EndDialog(d, 1);
            return TRUE;
        case IDCANCEL:
            EndDialog(d, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void WINAPI start(void)
{
    DWORD v = GetVersion();
    char *p;
    HMODULE u;
    hinst = GetModuleHandleA(NULL);
    ntmaj = (int)(v & 0xFF);
    ntmin = (int)((v >> 8) & 0xFF);
    if (v & 0x80000000UL) {
        MessageBoxA(NULL, "This WINVBECF is for Windows NT. On Windows 3.x / 95 / 98 / ME use the "
                    "WINVBECF.EXE of the main folder.", "WINVBE", MB_OK | MB_ICONSTOP);
        ExitProcess(1);
    }
    GetModuleFileNameA(NULL, exedir, sizeof(exedir));
    for (p = exedir + slen(exedir); p > exedir && p[-1] != '\\'; p--) ;
    *p = 0;
    u = GetModuleHandleA("user32.dll");
    if (u) pChangeDS = (PCDS)GetProcAddress(u, "ChangeDisplaySettingsA");
    find_device();
    load_info();
    nt3_setup_sync();
    DialogBoxParamA(hinst, MAKEINTRESOURCEA(IDD_MAIN), NULL, main_proc, 0);
    ExitProcess(0);
}
