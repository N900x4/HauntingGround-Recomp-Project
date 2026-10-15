#!/bin/sh
# Phase 4 (DualSense): the game's vibration as the pad HLE decodes it (HG_RUMBLELOG=1), scripted and headless.
# New Game card, main menu -> OPTIONS, Vibration row: Cross, Right (Off), Right (On: the game plays its test
# vibration, small motor and big motor 128 for 4 frames each), Cross; then the same in the controller page of our
# rows (Down past Screen Position to Controller, Cross): Vibration and Light bar changed, Triangle out.
# Prints the "rumble:" lines (VBlank, small 0/1, big 0-255); two runs, and a recording of a run replayed, must give
# the same lines.
# usage: rumbletest.sh TAG [env...]      outputs $HG_SCRATCH/rum_TAG.log and rum_TAG.txt (default HG_SCRATCH D:/ps2-work/scratch)
# HG_PADREC=DIR / HG_PADPLAY=DIR record and replay it (the card of a replay comes from the recording).
V=$(cd "$(dirname "$0")" && pwd); R=$(cd "$V/../.." && pwd); S=${HG_SCRATCH:-/d/ps2-work/scratch}
T=${1:-x}; shift
cd $R/recomp
M=$(mktemp -d $S/mc.XXXX)
rm -f $S/rum_$T.ini
P=600:cross,900:cross,1200:cross,1500:cross,1650:down,1750:down,1850:cross
P=$P,1960:down,1990:cross,2020:right,2060:right,2100:cross
P=$P,2140:down,2170:down,2200:down,2230:down,2260:down,2290:down,2320:down,2350:down,2380:down,2410:down,2440:cross
P=$P,2470:right,2500:down,2530:down,2560:down,2590:down,2620:down,2650:down,2680:cross,2710:left,2740:cross
P=$P,2770:down,2800:cross,2830:right,2860:cross,2890:triangle,2920:triangle
env HG_GAME_DIR="$R/game" HG_MOVIES=skip HG_MEMCARD_DIR=$M HG_INI=$S/rum_$T.ini HG_PAD=$P HG_RUMBLELOG=1 \
    HG_MAXVB=${HG_MAXVB:-3000} HG_WINDOW=0 "$@" ./hgboot.exe -q -t 6000 > $S/rum_$T.log 2>&1
rm -rf $M
grep -E "^rumble: VBlank [0-9]+ small" $S/rum_$T.log > $S/rum_$T.txt
printf "%s: %d rumble changes; " $T $(wc -l < $S/rum_$T.txt)
grep -cE "^settings: (vibration|lightbar)" $S/rum_$T.log | tr -d '\n'; echo " setting changes"
cat $S/rum_$T.txt; grep -E "^settings: (vibration|lightbar)" $S/rum_$T.log
