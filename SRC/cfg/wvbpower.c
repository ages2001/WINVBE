/*
 * WVBPOWER.SCR - "WINVBE Power Saver": monitor power management (DPMS) as
 * a screen saver for Windows 3.1 / Windows for Workgroups 3.1x (also runs
 * on Windows 95/98/ME, which have their own energy saving settings).
 *
 * When Windows starts the screen saver the screen goes black; after the
 * configured times the WINVBE display driver puts the monitor into stand by
 * and then switches it off (VBE/PM, DPMS).  A key or mouse movement turns
 * it on again.  Without VBE/PM in the video BIOS, or with another display
 * driver, the screen is only blanked.
 *
 * Control Panel > Desktop > Screen Saver: "WINVBE Power Saver", Setup sets
 * the times (CONTROL.INI [Screen Saver.WINVBE Power Saver]).
 *   WVBPOWER /s   run      WVBPOWER /c   setup      (no argument: setup)
 */
#include <windows.h>
#include <stdlib.h>
#include "wvbpower.h"

#define QUERYESCSUPPORT 8
#define WINVBE_DPMS     0x7702
#define DPMS_ON         0
#define DPMS_STANDBY    1
#define DPMS_OFF        4

static char ini[] = "CONTROL.INI";
static char sect[] = "Screen Saver.WINVBE Power Saver";
static HANDLE hInst;
static int o_stby, o_stbymin, o_off, o_offmin;

static void load_settings(void)
{
    o_stby = GetPrivateProfileInt(sect, "Standby", 1, ini);
    o_stbymin = GetPrivateProfileInt(sect, "StandbyMinutes", 0, ini);
    o_off = GetPrivateProfileInt(sect, "Off", 1, ini);
    o_offmin = GetPrivateProfileInt(sect, "OffMinutes", 10, ini);
    if (o_stbymin < 0 || o_stbymin > 999) o_stbymin = 0;
    if (o_offmin < 0 || o_offmin > 999) o_offmin = 10;
}

static void put_int(const char *key, int v)
{
    char b[8];
    wsprintf(b, "%d", v);
    WritePrivateProfileString(sect, key, b, ini);
}

/* WINVBE_DPMS escape of the display driver: FFFFh = query (100h | VBE/PM
   states, bit 7 = VBE/PM present), else the power state; 0 = no WINVBE */
static int dpms(WORD state)
{
    HDC dc = GetDC(NULL);
    int esc = WINVBE_DPMS, r = 0;
    if (dc) {
        if (Escape(dc, QUERYESCSUPPORT, sizeof(int), (LPCSTR)&esc, NULL) > 0)
            r = Escape(dc, WINVBE_DPMS, sizeof(WORD), (LPCSTR)&state, NULL);
        ReleaseDC(NULL, dc);
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* running                                                             */

static long secs;
static WORD power;          /* state set */
static POINT start_pt;
static BOOL have_pt, closing;

static void set_power(WORD st)
{
    if (st == power) return;
    if (dpms(st) > 0 || st == DPMS_ON) power = st;
}

static void close_saver(HWND w)
{
    if (closing) return;
    closing = TRUE;
    PostMessage(w, WM_CLOSE, 0, 0L);
}

LONG FAR PASCAL __export SaverProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        SetTimer(w, 1, 1000, NULL);
        return 0;
    case WM_TIMER:
        secs++;
        if (o_off && secs >= (long)o_offmin * 60) set_power(DPMS_OFF);
        else if (o_stby && secs >= (long)o_stbymin * 60) set_power(DPMS_STANDBY);
        return 0;
    case WM_SETCURSOR:
        SetCursor(NULL);
        return TRUE;
    case WM_MOUSEMOVE: {
        POINT p;
        p.x = LOWORD(lp); p.y = HIWORD(lp);
        if (!have_pt) { start_pt = p; have_pt = TRUE; return 0; }
        if (abs(p.x - start_pt.x) > 3 || abs(p.y - start_pt.y) > 3) close_saver(w);
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        close_saver(w);
        return 0;
    case WM_ACTIVATEAPP:
        if (!wp) close_saver(w);
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_SCREENSAVE || (wp & 0xFFF0) == SC_CLOSE) return 0;
        break;
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(w, &r);
        FillRect((HDC)wp, &r, GetStockObject(BLACK_BRUSH));
        return 1;
    }
    case WM_DESTROY:
        KillTimer(w, 1);
        set_power(DPMS_ON);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(w, msg, wp, lp);
}

static int run_saver(HANDLE prev)
{
    WNDCLASS wc;
    HWND w;
    MSG m;
    if (prev) return 0;                     /* already running */
    load_settings();
    wc.style = 0;
    wc.lpfnWndProc = SaverProc;
    wc.cbClsExtra = wc.cbWndExtra = 0;
    wc.hInstance = hInst;
    wc.hIcon = NULL;
    wc.hCursor = NULL;
    wc.hbrBackground = GetStockObject(BLACK_BRUSH);
    wc.lpszMenuName = NULL;
    wc.lpszClassName = "WvbPowerSaver";
    if (!RegisterClass(&wc)) return 0;
    w = CreateWindow("WvbPowerSaver", "WINVBE Power Saver", WS_POPUP | WS_VISIBLE, 0, 0,
                     GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), NULL, NULL, hInst, NULL);
    if (!w) return 0;
    SetWindowPos(w, (HWND)-1 /* HWND_TOPMOST */, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetFocus(w);
    ShowCursor(FALSE);
    UpdateWindow(w);
    while (GetMessage(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    ShowCursor(TRUE);
    return m.wParam;
}

/* ------------------------------------------------------------------ */
/* setup                                                               */

static void enable_fields(HWND d)
{
    EnableWindow(GetDlgItem(d, IDC_STBYMIN), IsDlgButtonChecked(d, IDC_STBY));
    EnableWindow(GetDlgItem(d, IDC_OFFMIN), IsDlgButtonChecked(d, IDC_OFF));
}

BOOL FAR PASCAL __export SetupProc(HWND d, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        int caps = dpms(0xFFFF);
        char b[160];
        CheckDlgButton(d, IDC_STBY, o_stby);
        CheckDlgButton(d, IDC_OFF, o_off);
        SetDlgItemInt(d, IDC_STBYMIN, o_stbymin, FALSE);
        SetDlgItemInt(d, IDC_OFFMIN, o_offmin, FALSE);
        if (caps <= 0)
            lstrcpy(b, "The display driver is not WINVBE: the screen is only blanked.");
        else if (!(caps & 0x80))
            lstrcpy(b, "The video BIOS has no VBE/PM power management: the screen is only "
                       "blanked (the picture is switched off).");
        else {
            lstrcpy(b, "VBE/PM power management of the video BIOS:");
            if (caps & 1) lstrcat(b, " stand by");
            if (caps & 2) lstrcat(b, (caps & 1) ? ", suspend" : " suspend");
            if (caps & 4) lstrcat(b, (caps & 3) ? ", off" : " off");
            if (!(caps & 7)) lstrcat(b, " (no states reported)");
            lstrcat(b, ".");
        }
        SetDlgItemText(d, IDC_STATUS, b);
        enable_fields(d);
        return TRUE;
    }
    case WM_COMMAND:
        switch (wp) {
        case IDC_STBY:
        case IDC_OFF:
            enable_fields(d);
            return TRUE;
        case IDC_TEST:
            /* five seconds stand by / off to see what the monitor does */
            if (dpms(DPMS_OFF) > 0) {
                DWORD t = GetTickCount();
                while (GetTickCount() - t < 5000) ;
                dpms(DPMS_ON);
            } else
                MessageBox(d, "WINVBE is not the display driver.", "WINVBE Power Saver", MB_OK | MB_ICONINFORMATION);
            return TRUE;
        case IDOK: {
            BOOL ok;
            int v;
            put_int("Standby", IsDlgButtonChecked(d, IDC_STBY) ? 1 : 0);
            v = GetDlgItemInt(d, IDC_STBYMIN, &ok, FALSE);
            put_int("StandbyMinutes", ok && v <= 999 ? v : 0);
            put_int("Off", IsDlgButtonChecked(d, IDC_OFF) ? 1 : 0);
            v = GetDlgItemInt(d, IDC_OFFMIN, &ok, FALSE);
            put_int("OffMinutes", ok && v <= 999 ? v : 10);
            EndDialog(d, 1);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(d, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */

int PASCAL WinMain(HANDLE inst, HANDLE prev, LPSTR cmd, int show)
{
    char c = 0;
    hInst = inst;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (*cmd == '/' || *cmd == '-') c = cmd[1];
    if (c == 's' || c == 'S') return run_saver(prev);
    if (c == 'p' || c == 'P' || c == 'a' || c == 'A') return 0;   /* 9x preview / password */
    load_settings();
    {
        FARPROC fp = MakeProcInstance((FARPROC)SetupProc, inst);
        DialogBox(inst, MAKEINTRESOURCE(IDD_SETUP), NULL, (DLGPROC)fp);
        FreeProcInstance(fp);
    }
    return 0;
}
