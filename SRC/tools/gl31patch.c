/*
 * gl31patch.c - let an OpenGL program load and run on Windows NT 3.1 with
 * WINVBE's OpenGL.
 *
 * Windows NT 3.1's GDI32.DLL has none of the OpenGL pixel-format or buffer
 * functions (ChoosePixelFormat, SetPixelFormat, DescribePixelFormat,
 * GetPixelFormat, SwapBuffers) - they arrived in NT 3.5. A program that
 * imports them from GDI32 therefore fails to load on NT 3.1 with
 * "the procedure entry point ChoosePixelFormat could not be located in
 * GDI32.DLL", before any OpenGL code runs.
 *
 * WINVBE ships WVGDI.DLL: it exports every NT 3.1 GDI32 function as a
 * forwarder to GDI32 (so it behaves exactly like GDI32) plus the five OpenGL
 * functions, forwarded to WINVBE's OPENGL32.DLL, which implements them.
 *
 * This tool makes the program use WVGDI.DLL in place of GDI32.DLL: it finds
 * the import that pulls in the OpenGL functions and renames its "GDI32.DLL"
 * to "WVGDI.DLL" in place (both nine characters). Nothing else changes - the
 * import table keeps its shape, which is why NT 3.1's loader is happy with it
 * (it does not accept added import descriptors). Every ordinary GDI call then
 * reaches GDI32 through WVGDI's forwarders, and the five OpenGL imports reach
 * OPENGL32.
 *
 * It makes a .bak copy first. Run it once per program:
 *     GL31PAT program.exe
 * Copy WVGDI.DLL, OPENGL32.DLL and WVBGL32.DLL/WVBGLX.DLL next to the program
 * (or into the system directory). It is a plain file rewriter (KERNEL32 only)
 * and runs on NT 3.1 itself as well as on any later Windows. On NT 3.5x and
 * later GDI32 already has these functions, so no patching is needed there.
 *
 * Build:  see gl/build.sh (output GL31PAT.EXE; an 8.3 name so it runs
 *         on NT 3.1 FAT volumes)
 */

#include <windows.h>

/* ---- no C runtime: the few helpers we need -------------------------------- */

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void mcpy(void *d, const void *s, unsigned n)
{ unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; }
static int seqi(const char *a, const char *b)       /* case-insensitive equal */
{
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* A GUI program (so it loads and runs the same way everywhere, NT 3.1
 * included): messages are collected and shown in a message box. */
static char msg[1024];
static unsigned msglen;
static void out(const char *s)
{
    while (*s && msglen < sizeof(msg) - 1) msg[msglen++] = *s++;
    msg[msglen] = 0;
}

/* ---- little-endian field access on the file image ------------------------- */

static unsigned char *img;
static DWORD imglen;
static DWORD rd32(DWORD o) { return img[o] | (img[o+1]<<8) | (img[o+2]<<16) | ((DWORD)img[o+3]<<24); }
static WORD  rd16(DWORD o) { return (WORD)(img[o] | (img[o+1]<<8)); }
static void  wr32(DWORD o, DWORD v) { img[o]=(BYTE)v; img[o+1]=(BYTE)(v>>8); img[o+2]=(BYTE)(v>>16); img[o+3]=(BYTE)(v>>24); }

#define MAXSEC 96
static WORD  nsec;
static DWORD sec_va[MAXSEC], sec_vs[MAXSEC], sec_raw[MAXSEC], sec_rs[MAXSEC];

static DWORD rva2off(DWORD rva)
{
    int i;
    for (i = 0; i < nsec; i++) {
        DWORD vend = sec_va[i] + (sec_vs[i] > sec_rs[i] ? sec_vs[i] : sec_rs[i]);
        if (rva >= sec_va[i] && rva < vend) return rva - sec_va[i] + sec_raw[i];
    }
    if (rva < sec_va[0]) return rva;
    return 0;
}

static const char *const glnames[] = {
    "ChoosePixelFormat", "SetPixelFormat", "DescribePixelFormat",
    "GetPixelFormat", "SwapBuffers"
};
#define NFN 5

int patch(void);
static char path[520];

static void report_exit(UINT code)
{
    MessageBoxA(NULL, msg, "gl31patch", MB_OK | (code ? MB_ICONWARNING : MB_ICONINFORMATION));
    ExitProcess(code);
}

void __stdcall start(void)
{
    char *cmd;
    HANDLE f;
    DWORD got, wrote, n;
    char bak[528];

    cmd = GetCommandLineA();
    if (*cmd == '"') { cmd++; while (*cmd && *cmd != '"') cmd++; if (*cmd) cmd++; }
    else while (*cmd && *cmd != ' ') cmd++;
    while (*cmd == ' ') cmd++;
    if (*cmd == '"') { cmd++; n = 0; while (*cmd && *cmd != '"' && n < sizeof(path)-1) path[n++] = *cmd++; }
    else { n = 0; while (*cmd && *cmd != ' ' && n < sizeof(path)-1) path[n++] = *cmd++; }
    path[n] = 0;
    if (!path[0]) {
        out("gl31patch - make an OpenGL program load on Windows NT 3.1\r\n");
        out("usage: GL31PAT program.exe\r\n");
        out("(copy WVGDI.DLL, OPENGL32.DLL and WVBGL32.DLL beside it)\r\n");
        report_exit(2);
    }

    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { out("cannot open "); out(path); out("\r\n"); report_exit(1); }
    imglen = GetFileSize(f, NULL);
    img = (unsigned char *) VirtualAlloc(NULL, imglen + 16, MEM_COMMIT, PAGE_READWRITE);
    if (!img) { out("out of memory\r\n"); report_exit(1); }
    if (!ReadFile(f, img, imglen, &got, NULL) || got != imglen) { out("read error\r\n"); report_exit(1); }
    CloseHandle(f);

    if (patch()) {
        n = 0; while (path[n]) { bak[n] = path[n]; n++; }
        mcpy(bak + n, ".bak", 5);
        DeleteFileA(bak);
        CopyFileA(path, bak, FALSE);
        f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) { out("cannot write "); out(path); out("\r\n"); report_exit(1); }
        WriteFile(f, img, imglen, &wrote, NULL);
        CloseHandle(f);
        out("patched "); out(path); out(" -> uses WVGDI.DLL (backup: "); out(bak); out(")\r\n");
        report_exit(0);
    }
    report_exit(1);
}

int patch(void)
{
    DWORD pe, opt, impdir, d, i;
    int done = 0;

    if (rd16(0) != 0x5A4D) { out("not an executable\r\n"); return 0; }
    pe = rd32(0x3C);
    if (rd32(pe) != 0x00004550) { out("not a PE file\r\n"); return 0; }
    if (rd16(pe + 24) != 0x10B) { out("not a 32-bit PE\r\n"); return 0; }
    opt = pe + 24;
    nsec = rd16(pe + 6);
    if (nsec >= MAXSEC) { out("too many sections\r\n"); return 0; }
    {
        DWORD s = opt + rd16(pe + 20);
        for (i = 0; i < nsec; i++, s += 40) {
            sec_vs[i] = rd32(s + 8);  sec_va[i] = rd32(s + 12);
            sec_rs[i] = rd32(s + 16); sec_raw[i] = rd32(s + 20);
        }
    }
    impdir = rd32(opt + 96 + 8);        /* DataDirectory[1] = import */
    if (!impdir) { out("no import table\r\n"); return 0; }

    for (d = rva2off(impdir); ; d += 20) {
        DWORD oft, ft, nameoff, t, k;
        DWORD name = rd32(d + 12);
        if (!rd32(d) && !name && !rd32(d + 16)) break;      /* null terminator */
        nameoff = rva2off(name);
        if (!nameoff || !seqi((char *) img + nameoff, "GDI32.DLL"))
            continue;
        /* does this GDI32 import pull in an OpenGL function? */
        oft = rd32(d + 0);
        ft  = rd32(d + 16);
        if (!oft) oft = ft;
        t = rva2off(oft);
        for (i = 0; ; i++) {
            DWORD e = rd32(t + 4 * i);
            int hit = 0;
            if (!e) break;
            if (e & 0x80000000UL) continue;                 /* by ordinal */
            {
                char *nm = (char *) img + rva2off(e) + 2;   /* skip the hint */
                for (k = 0; k < NFN; k++)
                    if (seqi(nm, glnames[k])) { hit = 1; break; }
            }
            if (hit) {
                mcpy((char *) img + nameoff, "WVGDI.DLL", 10);   /* 9 chars + NUL */
                done++;
                break;
            }
        }
    }
    if (!done) { out("no GDI32 OpenGL imports found - nothing to do (already patched, or not an OpenGL program)\r\n"); return 0; }

    /* disable bound imports so the loader resolves names (and WVGDI) afresh */
    wr32(opt + 96 + 8 * 11, 0);
    wr32(opt + 96 + 8 * 11 + 4, 0);
    return 1;
}
