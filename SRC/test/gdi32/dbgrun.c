/*
 * DBGRUN - runs a program under the Win32 debug API and logs the DLLs it
 * loads and its exceptions to DBGRUN.TXT (next to DBGRUN.EXE): registers,
 * return addresses on the stack, and for Delphi exceptions (0EEDFADEh)
 * the exception class and message ("EReadError: Failed to read ImageList
 * data from stream" found GitHub issue #1).  A program that would crash
 * is ended after its unhandled exception is logged.
 *   DBGRUN program.exe [args]
 * Without arguments the command line is read from DBGRUN.CMD.
 */
#include <windows.h>

static HANDLE lg;
void *memset(void *d, int c, size_t n) { char *p = d; while (n--) *p++ = (char)c; return d; }

static void logf_(const char *f, ...)
{
    char b[1024];
    DWORD n;
    va_list ap;
    va_start(ap, f);
    n = wvsprintfA(b, f, ap);
    va_end(ap);
    WriteFile(lg, b, n, &n, NULL);
    FlushFileBuffers(lg);
}

static HANDLE proc;

static DWORD rd(DWORD a)
{
    DWORD v = 0, n;
    ReadProcessMemory(proc, (LPCVOID)a, &v, 4, &n);
    return v;
}

static void rds(DWORD a, char *b, int max)
{
    DWORD n = 0;
    memset(b, 0, max);
    if (a) ReadProcessMemory(proc, (LPCVOID)a, b, max - 1, &n);
    b[max - 1] = 0;
}

void WINAPI start(void)
{
    char *cl = GetCommandLineA(), path[MAX_PATH];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DEBUG_EVENT de;
    HANDLE threads[64];
    DWORD tids[64];
    int nth = 0, nexc = 0;
    DWORD n = GetModuleFileNameA(NULL, path, sizeof(path));
    while (n && path[n - 1] != '\\') n--;
    lstrcpyA(path + n, "DBGRUN.TXT");
    lg = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    /* skip our own name */
    if (*cl == '"') { cl++; while (*cl && *cl != '"') cl++; if (*cl) cl++; }
    else while (*cl && *cl != ' ') cl++;
    while (*cl == ' ') cl++;
    if (!*cl) {     /* no arguments (WIN.INI run=): the command line is in DBGRUN.CMD */
        static char cmd[512];
        HANDLE f;
        DWORD k = 0;
        lstrcpyA(path + n, "DBGRUN.CMD");
        f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) { ReadFile(f, cmd, sizeof(cmd) - 1, &k, NULL); CloseHandle(f); }
        cmd[k] = 0;
        while (k && (cmd[k - 1] == '\r' || cmd[k - 1] == '\n')) cmd[--k] = 0;
        cl = cmd;
    }
    logf_("run: %s\r\n", cl);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(NULL, cl, NULL, NULL, FALSE, DEBUG_ONLY_THIS_PROCESS, NULL, NULL, &si, &pi)) {
        logf_("CreateProcess failed %lu\r\n", GetLastError());
        ExitProcess(1);
    }
    proc = pi.hProcess;
    for (;;) {
        DWORD cont = DBG_CONTINUE;
        if (!WaitForDebugEvent(&de, INFINITE)) { logf_("WaitForDebugEvent failed\r\n"); break; }
        switch (de.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            logf_("process base %lx\r\n", (DWORD)de.u.CreateProcessInfo.lpBaseOfImage);
            threads[nth] = de.u.CreateProcessInfo.hThread; tids[nth++] = de.dwThreadId;
            break;
        case CREATE_THREAD_DEBUG_EVENT:
            if (nth < 64) { threads[nth] = de.u.CreateThread.hThread; tids[nth++] = de.dwThreadId; }
            break;
        case LOAD_DLL_DEBUG_EVENT: {
            char nm[MAX_PATH];
            DWORD p = de.u.LoadDll.lpImageName ? rd((DWORD)de.u.LoadDll.lpImageName) : 0;
            nm[0] = 0;
            if (p && !de.u.LoadDll.fUnicode) rds(p, nm, sizeof(nm));
            logf_("dll %lx %s\r\n", (DWORD)de.u.LoadDll.lpBaseOfDll, nm);
            break;
        }
        case OUTPUT_DEBUG_STRING_EVENT: {
            char s[512];
            rds((DWORD)de.u.DebugString.lpDebugStringData, s, sizeof(s));
            logf_("ods: %s\r\n", s);
            break;
        }
        case EXCEPTION_DEBUG_EVENT: {
            EXCEPTION_RECORD *er = &de.u.Exception.ExceptionRecord;
            DWORD i;
            HANDLE th = NULL;
            CONTEXT c;
            if (er->ExceptionCode == EXCEPTION_BREAKPOINT && !nexc++) break;  /* loader breakpoint */
            logf_("exception %lx at %lx first %lu params %lu:", er->ExceptionCode, (DWORD)er->ExceptionAddress,
                  de.u.Exception.dwFirstChance, er->NumberParameters);
            for (i = 0; i < er->NumberParameters && i < 8; i++) logf_(" %lx", er->ExceptionInformation[i]);
            logf_("\r\n");
            if (er->ExceptionCode == 0x0EEDFADE && er->NumberParameters >= 2) {
                DWORD obj = er->ExceptionInformation[1], vmt = rd(obj), cn = rd(vmt - 44), msg = rd(obj + 4);
                char name[64], m[300];
                DWORD ln = rd(cn) & 0xFF;
                rds(cn + 1, name, ln + 1 < 64 ? (int)ln + 1 : 64);
                rds(msg, m, sizeof(m));
                logf_("  Delphi %s: %s (raised at %lx)\r\n", name, m, er->ExceptionInformation[0]);
            }
            for (i = 0; i < (DWORD)nth; i++) if (tids[i] == de.dwThreadId) th = threads[i];
            if (th) {
                memset(&c, 0, sizeof(c));
                c.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(th, &c)) {
                    logf_("  eax %lx ebx %lx ecx %lx edx %lx esi %lx edi %lx ebp %lx esp %lx eip %lx\r\n",
                          c.Eax, c.Ebx, c.Ecx, c.Edx, c.Esi, c.Edi, c.Ebp, c.Esp, c.Eip);
                    logf_("  stack:");
                    for (i = 0; i < 96; i++) {
                        DWORD v = rd(c.Esp + i * 4);
                        if (v >= 0x400000 && v < 0x80000000) logf_(" %lx", v);
                    }
                    logf_("\r\n");
                }
            }
            if (er->ExceptionCode != EXCEPTION_BREAKPOINT) cont = DBG_EXCEPTION_NOT_HANDLED;
            /* second chance, or Windows 9x's "noncontinuable" loop after an
               unhandled exception under a debugger */
            if (!de.u.Exception.dwFirstChance || er->ExceptionCode == 0xC0000025 || nexc > 200) {
                logf_("unhandled: program ended\r\n");
                TerminateProcess(proc, 1);
                cont = DBG_CONTINUE;
            }
            nexc++;
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:
            logf_("exit %lu\r\n", de.u.ExitProcess.dwExitCode);
            ContinueDebugEvent(de.dwProcessId, de.dwThreadId, DBG_CONTINUE);
            CloseHandle(lg);
            ExitProcess(0);
        }
        ContinueDebugEvent(de.dwProcessId, de.dwThreadId, cont);
    }
    CloseHandle(lg);
    ExitProcess(0);
}
