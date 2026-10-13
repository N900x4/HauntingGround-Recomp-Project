import sys, struct
for path in sys.argv[1:]:
    d = open(path, 'rb').read()
    out = {}
    i = 0
    while i + 20 <= len(d):
        if struct.unpack_from('<I', d, i)[0] == 0x41E00000 and struct.unpack_from('<I', d, i + 4)[0] == 0:
            name = d[i + 12:i + 20].split(b'\0')[0].decode(errors='replace')
            j = i + 20; fns = []
            while j + 8 <= len(d):
                a, b = struct.unpack_from('<II', d, j)
                if a == 0x03E00008 and (b >> 16) == 0x2400: fns.append(b & 0xFFFF); j += 8
                else: break
            out.setdefault(name, []).extend(fns); i = j
        else: i += 4
    print(path.split('/')[-1], {k: sorted(v) for k, v in out.items()})
