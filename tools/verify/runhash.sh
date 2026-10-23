#!/bin/sh
export HG_INI=${HG_INI:-off}   # gates never read the user's hg.ini (set HG_INI=FILE to use one)
# usage: runhash.sh BINARY OUT.md5 MAXVB [GSDRAW]  -> frames every 5 VBlanks hashed (md5 of the PPM) and deleted as they appear
# Route: New Game on an empty card (reference ref/hashA.md5, MAXVB 4000); HG_ROUTE=cs: LOAD GAME from the default card
# (../memcards) into the Castle Suite (reference ref/cs.md5, MAXVB 4200).
S=${HG_SCRATCH:-${TMPDIR:-/tmp}}
export HG_GAME_DIR="${HG_GAME_DIR:-$(cd "$(dirname "$0")/../../game" && pwd)}"
export HG_MOVIES=${HG_MOVIES:-skip}   # the HG_PAD timeline below is the movie-skip one
D=$(mktemp -d $S/fr.XXXX); M=$(mktemp -d $S/mc.XXXX)
PAD=600:cross,900:cross,1200:cross,1500:cross,1800:cross,2100:cross
if [ "$HG_ROUTE" = cs ]; then PAD=$PAD,2400:cross,2600:cross,2800:cross; else export HG_MEMCARD_DIR=$M; fi
if [ -n "$4" ]; then export HG_GSDRAW=$4; fi
HG_MAXVB=$3 HG_FRAMES=$D HG_FRAME_EVERY=5 HG_WINDOW=0 HG_PAD=$PAD $1 -q -t 6000 > $2.log 2>&1 &
P=$!
: > $2.tmp
while kill -0 $P 2>/dev/null; do
  sleep 2
  n=$(ls $D | wc -l); if [ $n -gt 1 ]; then ls $D | sort | head -n $((n-1)) | while read f; do (cd $D && md5sum -b $f >> $2.tmp && rm -f $f); done; fi
done
(cd $D && ls | sort | while read f; do md5sum -b $f; done >> $2.tmp)
sort -k2 $2.tmp > $2; rm -rf $D $M $2.tmp
grep -E "host time" $2.log; wc -l < $2
