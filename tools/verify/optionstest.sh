#!/bin/sh
export HG_INI=${HG_INI:-off}   # gates never read the user's hg.ini (set HG_INI=FILE to use one)
case $(uname -s) in MINGW*|MSYS*|CYGWIN*) HGEXE=hgboot.exe;; *) HGEXE=hgboot;; esac   # the runtime on this OS
# The Options menu's extra rows (phase 3), scripted: New Game card, main menu -> OPTIONS, Up past Controller Type
# (into the new rows) and back, down through the game's rows into the new ones, then every new row changed once:
# Renderer (and back), Resolution x3, Frame rate 60, Display full screen, Aspect ratio widescreen, Upscaling bilinear, Soft focus original, V-Sync on, HD textures off, a key captured on the keyboard
# page, a button on the controller page and that page's Reset Defaults, the display rows' Reset Defaults, 120 fps, round to
# Controller Type, leave (120 fps is not offered there: Reset Defaults made the resolution Auto). Frames are hashed every 5 VBlanks; two runs must give the same hashes (and so must a
# recording of it replayed: HG_PADREC=DIR, then HG_PADPLAY=DIR with the same script name).
# usage: optionstest.sh TAG [env...]      outputs $HG_SCRATCH/opt_TAG.md5, .log, .ini (default HG_SCRATCH ../scratch next to the repo)
# Settings go to a scratch hg.ini ($HG_SCRATCH/opt_TAG.ini, removed first), never to the one next to the executable.
V=$(cd "$(dirname "$0")" && pwd); R=$(cd "$V/../.." && pwd); S=${HG_SCRATCH:-$(cd "$R/.." && pwd)/scratch}
T=${1:-x}; shift
cd $R/recomp
D=$(mktemp -d $S/fr.XXXX); M=$(mktemp -d $S/mc.XXXX)
rm -f $S/opt_$T.ini
P=600:cross,900:cross,1200:cross,1500:cross,1650:down,1750:down,1850:cross,1960:up,1990:down
P=$P,2020:down,2050:down,2080:down,2110:down,2140:down,2170:cross,2200:right,2230:cross,2260:cross
P=$P,2290:right,2320:cross,2350:down,2380:cross,2410:right,2440:right,2470:cross,2500:down,2530:cross
P=$P,2560:right,2590:cross,2620:down,2650:cross,2680:right,2710:cross,2740:down,2770:cross,2800:right
P=$P,2830:cross,2860:down,2890:cross,2920:right,2950:cross,2980:down,3010:cross,3040:right,3070:cross
P=$P,3100:down,3130:cross,3160:right,3190:cross,3220:down,3250:cross,3280:right,3310:cross,3340:down
P=$P,3370:cross,3400:cross,3460:down,3490:right,3520:triangle,3550:down,3580:cross,3610:down,3640:cross
P=$P,3700:square,3720:left,3740:cross,3770:triangle,3800:square,3820:left,3840:cross,3870:down,3900:up
P=$P,3930:up,3950:up,3970:up,3990:up,4010:up,4030:up,4050:up,4070:up,4090:cross
P=$P,4110:right,4130:right,4150:cross,4180:down,4200:down,4220:down,4240:down,4260:down,4280:down
P=$P,4300:triangle
EV=3430:kb.cross=V,3670:pad.circle=y
env HG_GAME_DIR="$R/game" HG_MOVIES=skip HG_MEMCARD_DIR=$M HG_INI=$S/opt_$T.ini HG_GSVK=1 HG_PAD=$P HG_HOSTEV=$EV \
    HG_MAXVB=${HG_MAXVB:-4450} HG_FRAMES=$D HG_FRAME_EVERY=5 HG_WINDOW=0 "$@" ./$HGEXE -q -t 6000 > $S/opt_$T.log 2>&1 &
PID=$!
: > $S/opt_$T.tmp
while kill -0 $PID 2>/dev/null; do
  sleep 2
  n=$(ls $D | wc -l); if [ $n -gt 1 ]; then ls $D | sort | head -n $((n-1)) | while read f; do (cd $D && md5sum -b $f >> $S/opt_$T.tmp && rm -f $f); done; fi
done
(cd $D && ls | sort | while read f; do md5sum -b $f; done >> $S/opt_$T.tmp)
sort -k2 $S/opt_$T.tmp > $S/opt_$T.md5; rm -rf $D $M $S/opt_$T.tmp
printf "%s: %d frames; " $T $(wc -l < $S/opt_$T.md5)
grep -cE "^settings: ((renderer|resolution|fps|fullscreen|aspect|upscaler|softfocus|vsync|hd|kb|pad)|.* back to the defaults)" $S/opt_$T.log | tr -d '\n'; echo " setting changes"
grep -E "^settings: |GSVK: switched|replay: " $S/opt_$T.log
