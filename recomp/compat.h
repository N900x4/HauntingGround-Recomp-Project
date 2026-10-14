/* Host services that differ between Linux and Windows (MSYS2 UCRT64). Linux keeps the original POSIX calls
   (mmap, ucontext, setitimer signals, execinfo backtrace, fnmatch, gmtime_r); Windows uses VirtualAlloc, fibers,
   a timer thread, RtlVirtualUnwind and a small glob matcher. */
#ifndef COMPAT_H
#define COMPAT_H
#include <stddef.h>
#include <stdio.h>
#include <time.h>

void *host_alloc(size_t sz);                        /* zeroed, page-aligned; stops on failure */
int host_ncpus(void);
void host_gmtime(time_t t, struct tm *out);
int host_mkdir(const char *path);                   /* mode 0755 on Linux */
int host_fnmatch(const char *pat, const char *name); /* 0 on match, like fnmatch(pat, name, 0) */

/* cooperative fibers for the EE threads */
#ifdef _WIN32
typedef struct { void *h; } HostFiber;
#else
#include <ucontext.h>
typedef struct { ucontext_t uc; void *stack; size_t size; } HostFiber;
#endif
void host_fiber_make(HostFiber *f, void (*fn)(void), size_t stack_size);   /* (re)start fn on the fiber's own stack */
void host_fiber_switch(HostFiber *from, HostFiber *to);                    /* from = the running fiber */

/* periodic callbacks: prof every 1 ms (process CPU time on Linux, wall time on Windows), alarm every 1/60 s.
   On Linux both run as signal handlers on the interrupted thread; on Windows on a timer thread. */
void host_start_timers(void (*prof)(void), void (*alarm)(void));
/* write the calling thread's host call stack to f, with the translated guest function of each frame */
void host_backtrace(FILE *f);
/* HG_HOSTPROF=1 host-PC sampler (Windows; use perf on Linux): register the calling thread, write hostprof.txt */
void host_prof_thread(const char *name);
/* the calling thread a step above normal priority (the EE and GS threads set the pace; Windows only) */
void host_thread_boost(void);
/* crash reports: minidump in ../crashes and a guest-annotated backtrace in the log (Windows) */
void host_crash_init(void);
void host_prof_dump(void);
#ifdef _WIN32
void host_backtrace_main(FILE *f);   /* from another thread: suspends the main thread (for good) and walks its stack */
#endif
#endif
