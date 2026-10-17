/* wvbreg - test helper: register the WINVBE miniport service (NT 3.x/4).
   No C runtime (NT 3.1 has no MSVCRT.DLL). */
#include <windows.h>
static DWORD num(const char **pp)
{
    const char *p = *pp; DWORD v = 0;
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') v = v * 10 + *p++ - '0';
    *pp = p; return v;
}
static void out(const char *s)
{
    DWORD n = 0; while (s[n]) n++;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, n, &n, NULL);
}
static HKEY mk(HKEY root, const char *path)
{
    HKEY k; DWORD d;
    if (RegCreateKeyExA(root, path, 0, "", 0, KEY_ALL_ACCESS, NULL, &k, &d)) { out("cannot create key\r\n"); ExitProcess(1); }
    return k;
}
static void dw(HKEY k, const char *n, DWORD v) { RegSetValueExA(k, n, 0, REG_DWORD, (BYTE *)&v, 4); }
void __stdcall start(void)
{
    HKEY s, d;
    const char *p = GetCommandLineA();
    DWORD w, h, b;
    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
    else while (*p && *p != ' ') p++;
    w = num(&p); h = num(&p); b = num(&p);
    if (!w) w = 800; if (!h) h = 600; if (!b) b = 16;
    s = mk(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\winvbe");
    dw(s, "Type", 1); dw(s, "Start", 1); dw(s, "ErrorControl", 1);
    RegSetValueExA(s, "Group", 0, REG_SZ, (BYTE *)"Video", 6);
    RegSetValueExA(s, "ImagePath", 0, REG_EXPAND_SZ, (BYTE *)"\\SystemRoot\\System32\\drivers\\winvbe.sys", 40);
    d = mk(s, "Device0");
    RegSetValueExA(d, "InstalledDisplayDrivers", 0, REG_MULTI_SZ, (BYTE *)"winvbe\0", 8);
    dw(d, "VgaCompatible", 0);
    dw(d, "DefaultSettings.BitsPerPel", b); dw(d, "DefaultSettings.XResolution", w);
    dw(d, "DefaultSettings.YResolution", h); dw(d, "DefaultSettings.VRefresh", 60);
    dw(d, "DefaultSettings.Interlaced", 0); dw(d, "WinVbe.Debug", 1);
    /* NT 3.1: the stock VGA miniport stays (full screen DOS) but must load
       after us so that we get \Device\Video0: new tag, first in the
       Video group order */
    if (!RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\Vga", 0, KEY_ALL_ACCESS, &d)) {
        HKEY g; DWORD t[64], n = sizeof(t), ty, i, j, tag = 0, vtag = 0, sz = 4;
        dw(d, "Start", 1);
        RegQueryValueExA(d, "Tag", NULL, &ty, (BYTE *)&vtag, &sz);
        /* our tag goes just before the VGA tag: after Videoprt (also in
           this group), before the VGA miniport */
        if (!RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\GroupOrderList", 0, KEY_ALL_ACCESS, &g)) {
            if (!RegQueryValueExA(g, "Video", NULL, &ty, (BYTE *)t, &n) && n >= 4 && t[0] < 60) {
                for (i = 1; i <= t[0]; i++) if (t[i] > tag) tag = t[i];
                tag++;
                for (i = 1; i <= t[0] && t[i] != vtag; i++) ;
                for (j = t[0]; j >= i; j--) t[j + 1] = t[j];
                t[i] = tag; t[0]++;
                RegSetValueExA(g, "Video", 0, REG_BINARY, (BYTE *)t, (t[0] + 1) * 4);
                dw(s, "Tag", tag);
                out("tag set\r\n");
            }
        }
    }
    out("WINVBE registered\r\n");
    ExitProcess(0);
}
