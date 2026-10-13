/* IOP module HLE for the controller (DBCMAN, used by libdbc/libpad2) and the memory card (MCSERV, used by libmc).
   Protocols were read from the game's own EE libraries (addresses are SLUS_210.75 NTSC-U):
     libdbc   0x1ee740-0x1ef968   sid 0x80001300 (+0x131c, 0x131e, 0x131f); fn = 0x800013xx
     libpad2  0x1efa38-0x1f0160   socket buffer = 2 x 0x80 bytes, newest half chosen by the counter at +0x7c
     libmc    0x110938-0x1121xx   sid 0x80000400, "XMC" command numbers, descriptor at 0x47bd80/0x47bdb0
   Memory cards are host directories: $HG_MEMCARD_DIR/port0 (default ../memcards/port0). Port 1 is empty. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <strings.h>
#include <fnmatch.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "ee_runtime.h"
#include "hle.h"

static uint32_t R32(uint32_t a) { uint32_t v; memcpy(&v, gptr(a), 4); return v; }
static void W32(uint32_t a, uint32_t v) { memcpy(gptr(a), &v, 4); }
static uint32_t S32(const uint8_t *S, int off) { uint32_t v; memcpy(&v, S + off, 4); return v; }

/* ================================================================== controller */
/* button bits in the 16-bit word libpad2 hands the game (classic DualShock 2 order; data is active-low on the wire) */
enum { B_SELECT = 0, B_L3, B_R3, B_START, B_UP, B_RIGHT, B_DOWN, B_LEFT, B_L2, B_R2, B_L1, B_R1, B_TRIANGLE, B_CIRCLE, B_CROSS, B_SQUARE };
static const char *const bnames[16] = { "select", "l3", "r3", "start", "up", "right", "down", "left", "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square" };
#define PAD_DATA_LEN 18      /* 2 bytes buttons + 16 analog bytes (rx ry lx ly, then pressures) */
#define PAD_PROF_LEN 4       /* bitmask: 16 digital channels, 16 analog channels */

static uint32_t dbc_work;            /* EE address given by sceDbcSetWorkAddr: per-socket "connected" words */
typedef struct { int used; uint32_t buf; uint32_t counter; } Sock;
static Sock sock[16];
static uint16_t pad_held;            /* active-high, from the host input source */
static uint8_t pad_analog[16] = { 0x80, 0x80, 0x80, 0x80 };

/* scripted input for headless runs: HG_PAD="600:cross,900:cross+start,1200-1260:up"
   vblank[:-end]:buttons ; a single vblank holds the buttons for 6 VBlanks */
typedef struct { long from, to; uint16_t bits; } PadEv;
static PadEv pev[64]; static int npev = -1;
static void pad_script_load(void) {
    npev = 0;
    const char *e = getenv("HG_PAD"); if (!e) return;
    char *s = strdup(e), *save = 0;
    for (char *tok = strtok_r(s, ",", &save); tok && npev < 64; tok = strtok_r(0, ",", &save)) {
        PadEv *p = &pev[npev]; char *c = strchr(tok, ':'); if (!c) continue;
        *c = 0; p->from = strtol(tok, 0, 10); char *dash = strchr(tok, '-'); p->to = dash ? strtol(dash + 1, 0, 10) : p->from + 6;
        p->bits = 0;
        for (char *b = strtok(c + 1, "+"); b; b = strtok(0, "+"))
            for (int i = 0; i < 16; i++) if (!strcasecmp(b, bnames[i])) p->bits |= 1u << i;
        npev++;
    }
    free(s);
    L("pad: %d scripted input events from HG_PAD\n", npev);
}
void pad_set_input(uint16_t held, const uint8_t analog[4]) { pad_held = held; if (analog) memcpy(pad_analog, analog, 4); }

/* HG_EXPLORE=vb: from that VBlank on, a simple explorer drives Fiona (testing): it holds one direction until her
   position (player object *(0x44f818) + 0x10) stops changing, then turns to the next direction (pseudo-random),
   taps Cross every 2 s to read text and use doors, and logs the position every second. */
static uint16_t explore(uint64_t vb) {
    static long start = -2; if (start == -2) start = getenv("HG_EXPLORE") ? atol(getenv("HG_EXPLORE")) : -1;
    if (start < 0 || (long)vb < start) return 0;
    static const int dirs[4] = { B_UP, B_RIGHT, B_DOWN, B_LEFT };
    static int dir; static float lx, lz; static uint32_t rng; static long since, stuck_at = -1000;
    if (!rng) rng = getenv("HG_EXPLORE_SEED") ? (uint32_t)atol(getenv("HG_EXPLORE_SEED")) : 12345;
    uint32_t obj = 0; memcpy(&obj, gptr(0x44f818), 4);
    float x = 0, y = 0, z = 0;
    if (obj) { memcpy(&x, gptr(obj + 0x10), 4); memcpy(&y, gptr(obj + 0x14), 4); memcpy(&z, gptr(obj + 0x18), 4); }
    long t = (long)vb - start;
    if (t % 30 == 0) {
        float moved = fabsf(x - lx) + fabsf(z - lz);
        since += 30;
        if (moved < 1.0f && since >= 60 && stuck_at < t - 90) stuck_at = t;   /* blocked: try Cross (doors, objects) for a moment first */
        else if ((moved < 1.0f && since >= 60 && t - stuck_at >= 60) || since >= 900) { rng = rng * 1103515245u + 12345u; dir = (dir + 1 + (int)((rng >> 16) % 3)) & 3; since = 0; }
        lx = x; lz = z;
    }
    if (t % 60 == 0) L("explore: VBlank %llu pos %.1f %.1f %.1f (obj 0x%x) dir %s\n", (unsigned long long)vb, x, y, z, obj, bnames[dirs[dir]]);
    uint16_t h = 1u << dirs[dir];
    if (t % 120 < 6 || (t - stuck_at >= 6 && t - stuck_at < 12) || (t - stuck_at >= 30 && t - stuck_at < 36)) h |= 1u << B_CROSS;
    return h;
}
void pad_vblank(uint64_t vb) {
    if (npev < 0) pad_script_load();
    uint16_t ex = explore(vb);
    if (npev > 0 || ex) {
        uint16_t h = ex;
        for (int i = 0; i < npev; i++) if ((long)vb >= pev[i].from && (long)vb < pev[i].to) h |= pev[i].bits;
        static uint16_t last; if (h != last) { L("pad: buttons 0x%04x at VBlank %llu\n", h, (unsigned long long)vb); last = h; }
        pad_held = h;
    }
    if (dbc_work) for (int s = 0; s < 16; s++) W32(dbc_work + 4 * s, sock[s].used ? 1 : 0);
    for (int s = 0; s < 16; s++) {
        if (!sock[s].used || !sock[s].buf) continue;
        Sock *k = &sock[s]; k->counter++;
        uint8_t *b = gptr(k->buf + 0x80 * (k->counter & 1));
        b[0] = 1;                        /* state: stable (the game's pad task only reads data in state 1) */
        b[1] = 0; b[2] = PAD_DATA_LEN; b[3] = PAD_PROF_LEN;
        uint32_t one = 1; memcpy(b + 4, &one, 4);
        uint16_t raw = (uint16_t)~pad_held;
        b[0x1c] = raw & 0xff; b[0x1d] = raw >> 8;
        static const uint8_t pressure_btn[12] = { B_RIGHT, B_LEFT, B_UP, B_DOWN, B_TRIANGLE, B_CIRCLE, B_CROSS, B_SQUARE, B_L1, B_R1, B_L2, B_R2 };
        for (int i = 0; i < 16; i++) b[0x1e + i] = i < 4 ? pad_analog[i] : (pad_held >> pressure_btn[i - 4]) & 1 ? 0xff : 0;
        memset(b + 0x1c + PAD_DATA_LEN, 0xff, PAD_PROF_LEN);
        memcpy(b + 0x7c, &k->counter, 4);
    }
}

/* returns 1 when the sid belongs to DBCMAN */
int pad_rpc(uint32_t sid, uint32_t fn, const uint8_t *S, uint32_t ssize, uint32_t recv, uint32_t rsize) {
    (void)ssize;
    if ((sid & 0xFFFFFF00u) != 0x80001300u) return 0;
    if (!recv || rsize < 4) return 1;
    switch (fn) {
    case 0x80001363: W32(recv, 0x0310); break;                                   /* module version: libdbc wants 0x31x */
    case 0x80001304: dbc_work = S32(S, 4); W32(recv, 1); L("DBCMAN: SetWorkAddr 0x%x\n", dbc_work); break;
    case 0x80001301: {                                                           /* CreateSocket(params[5], buf, buf+0x80) */
        int s = 0; while (s < 15 && sock[s].used) s++;
        sock[s].used = 1; sock[s].buf = S32(S, 0x28); sock[s].counter = 0;
        W32(recv, 1); if (rsize >= 0x28) W32(recv + 0x24, s);
        L("DBCMAN: CreateSocket type 0x%x port %d -> socket %d, buffer 0x%x\n", S32(S, 0), S32(S, 4), s, sock[s].buf); break; }
    case 0x80001302: { uint32_t s = S32(S, 0); if (s < 16) sock[s].used = 0; W32(recv, 1); break; }  /* DeleteSocket */
    case 0x80001303: W32(recv, 0); W32(recv + 4, 0); break;                      /* GetDepNumber: device 0 */
    case 0x80001317: W32(recv, 0); W32(recv + 4, 1); break;                      /* GetDeviceStatus: present */
    case 0x8000131a:                                                             /* SRData: command to the device; answer "stable" */
        if (rsize >= 0x90) { W32(recv + 8, 1); *gptr(recv + 0xc) = 1; W32(recv + 0x8c, 0); }
        break;
    default: W32(recv, 1); if (rsize >= 8) W32(recv + 4, 0); break;
    }
    return 1;
}

/* ================================================================== memory card */
enum { MC_NOFORMAT = -2, MC_FULL = -3, MC_NOENTRY = -4, MC_DENIED = -5, MC_NOTEMPTY = -6, MC_UPLIMIT = -7, MC_NOCARD = -10 };
#define MC_FREE_KB 7800        /* reported free clusters on a fresh 8 MB card */
static char mc_root[2][1024];
static char mc_cwd[2][1024] = { "/", "/" };
typedef struct { FILE *f; int port; } McFd;
static McFd mcfd[32];
static uint64_t mc_calls;

static int mc_present(int port) { return port == 0; }
static const char *mc_dir(int port) {
    if (!mc_root[port][0]) {
        const char *b = getenv("HG_MEMCARD_DIR") ? getenv("HG_MEMCARD_DIR") : "../memcards";
        snprintf(mc_root[port], sizeof mc_root[port], "%s/port%d", b, port);
        char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", b); mkdir(tmp, 0755); mkdir(mc_root[port], 0755);
    }
    return mc_root[port];
}
/* card path (absolute or relative to the card cwd) -> host path; rejects "..": returns 0 on bad path */
static int mc_host(int port, const char *name, char *out, size_t n) {
    char p[1100];
    if (name[0] == '/') snprintf(p, sizeof p, "%s", name); else snprintf(p, sizeof p, "%s/%s", mc_cwd[port], name);
    for (char *c = p; (c = strstr(c, "..")); c += 2) if ((c == p || c[-1] == '/') && (c[2] == '/' || !c[2])) return 0;
    snprintf(out, n, "%s/%s", mc_dir(port), p + (p[0] == '/'));
    size_t l = strlen(out); while (l > 1 && out[l - 1] == '/') out[--l] = 0;
    return 1;
}
static void mc_datetime(uint8_t *d, time_t t) {       /* sceMcStDateTime: resv, sec, min, hour, day, month, year16 (JST) */
    t += 9 * 3600; struct tm tm; gmtime_r(&t, &tm);
    d[0] = 0; d[1] = tm.tm_sec; d[2] = tm.tm_min; d[3] = tm.tm_hour; d[4] = tm.tm_mday; d[5] = tm.tm_mon + 1;
    uint16_t y = tm.tm_year + 1900; memcpy(d + 6, &y, 2);
}
static void mc_entry(uint32_t ee, const char *name, const struct stat *st) {   /* sceMcTblGetDir, 64 bytes */
    uint8_t e[64] = {0};
    mc_datetime(e, st->st_mtime); mc_datetime(e + 8, st->st_mtime);
    uint32_t sz = S_ISDIR(st->st_mode) ? 0 : (uint32_t)st->st_size; memcpy(e + 0x10, &sz, 4);
    uint16_t attr = S_ISDIR(st->st_mode) ? 0x8427 : 0x8497; memcpy(e + 0x14, &attr, 2);
    strncpy((char *)e + 0x20, name, 31);
    memcpy(gptr(ee), e, 64);
}
/* Sony's MCSERV treats a negative maxent as no limit (the game asks with maxent -1 and no table to count a save
   directory's entries; seen on PCSX2 with the disc's own module: 5 for a save). With no table, only count. */
static int mc_getdir(int port, const char *name, uint32_t maxent_u, uint32_t table) {
    char path[1100]; if (!mc_host(port, name, path, sizeof path)) return MC_NOENTRY;
    int maxent = (int32_t)maxent_u < 0 ? 0x7FFFFFFF : (int)maxent_u;
    char *slash = strrchr(path, '/'); char pat[256]; snprintf(pat, sizeof pat, "%s", slash + 1); *slash = 0;
    int is_root = !strcmp(path, mc_dir(port));
    DIR *d = opendir(path); if (!d) return MC_NOENTRY;
    int n = 0; struct dirent *de; struct stat st;
    if (!is_root && strpbrk(pat, "*?")) {                 /* real cards list "." and ".." inside a directory */
        stat(path, &st);
        if (n < maxent && !fnmatch(pat, ".", 0)) { if (table) mc_entry(table + 64 * n, ".", &st); n++; }
        if (n < maxent && !fnmatch(pat, "..", 0)) { if (table) mc_entry(table + 64 * n, "..", &st); n++; }
    }
    while ((de = readdir(d)) && n < maxent) {
        if (de->d_name[0] == '.') continue;
        if (fnmatch(pat, de->d_name, 0)) continue;
        char fp[1400]; snprintf(fp, sizeof fp, "%s/%s", path, de->d_name);
        if (stat(fp, &st)) continue;
        if (table) mc_entry(table + 64 * n, de->d_name, &st);
        n++;
    }
    closedir(d);
    return n;
}
static int mc_open(int port, const char *name, uint32_t mode) {
    char path[1100]; if (!mc_host(port, name, path, sizeof path)) return MC_NOENTRY;
    struct stat st;
    if (mode & 0x40) { if (!stat(path, &st)) return MC_NOENTRY; return mkdir(path, 0755) ? MC_NOENTRY : 0; }   /* sceMcMkdir */
    int fd = 0; while (fd < 32 && mcfd[fd].f) fd++;
    if (fd == 32) return MC_UPLIMIT;
    int exists = !stat(path, &st);
    if (exists && S_ISDIR(st.st_mode)) return MC_DENIED;
    if (!exists && !(mode & 0x200)) return MC_NOENTRY;
    const char *fm = (mode & 3) == 1 ? "rb" : exists ? "r+b" : "w+b";
    FILE *f = fopen(path, fm); if (!f) return MC_NOENTRY;
    mcfd[fd].f = f; mcfd[fd].port = port;
    return fd;
}

int mc_rpc(uint32_t sid, uint32_t fn, const uint8_t *S, uint32_t ssize, uint32_t recv, uint32_t rsize) {
    (void)ssize;
    if (sid != 0x80000400u) return 0;
    mc_calls++;
    int32_t r = 0;
    /* descriptor A (0x30 bytes): fd port slot size offset origin buffer param data[16]
       descriptor B (0x414 bytes): port slot flags maxent table name[1024] */
    uint32_t fd = S32(S, 0), port = S32(S, 4), size = S32(S, 0xc), off = S32(S, 0x10), org = S32(S, 0x14), buf = S32(S, 0x18), par = S32(S, 0x1c);
    uint32_t bport = S32(S, 0), bflags = S32(S, 8), bmax = S32(S, 0xc), btab = S32(S, 0x10); const char *bname = (const char *)S + 0x14;
    McFd *F = fd < 32 ? &mcfd[fd] : 0;
    switch (fn) {
    case 0xfe:                                                     /* init: result, mcserv version, mcman version */
        if (rsize >= 12) { W32(recv, 0); W32(recv + 4, 0x20b); W32(recv + 8, 0x20e); }
        L("MCSERV: init (versions 0x20b/0x20e), card dir %s\n", mc_dir(0)); return 1;
    case 0x01:                                                     /* GetInfo(port, slot, type*, free*, format*) */
        if (!mc_present(port & 1)) r = MC_NOCARD;
        if (par) { W32(par, mc_present(port & 1) ? 2 : 0); W32(par + 4, mc_present(port & 1) ? MC_FREE_KB : 0); W32(par + 0x90, mc_present(port & 1)); }
        break;
    case 0x02: r = mc_present(bport & 1) ? mc_open(bport & 1, bname, bflags) : MC_NOCARD; break;   /* Open / Mkdir */
    case 0x03: if (F && F->f) { fclose(F->f); F->f = 0; } else r = MC_DENIED; break;                /* Close */
    case 0x04:                                                     /* Seek(fd, offset, origin) */
        if (F && F->f) { fseek(F->f, (int32_t)off, org == 1 ? SEEK_CUR : org == 2 ? SEEK_END : SEEK_SET); r = (int32_t)ftell(F->f); } else r = MC_DENIED; break;
    case 0x05:                                                     /* Read(fd, buf, size): data straight to EE, no unaligned head/tail */
        if (par) { W32(par, 0); W32(par + 4, 0); }
        if (F && F->f && size < 0x2000000) r = (int32_t)fread(gptr(buf), 1, size, F->f); else r = MC_DENIED; break;
    case 0x06:                                                     /* Write(fd): head bytes in data[], rest at buffer */
        if (F && F->f) { size_t w = 0; if (org) w += fwrite(S + 0x20, 1, org, F->f); if (size && buf) w += fwrite(gptr(buf), 1, size, F->f); r = (int32_t)w; }
        else r = MC_DENIED; break;
    case 0x0a: if (F && F->f) fflush(F->f); break;                 /* Flush */
    case 0x0c: {                                                   /* ChDir */
        char path[1100]; struct stat st;
        if (mc_host(bport & 1, bname, path, sizeof path) && !stat(path, &st) && S_ISDIR(st.st_mode)) {
            if (bname[0] == '/') snprintf(mc_cwd[bport & 1], 1024, "%s", bname); else { size_t l = strlen(mc_cwd[bport & 1]); snprintf(mc_cwd[bport & 1] + l, 1024 - l, "%s%s", l > 1 ? "/" : "", bname); }
        } else r = MC_NOENTRY; break; }
    case 0x0d: r = mc_present(bport & 1) ? mc_getdir(bport & 1, bname, bmax, btab) : MC_NOCARD; break;   /* GetDir */
    case 0x0e:                                                     /* SetFileInfo; flag 0x10 = rename to info.EntryName */
        if (bflags & 0x10) {
            char from[1100], to[1100], nn[33]; memcpy(nn, gptr(S32(S, 0x10) + 0x20), 32); nn[32] = 0;
            if (mc_host(bport & 1, bname, from, sizeof from)) { snprintf(to, sizeof to, "%s", from); char *sl = strrchr(to, '/'); snprintf(sl + 1, sizeof to - (sl + 1 - to), "%s", nn); r = rename(from, to) ? MC_NOENTRY : 0; }
            else r = MC_NOENTRY;
        } break;
    case 0x0f: {                                                   /* Delete */
        char path[1100]; struct stat st;
        if (!mc_host(bport & 1, bname, path, sizeof path) || stat(path, &st)) r = MC_NOENTRY;
        else if (S_ISDIR(st.st_mode)) r = rmdir(path) ? MC_NOTEMPTY : 0; else r = unlink(path) ? MC_DENIED : 0;
        break; }
    case 0x10: case 0x11: r = 0; break;                            /* Format / Unformat: the host directory is always "formatted" */
    case 0x12: r = MC_FREE_KB; break;                              /* GetEntSpace */
    case 0x14: r = 0; break;                                       /* ChangeThreadPriority */
    case 0x15: r = 1; break;                                       /* slots per port */
    case 0x35: r = 0; break;                                       /* UdCheckNewCard */
    default: L("MCSERV: unhandled fn 0x%x\n", fn); r = 0;
    }
    if (recv && rsize >= 4) W32(recv, (uint32_t)r);
    if (mc_calls < 400 || bg_verbose > 1) {
        if (fn == 0x02 || fn == 0x0d || fn == 0x0c || fn == 0x0e || fn == 0x0f) L("MCSERV: fn 0x%x port %d \"%s\" flags 0x%x max %u tab 0x%x -> %d\n", fn, bport, bname, bflags, bmax, btab, r);
        else L("MCSERV: fn 0x%x fd/port %d size 0x%x -> %d\n", fn, fn == 1 ? port : fd, size, r);
    }
    return 1;
}
