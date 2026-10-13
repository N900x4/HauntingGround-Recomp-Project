#!/bin/sh
# usage: runhash.sh BINARY OUT.md5 MAXVB [GSDRAW]  -> frames every 5 VBlanks hashed (md5 of the PPM) and deleted as they appear
S=${HG_SCRATCH:-${TMPDIR:-/tmp}}
export HG_GAME_DIR="/home/kali/ps2-work/Haunting Ground/iso"
export HG_MOVIES=${HG_MOVIES:-skip}   # the HG_PAD timeline below is the movie-skip one
D=$(mktemp -d $S/fr.XXXX); M=$(mktemp -d $S/mc.XXXX)
if [ -n "$4" ]; then export HG_GSDRAW=$4; fi
HG_MAXVB=$3 HG_FRAMES=$D HG_FRAME_EVERY=5 HG_WINDOW=0 HG_MEMCARD_DIR=$M HG_PAD=600:cross,900:cross,1200:cross,1500:cross,1800:cross,2100:cross $1 -q -t 6000 > $2.log 2>&1 &
P=$!
: > $2.tmp
while kill -0 $P 2>/dev/null; do
  sleep 2
  n=$(ls $D | wc -l); if [ $n -gt 1 ]; then ls $D | sort | head -n $((n-1)) | while read f; do (cd $D && md5sum $f >> $2.tmp && rm -f $f); done; fi
done
(cd $D && ls | sort | while read f; do md5sum $f; done >> $2.tmp)
sort -k2 $2.tmp > $2; rm -rf $D $M $2.tmp
grep -E "host time" $2.log; wc -l < $2
