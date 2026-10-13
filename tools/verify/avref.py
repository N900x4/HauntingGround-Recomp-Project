"""Decode an MPEG-2 elementary stream with the system libavcodec (ctypes) and compare with dumped IPU CSC inputs."""
import ctypes, ctypes.util, sys, glob, os, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
avu = ctypes.CDLL('libavutil.so.59') if os.path.exists('/usr/lib/x86_64-linux-gnu/libavutil.so.59') else ctypes.CDLL(ctypes.util.find_library('avutil'))
avc = ctypes.CDLL('libavcodec.so.61')
P = ctypes.c_void_p
avc.avcodec_find_decoder_by_name.restype = P; avc.avcodec_find_decoder_by_name.argtypes = [ctypes.c_char_p]
avc.avcodec_alloc_context3.restype = P; avc.avcodec_alloc_context3.argtypes = [P]
avc.avcodec_open2.argtypes = [P, P, P]
avc.av_parser_init.restype = P; avc.av_parser_init.argtypes = [ctypes.c_int]
avc.av_packet_alloc.restype = P
avu.av_frame_alloc.restype = P
avc.av_parser_parse2.argtypes = [P, P, ctypes.POINTER(P), ctypes.POINTER(ctypes.c_int), ctypes.c_char_p, ctypes.c_int, ctypes.c_int64, ctypes.c_int64, ctypes.c_int64]
avc.avcodec_send_packet.argtypes = [P, P]; avc.avcodec_receive_frame.argtypes = [P, P]
def decode(es):
    codec = avc.avcodec_find_decoder_by_name(b'mpeg2video'); ctx = avc.avcodec_alloc_context3(codec)
    assert avc.avcodec_open2(ctx, codec, None) == 0
    parser = avc.av_parser_init(2)   # AV_CODEC_ID_MPEG2VIDEO
    pkt = avc.av_packet_alloc(); frame = avu.av_frame_alloc()
    frames = []
    def drain():
        while avc.avcodec_receive_frame(ctx, frame) == 0:
            data = (P * 8).from_address(frame); ls = (ctypes.c_int * 8).from_address(frame + 64)
            w = ctypes.c_int.from_address(frame + 104).value; h = ctypes.c_int.from_address(frame + 108).value
            planes = []
            for k, (pw, ph) in enumerate(((w, h), (w // 2, h // 2), (w // 2, h // 2))):
                buf = ctypes.string_at(data[k], ls[k] * ph)
                planes.append(np.frombuffer(buf, np.uint8).reshape(ph, ls[k])[:, :pw].copy())
            frames.append(planes)
    buf = ctypes.create_string_buffer(es, len(es)); off = 0
    while True:
        outp = P(); outs = ctypes.c_int()
        n = avc.av_parser_parse2(parser, ctx, ctypes.byref(outp), ctypes.byref(outs), ctypes.cast(ctypes.addressof(buf) + off, ctypes.c_char_p) if off < len(es) else None, len(es) - off, -1, -1, 0)
        off += n
        if outs.value:
            ctypes.c_void_p.from_address(pkt + 24).value = outp.value; ctypes.c_int.from_address(pkt + 32).value = outs.value
            avc.avcodec_send_packet(ctx, pkt); drain()
        if off >= len(es) and not outs.value: break
    avc.avcodec_send_packet(ctx, None); drain()
    return frames
def raw_to_planes(raw, w, h):
    mbw, mbh = w // 16, h // 16; mbs = np.frombuffer(raw, np.uint8).reshape(-1, 384)
    Y = np.zeros((h, w), np.uint8); Cb = np.zeros((h // 2, w // 2), np.uint8); Cr = np.zeros_like(Cb)
    for i in range(min(len(mbs), mbw * mbh)):
        x, y = (i // mbh), (i % mbh)   # the player stores macroblocks column by column
        Y[y*16:y*16+16, x*16:x*16+16] = mbs[i, :256].reshape(16, 16)
        Cb[y*8:y*8+8, x*8:x*8+8] = mbs[i, 256:320].reshape(8, 8); Cr[y*8:y*8+8, x*8:x*8+8] = mbs[i, 320:].reshape(8, 8)
    return Y, Cb, Cr
if __name__ == '__main__':
    import cvm
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'm2parse.py')).read()
    g = {}; exec(src.split('class Bits')[0].replace("T = {n: table(n)", "T = {}; _x = {n: 0").replace("T['t_dct1'] = dict(T['t_dct1_short']); T['t_dct1'].update({c: v for c, v in T['t_dct0'].items() if len(c) >= 14})", ""), g)
    es = g['demux_video'](cvm.read(sys.argv[1]))
    ref = decode(es); print('libavcodec frames:', len(ref), 'size', ref[0][0].shape[::-1])
    w, h = ref[0][0].shape[1], ref[0][0].shape[0]
    dumps = sorted(glob.glob(os.path.join(sys.argv[2], 'csc_*.raw')))
    j = 0; worst = 99; diffs = []
    for k, f in enumerate(dumps):
        Y, Cb, Cr = raw_to_planes(open(f, 'rb').read(), w, h)
        # find the matching reference frame at or after j (display order)
        best = None
        for jj in range(max(0, j - 8), min(j + 8, len(ref))):
            d = np.abs(ref[jj][0].astype(int) - Y).mean()
            if best is None or d < best[0]: best = (d, jj)
        d, jj = best; j = jj + 1
        R = ref[jj]
        err = [np.abs(R[p].astype(int) - X.astype(int)) for p, X in enumerate((Y, Cb, Cr))]
        mse = np.mean([((R[p].astype(float) - X) ** 2).mean() for p, X in enumerate((Y, Cb, Cr))])
        psnr = 99 if mse == 0 else 10 * np.log10(255 ** 2 / mse)
        worst = min(worst, psnr); diffs.append((k, jj, psnr, max(e.max() for e in err), sum(int((e > 0).sum()) for e in err)))
    for t in diffs[:6] + diffs[-3:]: print('dump %3d = ref frame %3d: PSNR %.2f dB, max abs diff %d, %d samples differ' % t)
    print('pictures compared: %d, worst PSNR %.2f dB, identical pictures %d' % (len(diffs), worst, sum(1 for t in diffs if t[4] == 0)))
