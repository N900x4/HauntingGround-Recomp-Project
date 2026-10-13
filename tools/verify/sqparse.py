import struct
NDATA={0x80:1,0x90:2,0xa0:2,0xb0:2,0xc0:1,0xd0:1,0xe0:2}
def varlen(d,p):
    v=0
    while True:
        b=d[p]; p+=1; v=(v<<7)|(b&0x7f)
        if not b&0x80: return v,p
def parse(d, maxev=20000):
    """SCEI SQ (note-off 0x8n carries only the key): 'IECSidiM' chunk; events with running status; the last data byte has bit 7 set when no delta follows (delta 0)."""
    m=d.find(b'IECSidiM'); off=struct.unpack_from('<I',d,m+16)[0]
    p=m+off+4; res=struct.unpack_from('<H',d,p)[0]; p+=2
    t=0; tempo=500000; sec=0.0; ev=[]; status=None
    delta,p=varlen(d,p)
    while p<len(d) and len(ev)<maxev:
        t+=delta; sec+=delta*tempo/1e6/res
        b=d[p]
        if b==0xff:
            typ=d[p+1]; ln=d[p+2]; data=d[p+3:p+3+ln]; p+=3+ln
            if typ==0x51: tempo=int.from_bytes(data,'big')
            ev.append((t,sec,'meta',typ,data.hex()))
            if typ==0x2f: break
            delta,p=varlen(d,p); continue
        if b&0x80: status=b; p+=1
        n=NDATA[status&0xf0]; data=list(d[p:p+n]); p+=n
        nodelta=data[-1]&0x80; data=[x&0x7f for x in data]
        ev.append((t,sec,status,*data))
        if nodelta: delta=0
        else: delta,p=varlen(d,p)
    return res,ev
