#!/bin/sh
# usage: gscmp.sh BINARY OUTDIR  -> renders VB 2380-2520 of stage 1, compares with ref1
S=${HG_SCRATCH:-${TMPDIR:-/tmp}}
export HG_GAME_DIR="/home/kali/ps2-work/Haunting Ground/iso"
export HG_MOVIES=${HG_MOVIES:-skip}   # the HG_PAD timeline below is the movie-skip one
rm -rf $S/mc5 $2; mkdir -p $2
HG_GSDRAW=2380 HG_MAXVB=2520 HG_FRAMES=$2 HG_FRAME_EVERY=5 HG_WINDOW=0 HG_MEMCARD_DIR=$S/mc5 HG_PAD=600:cross,900:cross,1200:cross,1500:cross,1800:cross,2100:cross $1 -q -t 3000 > $2.log 2>&1
grep -E "host time|^GS sprites|^GS tri" $2.log
n=0; d=0; for f in $S/ref1/*.ppm; do b=$(basename $f); n=$((n+1)); cmp -s $f $2/$b || { d=$((d+1)); [ $d -le 5 ] && echo DIFF $b; }; done; echo "$n frames, $d differ"
