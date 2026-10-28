/*
 * INSTALL - DOS installer / configurator for the WINVBE display driver.
 *
 *   INSTALL                 interactive (asks for the Windows directory, default C:\WINDOWS)
 *   INSTALL /W:1024 /H:768 /D:16 [/R:60] [/LARGE] [/DIR:C:\WINDOWS]   unattended install
 *   INSTALL /LIST           list VBE modes and monitor information
 *   INSTALL /VGA            switch Windows back to the standard VGA driver
 *
 * Windows 1.x/2.x (WIN100.BIN / WIN200.BIN, no SYSTEM.INI): INSTALL links
 * the driver into WIN100/WIN200.BIN itself (no Windows disks needed) and
 * writes the [WINVBE] settings to WIN.INI; /VGA puts the original files back.
 *
 * Windows 3.x: files Windows does not have yet (VGA fonts, grabbers) are
 * taken from the WIN30 / WIN31 folder next to INSTALL.EXE.
 *
 * Intel integrated graphics whose video BIOS 915PATCH can patch: INSTALL
 * runs 915PATCH /AUTO at once (the monitor resolution is then in the mode
 * list), copies 915PATCH.EXE to the Windows directory and adds
 * "915PATCH.EXE /AUTO" to AUTOEXEC.BAT; on other PCs nothing of it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dos.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#include "vbeq.h"

static VQINFO far vi;
static char windir[128], sysdir[128], srcdir[128];
static char *ini;           /* SYSTEM.INI (WIN.INI for 1.x/2.x) contents */
static long inilen;
static int oldwin;          /* 1 or 2 for Windows 1.x/2.x, 0 for 3.x */
static char *inifile = "SYSTEM.INI", *inibak = "SYSTEM.WVB";

/* ---------------- INI editing ---------------- */
static int load_ini(void)
{
    char p[160];
    FILE *f;
    sprintf(p, "%s\\%s", windir, inifile);
    f = fopen(p, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); inilen = ftell(f); fseek(f, 0, SEEK_SET);
    ini = malloc((size_t)inilen + 4096);
    if (!ini) { fclose(f); return 0; }
    fread(ini, 1, (size_t)inilen, f);
    ini[inilen] = 0;
    fclose(f);
    return 1;
}

static int save_ini(void)
{
    char p[160], b[160];
    FILE *f;
    sprintf(p, "%s\\%s", windir, inifile);
    sprintf(b, "%s\\%s", windir, inibak);
    if (access(b, 0) != 0) {                         /* keep the very first backup */
        FILE *o = fopen(p, "rb"), *n = fopen(b, "wb");
        if (o && n) { int c; while ((c = fgetc(o)) != EOF) fputc(c, n); }
        if (o) fclose(o);
        if (n) fclose(n);
    }
    f = fopen(p, "wb");
    if (!f) return 0;
    fwrite(ini, 1, strlen(ini), f);
    fclose(f);
    return 1;
}

/* find start of line i.e. after '\n' */
static char *find_section(char *sec)
{
    char *p = ini;
    size_t n = strlen(sec);
    while (*p) {
        char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '[' && !strnicmp(q + 1, sec, n) && q[1 + n] == ']') {
            char *e = strchr(q, '\n');
            return e ? e + 1 : q + strlen(q);
        }
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    return NULL;
}

static void insert_text(char *at, char *txt)
{
    size_t l = strlen(txt);
    memmove(at + l, at, strlen(at) + 1);
    memcpy(at, txt, l);
}

static void delete_text(char *at, size_t l)
{
    memmove(at, at + l, strlen(at + l) + 1);
}

static void ini_set(char *sec, char *key, char *val)
{
    char line[200];
    char *s = find_section(sec), *p;
    size_t kl = strlen(key);
    sprintf(line, "%s=%s\r\n", key, val);
    if (!s) {
        size_t l = strlen(ini);
        if (l && ini[l - 1] != '\n') strcat(ini, "\r\n");
        strcat(ini, "[");
        strcat(ini, sec);
        strcat(ini, "]\r\n");
        strcat(ini, line);
        return;
    }
    p = s;
    while (*p && *p != '[') {
        char *q = p, *e = strchr(p, '\n');
        while (*q == ' ' || *q == '\t') q++;
        if (!strnicmp(q, key, kl)) {
            char *r = q + kl;
            while (*r == ' ' || *r == '\t') r++;
            if (*r == '=') {
                size_t ll = e ? (size_t)(e + 1 - p) : strlen(p);
                delete_text(p, ll);
                insert_text(p, line);
                return;
            }
        }
        if (!e) { p += strlen(p); break; }
        p = e + 1;
    }
    /* insert at end of section (before trailing blank lines) */
    while (p > s && (p[-1] == '\n' || p[-1] == '\r')) {
        char *b = p - 1;
        while (b > s && (b[-1] == '\r')) b--;
        if (b > s && b[-1] == '\n') p = b; else break;
    }
    insert_text(p, line);
}

/* line "key=val" (case-insensitive) in the section: pointer to it, or NULL */
static char *ini_line(char *sec, char *key, char *val)
{
    char *s = find_section(sec), *p;
    size_t kl = strlen(key), vl = strlen(val);
    if (!s) return NULL;
    for (p = s; *p && *p != '[';) {
        char *q = p, *e = strchr(p, '\n');
        while (*q == ' ' || *q == '\t') q++;
        if (!strnicmp(q, key, kl)) {
            char *r = q + kl;
            while (*r == ' ' || *r == '\t') r++;
            if (*r == '=') {
                r++;
                while (*r == ' ' || *r == '\t') r++;
                if (!strnicmp(r, val, vl) && (r[vl] == '\r' || r[vl] == '\n' || r[vl] == ' ' || !r[vl]))
                    return p;
            }
        }
        if (!e) break;
        p = e + 1;
    }
    return NULL;
}

/* add "key=val" to the section (for keys that may repeat, like device=) */
static void ini_add(char *sec, char *key, char *val)
{
    char line[200], *s;
    if (ini_line(sec, key, val)) return;
    s = find_section(sec);
    sprintf(line, "%s=%s\r\n", key, val);
    if (!s) { ini_set(sec, key, val); return; }
    insert_text(s, line);
}

static void ini_remove(char *sec, char *key, char *val)
{
    char *p = ini_line(sec, key, val);
    if (p) {
        char *e = strchr(p, '\n');
        delete_text(p, e ? (size_t)(e + 1 - p) : strlen(p));
    }
}

static void ini_get(char *sec, char *key, char *out, int n)
{
    char *s = find_section(sec), *p;
    size_t kl = strlen(key);
    out[0] = 0;
    if (!s) return;
    for (p = s; *p && *p != '[';) {
        char *q = p, *e = strchr(p, '\n');
        while (*q == ' ' || *q == '\t') q++;
        if (!strnicmp(q, key, kl)) {
            char *r = q + kl;
            while (*r == ' ' || *r == '\t') r++;
            if (*r == '=') {
                int i = 0;
                r++;
                while (*r == ' ') r++;
                while (*r && *r != '\r' && *r != '\n' && i < n - 1) out[i++] = *r++;
                out[i] = 0;
                return;
            }
        }
        if (!e) break;
        p = e + 1;
    }
}

/* ---------------- helpers ---------------- */
static int exists(char *path) { return access(path, 0) == 0; }

static int copy_file(char *src, char *dst)
{
    FILE *i = fopen(src, "rb"), *o;
    static char buf[4096];
    size_t n;
    if (!i) return 0;
    o = fopen(dst, "wb");
    if (!o) { fclose(i); return 0; }
    while ((n = fread(buf, 1, sizeof(buf), i)) > 0) fwrite(buf, 1, n, o);
    fclose(i); fclose(o);
    return 1;
}

/* 3 = Windows 3.x (SYSTEM.INI), 2 / 1 = Windows 2.x / 1.x, 0 = none */
static int win_kind(char *d)
{
    char p[160];
    sprintf(p, "%s\\SYSTEM.INI", d);
    if (exists(p)) return 3;
    sprintf(p, "%s\\WIN200.BIN", d);
    if (exists(p)) return 2;
    sprintf(p, "%s\\WIN100.BIN", d);
    if (exists(p)) return 1;
    return 0;
}

static int valid_windir(char *d)
{
    return win_kind(d) != 0;
}

static void norm_dir(char *d)
{
    int n;
    char *s = d;
    while (*s == ' ' || *s == '\t') s++;
    if (s != d) memmove(d, s, strlen(s) + 1);
    n = strlen(d);
    while (n > 0 && (d[n - 1] == '\n' || d[n - 1] == '\r' || d[n - 1] == ' ')) d[--n] = 0;
    if (n > 3 && d[n - 1] == '\\') d[--n] = 0;
    if (n == 2 && d[1] == ':') strcat(d, "\\");   /* "D:" -> "D:\" */
    strupr(d);
}

static char winname[40] = "Windows 3.0";
static int win31;              /* Windows 3.1 or Windows for Workgroups 3.1x */
static int win9x;              /* Windows 95/98/ME: installed with Device Manager */
static void detect_win3(void);

/* what kind of Windows is in dir: 0 none, 1/2/3 (3 = 3.x/9x), 4 = NT */
static int describe_win(char *dir, char *name)
{
    char p[160], ssys[128], sname[40];
    int k, s31, s9x;
    sprintf(p, "%s\\SYSTEM32\\NTOSKRNL.EXE", dir);
    if (exists(p)) { strcpy(name, "Windows NT/2000/XP"); return 4; }
    k = win_kind(dir);
    if (k == 1) strcpy(name, "Windows 1.0x");
    else if (k == 2) strcpy(name, "Windows 2.xx");
    else if (k == 3) {
        strcpy(ssys, sysdir); strcpy(sname, winname); s31 = win31; s9x = win9x;
        sprintf(sysdir, "%s\\SYSTEM", dir);
        if (!exists(sysdir)) strcpy(sysdir, dir);
        win31 = win9x = 0;
        strcpy(winname, "Windows 3.0");
        detect_win3();
        /* family names in the list: 3.0, 3.1x, Windows for Workgroups 3.1x */
        if (!strncmp(winname, "Windows for Workgroups", 22)) strcpy(name, "Windows for Workgroups 3.1x");
        else if (!strncmp(winname, "Windows 3.1", 11)) strcpy(name, "Windows 3.1x");
        else strcpy(name, winname);
        strcpy(sysdir, ssys); strcpy(winname, sname); win31 = s31; win9x = s9x;
    }
    return k;
}

/* hard disks C: ... Z: (no floppies, network or CD-ROM drives) */
static int hard_disk(int d)
{
    union REGS r;
    r.x.ax = 0x4409; r.h.bl = (unsigned char)d;       /* 1 = A: */
    intdos(&r, &r);
    if (r.x.cflag || (r.x.dx & 0x1000)) return 0;      /* invalid or remote */
    r.x.ax = 0x150B; r.x.cx = d - 1; r.x.bx = 0;       /* MSCDEX: CD-ROM? */
    int86(0x2F, &r, &r);
    if (r.x.bx == 0xADAD && r.x.ax) return 0;
    return 1;
}

#define MAXFOUND 16
static char found_dir[MAXFOUND][80], found_name[MAXFOUND][40];
static int found_kind[MAXFOUND], nfound;

/* the folders in the root of every hard disk (one level) */
static void scan_windows(void)
{
    int d;
    nfound = 0;
    for (d = 3; d <= 26 && nfound < MAXFOUND; d++) {
        struct find_t f;
        char pat[8];
        unsigned rc;
        if (!hard_disk(d)) continue;
        sprintf(pat, "%c:\\*.*", 'A' + d - 1);
        for (rc = _dos_findfirst(pat, _A_SUBDIR | _A_HIDDEN | _A_SYSTEM, &f); !rc && nfound < MAXFOUND;
             rc = _dos_findnext(&f)) {
            char dir[80], name[40];
            int k;
            if (!(f.attrib & _A_SUBDIR) || f.name[0] == '.') continue;
            sprintf(dir, "%c:\\%s", 'A' + d - 1, f.name);
            k = describe_win(dir, name);
            if (!k) continue;
            strcpy(found_dir[nfound], dir);
            strcpy(found_name[nfound], name);
            found_kind[nfound++] = k;
        }
    }
}

/* the Windows to install for: C:\WINDOWS by default; the installations
   found on the hard disks are listed and chosen by number, or any other
   directory can be typed (e.g. C:\WIN30) */
static int find_windows(int interactive)
{
    char b[128];
    int i, tries, def = -1;
    if (windir[0]) {                               /* given with /DIR: */
        norm_dir(windir);
        if (valid_windir(windir)) return 1;
        printf("%s is not a Windows directory.\n", windir);
        if (!interactive) return 0;
    }
    scan_windows();
    for (i = 0; i < nfound; i++)
        if (found_kind[i] != 4 && (def < 0 || !strcmp(found_dir[i], "C:\\WINDOWS"))) def = i;
    if (!interactive) {
        if (valid_windir("C:\\WINDOWS")) strcpy(windir, "C:\\WINDOWS");
        else if (def >= 0) strcpy(windir, found_dir[def]);
        else return 0;
        return 1;
    }
    if (nfound) {
        printf("Windows installations found:\n");
        for (i = 0; i < nfound; i++)
            printf("  %2d. %-24s %s%s\n", i + 1, found_dir[i], found_name[i],
                   found_kind[i] == 4 ? " - use WINVBECF.EXE in the NT folder" : "");
    } else
        printf("No Windows installation found in the folders of the hard disks.\n");
    for (tries = 0; tries < 5; tries++) {
        if (nfound) {
            if (def >= 0) printf("Number, or another Windows directory [%d]: ", def + 1);
            else printf("Number, or another Windows directory: ");
        } else
            printf("Windows directory [C:\\WINDOWS]: ");
        if (!fgets(b, sizeof(b), stdin)) b[0] = 0;
        norm_dir(b);
        if (!b[0]) {
            if (nfound && def >= 0) sprintf(b, "%d", def + 1);
            else if (!nfound) strcpy(b, "C:\\WINDOWS");
            else continue;
        }
        if (b[0] >= '0' && b[0] <= '9') {
            int n = atoi(b);
            if (n < 1 || n > nfound) { printf("Choose 1 to %d.\n", nfound); continue; }
            if (found_kind[n - 1] == 4) {
                printf("%s is Windows NT: start it and run WINVBECF.EXE from NT\\I386\\NT3X,\n"
                       "NT\\I386\\NT4-XP or NT\\AMD64 (see README.TXT).\n", found_dir[n - 1]);
                continue;
            }
            strcpy(windir, found_dir[n - 1]);
            return 1;
        }
        if (valid_windir(b)) {
            char name[40];
            if (describe_win(b, name) == 4) {
                printf("%s is Windows NT: use WINVBECF.EXE (see README.TXT).\n", b);
                continue;
            }
            strcpy(windir, b);
            return 1;
        }
        printf("%s is not a Windows directory - enter the directory Windows is installed in (e.g. C:\\WIN30).\n", b);
    }
    return 0;
}

static void print_info(void)
{
    QWORD i;
    printf("VBE %u.%u, %lu KB video memory\n", vi.ver >> 8, vi.ver & 0xFF, vi.memkb);
    printf("Adapter : %Fs %Fs\n", vi.vendor[0] ? vi.vendor : vi.oem, vi.product);
    if (vi.edid_ok) {
        printf("Monitor : %Fs (%Fs), %s, native %ux%u @ %u Hz\n", vi.monname, vi.mfg,
               vi.digital ? "digital/LCD" : "analog", vi.natw, vi.nath, vi.nathz);
        if (vi.range_ok)
            printf("Range   : %u-%u Hz V, %u-%u kHz H, %u MHz\n", vi.vmin, vi.vmax, vi.hmin, vi.hmax, vi.maxclk);
    } else
        printf("Monitor : no EDID data\n");
    if (vi.dispi_id) printf("Emulator adapter (Bochs/QEMU/VirtualBox): any resolution possible\n");
    printf("\nAvailable modes (L = linear frame buffer, B = banked window):\n");
    for (i = 0; i < vi.nmodes; i++) {
        VQMODE far *m = &vi.modes[i];
        char t[32];
        sprintf(t, "%u x %u x %u", m->w, m->h, m->bpp);
        printf("%3u %-17s%c%c ", i + 1, t, m->linear ? 'L' : ' ', m->banked ? 'B' : ' ');
        if ((i % 3) == 2 || i + 1 == vi.nmodes) printf("\n");
    }
}

static int ask_int(char *prompt, int def)
{
    char b[32];
    printf("%s [%d]: ", prompt, def);
    if (!fgets(b, sizeof(b), stdin)) return def;
    if (b[0] == '\n' || b[0] == 0) return def;
    return atoi(b);
}

/* ---------------- Windows 3.x version ----------------
 * From SYSTEM\USER.EXE: the file version in its VERSIONINFO resource
 * (Windows 3.1: 3.10, Windows for Workgroups 3.11: 3.11; Windows 3.0 has
 * none), else the expected Windows version in the NE header (3.00 / 3.10).
 * Only if USER.EXE cannot be read: SHELL.DLL (new in Windows 3.1). */
static char msdir[16];         /* WIN1X, WIN2X, WIN30 or WIN31: the files for this Windows */

static unsigned rd16(unsigned char *p) { return p[0] | (p[1] << 8); }

/* returns 0 if no NE module; *fv = file version (0x030A ...) or 0, *ev = NE expected version */
static int module_version(char *path, unsigned *fv, unsigned *ev)
{
    static unsigned char b[1024];
    unsigned char h[0x40];
    FILE *f = fopen(path, "rb");
    long ne;
    unsigned restab, resnam, rs, q, n;
    *fv = *ev = 0;
    if (!f) return 0;
    if (fread(h, 1, 0x40, f) != 0x40 || h[0] != 'M' || h[1] != 'Z') { fclose(f); return 0; }
    ne = rd16(h + 0x3C) | ((long)rd16(h + 0x3E) << 16);
    fseek(f, ne, SEEK_SET);
    if (fread(h, 1, 0x40, f) != 0x40 || h[0] != 'N' || h[1] != 'E') { fclose(f); return 0; }
    *ev = rd16(h + 0x3E);
    restab = rd16(h + 0x24); resnam = rd16(h + 0x26);
    if (resnam > restab + 2 && resnam - restab <= sizeof(b)) {
        fseek(f, ne + restab, SEEK_SET);
        fread(b, 1, resnam - restab, f);
        rs = rd16(b);
        q = 2;
        while (q + 8 <= resnam - restab && rd16(b + q)) {
            unsigned type = rd16(b + q), cnt = rd16(b + q + 2);
            q += 8;
            if (type == 0x8010 && cnt) {                /* RT_VERSION */
                long off = (long)rd16(b + q) << rs;
                fseek(f, off, SEEK_SET);
                n = fread(b, 1, 512, f);
                for (q = 0; q + 12 <= n; q += 2)        /* VS_FIXEDFILEINFO */
                    if (b[q] == 0xBD && b[q + 1] == 0x04 && b[q + 2] == 0xEF && b[q + 3] == 0xFE) {
                        *fv = (rd16(b + q + 10) << 8) | (rd16(b + q + 8) & 0xFF);
                        break;
                    }
                break;
            }
            q += cnt * 12;
        }
    }
    fclose(f);
    return 1;
}

/* "Windows for Workgroups 3.1" / "3.11" */
static void wfw_name(unsigned fv)
{
    unsigned mi = fv & 0xFF;
    if (mi % 10) sprintf(winname, "Windows for Workgroups %u.%02u", fv >> 8, mi);
    else sprintf(winname, "Windows for Workgroups %u.%u", fv >> 8, mi / 10);
}

static void detect_win3(void)
{
    char p[160];
    unsigned fv, ev;
    sprintf(p, "%s\\USER.EXE", sysdir);
    if (module_version(p, &fv, &ev)) {
        if (fv >= 0x400 || ev >= 0x400) {           /* Windows 95 (4.00), 98 (4.10), ME (4.90) */
            unsigned mi = fv >= 0x400 ? fv & 0xFF : 0;
            win9x = 1;
            strcpy(winname, mi >= 90 ? "Windows ME" : mi >= 10 ? "Windows 98" : "Windows 95");
            return;
        }
        win31 = fv ? fv >= 0x30A : ev >= 0x30A;
        if (fv >= 0x30B) {              /* 3.11 = Windows for Workgroups 3.11 */
            wfw_name(fv);
            return;
        }
    } else {
        sprintf(p, "%s\\SHELL.DLL", sysdir);      /* new in Windows 3.1 */
        win31 = exists(p);
    }
    if (win31) {
        /* Windows for Workgroups may have the USER.EXE of Windows 3.1; its
           network driver WFWNET.DRV tells it and its version (3.10, 3.11) */
        sprintf(p, "%s\\WFWNET.DRV", sysdir);
        if (module_version(p, &fv, &ev)) {
            if (fv < 0x30A) fv = 0x30A;
            wfw_name(fv);
        } else
            strcpy(winname, "Windows 3.1");
    } else
        strcpy(winname, "Windows 3.0");
}

/* Windows 3.x: a file of the WIN30 / WIN31 folder of WINVBE (Microsoft files
   Setup would ask the Windows disks for) into SYSTEM if Windows lacks it */

/* <srcdir>\<msdir>\name, or <srcdir>\name if the folder does not have it */
static char *src_path(char *out, char *name)
{
    sprintf(out, "%s%s\\%s", srcdir, msdir, name);
    if (!exists(out)) sprintf(out, "%s%s", srcdir, name);
    return out;
}

static int ms_file(char *name, char *as)
{
    char src[160], dst[160];
    sprintf(dst, "%s\\%s", sysdir, as);
    if (exists(dst)) return 1;
    sprintf(src, "%s%s\\%s", srcdir, msdir, name);
    if (!exists(src) || !copy_file(src, dst)) return 0;
    printf("Copied %s\n", dst);
    return 1;
}

/* ---------------- 915PATCH (Intel integrated graphics) ---------------- */
static int p915;                /* this PC has a video BIOS 915PATCH can patch */
static char p915src[160];

static void check915(void)
{
    sprintf(p915src, "%s915PATCH.EXE", srcdir);
    fflush(stdout);
    if (exists(p915src) && spawnl(P_WAIT, p915src, p915src, "/CHECK", NULL) == 0) p915 = 1;
}

static int boot_drive(void)
{
    union REGS r;
    r.x.ax = 0x3305;
    intdos(&r, &r);
    return r.h.dl >= 1 && r.h.dl <= 26 ? r.h.dl : 3;
}

static int has_915(char *l)
{
    char u[200];
    int i;
    for (i = 0; l[i] && i < (int)sizeof(u) - 1; i++) u[i] = (char)toupper((unsigned char)l[i]);
    u[i] = 0;
    while (*l == ' ' || *l == '\t' || *l == '@') l++;
    if (!strnicmp(l, "REM", 3)) return 0;
    return strstr(u, "915PATCH") != NULL;
}

/* AUTOEXEC.BAT of the boot drive: add (or with add = 0 remove) the line
   that runs 915PATCH /AUTO; older 915PATCH lines are replaced */
static int autoexec_915(int add)
{
    char path[16], bak[16], line[200], ours[180];
    FILE *f;
    char *in = NULL, *out, *p, *q;
    long n = 0, k = 0;
    int drv = boot_drive(), changed = 0, placed = 0;
    sprintf(path, "%c:\\AUTOEXEC.BAT", 'A' + drv - 1);
    sprintf(bak, "%c:\\AUTOEXEC.WVB", 'A' + drv - 1);
    sprintf(ours, "%s\\915PATCH.EXE /AUTO", windir);
    f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        in = malloc((size_t)n + 1);
        if (!in || fread(in, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(in); return 0; }
        in[n] = 0;
        fclose(f);
    } else if (!add)
        return 1;
    out = malloc((size_t)n + 256);
    if (!out) { free(in); return 0; }
    p = in ? in : "";
    while (*p) {
        int l;
        q = p;
        while (*q && *q != '\n') q++;
        l = (int)(q - p) + (*q == '\n');
        memcpy(line, p, l < (int)sizeof(line) - 1 ? l : sizeof(line) - 1);
        line[l < (int)sizeof(line) - 1 ? l : sizeof(line) - 1] = 0;
        if (has_915(line)) {
            changed = 1;                            /* replaced or removed */
        } else {
            memcpy(out + k, p, l); k += l;
            /* our line right after a first "@ECHO OFF" */
            if (add && !placed && p == (in ? in : p) && !strnicmp(line, "@ECHO OFF", 9)) {
                if (k && out[k - 1] != '\n') { out[k++] = '\r'; out[k++] = '\n'; }
                k += sprintf(out + k, "%s\r\n", ours);
                placed = 1;
            }
        }
        p = q + (*q == '\n');
    }
    if (add && !placed) {                           /* at the top */
        long m = strlen(ours) + 2;
        memmove(out + m, out, (size_t)k);
        sprintf(out, "%s\r", ours); out[m - 1] = '\n';
        k += m;
    }
    if (!add && !changed) { free(in); free(out); return 1; }
    if (in) {
        f = fopen(bak, "wb");
        if (f) { fwrite(in, 1, (size_t)n, f); fclose(f); }
    }
    f = fopen(path, "wb");
    if (!f) { free(in); free(out); return 0; }
    fwrite(out, 1, (size_t)k, f);
    fclose(f);
    free(in); free(out);
    if (add) printf("AUTOEXEC.BAT: added \"%s\" (backup: AUTOEXEC.WVB)\n", ours);
    else printf("AUTOEXEC.BAT: 915PATCH line removed (backup: AUTOEXEC.WVB)\n");
    return 1;
}

/* after a successful installation: 915PATCH at every start of the PC */
static void install_915(void)
{
    char dst[160];
    if (!p915) return;
    sprintf(dst, "%s\\915PATCH.EXE", windir);
    if (!copy_file(p915src, dst)) { printf("Cannot copy %s\n", dst); return; }
    printf("Copied %s (adds the monitor resolution to the Intel video BIOS)\n", dst);
    autoexec_915(1);
}

static int install(int w, int h, int d, int hz, int large, int access_m)
{
    char src[160], dst[160], b[32];
    if (!load_ini()) { printf("Cannot read %s\\SYSTEM.INI\n", windir); return 0; }
    if (!ms_file("VGASYS.FON", "VGASYS.FON") || !ms_file("VGAFIX.FON", "VGAFIX.FON") ||
        !ms_file("VGAOEM.FON", "VGAOEM.FON"))
        printf("Warning: VGA system fonts missing in %s\n", sysdir);
    if (win31 && large) {
        ms_file("8514SYS.FON", "8514SYS.FON");
        ms_file("8514FIX.FON", "8514FIX.FON");
        ms_file("8514OEM.FON", "8514OEM.FON");
    }
    src_path(src, "WINVBE.DRV");
    sprintf(dst, "%s\\WINVBE.DRV", sysdir);
    if (!copy_file(src, dst)) { printf("Cannot copy %s to %s\n", src, dst); return 0; }
    printf("Copied %s\n", dst);
    src_path(src, "WINVBECF.EXE");
    sprintf(dst, "%s\\WINVBECF.EXE", windir);
    if (copy_file(src, dst)) printf("Copied %s\n", dst);
    /* monitor power saver (screen saver): Windows 3.1 and later */
    src_path(src, "WVBPOWER.SCR");
    sprintf(dst, "%s\\WVBPOWER.SCR", windir);
    if (win31 && exists(src) && copy_file(src, dst)) printf("Copied %s\n", dst);
    /* 386 enhanced mode helper: full-screen MS-DOS boxes on VBE modes */
    src_path(src, "WINVBE.386");
    sprintf(dst, "%s\\WINVBE.386", sysdir);
    if (exists(src) && copy_file(src, dst)) {
        printf("Copied %s\n", dst);
        ini_add("386Enh", "device", "winvbe.386");
    }
    /* WINVBE's own 120 dpi system fonts are always installed, so large fonts
       can be switched on later without the Windows disks */
    {
        static char *wf[] = { "WVBSYS.FON", "WVBFIX.FON", "WVBOEM.FON" };
        int k;
        for (k = 0; k < 3; k++) {
            src_path(src, wf[k]);
            sprintf(dst, "%s\\%s", sysdir, wf[k]);
            if (exists(src) && !exists(dst)) copy_file(src, dst);
        }
    }
    if (large) {
        /* Microsoft's 8514 fonts if Windows already has them, else WINVBE's */
        static char *ms[] = { "8514SYS.FON", "8514FIX.FON", "8514OEM.FON" };
        static char *wf[] = { "WVBSYS.FON", "WVBFIX.FON", "WVBOEM.FON" };
        char **use = ms;
        int k;
        for (k = 0; k < 3; k++) {
            sprintf(dst, "%s\\%s", sysdir, ms[k]);
            if (!exists(dst)) use = wf;
        }
        if (use == wf)
            for (k = 0; k < 3; k++) {
                sprintf(dst, "%s\\%s", sysdir, wf[k]);
                if (!exists(dst)) { printf("%s is missing - using small fonts.\n", wf[k]); large = 0; break; }
            }
        if (large) {
            ini_set("boot", "fonts.fon", use[0]);
            ini_set("boot", "fixedfon.fon", use[1]);
            ini_set("boot", "oemfonts.fon", use[2]);
        }
    }
    if (!large) {
        ini_set("boot", "fonts.fon", "vgasys.fon");
        ini_set("boot", "fixedfon.fon", "vgafix.fon");
        ini_set("boot", "oemfonts.fon", "vgaoem.fon");
    }
    ini_set("boot", "display.drv", "winvbe.drv");
    /* Windows 3.1 names the VGA grabbers *.2GR / *.3GR, Windows 3.0 *.GR2 / *.GR3;
       if Windows was installed for another display they come from WIN3x */
    ini_set("386Enh", "display", "*vddvga");
    if (win31) {
        sprintf(dst, "%s\\VGACOLOR.2GR", sysdir);
        ini_set("boot", "286grabber", exists(dst) || !ms_file("WVBCOLOR.2GR", "WVBCOLOR.2GR") ?
                "vgacolor.2gr" : "wvbcolor.2gr");
        sprintf(dst, "%s\\VGA.3GR", sysdir);
        if (exists(dst))
            ini_set("boot", "386grabber", "vga.3gr");
        else if (ms_file("WVB30.3GR", "WVB30.3GR") && ms_file("VDDVGA30.386", "VDDVGA30.386")) {
            ini_set("boot", "386grabber", "wvb30.3gr");
            ini_set("386Enh", "display", "vddvga30.386");
        }
    } else {
        sprintf(dst, "%s\\VGACOLOR.GR2", sysdir);
        ini_set("boot", "286grabber", exists(dst) || !ms_file("WVBCOLOR.GR2", "WVBCOLOR.GR2") ?
                "vgacolor.gr2" : "wvbcolor.gr2");
        sprintf(dst, "%s\\VGA.GR3", sysdir);
        ini_set("boot", "386grabber", exists(dst) || !ms_file("WVB.GR3", "WVB.GR3") ?
                "vga.gr3" : "wvb.gr3");
    }
    ini_set("boot.description", "display.drv", "WINVBE Universal VESA/VGA/SVGA");
    sprintf(b, "%d", w); ini_set("WINVBE", "Width", b);
    sprintf(b, "%d", h); ini_set("WINVBE", "Height", b);
    sprintf(b, "%d", d); ini_set("WINVBE", "Depth", b);
    sprintf(b, "%d", hz); ini_set("WINVBE", "Refresh", b);
    ini_set("WINVBE", "DPI", large ? "120" : "96");
    sprintf(b, "%d", access_m); ini_set("WINVBE", "Access", b);
    ini_set("WINVBE", "Mode", "0");
    if (!save_ini()) { printf("Cannot write SYSTEM.INI\n"); return 0; }
    install_915();
    printf("SYSTEM.INI updated (backup: SYSTEM.WVB). Start Windows with WIN.\n");
    if (win31)
        printf("Monitor power saving: Control Panel > Desktop > Screen Saver \"WINVBE Power Saver\".\n");
    return 1;
}

/* ---------------- Windows 1.x/2.x: WIN100/WIN200.BIN ----------------
 * Windows 1.x/2.x Setup links the core modules (KERNEL ... DISPLAY ... USER)
 * into WIN100.BIN / WIN200.BIN and WIN100.OVL / WIN200.OVL:
 *  - .BIN: KERNEL's DOS header, then per module the NE header with its tables
 *    (without the non-resident names), the PRELOAD segments (with their
 *    relocation records) and the PRELOAD resources, each on a paragraph
 *    boundary; NE+08h holds the paragraph of the next module (0 = end),
 *  - .OVL: per module the non-resident names (NE+2Ch = paragraph in the
 *    .OVL), the segments that are not preloaded and a copy of every resource
 *    and of every preloaded discardable segment (to reload them),
 *  - segment/resource offsets are paragraphs in the .BIN for preloaded
 *    objects and in the .OVL otherwise; for preloaded discardable segments
 *    the minimum allocation field and for preloaded resources the reserved
 *    handle field hold the paragraph of the .OVL copy.
 * INSTALL replaces the DISPLAY module with WINVBE.DRV: its .OVL part is
 * appended to the .OVL, the modules after it move in the .BIN.  The
 * original files are kept as WIN?00.BAK / WIN?00.OBK; they are the source
 * of every later installation and are put back with INSTALL /VGA. */
#define NE_PRELOAD 0x0040
#define NE_DISCARD 0x1000
#define NE_RELOCS  0x0100
#define MAXSEG     16
#define MAXRES     64

static unsigned char nehdr[0x800];
static char cbuf[4096];

static unsigned rdw(unsigned char *p) { return p[0] | (p[1] << 8); }
static void wrw(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

static unsigned fgetw(FILE *f, long pos)
{
    unsigned char b[2] = { 0, 0 };
    fseek(f, pos, SEEK_SET);
    fread(b, 1, 2, f);
    return rdw(b);
}

static int fcopy(FILE *o, FILE *i, long pos, long n)
{
    fseek(i, pos, SEEK_SET);
    while (n > 0) {
        size_t k = n > (long)sizeof(cbuf) ? sizeof(cbuf) : (size_t)n;
        if (fread(cbuf, 1, k, i) != k || fwrite(cbuf, 1, k, o) != k) return 0;
        n -= k;
    }
    return 1;
}

static long fpad(FILE *o)
{
    long p = ftell(o);
    while (p & 15) { fputc(0, o); p++; }
    return p;
}

static long pad16(long n) { return (n + 15) & ~15L; }

static int copy_named(char *dir, char *a, char *b)
{
    char s[160], t[160];
    sprintf(s, "%s\\%s", dir, a);
    sprintf(t, "%s\\%s", dir, b);
    return copy_file(s, t);
}

/* position of the DISPLAY module in the .BIN and of the next module */
static int find_display(FILE *b, long *start, long *end)
{
    long ne = 0x400;
    int n;
    for (n = 0; n < 64; n++) {
        unsigned char h[0x40], name[16];
        unsigned nxt, rn;
        fseek(b, ne, SEEK_SET);
        if (fread(h, 1, 0x40, b) != 0x40 || h[0] != 'N' || h[1] != 'E') return 0;
        nxt = rdw(h + 8);
        rn = rdw(h + 0x26);
        fseek(b, ne + rn, SEEK_SET);
        fread(name, 1, 8, b);
        if (name[0] == 7 && !memcmp(name + 1, "DISPLAY", 7)) {
            *start = ne;
            *end = (long)nxt << 4;
            return nxt != 0;
        }
        if (!nxt) return 0;
        ne = (long)nxt << 4;
    }
    return 0;
}

/* is the DISPLAY module of these files WINVBE (description in the .OVL)? */
static int display_is_winvbe(char *binp, char *ovlp)
{
    FILE *b = fopen(binp, "rb"), *o = fopen(ovlp, "rb");
    long s, e;
    int r = 0;
    if (b && o && find_display(b, &s, &e)) {
        unsigned np = fgetw(b, s + 0x2C), i;
        unsigned char t[80];
        fseek(o, (long)np << 4, SEEK_SET);
        memset(t, 0, sizeof(t));
        fread(t, 1, sizeof(t) - 1, o);
        for (i = 0; i + 6 < sizeof(t); i++)
            if (!memcmp(t + i, "WINVBE", 6)) r = 1;
    }
    if (b) fclose(b);
    if (o) fclose(o);
    return r;
}

/* add delta paragraphs to the .BIN offsets of the modules from ne on */
static int shift_modules(FILE *f, long ne, int delta)
{
    int n;
    for (n = 0; n < 64 && ne; n++) {
        unsigned char h[0x40];
        unsigned nxt, segtab, restab, resnam, nseg, i;
        fseek(f, ne, SEEK_SET);
        if (fread(h, 1, 0x40, f) != 0x40) return 0;
        if (h[0] != 'N' || h[1] != 'E') return 1;      /* end block (zeros) */
        nxt = rdw(h + 8);
        if (nxt) wrw(h + 8, nxt + delta);
        fseek(f, ne, SEEK_SET);
        fwrite(h, 1, 0x40, f);
        segtab = rdw(h + 0x22); restab = rdw(h + 0x24); resnam = rdw(h + 0x26);
        nseg = rdw(h + 0x1C);
        if (nseg * 8U > sizeof(cbuf)) return 0;
        fseek(f, ne + segtab, SEEK_SET);
        fread(cbuf, 1, nseg * 8, f);
        for (i = 0; i < nseg; i++) {
            unsigned char *e = (unsigned char *)cbuf + i * 8;
            if (rdw(e) && (rdw(e + 4) & NE_PRELOAD)) wrw(e, rdw(e) + delta);
        }
        fseek(f, ne + segtab, SEEK_SET);
        fwrite(cbuf, 1, nseg * 8, f);
        if (resnam > restab + 2) {
            unsigned len = resnam - restab, q = 2;
            if (len > sizeof(cbuf)) return 0;
            fseek(f, ne + restab, SEEK_SET);
            fread(cbuf, 1, len, f);
            while (q + 8 <= len && rdw((unsigned char *)cbuf + q)) {
                unsigned cnt = rdw((unsigned char *)cbuf + q + 2), j;
                q += 8;
                for (j = 0; j < cnt && q + 12 <= len; j++, q += 12) {
                    unsigned char *e = (unsigned char *)cbuf + q;
                    if (rdw(e + 4) & NE_PRELOAD) wrw(e, rdw(e) + delta);
                }
            }
            fseek(f, ne + restab, SEEK_SET);
            fwrite(cbuf, 1, len, f);
        }
        ne = nxt ? ((long)(nxt + delta) << 4) : 0;
    }
    return 1;
}

/* build <windir>\WIN?00.BIN/.OVL from the backups with drv as DISPLAY */
static int failline;
#define FAIL { failline = __LINE__; goto out; }

static int link_display(char *drv, char *bak, char *obk, char *binp, char *ovlp)
{
    FILE *d = 0, *b = 0, *o = 0, *nb = 0, *no = 0;
    static char tb[160], to[160];
    long ne, nonres, start, end, cur, opos, ovlend, binlen;
    static long segpos[MAXSEG], segsz[MAXSEG], respos[MAXRES], ressz[MAXRES];
    unsigned hl, nseg, segtab, restab, resnam, i, nres = 0, q, rs = 4, nonreslen, ok = 0;
    sprintf(tb, "%s\\WINVBE.$B$", windir);
    sprintf(to, "%s\\WINVBE.$O$", windir);
    d = fopen(drv, "rb"); b = fopen(bak, "rb"); o = fopen(obk, "rb");
    if (!d || !b || !o) FAIL;
    if (fgetw(d, 0) != 0x5A4D) FAIL;
    ne = fgetw(d, 0x3C) | ((long)fgetw(d, 0x3E) << 16);
    fseek(d, ne, SEEK_SET);
    if (fread(nehdr, 1, 0x40, d) != 0x40 || nehdr[0] != 'N' || nehdr[1] != 'E') FAIL;
    nonres = rdw(nehdr + 0x2C) | ((long)rdw(nehdr + 0x2E) << 16);
    if (nonres <= ne || nonres - ne > (long)sizeof(nehdr)) FAIL;
    hl = (unsigned)(nonres - ne);
    fseek(d, ne, SEEK_SET);
    fread(nehdr, 1, hl, d);
    nonreslen = rdw(nehdr + 0x20);
    nseg = rdw(nehdr + 0x1C); segtab = rdw(nehdr + 0x22);
    restab = rdw(nehdr + 0x24); resnam = rdw(nehdr + 0x26);
    if (nseg > MAXSEG || rdw(nehdr + 0x32) != 4) FAIL;
    if (!find_display(b, &start, &end)) FAIL;
    fseek(b, 0, SEEK_END); binlen = ftell(b);
    fseek(o, 0, SEEK_END); ovlend = pad16(ftell(o));
    /* layout */
    cur = start + pad16(hl);
    opos = ovlend;
    wrw(nehdr + 0x2C, (unsigned)(opos >> 4)); wrw(nehdr + 0x2E, 0);
    opos += pad16(nonreslen);
    for (i = 0; i < nseg; i++) {
        unsigned char *e = nehdr + segtab + i * 8;
        unsigned fl = rdw(e + 4);
        segpos[i] = (long)rdw(e) << 4;
        segsz[i] = 0;
        if (!segpos[i]) continue;
        segsz[i] = rdw(e + 2) ? rdw(e + 2) : 0x10000L;
        if (fl & NE_RELOCS) segsz[i] += 2 + 8L * fgetw(d, segpos[i] + segsz[i]);
        if (fl & NE_PRELOAD) {
            wrw(e, (unsigned)(cur >> 4)); cur += pad16(segsz[i]);
            if (fl & NE_DISCARD) { wrw(e + 6, (unsigned)(opos >> 4)); opos += pad16(segsz[i]); }
        } else {
            wrw(e, (unsigned)(opos >> 4)); opos += pad16(segsz[i]);
        }
    }
    if (resnam > restab + 2) {
        rs = rdw(nehdr + restab);
        if (rs != 4) FAIL;
        q = restab + 2;
        while (q + 8 <= resnam && rdw(nehdr + q)) {
            unsigned cnt = rdw(nehdr + q + 2), j;
            q += 8;
            for (j = 0; j < cnt; j++, q += 12) {
                unsigned char *e = nehdr + q;
                if (nres >= MAXRES) FAIL;
                respos[nres] = (long)rdw(e) << 4;
                ressz[nres] = (long)rdw(e + 2) << 4;
                if (rdw(e + 4) & NE_PRELOAD) {
                    wrw(e, (unsigned)(cur >> 4)); cur += ressz[nres];
                    wrw(e + 8, (unsigned)(opos >> 4));
                } else {
                    wrw(e, (unsigned)(opos >> 4));
                    wrw(e + 8, 0);
                }
                opos += ressz[nres];
                nres++;
            }
        }
    }
    wrw(nehdr + 8, (unsigned)(cur >> 4));                 /* next module */
    nehdr[0x0A] = nehdr[0x0B] = 0;
    /* new .BIN */
    nb = fopen(tb, "w+b"); no = fopen(to, "wb");
    if (!nb || !no) FAIL;
    if (!fcopy(nb, b, 0, start)) FAIL;
    fwrite(nehdr, 1, hl, nb); fpad(nb);
    for (i = 0; i < nseg; i++)
        if (segpos[i] && (rdw(nehdr + segtab + i * 8 + 4) & NE_PRELOAD)) {
            if (!fcopy(nb, d, segpos[i], segsz[i])) FAIL;
            fpad(nb);
        }
    q = restab + 2; nres = 0;
    if (resnam > restab + 2)
        while (q + 8 <= resnam && rdw(nehdr + q)) {
            unsigned cnt = rdw(nehdr + q + 2), j;
            q += 8;
            for (j = 0; j < cnt; j++, q += 12, nres++)
                if (rdw(nehdr + q + 4) & NE_PRELOAD) {
                    if (!fcopy(nb, d, respos[nres], ressz[nres])) FAIL;
                    fpad(nb);
                }
        }
    if (ftell(nb) != cur) FAIL;
    if (!fcopy(nb, b, end, binlen - end)) FAIL;
    binlen = ftell(nb);
    if (!shift_modules(nb, cur, (int)((cur - end) >> 4))) FAIL;
    {   /* KERNEL's DOS header describes the whole file */
        unsigned char m[4];
        wrw(m, (unsigned)(binlen % 512)); wrw(m + 2, (unsigned)((binlen + 511) / 512));
        fseek(nb, 2, SEEK_SET); fwrite(m, 1, 4, nb);
    }
    /* new .OVL: the old one + DISPLAY's part */
    fseek(o, 0, SEEK_END);
    if (!fcopy(no, o, 0, ftell(o))) FAIL;
    fpad(no);
    if (!fcopy(no, d, nonres, nonreslen)) FAIL;
    fpad(no);
    for (i = 0; i < nseg; i++) {
        unsigned fl = rdw(nehdr + segtab + i * 8 + 4);
        if (segpos[i] && (!(fl & NE_PRELOAD) || (fl & NE_DISCARD))) {
            if (!fcopy(no, d, segpos[i], segsz[i])) FAIL;
            fpad(no);
        }
    }
    for (i = 0; i < nres; i++)
        if (!fcopy(no, d, respos[i], ressz[i])) FAIL;
    if (ftell(no) != opos) FAIL;
    ok = 1;
out:
    if (d) fclose(d);
    if (b) fclose(b);
    if (o) fclose(o);
    if (nb && fclose(nb)) ok = 0;
    if (no && fclose(no)) ok = 0;
    if (ok) {
        remove(binp); remove(ovlp);
        ok = !rename(tb, binp) && !rename(to, ovlp);
    } else {
        remove(tb); remove(to);
    }
    return ok;
}

/* the driver into WIN100/WIN200.BIN; 0 = error */
static int link_old(void)
{
    char drv[160], binp[160], ovlp[160], bak[160], obk[160], n[16];
    int v = oldwin == 1 ? 100 : 200;
    sprintf(drv, "%s%s\\WINVBE.DRV", srcdir, msdir);
    if (!exists(drv)) { printf("%s\\WINVBE.DRV not found next to INSTALL.EXE.\n", msdir); return 0; }
    sprintf(binp, "%s\\WIN%d.BIN", windir, v); sprintf(ovlp, "%s\\WIN%d.OVL", windir, v);
    sprintf(bak, "%s\\WIN%d.BAK", windir, v); sprintf(obk, "%s\\WIN%d.OBK", windir, v);
    if (!exists(ovlp)) { printf("%s not found.\n", ovlp); return 0; }
    /* the backups are the files of the last Windows Setup */
    if (!exists(bak) || !exists(obk) || !display_is_winvbe(binp, ovlp)) {
        sprintf(n, "WIN%d.BIN", v);
        if (!copy_named(windir, n, strrchr(bak, '\\') + 1)) { printf("Cannot write %s\n", bak); return 0; }
        sprintf(n, "WIN%d.OVL", v);
        if (!copy_named(windir, n, strrchr(obk, '\\') + 1)) { printf("Cannot write %s\n", obk); return 0; }
        printf("Original files saved as WIN%d.BAK and WIN%d.OBK.\n", v, v);
    }
    if (!link_display(drv, bak, obk, binp, ovlp)) {
        printf("Could not build WIN%d.BIN / WIN%d.OVL (disk full or unknown format, %d);\n"
               "the Windows files were not changed.\n", v, v, failline);
        return 0;
    }
    printf("WINVBE.DRV installed into WIN%d.BIN / WIN%d.OVL.\n", v, v);
    return 1;
}

/* INSTALL /VGA on Windows 1.x/2.x: put the files of the last Setup back */
static int restore_old(void)
{
    char bak[160], obk[160], n[16];
    int v = oldwin == 1 ? 100 : 200;
    sprintf(bak, "%s\\WIN%d.BAK", windir, v); sprintf(obk, "%s\\WIN%d.OBK", windir, v);
    if (!exists(bak) || !exists(obk)) {
        printf("No backup (WIN%d.BAK / WIN%d.OBK) - run Windows Setup again and choose\n"
               "the original display.\n", v, v);
        return 0;
    }
    sprintf(n, "WIN%d.BIN", v);
    if (!copy_named(windir, strrchr(bak, '\\') + 1, n)) return 0;
    sprintf(n, "WIN%d.OVL", v);
    if (!copy_named(windir, strrchr(obk, '\\') + 1, n)) return 0;
    printf("Windows %d.x switched back to its original display driver.\n", oldwin);
    autoexec_915(0);
    return 1;
}

/* Windows 1.x/2.x: driver into WIN100/WIN200.BIN, settings into WIN.INI [WINVBE] */
static int install_old(int w, int h, int d, int hz, int access_m)
{
    char b[32];
    if (!load_ini()) { printf("Cannot read %s\\WIN.INI\n", windir); return 0; }
    if (!link_old()) return 0;
    {
        char src[160], dst[160];
        sprintf(src, "%s%s\\WINVBECF.EXE", srcdir, msdir);
        sprintf(dst, "%s\\WINVBECF.EXE", windir);
        if (exists(src) && copy_file(src, dst)) printf("Copied %s (settings program)\n", dst);
    }
    sprintf(b, "%d", w); ini_set("WINVBE", "Width", b);
    sprintf(b, "%d", h); ini_set("WINVBE", "Height", b);
    sprintf(b, "%d", d); ini_set("WINVBE", "Depth", b);
    sprintf(b, "%d", hz); ini_set("WINVBE", "Refresh", b);
    sprintf(b, "%d", access_m); ini_set("WINVBE", "Access", b);
    ini_set("WINVBE", "Mode", "0");
    if (!save_ini()) { printf("Cannot write WIN.INI\n"); return 0; }
    install_915();
    printf("WIN.INI [WINVBE] updated (backup: WIN.WVB). Start Windows with WIN.\n");
    return 1;
}

static int revert_vga(void)
{
    if (!load_ini()) { printf("Cannot read SYSTEM.INI\n"); return 0; }
    ini_set("boot", "display.drv", "vga.drv");
    ini_remove("386Enh", "device", "winvbe.386");
    ini_set("boot", "fonts.fon", "vgasys.fon");
    ini_set("boot", "fixedfon.fon", "vgafix.fon");
    ini_set("boot", "oemfonts.fon", "vgaoem.fon");
    ini_set("boot.description", "display.drv", "VGA");
    if (!save_ini()) return 0;
    printf("Windows switched back to the standard VGA driver.\n");
    autoexec_915(0);
    return 1;
}

int main(int argc, char **argv)
{
    int i, w = -1, h = -1, d = -1, hz = 0, large = 0, list = 0, vga = 0, acc = 0, unattended = 0;
    char *s;
    printf("WINVBE v1.1 Setup - Universal VESA/VGA/SVGA display driver for Windows 1.x-3.11\n\n");
    strcpy(srcdir, argv[0]);
    s = strrchr(srcdir, '\\');
    if (s) s[1] = 0; else srcdir[0] = 0;
    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if (*a == '/' || *a == '-') a++;
        if (!strnicmp(a, "W:", 2)) { w = atoi(a + 2); unattended = 1; }
        else if (!strnicmp(a, "H:", 2)) { h = atoi(a + 2); unattended = 1; }
        else if (!strnicmp(a, "D:", 2)) { d = atoi(a + 2); unattended = 1; }
        else if (!strnicmp(a, "R:", 2)) hz = atoi(a + 2);
        else if (!strnicmp(a, "A:", 2)) acc = atoi(a + 2);
        else if (!stricmp(a, "LARGE")) large = 1;
        else if (!stricmp(a, "LIST")) list = 1;
        else if (!stricmp(a, "VGA")) vga = 1;
        else if (!stricmp(a, "AUTO")) { w = 0; h = 0; d = 0; unattended = 1; }
        else if (!strnicmp(a, "DIR:", 4)) strcpy(windir, a + 4);
        else if (!stricmp(a, "?") || !stricmp(a, "H")) {
            printf("INSTALL [/LIST] [/VGA] [/AUTO] [/W:x /H:y /D:bpp] [/R:hz] [/A:0|1|2] [/LARGE] [/DIR:windir]\n");
            return 0;
        }
    }
    {   /* NTVDM reports DOS 5.50 (INT 21h AX=3306h) */
        union REGS r;
        r.x.ax = 0x3306; r.x.bx = 0;
        intdos(&r, &r);
        if (r.x.bx == 0x3205) {
            printf("This is Windows NT. Use WINVBECF.EXE in NT\\I386\\NT3X, NT\\I386\\NT4-XP or\n"
                   "NT\\AMD64 instead (see README.TXT).\n");
            return 1;
        }
    }
    if (!vq_query(&vi)) printf("No VESA BIOS Extensions found - only standard VGA 320x200x256 will be used.\n");
    vq_sort(&vi);
    if (list) { print_info(); return 0; }
    if (!find_windows(!unattended && !vga)) {
        printf("Windows directory not found. Use /DIR:C:\\WINDOWS (or /DIR:C:\\WIN30 ...)\n");
        return 1;
    }
    {
        char p[160];
        sprintf(p, "%s\\SYSTEM32\\NTOSKRNL.EXE", windir);
        if (exists(p)) {
            printf("%s is a Windows NT directory. Start Windows NT and use WINVBECF.EXE\n"
                   "in NT\\I386\\NT3X, NT\\I386\\NT4-XP or NT\\AMD64 (see README.TXT).\n", windir);
            return 1;
        }
    }
    sprintf(sysdir, "%s\\SYSTEM", windir);
    if (!exists(sysdir)) strcpy(sysdir, windir);
    {
        int k = win_kind(windir);
        char p[160];
        if (k < 3) { oldwin = k; inifile = "WIN.INI"; inibak = "WIN.WVB"; }
        if (!oldwin) detect_win3();
        strcpy(msdir, oldwin == 1 ? "WIN1X" : oldwin == 2 ? "WIN2X" : win31 ? "WIN31" : "WIN30");
    }
    if (win9x) {
        printf("%s found in %s.\n"
               "\nWINVBE cannot be installed from DOS on %s.\n"
               "Install it in Windows with Device Manager: Control Panel > System >\n"
               "Device Manager > Display adapters > (your adapter) > Properties > Driver >\n"
               "Change Driver (Update Driver) > Have Disk > the WIN9X folder of WINVBE.\n"
               "Then change resolution and colors in Display Properties or with WINVBECF.\n",
               winname, windir, winname);
        return 1;
    }
    printf("%s found in %s - files from the %s folder.\n",
           oldwin == 1 ? "Windows 1.0x" : oldwin == 2 ? "Windows 2.xx" : winname, windir, msdir);
    if (oldwin == 1 && _osmajor >= 5)
        printf("Note: Windows 1.0x on MS-DOS 5 or later needs SETVER WIN100.BIN 3.30\n"
               "(and DEVICE=C:\\DOS\\SETVER.EXE in CONFIG.SYS) to start programs.\n");
    if (!vga) {
        /* Intel integrated graphics: the monitor resolution into the video
           BIOS now, so that it is in the mode list below */
        check915();
        if (p915) {
            printf("\nIntel graphics with a patchable video BIOS: 915PATCH /AUTO adds the\n"
                   "resolution of the monitor.\n");
            fflush(stdout);
            spawnl(P_WAIT, p915src, p915src, "/AUTO", "/NOLOG", NULL);   /* not on the installation disk */
            vq_query(&vi);
            vq_sort(&vi);
            printf("\n");
        }
    }
    if (vga && oldwin) return restore_old() ? 0 : 1;
    if (vga) return revert_vga() ? 0 : 1;
    if (!unattended) {
        int n;
        print_info();
        printf("\nEnter a mode number from the list, 0 = automatic (monitor native)");
        if (vi.dispi_id) printf(",\n-1 = custom size");
        printf("\n");
        n = ask_int("Mode", 0);
        if (n > 0 && n <= (int)vi.nmodes) {
            w = vi.modes[n - 1].w; h = vi.modes[n - 1].h; d = vi.modes[n - 1].bpp;
        } else if (n == -1) {
            w = ask_int("Width", 1024); h = ask_int("Height", 768); d = ask_int("Bits per pixel (8/15/16/24/32)", 16);
        } else { w = h = 0; d = ask_int("Bits per pixel (8/15/16/24/32, 0 = auto)", 16); }
        if (vi.ver >= 0x300)
            hz = ask_int("Refresh rate in Hz (0 = BIOS default, 1 = monitor native)", 0);
        if (!oldwin)
            large = ask_int("Font size: 0 = small (96 dpi), 1 = large (120 dpi)", 0);
        printf("Memory access: 0 = automatic, 1 = linear frame buffer only,\n"
               "               2 = VGA window at A000h (use when the frame buffer is above 4 GB)\n");
        acc = ask_int("Memory access", 0);
        if (acc < 0 || acc > 2) acc = 0;
    }
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (d < 0) d = 0;
    if (oldwin) {
        if (acc == 1) acc = 0;          /* 1.x/2.x run in real mode: no LFB */
        return install_old(w, h, d, hz, acc) ? 0 : 1;
    }
    return install(w, h, d, hz, large, acc) ? 0 : 1;
}
