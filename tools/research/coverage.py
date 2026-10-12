"""Opcode coverage of rec.py over the whole binary histogram (hist.json from stats.py)."""
import json, sys, collections, rec, r5900 as R
H=json.load(open(sys.argv[1]+'/hist.json'))
# populate rec.SUPPORTED by translating one instance of every mnemonic
seen={}
W=R.words(R.load('../../iso/SLUS_210.75'))
for i,w in enumerate(W[:(0x3a1b80-0x100000)//4]):
    m=R.decode(w)
    if m not in seen: seen[m]=w; rec.sem(w,0x100000+4*i)
sup=rec.SUPPORTED
for k in ('game','cri','sony','mw','total'):
    tot=sum(H[k].values()); ok=sum(v for m,v in H[k].items() if m in sup)
    print(f'{k}: {ok}/{tot} = {100*ok/tot:.3f}% covered')
miss=collections.Counter({m:v for m,v in H['total'].items() if m not in sup})
print('unsupported mnemonics (total / game):', ', '.join(f'{m} {v}/{H["game"].get(m,0)}' for m,v in miss.most_common()))
print('distinct mnemonics in binary', len(H['total']), 'supported', len([m for m in H['total'] if m in sup]))
