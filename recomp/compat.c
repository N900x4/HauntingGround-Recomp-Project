/* Host services that differ between Linux and Windows; see compat.h. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "compat.h"
#include "ee_runtime.h"
#include "hle.h"
extern const int rec_nfuncs;

#ifdef _WIN32
#include <windows.h>
#include <direct.h>

void *host_alloc(size_t sz) {
    void *p = VirtualAlloc(0, sz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) stop("VirtualAlloc of %zu bytes failed", sz);
    return p;
}
int host_ncpus(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }
void host_gmtime(time_t t, struct tm *out) { gmtime_s(out, &t); }
int host_mkdir(const char *path) { return _mkdir(path); }
/* *, ? and [...] (with ! or ^ negation and ranges), backslash escapes; no special case for '/' or a leading '.' */
int host_fnmatch(const char *p, const char *s) {
    for (;; p++, s++) {
        switch (*p) {
        case 0: return *s ? 1 : 0;
        case '?': if (!*s) return 1; break;
        case '*':
            while (*p == '*') p++;
            if (!*p) return 0;
            for (; *s; s++) if (!host_fnmatch(p, s)) return 0;
            return 1;
        case '[': {
            if (!*s) return 1;
            const char *q = p + 1; int neg = *q == '!' || *q == '^', hit = 0; if (neg) q++;
            do { char lo = *q, hi = lo; if (q[1] == '-' && q[2] && q[2] != ']') { hi = q[2]; q += 2; }
                 if ((unsigned char)*s >= (unsigned char)lo && (unsigned char)*s <= (unsigned char)hi) hit = 1; q++; } while (*q && *q != ']');
            if (!*q) { if (*s != '[') return 1; break; }   /* no closing ']': literal '[' */
            if (hit == neg) return 1;
            p = q; break; }
        case '\\': if (p[1]) p++; /* fall through */
        default: if (*p != *s) return 1;
        }
    }
}

static void *boot_fiber;
static VOID CALLBACK fiber_entry(LPVOID fn) { ((void (*)(void))fn)(); stop("fiber function returned"); }
void host_fiber_make(HostFiber *f, void (*fn)(void), size_t stack_size) {
    if (f->h) { if (f->h == GetCurrentFiber()) stop("restarting the running fiber"); DeleteFiber(f->h); }
    /* MXCSR (HG_FPRZ rounding) is thread state here: no FIBER_FLAG_FLOAT_SWITCH, so every fiber shares main()'s setting */
    f->h = CreateFiberEx(64 << 10, stack_size, 0, fiber_entry, (LPVOID)fn);
    if (!f->h) stop("CreateFiberEx failed (%lu)", GetLastError());
}
void host_fiber_switch(HostFiber *from, HostFiber *to) {
    if (!boot_fiber) { boot_fiber = ConvertThreadToFiber(0); if (!boot_fiber) stop("ConvertThreadToFiber failed (%lu)", GetLastError()); }
    if (!from->h) from->h = GetCurrentFiber();   /* the boot context */
    SwitchToFiber(to->h);
}

static HANDLE main_thread;
static void build_ord(void);
static void (*cb_prof)(void), (*cb_alarm)(void);

/* HG_HOSTPROF=1: every 1 ms the timer thread samples the host instruction pointer of the main thread and of threads
   that called host_prof_thread() (the GS thread). A sample inside a DLL is charged to the first frame back in
   hgboot.exe, marked with the high bit. host_prof_dump() writes "thread rva count" lines to hostprof.txt;
   tools/verify/hostprof.py names them from the exe's symbol table. */
#define HP_SLOTS (1 << 16)
#define HP_THREADS 20
typedef struct { HANDLE h; char name[16]; uint32_t key[HP_SLOTS], cnt[HP_SLOTS]; uint64_t total, lost; } HpThread;
static HpThread *hp; static volatile LONG hp_n; static volatile int hp_on; static uint64_t exe_lo, exe_hi;
static void hp_add(HpThread *t, uint32_t key) {
    uint32_t i = (key * 2654435761u) >> 16;
    for (int p = 0; p < 64; p++, i = (i + 1) & (HP_SLOTS - 1)) {
        if (t->key[i] == key && t->cnt[i]) { t->cnt[i]++; return; }
        if (!t->cnt[i]) { t->key[i] = key; t->cnt[i] = 1; return; }
    }
    t->lost++;
}
static void hp_register(HANDLE h, const char *name) {
    if (!hp) return;
    LONG k = InterlockedIncrement(&hp_n) - 1; if (k >= HP_THREADS) return;
    snprintf(hp[k].name, sizeof hp[k].name, "%s", name); hp[k].h = h;
}
void host_thread_boost(void) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); }
void host_prof_thread(const char *name) {
    HANDLE h; DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    hp_register(h, name);
}
static void hp_sample(void) {
    int n = hp_n < HP_THREADS ? hp_n : HP_THREADS;
    for (int k = 0; k < n; k++) {
        HpThread *t = &hp[k]; if (!t->h) continue;
        if (SuspendThread(t->h) == (DWORD)-1) continue;
        CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(t->h, &c)) {
            uint32_t dll = 0;
            for (int d = 0; d < 32 && !(c.Rip >= exe_lo && c.Rip < exe_hi); d++) {   /* in a DLL: walk back into the exe */
                dll = 0x80000000u;
                DWORD64 img; PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &img, 0);
                if (!rf) { c.Rip = *(DWORD64 *)c.Rsp; c.Rsp += 8; continue; }
                void *hd; DWORD64 est; RtlVirtualUnwind(UNW_FLAG_NHANDLER, img, c.Rip, rf, &c, &hd, &est, 0);
                if (!c.Rip) break;
            }
            if (c.Rip >= exe_lo && c.Rip < exe_hi) hp_add(t, (uint32_t)(c.Rip - exe_lo) | dll); else t->lost++;
            t->total++;
        }
        ResumeThread(t->h);
    }
}
void host_prof_dump(void) {
    if (!hp) return;
    hp_on = 0; Sleep(5);   /* let a sample in progress finish */
    FILE *f = fopen("hostprof.txt", "w"); if (!f) return;
    int n = hp_n < HP_THREADS ? hp_n : HP_THREADS;
    for (int k = 0; k < n; k++) {
        fprintf(f, "# %s samples %llu lost %llu\n", hp[k].name, (unsigned long long)hp[k].total, (unsigned long long)hp[k].lost);
        for (int i = 0; i < HP_SLOTS; i++) if (hp[k].cnt[i]) fprintf(f, "%s %08x %u\n", hp[k].name, hp[k].key[i], hp[k].cnt[i]);
    }
    fclose(f); L("host profile: %d threads sampled, written to hostprof.txt\n", n);
}

static DWORD WINAPI timer_main(LPVOID arg) {
    (void)arg;
    HANDLE t = CreateWaitableTimerExW(0, 0, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!t) t = CreateWaitableTimerW(0, FALSE, 0);
    LARGE_INTEGER due; due.QuadPart = -10000;          /* 1 ms, relative */
    SetWaitableTimer(t, &due, 1, 0, 0, FALSE);
    double next_alarm = host_now() + 1.0 / 60;
    for (;;) {
        WaitForSingleObject(t, INFINITE);
        cb_prof();
        if (hp_on) hp_sample();
        double n = host_now();
        if (n >= next_alarm) { next_alarm += 1.0 / 60; if (next_alarm < n) next_alarm = n + 1.0 / 60; cb_alarm(); }
    }
    return 0;
}
void host_start_timers(void (*prof)(void), void (*alarm)(void)) {
    cb_prof = prof; cb_alarm = alarm;
    build_ord();   /* now: the backtrace may run while the main thread is suspended (holding the heap lock, say) */
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    if (getenv("HG_HOSTPROF") && atoi(getenv("HG_HOSTPROF"))) {
        hp = calloc(HP_THREADS, sizeof *hp);
        uint8_t *b = (uint8_t *)GetModuleHandleW(0); IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
        exe_lo = (uintptr_t)b; exe_hi = exe_lo + nt->OptionalHeader.SizeOfImage;
        hp_register(main_thread, "main"); hp_on = 1;
    }
    HANDLE h = CreateThread(0, 64 << 10, timer_main, 0, 0, 0);
    if (h) { SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL); CloseHandle(h); }
}
/* Symbols: frames in hgboot.exe are named by the translated guest function whose host code starts at or below the
   address (the generated objects are linked first); frames past the last one are runtime code ("exe+offset" for
   addr2line). Other modules are named by their DLL. */
static int cmp_fn(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b;
    uintptr_t p = (uintptr_t)rec_table[x].fn, q = (uintptr_t)rec_table[y].fn; return p < q ? -1 : p > q; }
static int *ord;
static void build_ord(void) { ord = malloc(sizeof *ord * rec_nfuncs); for (int i = 0; i < rec_nfuncs; i++) ord[i] = i; qsort(ord, rec_nfuncs, sizeof *ord, cmp_fn); }
static void print_frame(FILE *f, int k, uint64_t pc) {
    if (!ord) build_ord();
    HMODULE m = 0; char name[MAX_PATH] = "?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)pc, &m);
    if (m != GetModuleHandleW(0)) { if (m) { GetModuleFileNameA(m, name, sizeof name); } const char *b = strrchr(name, '\\');
        fprintf(f, "  [%2d] %s+0x%llx\n", k, b ? b + 1 : name, (unsigned long long)(pc - (uintptr_t)m)); return; }
    fprintf(f, "  [%2d] exe+0x%llx", k, (unsigned long long)(pc - (uintptr_t)m));
    int lo = 0, hi = rec_nfuncs - 1, r = -1;
    while (lo <= hi) { int mid = (lo + hi) / 2; if ((uintptr_t)rec_table[ord[mid]].fn <= pc) { r = mid; lo = mid + 1; } else hi = mid - 1; }
    if (r >= 0 && r < rec_nfuncs - 1) fprintf(f, "  guest 0x%08x+0x%llx", rec_table[ord[r]].addr, (unsigned long long)(pc - (uintptr_t)rec_table[ord[r]].fn));
    else fprintf(f, "  (runtime)");
    fprintf(f, "\n");
}
static void walk(FILE *f, CONTEXT *c) {
    for (int k = 0; k < 64 && c->Rip; k++) {
        print_frame(f, k, c->Rip);
        DWORD64 img; PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c->Rip, &img, 0);
        if (!rf) { c->Rip = *(DWORD64 *)c->Rsp; c->Rsp += 8; continue; }   /* leaf function */
        void *hd; DWORD64 est; RtlVirtualUnwind(UNW_FLAG_NHANDLER, img, c->Rip, rf, c, &hd, &est, 0);
    }
    fflush(f);
}
void host_backtrace(FILE *f) { CONTEXT c; RtlCaptureContext(&c); walk(f, &c); }

/* Crash reports (Windows): an unhandled exception writes ../crashes/YYYYMMDD-HHMMSS.dmp (a minidump for WinDbg) and
   logs the exception, the guest function and VBlank, Fiona's stage and position, and the host stack with guest
   function names (more useful than the dump: the generated code has no PDB symbols). HG_CRASHTEST=vb crashes on
   purpose at that VBlank, to test this. */
#include <dbghelp.h>
extern uint32_t rec_cur;
extern uint64_t hle_vblank_count(void);
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    static volatile LONG once; if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    char name[128]; time_t t = time(0); struct tm *tm = localtime(&t);
    _mkdir("../crashes"); strftime(name, sizeof name, "../crashes/%Y%m%d-%H%M%S.dmp", tm);
    typedef BOOL (WINAPI *Mdwd)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    HMODULE dbg = LoadLibraryA("dbghelp.dll"); Mdwd dump = dbg ? (Mdwd)(void *)GetProcAddress(dbg, "MiniDumpWriteDump") : 0;
    int dumped = 0;
    HANDLE f = CreateFileA(name, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (dump && f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei = { GetCurrentThreadId(), ep, FALSE };
        dumped = dump(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpWithDataSegs | MiniDumpWithThreadInfo, &mei, 0, 0);
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    uint32_t obj = 0, stg = 0; float pos[3] = {0};
    if (ee_ram) { memcpy(&obj, ee_ram + 0x44f818, 4); if (obj && obj < 0x2000000 - 0x40) { memcpy(&stg, ee_ram + obj + 0x30, 4); memcpy(pos, ee_ram + obj + 0x10, 12); } }
    fprintf(stdout, "\n==== CRASH: exception 0x%08lx at exe+0x%llx; guest function 0x%x, VBlank %llu, stage %x, Fiona at %.1f %.1f %.1f; minidump %s\n",
            (unsigned long)ep->ExceptionRecord->ExceptionCode, (unsigned long long)((uintptr_t)ep->ExceptionRecord->ExceptionAddress - (uintptr_t)GetModuleHandleW(0)),
            rec_cur, (unsigned long long)hle_vblank_count(), stg, pos[0], pos[1], pos[2], dumped ? name : "(not written)");
    fprintf(stdout, "host backtrace at the fault:\n");
    CONTEXT c = *ep->ContextRecord; walk(stdout, &c);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}
void host_crash_init(void) { SetUnhandledExceptionFilter(crash_filter); }

void host_backtrace_main(FILE *f) {
    _lock_file(f); SuspendThread(main_thread); _unlock_file(f);   /* not stopped inside the log stream's lock */
    CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(main_thread, &c)) { fprintf(f, "  (GetThreadContext failed)\n"); return; }
    walk(f, &c);
}

#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>
#include <execinfo.h>
#include <fnmatch.h>

void *host_alloc(size_t sz) {
    void *p = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) stop("mmap of %zu bytes failed", sz);
    return p;
}
int host_ncpus(void) { return (int)sysconf(_SC_NPROCESSORS_ONLN); }
void host_gmtime(time_t t, struct tm *out) { gmtime_r(&t, out); }
int host_mkdir(const char *path) { return mkdir(path, 0755); }
int host_fnmatch(const char *pat, const char *name) { return fnmatch(pat, name, 0); }

void host_fiber_make(HostFiber *f, void (*fn)(void), size_t stack_size) {
    if (!f->stack) { f->size = stack_size; f->stack = host_alloc(stack_size); }   /* kept across thread restarts */
    getcontext(&f->uc); f->uc.uc_stack.ss_sp = f->stack; f->uc.uc_stack.ss_size = f->size; f->uc.uc_link = 0;
    makecontext(&f->uc, fn, 0);
}
void host_fiber_switch(HostFiber *from, HostFiber *to) { swapcontext(&from->uc, &to->uc); }

static void (*cb_prof)(void), (*cb_alarm)(void);
static void on_prof(int sig) { (void)sig; cb_prof(); }
static void on_alarm(int sig) { (void)sig; cb_alarm(); }
void host_start_timers(void (*prof)(void), void (*alarm)(void)) {
    cb_prof = prof; cb_alarm = alarm;
    struct sigaction sa = {0}; sa.sa_handler = on_prof; sa.sa_flags = SA_RESTART; sigaction(SIGPROF, &sa, 0);
    struct itimerval it = {{0, 1000}, {0, 1000}}; setitimer(ITIMER_PROF, &it, 0);
    sa.sa_handler = on_alarm; sigaction(SIGALRM, &sa, 0);
    struct itimerval ia = {{0, 16667}, {0, 16667}}; setitimer(ITIMER_REAL, &ia, 0);
}
void host_prof_thread(const char *name) { (void)name; }
void host_thread_boost(void) {}
void host_crash_init(void) {}
void host_prof_dump(void) {}
void host_backtrace(FILE *f) { void *bt[64]; int n = backtrace(bt, 64); fflush(f); backtrace_symbols_fd(bt, n, fileno(f)); }
#endif
