#!/usr/bin/env python3
"""Build the release folder (and zip) of Haunting Ground Recomp: no game code, a launcher that builds the game on the
player's PC from their own disc image with a bundled compiler and Python; from 0.2 also the HD texture pack
(--textures; the user's decision, 2026-10-25; the repository never holds it). tools/release/mkrelease_linux.py makes
the Linux release.

    python tools/release/mkrelease.py [--out DIR] [--version 0.2] [--msys C:/msys64] [--textures DIR] [--no-zip]

Run it on the development PC with the python.org Python whose copy is to be bundled (3.14 for 0.1), MSYS2 UCRT64 with
the packages the README's developer build lists, and a finished development build in recomp/ (recomp/gen and
recomp/obj are read to find which headers and libraries the build uses; nothing from them is copied). Output:
DIR/HauntingGround-Recomp-VERSION/ and DIR/HauntingGround-Recomp-VERSION-win64.zip. DIR must be outside the repository.

What goes in (see tools/release/README.md):
  HauntingGround.exe            the launcher (tools/release/launcher.c), built here
  textures/                     the HD replacements (<id>.dds BC7, a few <id>.png), from --textures
  README.txt, LICENSE.txt, THIRD_PARTY_NOTICES.txt
  engine/version.txt
  engine/src/                   the repository's tracked sources the build needs (recomp/, data/, tools/release/),
                                plus prebuilt/gs_*_spv.h (our own shader, compiled here with glslangValidator)
  engine/ucrt64/                gcc, binutils, the MinGW-w64 headers and libraries, SDL2, Vulkan headers and loader:
                                only the files the build uses (found with gcc -M and ld --trace) and their DLLs
  engine/python/                python.exe, its DLL, the standard library (zip) without tests, Tk, IDLE and pip
  engine/licenses/              the licence texts of everything bundled
"""
import os, sys, re, json, shutil, subprocess, argparse, zipfile, time, glob, hashlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import hgbuild

def log(*a): print(*a, flush=True)
def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace', **kw)
    if r.returncode != 0: raise SystemExit('failed: %s\n%s%s' % (' '.join(cmd), r.stdout, r.stderr))
    return r.stdout
def norm(p): return os.path.normcase(os.path.normpath(p))
def rel_under(path, base):
    """path relative to base, keeping the file name's case; None when outside."""
    p = os.path.normpath(os.path.abspath(path)); b = os.path.normpath(base)
    if not norm(p).startswith(norm(b) + os.sep): return None
    return p[len(b) + 1:]
def du(p):
    t = 0
    for dp, dn, fn in os.walk(p):
        for f in fn: t += os.path.getsize(os.path.join(dp, f))
    return t

# ------------------------------------------------------------------ checks
def check_rules(msys):
    """hgbuild's commands must match `make -n -B` (flag sets per object, link libraries)."""
    bash = os.path.join(msys, 'usr', 'bin', 'bash.exe')
    env = dict(os.environ, MSYSTEM='UCRT64', CHERE_INVOKING='1')
    out = run([bash, '-lc', 'cd "%s" && make -n -B' % os.path.join(REPO, 'recomp').replace('\\', '/')], env=env)
    make = {}
    link = None
    for line in out.splitlines():
        line = line.replace('@mkdir -p obj;', '').replace('mkdir -p obj;', '').strip()
        toks = line.split()
        if not toks or toks[0] not in ('cc', 'gcc'): continue
        if '-c' in toks:
            o = toks[toks.index('-o') + 1]; make[os.path.basename(o)] = toks
        elif '-o' in toks and toks[toks.index('-o') + 1].endswith('.exe'): link = toks
    mine = {os.path.basename(o): c for o, c in hgbuild.plan(os.path.join(REPO, 'recomp'), os.path.join(REPO, 'recomp', 'gen'), 'obj', 'SDLINC')}
    if set(make) != set(mine): raise SystemExit('object lists differ: make %s, hgbuild %s' % (sorted(set(make) - set(mine)), sorted(set(mine) - set(make))))
    def flags(t):   # compare flag sets; the include paths differ only in where they point
        return sorted(x for x in t if x.startswith('-') and not x.startswith('-I') and x not in ('-c', '-o'))
    for o in make:
        if flags(make[o]) != flags(mine[o]): raise SystemExit('flags differ for %s:\n make    %s\n hgbuild %s' % (o, flags(make[o]), flags(mine[o])))
        if any(x.startswith('-I') and 'SDL2' in x for x in make[o]) != any(x == '-ISDLINC' for x in mine[o]):
            raise SystemExit('SDL include differs for ' + o)
    libs = [x for x in link if x.startswith('-l')]
    if libs != hgbuild.LIBS: raise SystemExit('link libraries differ: make %s, hgbuild %s' % (libs, hgbuild.LIBS))
    objs = [os.path.basename(x) for x in link if x.endswith('.o')]
    if objs != [os.path.basename(o) for o, _ in hgbuild.plan(os.path.join(REPO, 'recomp'), os.path.join(REPO, 'recomp', 'gen'), 'obj', 'SDLINC')]: raise SystemExit('link order differs')
    log('build rules: hgbuild.py matches the Makefile (%d objects)' % len(make))

# ------------------------------------------------------------------ toolchain
def stage_toolchain(msys, dst):
    U = os.path.join(msys, 'ucrt64'); gcc = os.path.join(U, 'bin', 'gcc.exe')
    env = dict(os.environ); env['PATH'] = os.path.join(U, 'bin') + os.pathsep + env['PATH']
    ver = run([gcc, '-dumpversion'], env=env).strip(); mach = run([gcc, '-dumpmachine'], env=env).strip()
    need = set()
    # programs
    for p in ('bin/gcc.exe', 'bin/as.exe', 'bin/ld.exe', 'bin/SDL2.dll', 'bin/vulkan-1.dll', 'bin/libwinpthread-1.dll',
              f'lib/gcc/{mach}/{ver}/cc1.exe', f'lib/gcc/{mach}/{ver}/collect2.exe', f'lib/gcc/{mach}/{ver}/lto-wrapper.exe',
              f'lib/gcc/{mach}/{ver}/liblto_plugin.dll'):
        if not os.path.exists(os.path.join(U, p)): raise SystemExit('missing ' + p)
        need.add(os.path.normpath(p))
    # headers: gcc -M over every translation unit kind, with the build's own flags
    gen = os.path.join(REPO, 'recomp', 'gen'); tmp = os.path.join(dst + '.tmp'); os.makedirs(tmp, exist_ok=True)
    jobs = hgbuild.plan(os.path.join(REPO, 'recomp'), gen, tmp, os.path.join(U, 'include', 'SDL2'))
    seen_kind = set()
    for o, c in jobs:
        kind = re.sub(r'\d+', '', os.path.basename(o))
        if kind in seen_kind and kind.startswith(('part', 'vu_aot')): continue
        seen_kind.add(kind)
        cc = [x for x in c]; i = cc.index('-c'); src = cc[i + 1]; cc = cc[:i] + ['-M', '-MF', os.path.join(tmp, 'deps.d'), src]
        run([gcc] + cc, env=env)
        deps = open(os.path.join(tmp, 'deps.d')).read().replace('\\\n', ' ').split(':', 1)[1].split()
        for d in deps:
            r = rel_under(d.replace('\\ ', ' '), U)
            if r: need.add(r)
    # libraries and startup objects: the dev build's objects linked again with --trace
    objs = [o.replace(tmp, os.path.join(REPO, 'recomp', 'obj')) for o, _ in jobs]
    out = run([gcc, '-o', os.path.join(tmp, 't.exe')] + objs + hgbuild.LIBS + ['-Wl,--trace'], env=env)
    for line in out.splitlines():
        line = re.sub(r'\(.*\)$', '', line.strip())
        if not line or not os.path.exists(line): continue
        r = rel_under(line, U)
        if r: need.add(r)
    # the DLLs the programs load
    od = os.path.join(U, 'bin', 'objdump.exe'); todo = [p for p in need if p.lower().endswith(('.exe', '.dll'))]
    have = {f.lower(): f for f in os.listdir(os.path.join(U, 'bin'))}
    while todo:
        p = todo.pop()
        for l in run([od, '-p', os.path.join(U, p)]).splitlines():
            if 'DLL Name:' in l:
                n = l.split(':', 1)[1].strip().lower()
                q = os.path.normpath(os.path.join('bin', have[n])) if n in have else None
                if q and q not in need: need.add(q); todo.append(q)
    need = sorted(need)
    for p in need:
        os.makedirs(os.path.dirname(os.path.join(dst, p)), exist_ok=True); shutil.copy2(os.path.join(U, p), os.path.join(dst, p))
    shutil.rmtree(tmp, ignore_errors=True)
    log('toolchain: %d files, %.1f MB (gcc %s)' % (len(need), du(dst) / 1e6, ver))
    return need

def package_info(msys, files):
    """{package: (version, [licence dirs])} for the MSYS2 packages that own the staged files."""
    bash = os.path.join(msys, 'usr', 'bin', 'bash.exe'); env = dict(os.environ, MSYSTEM='UCRT64', CHERE_INVOKING='1')
    owners = {}
    for line in run([bash, '-lc', 'pacman -Ql'], env=env).splitlines():
        pkg, _, path = line.partition(' ')
        if path.startswith('/ucrt64/') and not path.endswith('/'): owners[norm(path[len('/ucrt64/'):])] = pkg
    used = {}
    for f in files:
        pkg = owners.get(norm(f))
        if not pkg: raise SystemExit('no package owns ' + f)
        used.setdefault(pkg, []).append(f)
    info = {}
    for pkg in sorted(used):
        q = run([bash, '-lc', 'pacman -Qi ' + pkg], env=env)
        g = lambda k: re.search(r'^%s\s*:\s*(.*)$' % k, q, re.M).group(1).strip()
        info[pkg] = {'version': g('Version'), 'licenses': g('Licenses'), 'url': g('URL'), 'files': len(used[pkg]),
                     'base': g('Name').replace('mingw-w64-ucrt-x86_64-', '')}
    return info

# ------------------------------------------------------------------ Python
SKIP_LIB = {'test', 'tests', 'idlelib', 'tkinter', 'turtledemo', 'ensurepip', 'site-packages', 'lib2to3', 'pydoc_data',
            '__pycache__', 'venv', 'sqlite3', 'curses', 'msilib', 'dbm', 'xmlrpc', 'wsgiref', 'http', 'urllib', 'email',
            'xml', 'html', 'asyncio', 'multiprocessing', 'unittest', 'ctypes', 'distutils', 'turtle.py', 'pydoc.py',
            'doctest.py', 'mailbox.py', 'imaplib.py', 'smtplib.py', 'ftplib.py', 'poplib.py', 'nntplib.py', '_pyrepl',
            'sysconfigdata', 'profiling', 'compression', 'zoneinfo', 'tomllib', 'lib-dynload'}
def stage_python(dst):
    P = sys.base_prefix; ver = '%d%d' % sys.version_info[:2]
    if not os.path.exists(os.path.join(P, 'python%s.dll' % ver)): raise SystemExit('run mkrelease.py with a python.org Windows Python (%s)' % P)
    os.makedirs(dst, exist_ok=True)
    for f in ('python.exe', 'python%s.dll' % ver, 'vcruntime140.dll', 'vcruntime140_1.dll', 'LICENSE.txt'):
        shutil.copy2(os.path.join(P, f), dst)
    z = zipfile.ZipFile(os.path.join(dst, 'python%s.zip' % ver), 'w', zipfile.ZIP_DEFLATED, compresslevel=9)
    lib = os.path.join(P, 'Lib'); n = 0
    for dp, dn, fn in os.walk(lib):
        dn[:] = [d for d in dn if d not in SKIP_LIB]
        for f in fn:
            if f.endswith('.py') and f not in SKIP_LIB:
                z.write(os.path.join(dp, f), os.path.relpath(os.path.join(dp, f), lib).replace('\\', '/')); n += 1
    z.close()
    open(os.path.join(dst, 'python%s._pth' % ver), 'w').write('python%s.zip\n.\n' % ver)
    log('python: %s, %d stdlib modules, %.1f MB' % (sys.version.split()[0], n, du(dst) / 1e6))
    return sys.version.split()[0]

# ------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default='D:/ps2-work/scratch/release-0.1'); ap.add_argument('--version', default='0.1')
    ap.add_argument('--msys', default='C:/msys64'); ap.add_argument('--no-zip', action='store_true')
    ap.add_argument('--skip-rule-check', action='store_true'); ap.add_argument('--textures', default=None)
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    if norm(out).startswith(norm(REPO) + os.sep) or norm(out) == norm(REPO): raise SystemExit('the release must be built outside the repository')
    t0 = time.time()
    name = 'HauntingGround-Recomp-%s' % a.version; S = os.path.join(out, name); E = os.path.join(S, 'engine')
    if os.path.exists(S): shutil.rmtree(S)
    os.makedirs(E)
    if not a.skip_rule_check: check_rules(a.msys)
    # sources: tracked files only (never anything from the disc)
    tracked = run(['git', '-C', REPO, 'ls-files']).splitlines()
    take = [f for f in tracked if f.startswith(('recomp/', 'data/')) and not f.endswith(('.bat',))] + ['LICENSE']
    for dp, dn, fn in os.walk(HERE):   # the release tools themselves (tracked or not yet)
        dn[:] = [d for d in dn if d != '__pycache__']
        take += [os.path.relpath(os.path.join(dp, f), REPO).replace(os.sep, '/') for f in fn]
    for f in take:
        d = os.path.join(E, 'src', f); os.makedirs(os.path.dirname(d), exist_ok=True); shutil.copy2(os.path.join(REPO, f), d)
    pre = os.path.join(E, 'src', 'prebuilt'); os.makedirs(pre)
    gv = os.path.join(a.msys, 'ucrt64', 'bin', 'glslangValidator.exe'); comp = os.path.join(REPO, 'recomp', 'vk', 'gs_sprite.comp')
    for vn, defs in (('gs_sprite_spv', []), ('gs_batch_spv', ['-DBATCH']), ('gs_batchc_spv', ['-DBATCH', '-DCOPIES'])):
        run([gv, '-V', '--target-env', 'vulkan1.1'] + defs + ['--vn', vn, '-o', os.path.join(pre, vn + '.h'), comp])
    commit = run(['git', '-C', REPO, 'rev-parse', '--short', 'HEAD']).strip()
    dirty = run(['git', '-C', REPO, 'status', '--porcelain', '--untracked-files=no']).strip()
    open(os.path.join(E, 'version.txt'), 'w').write(a.version + '\n')
    open(os.path.join(E, 'src', 'COMMIT.txt'), 'w').write('%s%s\n' % (commit, ' (with uncommitted changes)' if dirty else ''))
    log('sources: %d files, commit %s%s' % (len(take), commit, ' (dirty)' if dirty else ''))
    # toolchain, Python
    files = stage_toolchain(a.msys, os.path.join(E, 'ucrt64'))
    pkgs = package_info(a.msys, files)
    pyver = stage_python(os.path.join(E, 'python'))
    # licences
    L = os.path.join(E, 'licenses')
    for pkg, i in pkgs.items():
        src = os.path.join(a.msys, 'ucrt64', 'share', 'licenses', i['base'])
        if os.path.isdir(src): shutil.copytree(src, os.path.join(L, i['base'])); continue
        # packages that install no licence folder: our notes plus the standard texts
        d = os.path.join(L, i['base']); os.makedirs(d)
        lic = os.path.join(a.msys, 'ucrt64', 'share', 'licenses', 'binutils')
        if i['base'] in ('gmp', 'mpfr', 'mpc'):
            shutil.copy2(os.path.join(HERE, 'licenses', 'gmp-mpfr-mpc.txt'), d)
            for f in ('COPYING3', 'COPYING3.LIB'): shutil.copy2(os.path.join(lic, f), d)
            doc = os.path.join(a.msys, 'ucrt64', 'share', 'doc', i['base'])
            for f in ('COPYING', 'COPYING.LESSER', 'AUTHORS'):
                if os.path.exists(os.path.join(doc, f)): shutil.copy2(os.path.join(doc, f), os.path.join(d, f + '.' + i['base']))
        elif os.path.exists(os.path.join(HERE, 'licenses', i['base'] + '.txt')): shutil.copy2(os.path.join(HERE, 'licenses', i['base'] + '.txt'), d)
        else: raise SystemExit('no licence text for %s (%s): add tools/release/licenses/%s.txt' % (pkg, i['licenses'], i['base']))
    os.makedirs(os.path.join(L, 'python'), exist_ok=True); shutil.copy2(os.path.join(sys.base_prefix, 'LICENSE.txt'), os.path.join(L, 'python', 'LICENSE.txt'))
    # the launcher
    U = os.path.join(a.msys, 'ucrt64'); env = dict(os.environ); env['PATH'] = os.path.join(U, 'bin') + os.pathsep + env['PATH']
    res = os.path.join(out, 'launcher.res')
    run([os.path.join(U, 'bin', 'windres.exe'), 'launcher.rc', '-O', 'coff', '-o', res], cwd=HERE, env=env)
    run([os.path.join(U, 'bin', 'gcc.exe'), '-O2', '-Wall', '-municode', '-mwindows', 'launcher.c', res, '-o', os.path.join(S, 'HauntingGround.exe'),
         '-lcomctl32', '-lcomdlg32', '-lshell32', '-static', '-s'], cwd=HERE, env=env)
    os.remove(res)
    # documents
    shutil.copy2(os.path.join(REPO, 'README.md'), os.path.join(S, 'README.txt'))
    shutil.copy2(os.path.join(REPO, 'LICENSE'), os.path.join(S, 'LICENSE.txt'))
    for f in ('docs/known-issues.md', 'docs/controller.md', 'docs/options-menu.md', 'docs/release/crash-reports.md'):   # the README links them
        d = os.path.join(S, f); os.makedirs(os.path.dirname(d), exist_ok=True); shutil.copy2(os.path.join(REPO, f), d)
    notes = open(os.path.join(HERE, 'THIRD_PARTY_NOTICES.md'), encoding='utf-8').read()
    rows = ['| %s | %s | %s | %d |' % (p.replace('mingw-w64-ucrt-x86_64-', ''), i['version'], i['licenses'], i['files']) for p, i in pkgs.items()]
    notes = notes.replace('<!-- PACKAGES -->', '| MSYS2 package (mingw-w64-ucrt-x86_64-...) | Version | Licence | Files bundled |\n|---|---|---|---|\n' + '\n'.join(rows))
    notes = notes.replace('<!-- PYTHON -->', pyver)
    open(os.path.join(S, 'THIRD_PARTY_NOTICES.txt'), 'w', encoding='utf-8', newline='\r\n').write(notes)
    ntex = 0
    if a.textures:
        T = os.path.join(S, 'textures'); os.makedirs(T)
        for f in sorted(os.listdir(a.textures)):
            if re.fullmatch(r'[0-9a-f]{8}_[0-9a-f]{8}(_c\d+)?\.(dds|png)', f): shutil.copy2(os.path.join(a.textures, f), os.path.join(T, f)); ntex += 1
        log('textures: %d files, %.1f MB' % (ntex, du(T) / 1e6))
    json.dump({'release': a.version, 'commit': commit, 'python': pyver, 'packages': pkgs, 'textures': ntex}, open(os.path.join(E, 'bundle.json'), 'w'), indent=1)
    # size report, zip
    for d in ('ucrt64', 'python', 'src', 'licenses'): log('  engine/%-9s %7.1f MB' % (d, du(os.path.join(E, d)) / 1e6))
    log('release folder: %s, %.1f MB' % (S, du(S) / 1e6))
    if not a.no_zip:
        zp = os.path.join(out, name + '-win64.zip')
        with zipfile.ZipFile(zp, 'w', zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as z:
            for dp, dn, fn in os.walk(S):
                for f in sorted(fn):
                    p = os.path.join(dp, f); z.write(p, os.path.relpath(p, out).replace('\\', '/'))
        log('zip: %s, %.1f MB, sha256 %s' % (zp, os.path.getsize(zp) / 1e6, hashlib.sha256(open(zp, 'rb').read()).hexdigest()))
    log('done in %.0f s' % (time.time() - t0))

if __name__ == '__main__': main()
