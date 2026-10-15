#!/usr/bin/env python3
"""First-start build of the release (run by HauntingGround.exe with the bundled Python; also usable by hand).

    setup.py --root ROOT [--iso DISC.iso] [--jobs N]

ROOT is the release folder (engine/ holds the sources, Python and the compiler). Steps:
  1. the game files: ROOT/game/ must hold SLUS_210.75, DATA.CVM and MODULES/ exactly as on the NTSC-U disc
     (SHA-256 of every file, tools/release/disc.json). If they are missing or wrong, --iso is needed: the disc image is
     opened read-only, checked (ISO 9660, SYSTEM.CNF, region, every file's hash) and only those files are copied out.
     Without --iso the script stops with "@need-iso" (exit status 3) so the launcher can ask for the image.
  2. translate: recomp/bulk2.py writes the C code into ROOT/build/gen
  3. compile and link with the bundled gcc (hgbuild.py, all cores) into ROOT/bin/hgboot.exe, with SDL2.dll,
     libwinpthread-1.dll and vulkan-1.dll next to it
  4. ROOT/bin/build.txt records the release; ROOT/build is deleted.
Lines for the launcher: "@stage TEXT", "@progress PERCENT", "@error TEXT", "@need-iso", "@done". Everything else is log.
Nothing here writes to the disc image, and nothing leaves ROOT."""
import os, sys, json, struct, hashlib, shutil, subprocess, argparse, time, threading, glob

HERE = os.path.dirname(os.path.abspath(__file__))
SECTOR = 2048
REGIONS = {'SLES': 'the European (PAL) version', 'SLPM': 'the Japanese version (Demento)', 'SLPS': 'a Japanese version',
           'SLKA': 'the Korean version', 'SCES': 'a PAL version', 'SLAJ': 'an Asian version'}

def say(*a): print(*a, flush=True)
def stage(t): say('@stage ' + t)
def progress(p): say('@progress %d' % max(0, min(100, int(p))))
class Fail(Exception): pass

# ------------------------------------------------------------------ disc image (read-only)
def iso_files(f, size):
    def rd(lba, n):
        f.seek(lba * SECTOR); return f.read(n)
    pvd = rd(16, SECTOR)
    if len(pvd) < SECTOR or pvd[1:6] != b'CD001' or pvd[0] != 1:
        raise Fail('This file is not a DVD disc image (.iso): no ISO 9660 volume found. Please choose the .iso you dumped '
                   'from your Haunting Ground disc. Compressed images (.chd, .cso, .zip, .7z) need to be turned back into '
                   'a plain .iso first.')
    root = pvd[156:156 + 34]
    out = {}
    def walk(lba, sz, prefix, depth):
        if depth > 8: return
        data = rd(lba, sz); off = 0
        while off < len(data):
            l = data[off]
            if l == 0: off = (off // SECTOR + 1) * SECTOR; continue
            rec = data[off:off + l]
            if len(rec) < 34: break
            elba, esz = struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0]
            flags = rec[25]; nl = rec[32]; name = rec[33:33 + nl]
            if name not in (b'\x00', b'\x01'):
                nm = name.decode('ascii', 'replace').split(';')[0]
                out[(prefix + nm).upper()] = (elba, esz, bool(flags & 2))
                if flags & 2: walk(elba, esz, prefix + nm + '/', depth + 1)
            off += l
    walk(struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0], '', 0)
    for p, (lba, sz, d) in out.items():
        if not d and lba * SECTOR + sz > size:
            raise Fail('The disc image is incomplete (it is %.2f GB, but its files reach further). The dump or the copy was '
                       'cut short: please dump the disc again.' % (size / 1e9))
    return out

def copy_hashed(f, lba, size, dst, cb=None):
    h = hashlib.sha256(); f.seek(lba * SECTOR); left = size
    with open(dst, 'wb') as o:
        while left:
            b = f.read(min(left, 1 << 22))
            if not b: raise Fail('Reading the disc image failed (unexpected end of file).')
            h.update(b); o.write(b); left -= len(b)
            if cb: cb(size - left)
    return h.hexdigest()

def file_hash(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while True:
            b = f.read(1 << 22)
            if not b: break
            h.update(b)
    return h.hexdigest()

def game_ok(game, disc, quick):
    """True when ROOT/game holds every file of disc.json (size, and the hash unless quick)."""
    for rel, (size, sha) in disc['files'].items():
        p = os.path.join(game, rel)
        if not os.path.isfile(p) or os.path.getsize(p) != size: return False
        if not quick and file_hash(p) != sha: return False
    return True

def extract(iso, game, disc):
    stage('Checking the disc image')
    say('disc image: ' + iso)
    if not os.path.isfile(iso): raise Fail('The disc image was not found: ' + iso)
    low = iso.lower()
    for ext in ('.zip', '.7z', '.rar', '.chd', '.cso', '.gz'):
        if low.endswith(ext): raise Fail('This is a compressed file (%s). Please extract or convert it to a plain .iso first, then choose the .iso.' % ext)
    size = os.path.getsize(iso)
    say('size: %d bytes (%.2f GB)' % (size, size / 1e9))
    with open(iso, 'rb') as f:                       # read-only; the image is never written or copied whole
        files = iso_files(f, size)
        if 'SYSTEM.CNF' not in files: raise Fail('This disc image has no SYSTEM.CNF: it is not a PlayStation 2 game disc.')
        lba, sz, _ = files['SYSTEM.CNF']; f.seek(lba * SECTOR); cnf = f.read(min(sz, 4096)).decode('ascii', 'replace')
        say('SYSTEM.CNF: ' + ' | '.join(l.strip() for l in cnf.splitlines() if l.strip()))
        boot = ''
        for line in cnf.splitlines():
            if line.strip().upper().startswith('BOOT2'):
                boot = line.split('\\')[-1].split(';')[0].strip().upper()
        if not boot: raise Fail('This disc image is not a PlayStation 2 game (no BOOT2 line in SYSTEM.CNF).')
        if boot != disc['boot']:
            region = REGIONS.get(boot[:4])
            if region:
                raise Fail('This disc image is %s (%s). Version 0.1 supports only the North American (NTSC-U) disc, '
                           'SLUS-21075.' % (region, boot))
            raise Fail('This disc image is not Haunting Ground NTSC-U (it boots %s; SLUS_210.75 is needed).' % boot)
        missing = [r for r in disc['files'] if r.upper() not in files]
        if missing: raise Fail('The disc image lacks %s: it is damaged or modified. Please dump the disc again.' % ', '.join(missing))
        total = sum(s for s, _ in disc['files'].values())
        os.makedirs(os.path.join(game, 'MODULES'), exist_ok=True)
        done = 0; t0 = time.time(); bad = []
        stage('Copying the game files from your disc image (%.1f GB)' % (total / 1e9))
        for rel, (want_size, want_sha) in disc['files'].items():
            lba, sz, _ = files[rel.upper()]
            if sz != want_size:
                bad.append(rel)
                if rel == disc['elf']: break
                continue
            dst = os.path.join(game, rel); tmp = dst + '.part'
            sha = copy_hashed(f, lba, sz, tmp, lambda n, d0=done: progress(2 + 12 * (d0 + n) / total))
            if sha != want_sha:
                bad.append(rel); os.remove(tmp)
                if rel == disc['elf']: break     # the wrong game program: no point copying 1.6 GB more
                continue
            os.replace(tmp, dst); done += sz
            say('  %-22s %12d bytes  sha256 ok' % (rel, sz))
        if bad:
            if disc['elf'] in bad:
                raise Fail('This is not the supported release of Haunting Ground: its game program differs from the NTSC-U '
                           'v1.01 disc (SLUS-21075). Other revisions and modified (patched, translated) images are not '
                           'supported in 0.1.')
            raise Fail('Your disc image is damaged (a bad dump): these files do not match the original disc: %s. Please '
                       'dump the disc again (and check it against redump.org if you can).' % ', '.join(bad))
    say('game files copied and verified in %.1f s' % (time.time() - t0))

# ------------------------------------------------------------------ build
def run_logged(cmd, cwd, env, on_line=None):
    say('$ ' + ' '.join('"%s"' % c if ' ' in c else c for c in cmd))
    p = subprocess.Popen(cmd, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace',
                         creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
    for line in p.stdout:
        line = line.rstrip()
        if on_line and on_line(line): continue
        say(line)
    return p.wait()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True); ap.add_argument('--iso'); ap.add_argument('--jobs', type=int, default=0)
    ap.add_argument('--keep-build', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root); eng = os.path.join(root, 'engine'); src = os.path.join(eng, 'src')
    recomp = os.path.join(src, 'recomp'); tc = os.path.join(eng, 'ucrt64')
    game = os.path.join(root, 'game'); build = os.path.join(root, 'build'); bindir = os.path.join(root, 'bin')
    disc = json.load(open(os.path.join(HERE, 'disc.json')))
    version = open(os.path.join(eng, 'version.txt')).read().strip()
    t_all = time.time()
    say('Haunting Ground Recomp %s setup, %s, Python %s, %d CPUs' % (version, time.strftime('%Y-%m-%d %H:%M:%S'), sys.version.split()[0], os.cpu_count() or 0))
    try:
        progress(0)
        if not a.iso and not game_ok(game, disc, quick=True):
            say('@need-iso'); return 3
        free = shutil.disk_usage(root).free
        need = 0.8e9 + (0 if game_ok(game, disc, quick=True) else sum(s for s, _ in disc['files'].values()))
        if free < need: raise Fail('Not enough free disk space on this drive: %.1f GB free, about %.1f GB needed.' % (free / 1e9, need / 1e9))
        if a.iso: extract(a.iso, game, disc)
        else:
            stage('Checking the game files'); progress(2)
            if not game_ok(game, disc, quick=False):
                for p in glob.glob(os.path.join(game, '*.part')): os.remove(p)
                say('@need-iso'); return 3
            say('game files present and verified')
        progress(15)
        # 2. translate
        stage('Translating the game program to C')
        gen = os.path.join(build, 'gen'); obj = os.path.join(build, 'obj')
        shutil.rmtree(build, ignore_errors=True); os.makedirs(gen); os.makedirs(obj)
        env = dict(os.environ); env['HG_GAME_DIR'] = game
        nparts = 16
        stop = threading.Event()
        def watch():     # bulk2 is quiet while it works: count the parts it has written
            while not stop.wait(0.5):
                n = len(glob.glob(os.path.join(gen, 'part*.c'))) + len(glob.glob(os.path.join(gen, 'vu1_aot_*.c')))
                progress(15 + 15 * min(n, nparts + 20) / (nparts + 20))
        th = threading.Thread(target=watch, daemon=True); th.start()
        t0 = time.time()
        # bulk2.py imports its neighbours (rec2, r5900, vu1gen); the embedded Python does not put the script's folder on
        # sys.path by itself (python314._pth), so the wrapper does
        boot = 'import sys, runpy; sys.path.insert(0, sys.argv[1]); sys.argv = sys.argv[2:]; runpy.run_path(sys.argv[0], run_name="__main__")'
        r = run_logged([sys.executable, '-u', '-c', boot, recomp, os.path.join(recomp, 'bulk2.py'), gen, str(nparts)], recomp, env)
        stop.set()
        if r != 0 or not os.path.exists(os.path.join(gen, 'table.c')): raise Fail('Translating the game program failed (status %d).' % r)
        for h in glob.glob(os.path.join(src, 'prebuilt', 'gs_*_spv.h')): shutil.copy(h, gen)
        say('translated in %.1f s' % (time.time() - t0)); progress(30)
        # 3. compile
        stage('Compiling (this takes a few minutes; all CPU cores are used)')
        gcc = os.path.join(tc, 'bin', 'gcc.exe')
        if not os.path.exists(gcc): raise Fail('The bundled compiler is missing (%s). Please unzip the release again.' % gcc)
        env = dict(os.environ); env['PATH'] = os.path.join(tc, 'bin') + os.pathsep + env.get('PATH', '')
        for k in ('C_INCLUDE_PATH', 'CPATH', 'LIBRARY_PATH', 'GCC_EXEC_PREFIX', 'COMPILER_PATH'): env.pop(k, None)
        os.makedirs(bindir, exist_ok=True)
        exe_tmp = os.path.join(build, 'hgboot.exe')
        def on_line(line):
            if line.startswith('@progress '):
                d, t = line.split()[1:3]; progress(30 + 68 * int(d) / int(t)); return True
            return False
        t0 = time.time()
        cmd = [sys.executable, '-u', os.path.join(HERE, 'hgbuild.py'), '--src', recomp, '--gen', gen, '--obj', obj, '--out', exe_tmp, '--cc', gcc]
        if a.jobs: cmd += ['--jobs', str(a.jobs)]
        r = run_logged(cmd, build, env, on_line)
        if r != 0 or not os.path.exists(exe_tmp): raise Fail('Compiling failed (status %d). The log has the compiler\'s messages.' % r)
        say('compiled and linked in %.1f s' % (time.time() - t0))
        # 4. install
        stage('Finishing')
        for d in ('SDL2.dll', 'libwinpthread-1.dll', 'vulkan-1.dll'): shutil.copy(os.path.join(tc, 'bin', d), bindir)
        shutil.move(exe_tmp, os.path.join(bindir, 'hgboot.exe'))
        with open(os.path.join(bindir, 'build.txt'), 'w', newline='\n') as f:
            f.write('release=%s\nbuilt=%s\nbuild_seconds=%.0f\n' % (version, time.strftime('%Y-%m-%d %H:%M:%S'), time.time() - t_all))
        if not a.keep_build: shutil.rmtree(build, ignore_errors=True)
        progress(100)
        say('setup finished in %.1f s' % (time.time() - t_all))
        say('@done'); return 0
    except Fail as e:
        say('@error ' + str(e)); return 1
    except OSError as e:
        say('@error A file operation failed: %s' % e); return 1

if __name__ == '__main__': sys.exit(main())
