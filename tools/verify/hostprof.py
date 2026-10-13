"""Names the samples of an HG_HOSTPROF=1 run (Windows build).
usage: hostprof.py hostprof.txt hgboot.exe [top=25] [--lines N]
Per thread: share of samples by host function (from `nm`), samples that were in a DLL (charged to the calling exe
function) shown as "(via DLL)". --lines N also names the N hottest addresses with addr2line (file:line)."""
import sys, subprocess, bisect, collections, re, shutil
args = [a for a in sys.argv[1:] if not a.startswith('--')]
prof, exe = args[0], args[1]; top = int(args[2]) if len(args) > 2 else 25
nlines = int(sys.argv[sys.argv.index('--lines') + 1]) if '--lines' in sys.argv else 0
tool = lambda t: shutil.which(t) or shutil.which('C:/msys64/ucrt64/bin/' + t + '.exe') or t
base = int(re.search(r'ImageBase\s+([0-9a-fA-F]+)', subprocess.run([tool('objdump'), '-p', exe], capture_output=True, text=True).stdout).group(1), 16)
syms = []
for l in subprocess.run([tool('nm'), '-n', '--defined-only', exe], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3 and p[1] in 'tT': syms.append((int(p[0], 16) - base, p[2]))
syms.sort(); addrs = [a for a, _ in syms]
def name(rva):
    i = bisect.bisect_right(addrs, rva) - 1
    return syms[i][1] if i >= 0 else '?'
by = collections.defaultdict(collections.Counter); raw = collections.defaultdict(collections.Counter); tot = {}
for l in open(prof):
    if l.startswith('#'):
        p = l.split(); tot[p[1]] = int(p[3]); continue
    th, k, n = l.split(); k = int(k, 16); n = int(n)
    nm = name(k & 0x7FFFFFFF) + (' (via DLL)' if k & 0x80000000 else '')
    by[th][nm] += n; raw[th][k & 0x7FFFFFFF] += n
for th, c in by.items():
    t = tot.get(th) or sum(c.values())
    print(f'== {th}: {t} samples ({t / 1000:.1f} s)')
    for nm, n in c.most_common(top): print(f'  {100 * n / t:5.1f}%  {nm}')
    if nlines:
        hot = raw[th].most_common(nlines)
        out = subprocess.run([tool('addr2line'), '-e', exe, '-f', '-C'] + [hex(base + a) for a, _ in hot], capture_output=True, text=True).stdout.splitlines()
        for (a, n), (fn, fl) in zip(hot, zip(out[0::2], out[1::2])): print(f'    {100 * n / t:5.1f}%  {fn}  {fl}')
