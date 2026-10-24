#!/usr/bin/env python3
"""Make a copy of a PS2 (Emotion Engine) ELF that rea will analyse, plus a Ghidra seed file of every function start.

rea 6.7 refuses MIPS ELFs that name a machine variant (the PS2's e_flags 0x20924000: MIPS III, R5900); the copy
declares plain MIPS32 (e_flags 0x50000001) and nothing else changes. rea then imports it with the language set in
REA_GHIDRA_LANGUAGE_ID (r5900:LE:32:default, from ghidra-emotionengine-reloaded; see tools/rea/README.md).
The copy is the game's program: write it outside the repository and never commit it.

usage: ps2elf_for_rea.py OUTDIR   (writes OUTDIR/SLUS_210.75.elf and OUTDIR/hg_seeds.tsv)"""
import os, struct, sys

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
GAME = os.environ.get('HG_GAME_DIR', os.path.join(REPO, 'game'))
DATA = os.path.join(REPO, 'data')

# functions named so far (docs/textures.md, docs/frame-timing.md); Ghidra gets them as labels
NAMES = {
    0x1bfec0: 'vram_register_tex_entry', 0x1c1e00: 'vram_alloc_tex', 0x1c02a0: 'vram_upload_tex_clut',
    0x1c12e0: 'vram_build_tex0', 0x1bb470: 'texcache_reupload_path2', 0x1bb010: 'upload_title_back_bin',
    0x1bb230: 'upload_psmt8_generic', 0x26bb00: 'texset_register_blob', 0x26b690: 'texset_register_type1',
    0x26b570: 'texset_register_type2', 0x1f41a0: 'texlru_get_handle', 0x120720: 'stage_load_pac',
    0x1a3ee0: 'scene_load_textures', 0x12f720: 'title_load_textures', 0x12d7f0: 'title_draw',
    0x1b8d30: 'runtime_clut_write', 0x1f9d90: 'clut_effect_pass', 0x387f00: 'load_gallery_len_art',
    0x1beef0: 'frame_driver', 0x1b86a0: 'frame_begin', 0x1b8750: 'frame_end', 0x10d200: 'wait_vblank',
    0x10c8b8: 'gs_csr_poll',
}

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else sys.exit(__doc__)
    os.makedirs(out, exist_ok=True)
    elf = bytearray(open(os.path.join(GAME, 'SLUS_210.75'), 'rb').read())
    if elf[:4] != b'\x7fELF' or struct.unpack_from('<H', elf, 18)[0] != 8:
        sys.exit('not a MIPS ELF')
    struct.pack_into('<I', elf, 36, 0x50000001)
    open(os.path.join(out, 'SLUS_210.75.elf'), 'wb').write(elf)
    starts = set(NAMES)
    for f in ('mystarts.txt', 'extra_starts.txt'):
        p = os.path.join(DATA, f)
        if os.path.exists(p):
            starts |= {int(x, 16) for x in open(p).read().split()}
    with open(os.path.join(out, 'hg_seeds.tsv'), 'w') as o:
        o.write('# every function start from the recompiler (data/mystarts.txt, extra_starts.txt) and the names known so far\n')
        for a in sorted(starts):
            o.write('0x%x\tfunction%s\n' % (a, '\t' + NAMES[a] if a in NAMES else ''))
    print('%s: ELF copy and %d function seeds (%d named)' % (out, len(starts), len(NAMES)))

if __name__ == '__main__':
    main()
