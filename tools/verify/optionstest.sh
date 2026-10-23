#!/bin/sh
export HG_INI=${HG_INI:-off}   # gates never read the user's hg.ini (set HG_INI=FILE to use one)
# The Options menu's extra rows (phase 3), scripted: New Game card, main menu -> OPTIONS, Up past Controller Type
# (into the new rows) and back, down through the game's rows into the new ones, then every new row changed once:
# Renderer (and back), Resolution x3, Frame rate 60, Display full screen, V-Sync on, a key captured on the keyboard
# page, a button on the controller page and that page's Reset Defaults, the display rows' Reset Defaults, 120 fps, round to
# Controller Type, leave. Frames are hashed every 5 VBlanks; two runs must give the same hashes (and so must a
# recording of it replayed: HG_PADREC=DIR, then HG_PADPLAY=DIR with the same script name).
# usage: optionstest.sh TAG [env...]      outputs $HG_SCRATCH/opt_TAG.md5, .log, .ini (default HG_SCRATCH D:/ps2-work/scratch)
# Settings go to a scratch hg.ini ($HG_SCRATCH/opt_TAG.ini, removed first), never to the one next to the executable.
V=$(cd "$(dirname "$0")" && pwd); R=$(cd "$V/../.." && pwd); S=${HG_SCRATCH:-/d/ps2-work/scratch}
T=${1:-x}; shift
cd $R/recomp
D=$(mktemp -d $S/fr.XXXX); M=$(mktemp -d $S/mc.XXXX)
rm -f $S/opt_$T.ini
P=600:cross,900:cross,1200:cross,1500:cross,1650:down,1750:down,1850:cross
P=$P,1960:up,1990:down,2020:down,2050:down,2080:down,2110:down,2140:down
P=$P,2170:cross,2200:right,2230:cross,2260:cross,2290:right,2320:cross
P=$P,2350:down,2380:cross,2410:right,2440:right,2470:cross
P=$P,2500:down,2530:cross,2560:right,2590:cross
P=$P,2620:down,2650:cross,2680:right,2710:cross
P=$P,2740:down,2770:cross,2800:right,2830:cross
P=$P,2860:down,2890:cross,2920:cross,2980:down,3010:right,3040:triangle
P=$P,3070:down,3100:cross,3130:down,3160:cross,3220:square,3240:left,3260:cross,3290:triangle
P=$P,3320:square,3340:left,3360:cross,3390:down,3420:up
P=$P,3450:up,3470:up,3490:up,3510:up,3530:cross,3550:right,3570:right,3590:cross,3620:down,3640:down,3660:down,3680:down,3700:down,3720:triangle
EV=2950:kb.cross=V,3190:pad.circle=y
env HG_GAME_DIR="$R/game" HG_MOVIES=skip HG_MEMCARD_DIR=$M HG_INI=$S/opt_$T.ini HG_GSVK=1 HG_PAD=$P HG_HOSTEV=$EV \
    HG_MAXVB=${HG_MAXVB:-3850} HG_FRAMES=$D HG_FRAME_EVERY=5 HG_WINDOW=0 "$@" ./hgboot.exe -q -t 6000 > $S/opt_$T.log 2>&1 &
PID=$!
: > $S/opt_$T.tmp
while kill -0 $PID 2>/dev/null; do
  sleep 2
  n=$(ls $D | wc -l); if [ $n -gt 1 ]; then ls $D | sort | head -n $((n-1)) | while read f; do (cd $D && md5sum $f >> $S/opt_$T.tmp && rm -f $f); done; fi
done
(cd $D && ls | sort | while read f; do md5sum $f; done >> $S/opt_$T.tmp)
sort -k2 $S/opt_$T.tmp > $S/opt_$T.md5; rm -rf $D $M $S/opt_$T.tmp
printf "%s: %d frames; " $T $(wc -l < $S/opt_$T.md5)
grep -cE "^settings: ((renderer|resolution|fps|fullscreen|vsync|kb|pad)|.* back to the defaults)" $S/opt_$T.log | tr -d '\n'; echo " setting changes"
grep -E "^settings: |GSVK: switched|replay: " $S/opt_$T.log
