/* HauntingGround.exe: the release launcher (Windows, Win32 only, no other libraries).
 *
 * First start: a small window asks for the player's own disc image and runs engine/src/tools/release/setup.py with the
 * bundled Python. setup.py checks the image (read-only), copies out the files the game needs, translates the game
 * program to C and compiles it with the bundled gcc into bin/hgboot.exe. The launcher shows the progress, keeps
 * logs/launcher.log, writes a default hg.ini (the GPU renderer when Vulkan works, otherwise the software renderer)
 * and starts the game. Later starts go straight to the game; a new release (engine/version.txt differs from
 * bin/build.txt) builds again, reusing the verified game files.
 *
 * The game runs with bin/ as its working directory, so its own defaults land in the release folder: ../game,
 * ../memcards (saves), ../crashes, ../recordings; the launcher adds HG_INI=../hg.ini and sends its output to
 * logs/play.log. When the game stops for any reason other than the player closing it, a message says where the logs are.
 *
 * Options: --setup (build again), --iso PATH (use this image, no file picker), --batch (no windows or messages at all:
 * for tests; the exit status tells the result), --no-start (build only).
 *
 * Built by tools/release/mkrelease.py: gcc -O2 -municode -mwindows launcher.c launcher.res -lcomctl32 -lcomdlg32 -lshell32 -static
 * Licence: GPL-3.0-or-later, like the rest of the project. */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <vulkan/vulkan.h>

#define APP_TITLE L"Haunting Ground Recomp"
static wchar_t root[MAX_PATH], version[64] = L"?";
static FILE *logf;
static int batch, no_start, force_setup;
static wchar_t iso_arg[MAX_PATH * 2];

/* ------------------------------------------------------------------ small helpers */
static void P(wchar_t *out, size_t n, const wchar_t *rel) { _snwprintf(out, n, L"%ls\\%ls", root, rel); out[n - 1] = 0; }
static void to_utf8(const wchar_t *w, char *out, int n) { int r = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, 0, 0); if (r <= 0) out[0] = 0; }
static void logw(const wchar_t *fmt, ...) {
    if (!logf) return;
    wchar_t buf[4096]; va_list ap; va_start(ap, fmt); _vsnwprintf(buf, 4095, fmt, ap); va_end(ap); buf[4095] = 0;
    char u[12288]; to_utf8(buf, u, sizeof u);
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(logf, "%02d:%02d:%02d %s\n", t.wHour, t.wMinute, t.wSecond, u); fflush(logf);
}
static void log_utf8(const char *line) { if (logf) { fprintf(logf, "  %s\n", line); fflush(logf); } }
static int exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static int read_small(const wchar_t *p, char *out, int n) {   /* whole small text file, 0-terminated */
    FILE *f = _wfopen(p, L"rb"); if (!f) { out[0] = 0; return 0; }
    int k = (int)fread(out, 1, n - 1, f); fclose(f); out[k < 0 ? 0 : k] = 0; return 1;
}
static void message(const wchar_t *text, UINT icon) {
    logw(L"message: %ls", text);
    if (!batch) MessageBoxW(0, text, APP_TITLE, MB_OK | icon | MB_SETFOREGROUND);
}

/* ------------------------------------------------------------------ Vulkan probe (for the default renderer) */
static int probe_vulkan(wchar_t *what, size_t n) {
    wchar_t dll[MAX_PATH]; P(dll, MAX_PATH, L"bin\\vulkan-1.dll");
    HMODULE m = LoadLibraryW(dll); if (!m) m = LoadLibraryExW(L"vulkan-1.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);   /* Windows' own, never one from the current folder */
    if (!m) { _snwprintf(what, n, L"no Vulkan loader"); return 0; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(m, "vkGetInstanceProcAddr");
    if (!gipa) { _snwprintf(what, n, L"broken Vulkan loader"); return 0; }
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(0, "vkCreateInstance");
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "HauntingGround launcher", 1, 0, 0, VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai, 0, 0, 0, 0 };
    VkInstance inst = 0;
    if (!create || create(&ci, 0, &inst) != VK_SUCCESS) { _snwprintf(what, n, L"no Vulkan driver"); return 0; }
    PFN_vkEnumeratePhysicalDevices en = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties props = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties qf = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    int ok = 0; _snwprintf(what, n, L"no suitable Vulkan GPU");
    VkPhysicalDevice dev[16]; uint32_t nd = 16;
    if (en && props && qf && en(inst, &nd, dev) >= 0) {
        int best = 0;   /* the game prefers a discrete GPU (gsvk.c): name the same one */
        for (uint32_t i = 0; i < nd; i++) {
            VkPhysicalDeviceProperties p; props(dev[i], &p);
            VkQueueFamilyProperties q[32]; uint32_t nq = 32; qf(dev[i], &nq, q);
            int compute = 0; for (uint32_t k = 0; k < nq; k++) if (q[k].queueFlags & VK_QUEUE_COMPUTE_BIT) compute = 1;
            wchar_t name[256]; MultiByteToWideChar(CP_UTF8, 0, p.deviceName, -1, name, 256);
            logw(L"Vulkan device %u: %ls, type %d, API %u.%u, compute %d", i, name, (int)p.deviceType,
                 VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), compute);
            int rank = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
            if (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU && p.apiVersion >= VK_API_VERSION_1_1 && compute && rank > best) {
                ok = 1; best = rank; _snwprintf(what, n, L"%ls", name);
            }
        }
    }
    if (destroy) destroy(inst, 0);
    return ok;
}
static void write_default_ini(void) {
    wchar_t ini[MAX_PATH]; P(ini, MAX_PATH, L"hg.ini");
    if (exists(ini)) { logw(L"hg.ini exists, kept"); return; }
    wchar_t what[300]; int gpu = probe_vulkan(what, 300);
    char u[700]; to_utf8(what, u, sizeof u);
    FILE *f = _wfopen(ini, L"wb"); if (!f) { logw(L"cannot write %ls", ini); return; }
    fprintf(f, "# Haunting Ground Recomp settings. The launcher wrote this file at the first start; the game's Options menu\r\n"
               "# (Down past Screen Position) changes and rewrites it. Delete it to get the defaults back.\r\n"
               "renderer = %s        # %s: %s\r\n", gpu ? "gpu" : "software", gpu ? "Vulkan GPU found" : "software renderer, because", u);
    fclose(f);
    logw(L"hg.ini written: renderer %ls (%ls)", gpu ? L"gpu" : L"software", what);
}

/* ------------------------------------------------------------------ the game */
static void prune_recordings(int keep) {   /* ../recordings/YYYYMMDD-HHMMSS: keep the newest */
    wchar_t pat[MAX_PATH], names[512][32]; int n = 0; P(pat, MAX_PATH, L"recordings\\????????" L"-??????");
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcslen(fd.cFileName) == 15 && n < 512) wcscpy(names[n++], fd.cFileName);
    while (FindNextFileW(h, &fd));
    FindClose(h);
    qsort(names, n, sizeof names[0], (int (*)(const void *, const void *))wcscmp);
    for (int i = 0; i < n - keep; i++) {     /* only the files a recording holds: pad.txt, card/, replay_card/ */
        wchar_t d[MAX_PATH], from[MAX_PATH + 2] = {0};
        _snwprintf(d, MAX_PATH, L"recordings\\%ls", names[i]); P(from, MAX_PATH, d);
        wchar_t padtxt[MAX_PATH]; _snwprintf(padtxt, MAX_PATH, L"%ls\\pad.txt", from);
        if (!exists(padtxt)) continue;
        SHFILEOPSTRUCTW op = { 0, FO_DELETE, from, 0, FOF_NO_UI, 0, 0, 0 };   /* from is double-0-terminated */
        logw(L"old recording removed: %ls (%d)", names[i], SHFileOperationW(&op));
    }
}
static int start_game(void) {
    wchar_t exe[MAX_PATH], bin[MAX_PATH], playlog[MAX_PATH], prev[MAX_PATH];
    P(exe, MAX_PATH, L"bin\\hgboot.exe"); P(bin, MAX_PATH, L"bin"); P(playlog, MAX_PATH, L"logs\\play.log"); P(prev, MAX_PATH, L"logs\\play.prev.log");
    MoveFileExW(playlog, prev, MOVEFILE_REPLACE_EXISTING);
    prune_recordings(20);
    SECURITY_ATTRIBUTES sa = { sizeof sa, 0, TRUE };
    HANDLE out = CreateFileW(playlog, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    SetEnvironmentVariableW(L"HG_INI", L"..\\hg.ini");
    if (!GetEnvironmentVariableW(L"HG_PADREC", 0, 0)) SetEnvironmentVariableW(L"HG_PADREC", L"auto");
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, 0);
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = nul; si.hStdOutput = out; si.hStdError = out;
    wchar_t cmd[MAX_PATH + 16]; _snwprintf(cmd, MAX_PATH + 16, L"\"%ls\" -q", exe); cmd[MAX_PATH + 15] = 0;
    logw(L"starting the game: %ls", cmd);
    if (!CreateProcessW(exe, cmd, 0, 0, TRUE, CREATE_NO_WINDOW, 0, bin, &si, &pi)) {
        DWORD e = GetLastError(); if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        wchar_t m[600]; _snwprintf(m, 600, L"The game could not be started (Windows error %lu).\n\nTry starting HauntingGround.exe with --setup to build it again. "
                                          L"Details: logs\\launcher.log", e);
        message(m, MB_ICONERROR); return 1;
    }
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    /* why did it stop? the last "STOP:" or "==== CRASH" line of play.log */
    char tail[65537] = {0}, stopline[600] = {0}, crash[600] = {0};
    FILE *f = _wfopen(playlog, L"rb");
    if (f) { _fseeki64(f, 0, SEEK_END); long long sz = _ftelli64(f); _fseeki64(f, sz > 65536 ? sz - 65536 : 0, SEEK_SET);
             size_t k = fread(tail, 1, 65536, f); tail[k] = 0; fclose(f); }
    for (char *l = tail; l && *l; ) {
        char *e = strchr(l, '\n'); size_t len = e ? (size_t)(e - l) : strlen(l); if (len > 590) len = 590;
        if (!strncmp(l, "STOP: ", 6)) { memcpy(stopline, l + 6, len - 6); stopline[len - 6] = 0; }
        /* the stats summary repeats it; after a long session the first one is more than 64 KB from the end */
        if (!strncmp(l, "==== STOP: ", 11)) { memcpy(stopline, l + 11, len - 11); stopline[len - 11] = 0; }
        if (!strncmp(l, "==== CRASH", 10)) { memcpy(crash, l, len); crash[len] = 0; }
        l = e ? e + 1 : 0;
    }
    for (char *c = stopline + strlen(stopline); c > stopline && (c[-1] == '\r' || c[-1] == ' '); ) *--c = 0;
    for (char *c = crash + strlen(crash); c > crash && (c[-1] == '\r' || c[-1] == ' '); ) *--c = 0;
    logw(L"the game ended: exit status 0x%lx", code);
    if (stopline[0]) log_utf8(stopline);
    if (!crash[0] && code == 0 && !strcmp(stopline, "window closed")) return 0;
    wchar_t why[700]; MultiByteToWideChar(CP_UTF8, 0, crash[0] ? crash : stopline[0] ? stopline : "(no reason in the log)", -1, why, 700);
    wchar_t m[2000]; _snwprintf(m, 2000, L"The game stopped unexpectedly (exit status 0x%lx):\n\n%ls\n\nPlease report it (see README.txt, \"Reporting bugs\") "
                                        L"and attach logs\\play.log and logs\\launcher.log from the folder %ls.", code, why, root);
    message(m, MB_ICONWARNING);
    return 2;
}

/* ------------------------------------------------------------------ setup (setup.py with the bundled Python) */
enum { ID_CHOOSE = 101, ID_CLOSE, ID_LOGS, WM_APP_LINE = WM_APP + 1, WM_APP_DONE };
static HWND wnd, w_text, w_choose, w_close, w_logs, w_bar, w_stage, w_log;
static HANDLE job, worker;
static int need_iso, setup_done, building;
static wchar_t last_error[2048];
static HFONT font;

static void ui_line(char *line) {   /* one line from setup.py (UTF-8, without the newline) */
    log_utf8(line);
    /* at most 2047 characters, always ended (a longer line made the conversion fail and left w unset: the security audit) */
    wchar_t w[2048]; const int k = MultiByteToWideChar(CP_UTF8, 0, line, (int)strnlen(line, 2047), w, 2047); w[k > 0 ? k : 0] = 0;
    if (!wcsncmp(w, L"@progress ", 10)) { if (w_bar) SendMessageW(w_bar, PBM_SETPOS, _wtoi(w + 10), 0); return; }
    if (!wcsncmp(w, L"@stage ", 7)) { if (w_stage) SetWindowTextW(w_stage, w + 7); }
    else if (!wcsncmp(w, L"@error ", 7)) { wcsncpy(last_error, w + 7, 2047); }
    else if (!wcscmp(w, L"@need-iso")) { need_iso = 1; return; }
    else if (!wcscmp(w, L"@done")) { setup_done = 1; return; }
    if (w_log) {
        int len = GetWindowTextLengthW(w_log);
        if (len > 60000) { SendMessageW(w_log, EM_SETSEL, 0, 20000); SendMessageW(w_log, EM_REPLACESEL, 0, (LPARAM)L""); len = GetWindowTextLengthW(w_log); }
        SendMessageW(w_log, EM_SETSEL, len, len);
        SendMessageW(w_log, EM_REPLACESEL, 0, (LPARAM)(w[0] == '@' ? w + 1 : w));
        SendMessageW(w_log, EM_REPLACESEL, 0, (LPARAM)L"\r\n");
    }
}
typedef struct { HANDLE rd, proc; } Pipe;
static DWORD WINAPI reader(LPVOID arg) {   /* setup.py's output, line by line, to the UI thread (or straight in batch mode) */
    Pipe *p = arg; char buf[8192], line[8192]; int n = 0; DWORD got;
    while (ReadFile(p->rd, buf, sizeof buf, &got, 0) && got) {
        for (DWORD i = 0; i < got; i++) {
            if (buf[i] == '\r') continue;
            if (buf[i] == '\n' || n == (int)sizeof line - 1) {
                line[n] = 0; n = 0;
                if (batch) ui_line(line);
                else PostMessageW(wnd, WM_APP_LINE, 0, (LPARAM)_strdup(line));
            } else line[n++] = buf[i];
        }
    }
    if (n) { line[n] = 0; if (batch) ui_line(line); else PostMessageW(wnd, WM_APP_LINE, 0, (LPARAM)_strdup(line)); }
    WaitForSingleObject(p->proc, INFINITE);
    DWORD code = 1; GetExitCodeProcess(p->proc, &code);
    CloseHandle(p->rd); CloseHandle(p->proc);
    if (!batch) PostMessageW(wnd, WM_APP_DONE, code, 0);
    free(p);
    return code;
}
static int run_setup(const wchar_t *iso) {   /* starts setup.py; returns 0 when it could not start */
    wchar_t py[MAX_PATH], script[MAX_PATH], tcbin[MAX_PATH], cmd[6 * MAX_PATH + 64], sys32[MAX_PATH], path[3 * MAX_PATH];   /* (room for py, script, root and a 2 x MAX_PATH iso) */
    P(py, MAX_PATH, L"engine\\python\\python.exe"); P(script, MAX_PATH, L"engine\\src\\tools\\release\\setup.py");
    P(tcbin, MAX_PATH, L"engine\\ucrt64\\bin");
    if (!exists(py) || !exists(script)) {
        _snwprintf(last_error, 2048, L"Parts of the release are missing (%ls). Please unzip the whole release again.", exists(py) ? script : py);
        return 0;
    }
    const size_t nc = sizeof cmd / sizeof cmd[0];
    const int r = iso && *iso ? _snwprintf(cmd, nc, L"\"%ls\" -u -I \"%ls\" --root \"%ls\" --iso \"%ls\"", py, script, root, iso)
                              : _snwprintf(cmd, nc, L"\"%ls\" -u -I \"%ls\" --root \"%ls\"", py, script, root);
    cmd[nc - 1] = 0;
    if (r < 0 || (size_t)r >= nc) {   /* never run a command cut short */
        _snwprintf(last_error, 2048, L"The game's folder or the disc image's path is too long. Please move them to a shorter path (e.g. C:\\Games)."); return 0;
    }
    /* a known PATH: the bundled compiler first, then Windows; nothing else the PC has installed */
    GetSystemDirectoryW(sys32, MAX_PATH);
    wchar_t windir[MAX_PATH]; GetWindowsDirectoryW(windir, MAX_PATH);
    _snwprintf(path, 3 * MAX_PATH, L"%ls;%ls;%ls", tcbin, sys32, windir);
    SetEnvironmentVariableW(L"PATH", path);
    SetEnvironmentVariableW(L"PYTHONHOME", 0); SetEnvironmentVariableW(L"PYTHONPATH", 0);
    logw(L"setup: %ls", cmd);
    SECURITY_ATTRIBUTES sa = { sizeof sa, 0, TRUE }; HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { _snwprintf(last_error, 2048, L"CreatePipe failed (%lu)", GetLastError()); return 0; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, 0);
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = nul;
    wchar_t cwd[MAX_PATH]; P(cwd, MAX_PATH, L"engine");
    if (!CreateProcessW(py, cmd, 0, 0, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, 0, cwd, &si, &pi)) {
        _snwprintf(last_error, 2048, L"The bundled Python could not be started (Windows error %lu).", GetLastError());
        CloseHandle(rd); CloseHandle(wr); if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul); return 0;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);   /* closing the launcher stops the build and its compilers */
    ResumeThread(pi.hThread); CloseHandle(pi.hThread); CloseHandle(wr); if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    Pipe *p = malloc(sizeof *p); p->rd = rd; p->proc = pi.hProcess;
    need_iso = setup_done = 0; last_error[0] = 0; building = 1;
    if (batch) return (int)reader(p) + 1000;   /* batch: runs to the end here */
    worker = CreateThread(0, 0, reader, p, 0, 0);
    return 1;
}
static void set_busy(int b) {
    EnableWindow(w_choose, !b);
    SetWindowTextW(w_close, b ? L"Cancel" : L"Close");
}
static void choose_iso(void) {
    wchar_t file[MAX_PATH * 2] = {0};
    OPENFILENAMEW of = { sizeof of };
    of.hwndOwner = wnd; of.lpstrFilter = L"PS2 disc images (*.iso)\0*.iso\0All files (*.*)\0*.*\0";
    of.lpstrFile = file; of.nMaxFile = MAX_PATH * 2; of.lpstrTitle = L"Choose your Haunting Ground (NTSC-U, SLUS-21075) disc image";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&of)) return;
    logw(L"disc image chosen: %ls", file);
    SendMessageW(w_bar, PBM_SETPOS, 0, 0);
    if (!run_setup(file)) { message(last_error, MB_ICONERROR); return; }
    set_busy(1);
}
static void setup_finished(DWORD code) {
    building = 0; worker = 0; set_busy(0);
    logw(L"setup ended with status %lu (done %d, need-iso %d)", code, setup_done, need_iso);
    if (code == 0 && setup_done) {
        write_default_ini();
        SetWindowTextW(w_stage, no_start ? L"Ready." : L"Ready. Starting the game...");
        if (!no_start) { DestroyWindow(wnd); return; }   /* the message loop ends; wWinMain starts the game */
        return;
    }
    if (need_iso) {
        SetWindowTextW(w_stage, L"Choose your disc image to begin.");
        if (iso_arg[0]) { iso_arg[0] = 0; }
        return;
    }
    wchar_t m[3000];
    _snwprintf(m, 3000, L"%ls\n\nThe details are in logs\\launcher.log (in %ls). If you report this, please attach that file.",
               last_error[0] ? last_error : L"The build stopped.", root);
    SetWindowTextW(w_stage, L"Stopped. See the message, then choose a disc image to try again.");
    message(m, MB_ICONERROR);
}
static void layout(HWND h) {
    RECT r; GetClientRect(h, &r);
    int dpi = GetDpiForWindow(h); if (!dpi) dpi = 96;
#define S(x) MulDiv((x), dpi, 96)
    int W = r.right, H = r.bottom, m = S(12), bh = S(28), y = m;
    MoveWindow(w_text, m, y, W - 2 * m, S(70), TRUE); y += S(74);
    MoveWindow(w_choose, m, y, S(200), bh, TRUE); MoveWindow(w_logs, W - m - S(120) - S(8) - S(100), y, S(120), bh, TRUE);
    MoveWindow(w_close, W - m - S(100), y, S(100), bh, TRUE); y += bh + S(10);
    MoveWindow(w_stage, m, y, W - 2 * m, S(20), TRUE); y += S(22);
    MoveWindow(w_bar, m, y, W - 2 * m, S(18), TRUE); y += S(26);
    MoveWindow(w_log, m, y, W - 2 * m, H - y - m, TRUE);
#undef S
}
static void make_font(HWND h) {
    NONCLIENTMETRICSW nm = { sizeof nm };
    int dpi = GetDpiForWindow(h); if (!dpi) dpi = 96;
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof nm, &nm, 0, dpi)) { if (font) DeleteObject(font); font = CreateFontIndirectW(&nm.lfMessageFont); }
    HWND kids[] = { w_text, w_choose, w_close, w_logs, w_stage, w_log };
    for (int i = 0; i < 6; i++) SendMessageW(kids[i], WM_SETFONT, (WPARAM)font, TRUE);
}
static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hi = ((CREATESTRUCTW *)lp)->hInstance;
        wchar_t intro[700];
        _snwprintf(intro, 700, L"Haunting Ground Recomp %ls builds the game on this PC from your own copy. You need a disc image (.iso) "
                               L"of the North American disc, SLUS-21075. The image is only read; nothing is downloaded. "
                               L"The first start takes a few minutes; later starts go straight to the game.", version);
        w_text = CreateWindowW(L"STATIC", intro, WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, h, 0, hi, 0);
        w_choose = CreateWindowW(L"BUTTON", L"Choose disc image...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 10, 10, h, (HMENU)ID_CHOOSE, hi, 0);
        w_logs = CreateWindowW(L"BUTTON", L"Open logs folder", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, h, (HMENU)ID_LOGS, hi, 0);
        w_close = CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, h, (HMENU)ID_CLOSE, hi, 0);
        w_stage = CreateWindowW(L"STATIC", L"Checking...", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0, 0, 10, 10, h, 0, hi, 0);
        w_bar = CreateWindowW(PROGRESS_CLASSW, 0, WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, h, 0, hi, 0);
        SendMessageW(w_bar, PBM_SETRANGE32, 0, 100);
        w_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", 0, WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                0, 0, 10, 10, h, 0, hi, 0);
        wnd = h; make_font(h); layout(h);
        return 0; }
    case WM_SIZE: layout(h); return 0;
    case WM_DPICHANGED: { RECT *r = (RECT *)lp; SetWindowPos(h, 0, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE); make_font(h); layout(h); return 0; }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_CHOOSE) choose_iso();
        else if (LOWORD(wp) == ID_LOGS) { wchar_t d[MAX_PATH]; P(d, MAX_PATH, L"logs"); ShellExecuteW(h, L"open", d, 0, 0, SW_SHOWNORMAL); }
        else if (LOWORD(wp) == ID_CLOSE) SendMessageW(h, WM_CLOSE, 0, 0);
        return 0;
    case WM_APP_LINE: ui_line((char *)lp); free((void *)lp); return 0;
    case WM_APP_DONE: setup_finished((DWORD)wp); return 0;
    case WM_CLOSE:
        if (building && MessageBoxW(h, L"Stop the build? It will start again from the beginning next time.", APP_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
        if (building) { logw(L"build cancelled by the player"); if (job) TerminateJobObject(job, 1); setup_done = 0; }
        DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static int setup_window(HINSTANCE hi) {
    INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES }; InitCommonControlsEx(&ic);
    WNDCLASSW wc = {0}; wc.lpfnWndProc = wndproc; wc.hInstance = hi; wc.lpszClassName = L"HGRecompLauncher";
    wc.hCursor = LoadCursor(0, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    wchar_t title[128]; _snwprintf(title, 128, L"%ls %ls - first start", APP_TITLE, version);
    int dpi = GetDpiForSystem(); if (!dpi) dpi = 96;
    HWND h = CreateWindowW(L"HGRecompLauncher", title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(640, dpi, 96), MulDiv(460, dpi, 96), 0, 0, hi, 0);
    if (!h) return 1;
    ShowWindow(h, SW_SHOW); UpdateWindow(h);
    set_busy(1);
    if (!run_setup(iso_arg)) { message(last_error, MB_ICONERROR); set_busy(0); }   /* without an image: checks the game files */
    MSG m; while (GetMessageW(&m, 0, 0, 0) > 0) { if (!IsDialogMessageW(h, &m)) { TranslateMessage(&m); DispatchMessageW(&m); } }
    return setup_done ? 0 : 1;
}

/* ------------------------------------------------------------------ main */
static int built_ok(void) {
    wchar_t exe[MAX_PATH], stamp[MAX_PATH]; P(exe, MAX_PATH, L"bin\\hgboot.exe"); P(stamp, MAX_PATH, L"bin\\build.txt");
    char s[512], want[160], v8[64]; if (!exists(exe) || !read_small(stamp, s, sizeof s)) return 0;
    to_utf8(version, v8, sizeof v8); snprintf(want, sizeof want, "release=%s", v8);
    size_t k = strlen(want);
    return strncmp(s, want, k) == 0 && (s[k] == '\n' || s[k] == '\r');
}
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev; (void)cmdline; (void)show;
    int argc; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--batch")) batch = 1;
        else if (!wcscmp(argv[i], L"--no-start")) no_start = 1;
        else if (!wcscmp(argv[i], L"--setup")) force_setup = 1;
        else if (!wcscmp(argv[i], L"--iso") && i + 1 < argc) { wcsncpy(iso_arg, argv[++i], MAX_PATH * 2 - 1); force_setup = 1; }
    }
    GetModuleFileNameW(0, root, MAX_PATH); { wchar_t *s = wcsrchr(root, L'\\'); if (s) *s = 0; }
    for (const wchar_t *c = root; *c; c++) if (*c > 126) {
        wchar_t m[MAX_PATH + 400]; _snwprintf(m, MAX_PATH + 400, L"Please move the Haunting Ground Recomp folder to a path with only plain English letters, digits "
                                                                 L"and spaces (for example C:\\Games\\HauntingGround). The compiler cannot work in:\n\n%ls", root);
        message(m, MB_ICONERROR); return 4;
    }
    wchar_t logs[MAX_PATH], lp[MAX_PATH], vf[MAX_PATH]; P(logs, MAX_PATH, L"logs"); P(lp, MAX_PATH, L"logs\\launcher.log"); P(vf, MAX_PATH, L"engine\\version.txt");
    CreateDirectoryW(logs, 0);
    logf = _wfopen(lp, L"ab");
    if (!logf) {
        wchar_t m[MAX_PATH + 300]; _snwprintf(m, MAX_PATH + 300, L"This folder cannot be written to:\n\n%ls\n\nPlease move the Haunting Ground Recomp folder somewhere "
                                                                 L"you can write, for example C:\\Games\\HauntingGround or your Documents folder.", root);
        message(m, MB_ICONERROR); return 4;
    }
    char v[64]; if (!read_small(vf, v, sizeof v)) {
        message(L"The engine folder is missing. Please unzip the whole release, keeping its folders.", MB_ICONERROR); return 4;
    }
    for (char *c = v + strlen(v); c > v && (c[-1] == '\n' || c[-1] == '\r' || c[-1] == ' '); ) *--c = 0;
    MultiByteToWideChar(CP_UTF8, 0, v, -1, version, 64);
    OSVERSIONINFOW ov = { sizeof ov }; SYSTEM_INFO si; GetNativeSystemInfo(&si);
    logw(L"==== launcher %ls in %ls (%lu CPUs)", version, root, si.dwNumberOfProcessors); (void)ov;
    job = CreateJobObjectW(0, 0);
    if (job) { JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0}; li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
               SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li); }
    if (force_setup || !built_ok()) {
        if (batch) {
            int r = run_setup(iso_arg);
            if (!r) { logw(L"setup could not start: %ls", last_error); return 1; }
            int code = r - 1000;
            if (code == 0 && setup_done) write_default_ini();
            else { logw(L"setup failed (status %d): %ls%ls", code, need_iso ? L"a disc image is needed " : L"", last_error); return code ? code : 1; }
        } else if (setup_window(hi) != 0) return 1;
        if (!built_ok()) { logw(L"no build after setup"); return 1; }
    }
    if (no_start) { logw(L"built; --no-start"); return 0; }
    return start_game();
}
