#!/bin/sh
# Sound regression check: replay a recorded session headless (no drawing) with the SPU2's mixed output written to a
# WAV, then audiocheck.py looks for stuck tones, DC and clicks (see its header). The recording is copied to scratch
# first (a replay writes replay_card into the recording's folder); the WAV is deleted afterwards (--keep keeps it).
# usage: audiocheck.sh TAG [RECORDING_DIR [EXE]] [--keep]
#   default recording ../recordings/20261015-184308 (New Game, intro skipped, the guest room: the 2026-10-15 beep),
#   default exe recomp/hgboot.exe; outputs in $HG_SCRATCH (default D:/ps2-work/scratch)
V=$(cd "$(dirname "$0")" && pwd); R=$(cd "$V/../.." && pwd); S=${HG_SCRATCH:-/d/ps2-work/scratch}
T=$1; REC=${2:-$R/recordings/20261015-184308}; EXE=${3:-$R/recomp/hgboot.exe}
KEEP=0; for a in "$@"; do [ "$a" = --keep ] && KEEP=1; done
[ -f "$REC/pad.txt" ] || { echo "no recording at $REC"; exit 2; }
C=$S/audiocheck_$T; rm -rf $C; mkdir -p $C/rec; cp -rp "$REC/card" "$REC/pad.txt" $C/rec/
LAST=$(grep -E '^[0-9]+ ' "$REC/pad.txt" | tail -1 | cut -d' ' -f1)
WIN=$(cd $C && pwd -W 2>/dev/null || pwd)
cd $R/recomp
env HG_GSDRAW=0 HG_GSVK=0 HG_WINDOW=0 HG_PADREC= HG_PADPLAY=$WIN/rec HG_WAV=$WIN/out.wav HG_MAXVB=$((LAST + 60)) \
    nice -n 19 "$EXE" -q -t 6000 > $C/run.log 2>&1
grep -E "^SPU2:" $C/run.log
PY=${PYTHON:-$(command -v python || command -v py || ls /c/Users/*/AppData/Local/Programs/Python/Python3*/python.exe 2>/dev/null | tail -1)}
"$PY" "$V/audiocheck.py" $C/out.wav; rc=$?
[ $KEEP = 1 ] || rm -f $C/out.wav
rm -rf $C/rec/replay_card
exit $rc
