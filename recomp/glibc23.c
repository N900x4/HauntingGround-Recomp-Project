/* Linux: glibc 2.38 and later redirect strtol, strtoul, strtoll, strtoull and sscanf to their C23 versions
   (__isoc23_*, which also accept a "0b" binary prefix) whenever _GNU_SOURCE is defined, as it is in runtime.c, hle.c,
   compat.c and iop_pad_mc.c (and in C23 mode, gcc 15's default). That made the game need glibc 2.38. These definitions, compiled here without
   _GNU_SOURCE (so the calls below are the classic functions), satisfy those references inside the game, so it
   runs with glibc 2.34 (Ubuntu 22.04, SteamOS and later), and numbers are parsed as on Windows (no "0b"). */
#ifdef _GNU_SOURCE
#error glibc23.c must be compiled without _GNU_SOURCE
#endif
#include <stdarg.h>
/* libc's classic functions by their symbol names: the headers would redirect the plain names back to __isoc23_*
   in C23 mode (gcc 15's default), and these would call themselves */
extern long c_strtol(const char *, char **, int) __asm__("strtol");
extern unsigned long c_strtoul(const char *, char **, int) __asm__("strtoul");
extern long long c_strtoll(const char *, char **, int) __asm__("strtoll");
extern unsigned long long c_strtoull(const char *, char **, int) __asm__("strtoull");
extern int c_vsscanf(const char *, const char *, va_list) __asm__("__isoc99_vsscanf");
#define H __attribute__((visibility("hidden")))   /* the game's own references only: not exported (-rdynamic), so the
                                                    system's libraries keep libc's C23 versions */
H long __isoc23_strtol(const char *s, char **e, int b) { return c_strtol(s, e, b); }
H unsigned long __isoc23_strtoul(const char *s, char **e, int b) { return c_strtoul(s, e, b); }
H long long __isoc23_strtoll(const char *s, char **e, int b) { return c_strtoll(s, e, b); }
H unsigned long long __isoc23_strtoull(const char *s, char **e, int b) { return c_strtoull(s, e, b); }
H int __isoc23_sscanf(const char *s, const char *fmt, ...) { va_list ap; va_start(ap, fmt); const int r = c_vsscanf(s, fmt, ap); va_end(ap); return r; }
H int __isoc23_vsscanf(const char *s, const char *fmt, va_list ap) { return c_vsscanf(s, fmt, ap); }
