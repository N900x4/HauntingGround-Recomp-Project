#!/bin/sh
# Gate G10 (pictures): dumps the pictures handed to the window (HG_FRAMES_SHOWN, real and in-between) in two ranges,
# stage 1 (VB 3300-3311) and the Castle Suite (VB 3600-3611), headless, into $HG_SCRATCH/pic_TAG/{st1,cs}; compare
# two such sets with piccmp.py. usage: picgate.sh TAG [env...]   e.g. picgate.sh x5i HG_GSVK=1 HG_GSVK_SCALE=5 HG_GSVK_INTERP=1
V=$(cd "$(dirname "$0")" && pwd); R=$(cd "$V/../.." && pwd); S=${HG_SCRATCH:-/d/ps2-work/scratch}
T=$1; shift; cd $R/recomp
P6=600:cross,900:cross,1200:cross,1500:cross,1800:cross,2100:cross
O=$S/pic_$T; rm -rf $O $S/mc_pic; mkdir -p $O/st1 $O/cs $S/mc_pic
env "$@" HG_FRAMES_SHOWN=$(cd $O/st1 && pwd -W):3300:3311 HG_WINDOW=0 HG_MOVIES=skip HG_MEMCARD_DIR=$S/mc_pic HG_PAD=$P6 HG_MAXVB=3312 ./hgboot.exe -q -t 3000 > $O/st1.log 2>&1 &
env "$@" HG_FRAMES_SHOWN=$(cd $O/cs && pwd -W):3600:3611 HG_WINDOW=0 HG_MOVIES=skip HG_PAD=$P6,2400:cross,2600:cross,2800:cross HG_MAXVB=3612 ./hgboot.exe -q -t 3000 > $O/cs.log 2>&1 &
wait
echo "$T: $(ls $O/st1 | wc -l) + $(ls $O/cs | wc -l) pictures in $O"
