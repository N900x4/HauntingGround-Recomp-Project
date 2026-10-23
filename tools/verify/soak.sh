#!/bin/sh
export HG_INI=${HG_INI:-off}   # gates never read the user's hg.ini (set HG_INI=FILE to use one)
# Gate G3: replay the user's session recordings/20261014-200745 to its end (VB ~20600), hashing a frame every 20
# VBlanks; compare with ref/soak.md5 (software GS). usage: soak.sh TAG [env...]   e.g. soak.sh x4 HG_GSVK=1 HG_GSVK_SCALE=4
V=$(cd "$(dirname "$0")" && pwd); R=$(cd "$V/../.." && pwd); S=${HG_SCRATCH:-/d/ps2-work/scratch}
T=$1; shift; cd $R/recomp
D=$(mktemp -d $S/fr.XXXX)
env "$@" HG_PADPLAY=../recordings/20261014-200745 HG_PADREC= HG_FRAMES=$D HG_FRAME_EVERY=20 HG_WINDOW=0 HG_MAXVB=20600 ./hgboot.exe -q -t 6000 > $S/soak_$T.log 2>&1 &
P=$!
: > $S/soak_$T.tmp
while kill -0 $P 2>/dev/null; do
  sleep 2
  n=$(ls $D | wc -l); if [ $n -gt 1 ]; then ls $D | sort | head -n $((n-1)) | while read f; do (cd $D && md5sum $f >> $S/soak_$T.tmp && rm -f $f); done; fi
done
(cd $D && ls | sort | while read f; do md5sum $f; done >> $S/soak_$T.tmp)
sort -k2 $S/soak_$T.tmp > $S/soak_$T.md5; rm -rf $D $S/soak_$T.tmp
printf "$T: %d frames, " $(wc -l < $S/soak_$T.md5); if [ $(sort $V/ref/soak.md5 $S/soak_$T.md5 | uniq -u | wc -l) = 0 ]; then echo "identical to ref/soak.md5"; else echo DIFFERENT; fi
grep -E "host time|GSVK live|crash|fault" $S/soak_$T.log | head -5
