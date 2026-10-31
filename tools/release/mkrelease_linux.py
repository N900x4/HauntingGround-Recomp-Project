#!/usr/bin/env python3
"""Build the Linux release folder (and .tar.gz) of Haunting Ground Recomp, the counterpart of mkrelease.py (Windows): a
launcher script that builds the game on the player's PC from their own disc image with a bundled compiler (gcc), plus
the HD texture pack (the user's decision for the release bundles, 2026-10-25; the repository never holds it). The
compiler is gcc from a Bootlin toolchain (x86-64, glibc 2.39 sysroot: the game then needs glibc 2.34, see
recomp/glibc23.c); zig was tried first, but its clang takes 4 to 8 minutes per compiled VU1 program (gcc: seconds).

    python3 tools/release/mkrelease_linux.py --gcc DIR --gcc-licenses DIR --textures DIR [--out DIR] [--version 0.1] [--no-zip]

Run it on a Linux development PC with a finished development build in recomp/ (its binary's imports give the link
stubs' symbols; recomp/gen is not copied), glslangValidator, and the SDL2 and Vulkan headers in /usr/include.
Output: OUT/HauntingGround-Recomp-VERSION/ and OUT/HauntingGround-Recomp-VERSION-linux-x64.tar.gz (symbolic links
and permissions kept). OUT must be outside the repository.

What goes in:
  HauntingGround.sh             the launcher (tools/release/HauntingGround.sh)
  README.txt, LICENSE.txt, THIRD_PARTY_NOTICES.txt, docs/
  textures/                     the HD replacements (<id>.dds BC7, a few <id>.png), from --textures
  engine/version.txt
  engine/src/                   the tracked sources the build needs, plus prebuilt/gs_*_spv.h
  engine/gcc/                   the Bootlin toolchain (--gcc: x86-64--glibc--stable-2024.05-1), without C++, gdb, gcov,
                                locales and documentation
  engine/include/               the SDL2 and Vulkan headers (zlib; Apache-2.0 or MIT)
  engine/link/                  link stubs libSDL2.so (soname libSDL2-2.0.so.0) and libvulkan.so (libvulkan.so.1):
                                every function the game imports, empty; the system's libraries are used at run time
  engine/licenses/              the licence texts of what is bundled
"""
import os, sys, re, json, shutil, subprocess, argparse, time, hashlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import hgbuild

def log(*a): print(*a, flush=True)
def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace', **kw)
    if r.returncode != 0: raise SystemExit('failed: %s\n%s%s' % (' '.join(cmd), r.stdout, r.stderr))
    return r.stdout
def du(p):
    t = 0
    for dp, dn, fn in os.walk(p):
        for f in fn: t += os.lstat(os.path.join(dp, f)).st_size   # (the toolchain's sysroot has dangling links: dev/fd)
    return t

def check_rules():
    """hgbuild's Linux commands must match `make -n -B` (flag sets per object, link libraries)."""
    out = run(['make', '-n', '-B'], cwd=os.path.join(REPO, 'recomp'))
    make = {}; link = None
    for line in out.splitlines():
        line = line.replace('@mkdir -p obj-linux;', '').replace('mkdir -p obj-linux;', '').strip()
        toks = line.split()
        if not toks or toks[0] not in ('cc', 'gcc'): continue
        if '-c' in toks: make[os.path.basename(toks[toks.index('-o') + 1])] = toks
        elif '-o' in toks and os.path.basename(toks[toks.index('-o') + 1]) == 'hgboot': link = toks
    mine = {os.path.basename(o): c for o, c in hgbuild.plan(os.path.join(REPO, 'recomp'), os.path.join(REPO, 'recomp', 'gen'), 'obj', 'SDLINC', hgbuild.LINUX_SDL)}
    if set(make) != set(mine): raise SystemExit('object lists differ: make %s, hgbuild %s' % (sorted(set(make) - set(mine)), sorted(set(mine) - set(make))))
    flags = lambda t: sorted(x for x in t if x.startswith('-') and not x.startswith('-I') and x not in ('-c', '-o'))
    for o in make:
        if flags(make[o]) != flags(mine[o]): raise SystemExit('flags differ for %s:\n make    %s\n hgbuild %s' % (o, flags(make[o]), flags(mine[o])))
    libs = sorted(x for x in link if x.startswith('-l') or x == '-rdynamic'); want = sorted(x for x in hgbuild.LINUX_LIBS if x != '-ldl')
    if [x for x in libs if x != '-ldl'] != want: raise SystemExit('link libraries differ: make %s, hgbuild %s' % (libs, hgbuild.LINUX_LIBS))
    log('build rules: hgbuild.py --target linux matches the Makefile (%d objects)' % len(make))

GCC_DROP_BIN = ('c++', 'g++', 'gdb', 'gcov', 'gprof', 'lto-dump')
def stage_gcc(src, dst):
    """the Bootlin toolchain without what a C build never runs"""
    def ignore(d, names):
        out = set()
        rel = os.path.relpath(d, src)
        for n in names:
            if rel == 'bin' and any(k in n for k in GCC_DROP_BIN): out.add(n)
            if rel == 'share' and n in ('locale', 'gdb', 'doc', 'man', 'info'): out.add(n)
            if n in ('cc1plus', 'g++-mapper-server', 'debug-root', 'gdb'): out.add(n)
            if rel.endswith(os.path.join('x86_64-buildroot-linux-gnu', 'include')) and n == 'c++': out.add(n)
            if rel.endswith(os.path.join('sysroot', 'usr', 'share')) and n in ('locale', 'i18n', 'zoneinfo'): out.add(n)
        return out
    shutil.copytree(src, dst, symlinks=True, ignore=ignore)
    gcc = os.path.join(dst, 'bin', 'x86_64-buildroot-linux-gnu-gcc')
    ver = run([gcc, '-dumpversion']).strip()
    log('gcc %s (Bootlin): %.1f MB (from %.1f MB)' % (ver, du(dst) / 1e6, du(src) / 1e6))
    return ver, gcc

def stage_link(dst, gcc):
    """link stubs: every SDL_* and vk* function the development build imports, empty."""
    os.makedirs(dst)
    out = run(['nm', '-D', '--undefined-only', os.path.join(REPO, 'recomp', 'hgboot')])
    syms = sorted({l.split()[-1].split('@')[0] for l in out.splitlines() if l.split() and l.split()[-1].startswith(('SDL_', 'vk'))})
    for lib, pre, soname in (('libSDL2.so', 'SDL_', 'libSDL2-2.0.so.0'), ('libvulkan.so', 'vk', 'libvulkan.so.1')):
        c = os.path.join(dst, lib + '.c')
        with open(c, 'w') as f:
            f.write('/* link stub: the names only; the system library (%s) is loaded at run time */\n' % soname)
            for s in syms:
                if s.startswith(pre): f.write('void %s(void) {}\n' % s)
        run([gcc, '-shared', '-fPIC', '-fno-builtin', '-w', '-Wl,-soname,' + soname, '-o', os.path.join(dst, lib), c])
        os.remove(c)
    log('link stubs: %d SDL2 and %d Vulkan functions' % (sum(s.startswith('SDL_') for s in syms), sum(s.startswith('vk') for s in syms)))

def tar_dir(src, tp, base):   # .tar.gz: keeps the toolchain's symbolic links (lib64 -> lib) and the permissions
    import tarfile
    def norm(ti):
        ti.uid = ti.gid = 0; ti.uname = ti.gname = ''; return ti
    with tarfile.open(tp, 'w:gz', compresslevel=6) as t: t.add(src, arcname=os.path.relpath(src, base), filter=norm)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gcc', required=True); ap.add_argument('--gcc-licenses', required=True); ap.add_argument('--textures', required=True)
    ap.add_argument('--out', default=os.path.join(os.path.dirname(REPO), 'scratch', 'release-linux'))
    ap.add_argument('--version', default='0.1'); ap.add_argument('--no-zip', action='store_true')   # (no archive)
    ap.add_argument('--skip-rule-check', action='store_true')
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    if (out + os.sep).startswith(REPO + os.sep): raise SystemExit('the release must be built outside the repository')
    t0 = time.time()
    name = 'HauntingGround-Recomp-%s' % a.version; S = os.path.join(out, name); E = os.path.join(S, 'engine')
    if os.path.exists(S): shutil.rmtree(S)
    os.makedirs(E)
    if not a.skip_rule_check: check_rules()
    # sources: tracked files only (never anything from the disc)
    tracked = run(['git', '-C', REPO, 'ls-files']).splitlines()
    take = [f for f in tracked if f.startswith(('recomp/', 'data/')) and not f.endswith(('.bat',))] + ['LICENSE']
    for dp, dn, fn in os.walk(HERE):
        dn[:] = [d for d in dn if d != '__pycache__']
        take += [os.path.relpath(os.path.join(dp, f), REPO) for f in fn]
    for f in take:
        d = os.path.join(E, 'src', f); os.makedirs(os.path.dirname(d), exist_ok=True); shutil.copy2(os.path.join(REPO, f), d)
    pre = os.path.join(E, 'src', 'prebuilt'); os.makedirs(pre)
    comp = os.path.join(REPO, 'recomp', 'vk', 'gs_sprite.comp')
    for vn, defs in (('gs_sprite_spv', []), ('gs_batch_spv', ['-DBATCH']), ('gs_batchc_spv', ['-DBATCH', '-DCOPIES'])):
        run(['glslangValidator', '-V', '--target-env', 'vulkan1.1'] + defs + ['--vn', vn, '-o', os.path.join(pre, vn + '.h'), comp])
    fsr = os.path.join(REPO, 'recomp', 'vk', 'fsr.comp'); fsri = '-I' + os.path.join(REPO, 'recomp', 'third_party', 'fsr1')   # FSR 1 for the window
    for vn, d in (('gs_fsr_easu_spv', '-DEASU'), ('gs_fsr_rcas_spv', '-DRCAS')):
        run(['glslangValidator', '-V', '--target-env', 'vulkan1.1', d, fsri, '--vn', vn, '-o', os.path.join(pre, vn + '.h'), fsr])
    commit = run(['git', '-C', REPO, 'rev-parse', '--short', 'HEAD']).strip()
    dirty = run(['git', '-C', REPO, 'status', '--porcelain', '--untracked-files=no']).strip()
    open(os.path.join(E, 'version.txt'), 'w').write(a.version + '\n')
    open(os.path.join(E, 'src', 'COMMIT.txt'), 'w').write('%s%s\n' % (commit, ' (with uncommitted changes)' if dirty else ''))
    log('sources: %d files, commit %s%s' % (len(take), commit, ' (dirty)' if dirty else ''))
    # compiler, headers, link stubs
    gver, gcc = stage_gcc(os.path.abspath(a.gcc), os.path.join(E, 'gcc'))
    I = os.path.join(E, 'include'); os.makedirs(I)
    for h in ('SDL2', 'vulkan', 'vk_video'): shutil.copytree(os.path.join('/usr/include', h), os.path.join(I, h))
    stage_link(os.path.join(E, 'link'), gcc)
    # licences
    Lc = os.path.join(E, 'licenses'); os.makedirs(Lc)
    shutil.copytree(os.path.abspath(a.gcc_licenses), os.path.join(Lc, 'gcc-toolchain'))
    for name_, cands in (('SDL2-LICENSE.txt', ('/usr/share/licenses/sdl2-compat/LICENSE.txt', '/usr/share/licenses/sdl2/LICENSE.txt')),
                         ('Vulkan-Headers-LICENSE.txt', ('/usr/share/licenses/vulkan-headers/MIT.txt', '/usr/share/licenses/vulkan-headers/LICENSE.md', '/usr/share/licenses/vulkan-headers/LICENSE.txt'))):
        for c in cands:
            if os.path.exists(c): shutil.copy2(c, os.path.join(Lc, name_)); break
        else: raise SystemExit('no licence text for ' + name_ + ' (looked in ' + ', '.join(cands) + ')')
    # the launcher, documents, textures
    shutil.copy2(os.path.join(HERE, 'HauntingGround.sh'), os.path.join(S, 'HauntingGround.sh')); os.chmod(os.path.join(S, 'HauntingGround.sh'), 0o755)
    shutil.copy2(os.path.join(REPO, 'README.md'), os.path.join(S, 'README.txt'))
    shutil.copy2(os.path.join(REPO, 'LICENSE'), os.path.join(S, 'LICENSE.txt'))
    for f in ('docs/known-issues.md', 'docs/controller.md', 'docs/options-menu.md', 'docs/release/crash-reports.md'):
        d = os.path.join(S, f); os.makedirs(os.path.dirname(d), exist_ok=True); shutil.copy2(os.path.join(REPO, f), d)
    notes = open(os.path.join(HERE, 'THIRD_PARTY_NOTICES_linux.md'), encoding='utf-8').read().replace('<!-- GCC -->', gver)
    open(os.path.join(S, 'THIRD_PARTY_NOTICES.txt'), 'w', encoding='utf-8').write(notes)
    T = os.path.join(S, 'textures'); os.makedirs(T); n = 0
    for f in sorted(os.listdir(a.textures)):
        if re.fullmatch(r'[0-9a-f]{8}_[0-9a-f]{8}(_c\d+)?\.(dds|png)', f): shutil.copy2(os.path.join(a.textures, f), os.path.join(T, f)); n += 1
    log('textures: %d files, %.1f MB' % (n, du(T) / 1e6))
    json.dump({'release': a.version, 'commit': commit, 'gcc': gver, 'toolchain': os.path.basename(os.path.abspath(a.gcc)), 'textures': n}, open(os.path.join(E, 'bundle.json'), 'w'), indent=1)
    for d in ('gcc', 'include', 'link', 'src', 'licenses'): log('  engine/%-9s %7.1f MB' % (d, du(os.path.join(E, d)) / 1e6))
    log('release folder: %s, %.1f MB' % (S, du(S) / 1e6))
    if not a.no_zip:
        zp = os.path.join(out, name + '-linux-x64.tar.gz'); tar_dir(S, zp, out)
        h = hashlib.sha256()
        with open(zp, 'rb') as f:
            for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
        log('archive: %s, %.1f MB, sha256 %s' % (zp, os.path.getsize(zp) / 1e6, h.hexdigest()))
    log('done in %.0f s' % (time.time() - t0))

if __name__ == '__main__': main()
