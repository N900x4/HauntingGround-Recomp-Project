#!/usr/bin/env python3
"""Smoothness and A/V report of one session's log (recomp/play.log, a window test's log).

usage: sessionreport.py LOG [--hitches N]

Reads the lines the runtime writes (recomp/host_sdl.c, "smoothness and A/V sync"):
  smooth: ...          every 10 s: pictures held exactly N refreshes (first-pixel-out times), N-1 / N+1 / more,
                       in-between pictures left out, pictures not ready at their tick, sound underruns and drops,
                       the A/V offset (+: the picture later than its sound)
  smooth minute M: ... per minute, written at exit
  smooth hitch: ...    every hitch (time, VBlank, stage, movie), written at exit
  smooth session: ...  the whole session
  movie: "NAME" starts at VBlank V / stopped at VBlank V
  av: VB V ...         once a second while a movie plays: the picture's and the sound's delay and the offset
and prints the summary, the per-minute table, the hitches (grouped when they come in bursts) and A/V per movie.
"""
import re
import sys


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    maxh = 200
    if '--hitches' in sys.argv:
        maxh = int(sys.argv[sys.argv.index('--hitches') + 1])
    text = open(path, encoding='utf-8', errors='replace').read()
    lines = text.splitlines()

    settings = next((l for l in lines if l.startswith('settings:')), None)
    if settings:
        print(settings[:200])
    for l in lines:
        if l.startswith('host: display') or l.startswith('host: audio open'):
            print(l)
    sess = [l for l in lines if l.startswith('smooth session:')]
    if sess:
        print()
        print(sess[-1])
    else:
        print('\n(no "smooth session" line: the session did not end normally, or the log is from before the smoothness log)')

    minutes = []
    for l in lines:
        m = re.match(r'smooth minute (\d+): (\d+) pictures, ok (\d+), short (\d+), long (\d+), longer (\d+), left out (\d+), not ready (\d+), '
                     r'underruns (\d+), drops (\d+), A/V ([-+\d]+) \(([-+\d]+)\.\.([-+\d]+)\)', l)
        if m:
            minutes.append([int(x) for x in m.groups()[:10]] + [m.group(11), m.group(12), m.group(13)])
    if minutes:
        print('\nper minute:')
        print('  min  pictures  held N %    N-1    N+1   more  left out  not ready  underruns  drops   A/V ms (min..max)')
        for r in minutes:
            mi, pics, ok, sh, l1, l2, sk, nr, un, dr, av, avmin, avmax = r
            pct = 100.0 * ok / pics if pics else 0.0
            print('  %3d  %8d  %7.2f  %5d  %5d  %5d  %8d  %9d  %9d  %5d   %s (%s..%s)' % (mi, pics, pct, sh, l1, l2, sk, nr, un, dr, av, avmin, avmax))
    else:
        tens = [l for l in lines if l.startswith('smooth: ')]
        if tens:
            print('\nevery 10 s (no per-minute table: the session did not end normally):')
            for l in tens:
                print('  ' + l[8:])

    hits = []
    for l in lines:
        m = re.match(r'smooth hitch: ([\d.]+) s VB (\d+) stage ([0-9a-f]+)( movie)?: (.*?) (-?\d+)$', l)
        if m:
            hits.append((float(m.group(1)), int(m.group(2)), m.group(3), bool(m.group(4)), m.group(5), int(m.group(6))))
    if hits:
        print('\nhitches: %d (held = refreshes a picture was held; not ready = no picture at its tick; underrun / drop = sound; left out = in-between pictures)' % len(hits))
        kinds = {}
        for h in hits:
            kinds[h[4]] = kinds.get(h[4], 0) + 1
        print('  by kind: ' + ', '.join('%s %d' % kv for kv in sorted(kinds.items())))
        # bursts: hitches less than 1 s apart
        bursts = []
        for h in hits:
            if bursts and h[0] - bursts[-1][-1][0] < 1.0:
                bursts[-1].append(h)
            else:
                bursts.append([h])
        print('  %d bursts (hitches less than 1 s apart grouped):' % len(bursts))
        print('     time s     VBlank  stage  movie  what')
        for b in bursts[:maxh]:
            what = {}
            for h in b:
                key = h[4] + (' %d' % h[5] if h[4] == 'held' else '')
                what[key] = what.get(key, 0) + (1 if h[4] == 'held' else h[5] if h[4] in ('left out', 'underrun', 'drop') else 1)
            desc = ', '.join('%s x%d' % kv for kv in what.items())
            print('  %9.3f  %9d  %5s  %5s  %s%s' % (b[0][0], b[0][1], b[0][2], 'yes' if any(h[3] for h in b) else '', desc,
                                                  ' (to %.3f s, VB %d)' % (b[-1][0], b[-1][1]) if len(b) > 1 else ''))
        if len(bursts) > maxh:
            print('  ... %d more (--hitches N)' % (len(bursts) - maxh))

    movies = []
    for l in lines:
        m = re.match(r'movie: "(.*)" (starts|stopped) at VBlank (\d+)', l)
        if m:
            movies.append((m.group(1), m.group(2), int(m.group(3))))
    av = []
    for l in lines:
        m = re.match(r'av: VB (\d+) .*?picture (-?\d+) ms after its VBlank began, sound (-?\d+) ms: offset ([-+]\d+) ms', l)
        if m:
            av.append((int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))))
    if movies or av:
        print('\nmovies and A/V (offset = picture time - sound time; + the picture is late):')
        for mv in movies:
            print('  %s "%s" at VBlank %d' % (mv[1], mv[0], mv[2]))
        if av:
            # group the av samples by the movie span they fall in (or by gaps of more than 120 VBlanks)
            groups = []
            for a in av:
                if groups and a[0] - groups[-1][-1][0] <= 120:
                    groups[-1].append(a)
                else:
                    groups.append([a])
            for g in groups:
                offs = sorted(a[3] for a in g)
                vl = sorted(a[1] for a in g)
                al = sorted(a[2] for a in g)
                print('  VB %d-%d: %d samples, offset median %+d ms (min %+d, max %+d); picture %d ms, sound %d ms after the VBlank (medians)' % (
                    g[0][0], g[-1][0], len(g), offs[len(offs) // 2], offs[0], offs[-1], vl[len(vl) // 2], al[len(al) // 2]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
