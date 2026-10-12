"""Attribute functions to libraries by referenced strings / syscalls; print address clustering."""
import r5900 as R, consts, re, bisect, collections, sys
S=sys.argv[1]
b=R.load('../../iso/SLUS_210.75'); W=R.words(b)
funcs=sorted(int(l.split('\t')[0],16) for l in open(S+'/ghfuncs.tsv'))
def cstr(a):
    o=a-0x100000
    if not (0<=o<len(b)): return None
    e=b.find(b'\0',o,o+200)
    if e<=o+3: return None
    s=b[o:e]
    if all(32<=c<127 or c in (9,10) for c in s): return s.decode()
    return None
TAGS=[('cri_sofdec',r'MWSFD|mwPly|mwSfd|SFD|SFH|M2V|MPV|MPS|SFX|SUD|CFT|mwsf'),
      ('cri_adx',r'ADX|adx|SJ|sj|CVFS|cvFs|DVCI|dvCi|HTCI|MFCI|LSC|PS2RNA|SVM|svm|ROFS|ROCI|RSU|SRD|DTR|DTX|SKG|CRI|PL2ENC|ps2rna|rna'),
      ('sce_mc',r'sceMc|libmc'),('sce_pad',r'scePad|libpad'),('sce_dbc',r'sceDbc|libdbc'),
      ('sce_cdvd',r'sceCd|libcdvd|cdrom0'),('sce_sif',r'sceSif|Sif'),('sce_graph',r'sceGs|libgraph'),
      ('sce_dma',r'sceDma|libdma'),('sce_vu0',r'sceVu0|libvu0'),('sce_ipu',r'sceIpu|libipu'),
      ('sce_kernel',r'libkernl|sceKernel|kernel'),('msl',r'std::|%[-+ #0-9.]*[dsxfgeculp]|nan|inf|ios_base|locale'),]
tagof=collections.defaultdict(collections.Counter)
for a,v,k in consts.address_refs(W):
    s=cstr(v)
    if not s: continue
    i=bisect.bisect_right(funcs,a)-1
    f=funcs[i]
    for t,rx in TAGS:
        if re.search(rx,s): tagof[f][t]+=1; break
    else: tagof[f]['game_str']+=1
# syscalls
for i,w in enumerate(W[:(0x3a1b80-0x100000)//4]):
    if (w&0xfc00003f)==0x0000000c:
        a=0x100000+4*i; f=funcs[bisect.bisect_right(funcs,a)-1]; tagof[f]['syscall']+=1
# bins
binsz=0x4000
for lo in range(0x100000,0x3a4000,binsz):
    c=collections.Counter(); nf=0
    for f in funcs:
        if lo<=f<lo+binsz:
            nf+=1
            for t,n in tagof[f].items(): c[t]+=n
    print(hex(lo), nf, ' '.join('%s:%d'%x for x in c.most_common(4)))
