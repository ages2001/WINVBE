/*
 * 915PATCH.VXD - real mode initialization part.
 *
 * Windows ME has no MS-DOS mode, so 915PATCH.EXE cannot run before Windows.
 * A VxD listed in SYSTEM.INI ([386Enh] device=915PATCH.VXD) gets control in
 * real mode while Windows (3.x enhanced mode, 95, 98, ME) is being loaded,
 * before any Windows display driver uses the video BIOS.  This code runs the
 * same patch as 915PATCH.EXE there and then tells the loader not to load
 * the (empty) protected mode part.
 *
 * Settings (all optional) in SYSTEM.INI:
 *   [915PATCH]
 *   Patch=/AUTO                 default: monitor native resolution (EDID)
 *   Patch=/AUTO 1280 800        given resolution
 *   Patch=5C 1280 800 32; 5A 1280 800 16    one or more modes, ';' between
 *   Log=1                       write 915PATCH.LOG to the Windows directory
 *                               (default 1, Log=0 turns it off)
 */
#include <string.h>
#include <stdarg.h>
#include <dos.h>
#include <fcntl.h>
#include "i915core.h"

int patch_run(int argc, char **argv);

static char logbuf[1100];
static unsigned loglen;

#ifdef DEBUGE9
#include <conio.h>
#endif
static void put(char c)
{
#ifdef DEBUGE9
    outp(0xE9, c);
#endif
    if (loglen >= sizeof(logbuf) - 2) return;
    if (c == '\n') logbuf[loglen++] = '\r';
    logbuf[loglen++] = c;
}

/* printf subset: %[-][0][width][l](d|u|x|X|s|c) */
void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    while (*fmt) {
        char tmp[12], *t;
        const char *s;
        int width = 0, zero = 0, left = 0, lng = 0, neg = 0, base = 10, upper = 0, len;
        unsigned long v;
        if (*fmt != '%') { put(*fmt++); continue; }
        fmt++;
        if (*fmt == '-') { left = 1; fmt++; }
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + *fmt++ - '0';
        if (*fmt == 'l') { lng = 1; fmt++; }
        switch (*fmt++) {
        case 's':
            s = va_arg(ap, const char *);
            if (!s) s = "";
            len = strlen(s);
            if (!left) while (width-- > len) put(' ');
            while (*s) put(*s++);
            if (left) while (width-- > len) put(' ');
            continue;
        case 'c':
            put((char)va_arg(ap, int));
            continue;
        case 'd':
            if (lng) { long x = va_arg(ap, long); if (x < 0) { neg = 1; x = -x; } v = (unsigned long)x; }
            else { int x = va_arg(ap, int); if (x < 0) { neg = 1; x = -x; } v = (unsigned)x; }
            break;
        case 'X': upper = 1;    /* fall through */
        case 'x': base = 16;    /* fall through */
        case 'u':
            v = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            break;
        case '%':
            put('%');
            continue;
        default:
            continue;
        }
        t = tmp + sizeof(tmp);
        *--t = 0;
        do {
            int d = (int)(v % base);
            *--t = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
            v /= base;
        } while (v);
        len = strlen(t) + neg;
        if (neg && (zero || left)) put('-');
        if (!left) while (width-- > len) put(zero ? '0' : ' ');
        if (neg && !zero && !left) put('-');
        while (*t) put(*t++);
        if (left) while (width-- > len) put(' ');
    }
    va_end(ap);
}

static unsigned cur_psp(void);
#pragma aux cur_psp = "mov ah,62h" "int 21h" value [bx] modify [ax];

static char windir[80];
static char patch[160] = "/AUTO";
static int  dolog = 1;

/* windir=... from a DOS environment block */
static int env_windir(unsigned seg)
{
    char __far *e = (char __far *)MK_FP(seg, 0);
    unsigned n = 0;
    if (!seg) return 0;
    while (n < 0x8000 && e[n]) {
        if (!_fmemcmp(e + n, "windir=", 7) || !_fmemcmp(e + n, "WINDIR=", 7)) {
            unsigned i = 0;
            n += 7;
            while (e[n] && i < sizeof(windir) - 14) windir[i++] = e[n++];
            windir[i] = 0;
            if (i && windir[i - 1] == '\\') windir[i - 1] = 0;
            return i > 0;
        }
        while (n < 0x8000 && e[n]) n++;
        n++;
    }
    return 0;
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n' || e[-1] == 0x1A)) *--e = 0;
    return s;
}

static void ini_line(char *l, int *insec)
{
    char *k, *v;
    l = trim(l);
    if (*l == '[') {
        *insec = !strnicmp(l, "[915PATCH]", 10);
        return;
    }
    if (!*insec || *l == ';') return;
    v = strchr(l, '=');
    if (!v) return;
    *v++ = 0;
    k = trim(l);
    v = trim(v);
    if (!stricmp(k, "Patch") && *v) {
        strncpy(patch, v, sizeof(patch) - 1);
        patch[sizeof(patch) - 1] = 0;
    } else if (!stricmp(k, "Log")) {
        dolog = *v != '0';
    }
}

static void read_ini(void)
{
    static char path[96], buf[256], line[200];
    int h, insec = 0;
    unsigned got, i, ll = 0;
    strcpy(path, windir);
    strcat(path, "\\SYSTEM.INI");
    if (_dos_open(path, O_RDONLY, &h)) { say("%s not found, using Patch=%s\n", path, patch); return; }
    while (!_dos_read(h, buf, sizeof(buf), &got) && got) {
        for (i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\n') {
                line[ll] = 0;
                ini_line(line, &insec);
                ll = 0;
            } else if (ll < sizeof(line) - 1) {
                line[ll++] = c;
            }
        }
    }
    if (ll) { line[ll] = 0; ini_line(line, &insec); }
    _dos_close(h);
}

static void write_log(void)
{
    static char path[96];
    int h;
    unsigned done;
    strcpy(path, windir);
    strcat(path, "\\915PATCH.LOG");
    if (_dos_creat(path, _A_NORMAL, &h)) return;
    _dos_write(h, logbuf, loglen, &done);
    _dos_close(h);
}

/* called by vxdrm.asm; envseg = SI at entry (environment segment) */
void vxd_main(unsigned envseg)
{
    static char *argv[10];
    char *p, *cmd;
    unsigned psp = cur_psp();

    if (!env_windir(*(unsigned __far *)MK_FP(psp, 0x2C)) && !env_windir(envseg))
        strcpy(windir, "C:\\WINDOWS");
    say("915PATCH.VXD - WINVBE v1.1 video BIOS mode patcher for Intel graphics\n\n");
    read_ini();
    say("Patch=%s\n", patch);
    for (cmd = patch; cmd; ) {
        int argc = 1;
        char *semi = strchr(cmd, ';');
        if (semi) *semi++ = 0;
        argv[0] = "915PATCH";
        for (p = cmd; *p && argc < 9; ) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = 0;
        }
        argv[argc] = 0;
        if (argc > 1) {
            say("\n> ");
            { int i; for (i = 1; i < argc; i++) say("%s ", argv[i]); }
            say("\n");
            patch_run(argc, argv);
        }
        cmd = semi;
    }
    if (dolog) write_log();
}
