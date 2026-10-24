#!/usr/bin/env python3
"""Check a runtime texture log (HG_TEXLOG, gs.c) against the disc manifest (texdump.py): which uploads are disc
textures or CLUTs, and which texture setups (TEX0) the game drew with map to a disc texture id.
usage: texmatch.py MANIFEST.tsv TEXLOG [--list N]"""
import sys, collections

def main():
    man, log = sys.argv[1], sys.argv[2]
    nlist = int(sys.argv[sys.argv.index('--list') + 1]) if '--list' in sys.argv else 15
    img_ids, clut_crcs, ids, where = collections.defaultdict(set), set(), set(), {}
    for line in open(man):
        f = line.rstrip('\n').split('\t')
        if f[0] == 'id': continue
        img_ids[f[1]].add(f[0]); clut_crcs.add(f[2]); ids.add(f[0]); where.setdefault(f[0], '%s#%s' % (f[3], f[5]))
    up = collections.Counter(); up_bytes = collections.Counter(); unk_up = collections.Counter()
    use = collections.Counter(); used_ids = set(); unk_use = collections.Counter(); first = {}
    for line in open(log):
        f = line.split()
        if f[0] == 'UP':
            crc, psm, n = f[9], f[4], int(f[10])
            kind = 'disc image' if crc in img_ids else 'disc CLUT' if crc in clut_crcs else 'other'
            up[kind] += 1; up_bytes[kind] += n
            if kind == 'other': unk_up[(psm, f[7], f[8], f[2])] += 1
        elif f[0] == 'USE':
            tbp, psm, cbp, img, clut = f[2], f[4], f[7], f[10], f[11]
            if psm not in ('0x13', '0x14', '0x1b', '0x24', '0x2c'):   # direct colour: render targets and effect passes
                use['direct colour (render target / effect)'] += 1; continue
            tid = img + '_' + clut
            if tid in ids:
                use['disc texture'] += 1; used_ids.add(tid); first.setdefault(tid, f[1])
            elif img in img_ids:
                use['disc image, other CLUT (runtime palette)'] += 1; unk_use[('runtime CLUT', psm, f[5], f[6], clut)] += 1
            elif img == '00000000':
                use['no upload at tbp (render target read as indices)'] += 1; unk_use[('no upload', psm, tbp, f[5], f[6])] += 1
            else:
                use['unknown image'] += 1; unk_use[('unknown image', psm, tbp, f[5], f[6], img)] += 1
    print('uploads (content changes):')
    for k, v in up.most_common(): print('  %-12s %6d  %8.1f MB' % (k, v, up_bytes[k] / 2**20))
    print('texture setups (TEX0, unique with content):')
    for k, v in use.most_common(): print('  %-50s %6d' % (k, v))
    print('disc texture ids drawn: %d of %d' % (len(used_ids), len(ids)))
    if unk_up:
        print('uploads not on the disc (psm w h bp: count), top %d:' % nlist)
        for k, v in unk_up.most_common(nlist): print('  ', k, v)
    if unk_use:
        print('setups without a disc id, top %d:' % nlist)
        for k, v in unk_use.most_common(nlist): print('  ', k, v)

if __name__ == '__main__':
    main()
