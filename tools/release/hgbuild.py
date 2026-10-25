#!/usr/bin/env python3
"""Compile and link hgboot.exe without make or a shell: the release launcher's build (the player's PC, bundled gcc).
The rules mirror recomp/Makefile (Windows branch, SDL2 and Vulkan on); tools/release/mkrelease.py checks them against
`make -n -B` before it builds a release. Jobs run in parallel (all cores by default).

usage: hgbuild.py --src RECOMP_DIR --gen GEN_DIR --obj OBJ_DIR --out EXE [--cc GCC] [--jobs N] [--print]
                  [--target windows|linux] [--lib-dir DIR]
  --target linux  the Linux release: --cc is the bundled zig ("zig cc" for x86_64 baseline and glibc 2.31, so the game
                  runs on older distributions and does not depend on the player's CPU), SDL2 and Vulkan linked from
                  --lib-dir (link stubs; the system's libraries at run time), -rdynamic as the Makefile's Linux branch
  RECOMP_DIR  the repository's recomp/ (the runtime sources)
  GEN_DIR     what bulk2.py wrote (part*.c, table.c, vu1_aot_*.c, protos.h) plus the gs_*_spv.h shader headers
Progress lines on stdout: "@progress DONE TOTAL NAME"; errors: "@error TEXT". Exit status 0 when the exe was linked."""
import os, sys, glob, subprocess, argparse, threading, time
from concurrent.futures import ThreadPoolExecutor

GENFLAGS = ['-O1', '-w', '-fno-strict-aliasing']
RTFLAGS = ['-O1', '-g', '-Wall', '-Wno-unused', '-fno-strict-aliasing']
SDL = ['-Dmain=SDL_main', '-DHAVE_SDL']          # plus -I<include>/SDL2
VK = ['-DHAVE_VULKAN']
O2FP = ['-O2', '-ffp-contract=off']
# runtime object -> (source relative to recomp/, extra flags after RTFLAGS); same order as the Makefile's link line
RUNTIME = [
    ('runtime', 'runtime.c', []), ('hle', 'hle.c', []), ('interp', 'interp.c', []), ('iop_pad_mc', 'iop_pad_mc.c', []),
    ('overrides', 'overrides.c', []), ('options', 'game/options.c', []), ('settings', 'settings.c', ['SDL']),
    ('gs', 'gs.c', VK + O2FP), ('dmac', 'dmac.c', []), ('host_sdl', 'host_sdl.c', ['SDL'] + VK), ('vu1', 'vu1.c', O2FP),
    ('vu1_ref', 'vu1_ref.c', O2FP), ('ipu', 'ipu.c', ['-O2']), ('iop_cpu', 'iop_cpu.c', ['-O2']),
    ('iop_kernel', 'iop_kernel.c', ['-O2']), ('spu2', 'spu2.c', ['-O2']), ('compat', 'compat.c', []), ('hdtex', 'hdtex.c', VK + ['-O2']), ('gsvk', 'gsvk.c', []),
]
LIBS = ['-lm', '-lpthread', '-lmingw32', '-lSDL2', '-lvulkan-1']
LINUX_SDL = ['-DHAVE_SDL', '-D_REENTRANT']                          # pkg-config --cflags sdl2 on Linux (plus -I)
LINUX_LIBS = ['-rdynamic', '-lm', '-lpthread', '-ldl', '-lSDL2', '-lvulkan']
ZIG_TARGET = ['-target', 'x86_64-linux-gnu.2.31', '-mcpu=baseline']

def plan(src, gen, obj, sdl_inc, sdl=None, extra_inc=()):
    """[(object, command)] in link order; command without the compiler."""
    inc = ['-I' + src, '-I' + gen, '-I' + os.path.dirname(os.path.abspath(gen))] + ['-I' + d for d in extra_inc]   # the third for gsvk.c's "gen/gs_*_spv.h"
    jobs = []
    gensrc = sorted(glob.glob(os.path.join(gen, 'part*.c'))) + [os.path.join(gen, 'table.c')] + sorted(glob.glob(os.path.join(gen, 'vu1_aot_*.c')))
    for c in gensrc:
        name = os.path.splitext(os.path.basename(c))[0]; o = os.path.join(obj, name + '.o')
        if name.startswith('vu1_aot_'): flags = RTFLAGS + inc + O2FP + ['-g0']
        else: flags = GENFLAGS + inc
        jobs.append((o, flags + ['-c', c, '-o', o]))
    for name, rel, extra in RUNTIME:
        o = os.path.join(obj, name + '.o'); flags = list(RTFLAGS) + inc
        for e in extra: flags += ((sdl or SDL) + ['-I' + sdl_inc]) if e == 'SDL' else [e]
        jobs.append((o, flags + ['-c', os.path.join(src, rel), '-o', o]))
    return jobs

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', required=True); ap.add_argument('--gen', required=True); ap.add_argument('--obj', required=True)
    ap.add_argument('--out', required=True); ap.add_argument('--cc', default='gcc'); ap.add_argument('--jobs', type=int, default=0)
    ap.add_argument('--sdl-include', default=None); ap.add_argument('--print', action='store_true')
    ap.add_argument('--warnings', action='store_true')
    ap.add_argument('--target', default='windows', choices=('windows', 'linux')); ap.add_argument('--lib-dir', default=None)
    ap.add_argument('--include', action='append', default=[])   # (Linux: the bundled SDL2 and Vulkan headers)
    a = ap.parse_args()
    linux = a.target == 'linux'
    cc = [a.cc, 'cc'] + ZIG_TARGET if linux else [a.cc]
    if a.sdl_include: sdl_inc = a.sdl_include
    else:   # <toolchain>/bin/gcc.exe -> <toolchain>/include/SDL2
        sdl_inc = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(a.cc))), 'include', 'SDL2') if os.path.isabs(a.cc) else 'SDL2'
    for need in ('table.c', 'gs_sprite_spv.h', 'gs_batch_spv.h', 'gs_batchc_spv.h'):
        if not os.path.exists(os.path.join(a.gen, need)) and not a.print:
            print('@error missing ' + os.path.join(a.gen, need), flush=True); return 2
    jobs = plan(a.src, a.gen, a.obj, sdl_inc, LINUX_SDL if linux else None, a.include)
    link = cc + ['-o', a.out] + [o for o, _ in jobs] + ((['-L' + a.lib_dir] if a.lib_dir else []) + LINUX_LIBS if linux else LIBS)
    if a.print:
        for o, c in jobs: print(' '.join(cc + c))
        print(' '.join(link)); return 0
    os.makedirs(a.obj, exist_ok=True)
    n = a.jobs or os.cpu_count() or 4
    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    # the biggest translation units first, so the long ones do not end up last
    order = sorted(jobs, key=lambda j: -os.path.getsize(j[1][j[1].index('-c') + 1]))
    lock = threading.Lock(); done = [0]; failed = []; warned = [0]
    show_warnings = a.warnings   # the runtime's -Wall warnings mean nothing to a player: only on request or on failure
    t0 = time.time()
    def one(job):
        o, c = job
        if failed: return
        r = subprocess.run(cc + c, stdin=subprocess.DEVNULL, capture_output=True, text=True, errors='replace', creationflags=flags)
        with lock:
            done[0] += 1
            msg = (r.stdout + r.stderr).rstrip()
            if msg and (r.returncode != 0 or show_warnings): print(msg, flush=True)
            elif msg: warned[0] += 1
            if r.returncode != 0: failed.append(os.path.basename(o)); print(f'@error compiling {os.path.basename(o)} failed (status {r.returncode})', flush=True)
            else: print(f'@progress {done[0]} {len(jobs) + 1} {os.path.basename(o)}', flush=True)
    print(f'compiling {len(jobs)} files with {n} jobs ({" ".join(cc)})', flush=True)
    with ThreadPoolExecutor(max_workers=n) as ex: list(ex.map(one, order))
    if failed: return 1
    print(f'compiled in {time.time() - t0:.1f} s ({warned[0]} files with compiler warnings, not shown; --warnings shows them); linking', flush=True)
    r = subprocess.run(link, stdin=subprocess.DEVNULL, capture_output=True, text=True, errors='replace', creationflags=flags)
    if r.stdout.strip() or r.stderr.strip(): print((r.stdout + r.stderr).rstrip(), flush=True)
    if r.returncode != 0 or not os.path.exists(a.out): print(f'@error linking failed (status {r.returncode})', flush=True); return 1
    print(f'@progress {len(jobs) + 1} {len(jobs) + 1} {os.path.basename(a.out)}', flush=True)
    print(f'built {a.out} in {time.time() - t0:.1f} s', flush=True)
    return 0

if __name__ == '__main__': sys.exit(main())
