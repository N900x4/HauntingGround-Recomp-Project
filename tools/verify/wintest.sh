#!/bin/sh
export HG_INI=${HG_INI:-off}   # gates never read the user's hg.ini (set HG_INI=FILE to use one)
case $(uname -s) in MINGW*|MSYS*|CYGWIN*) HGEXE=hgboot.exe;; *) HGEXE=hgboot;; esac   # the runtime on this OS
# Gate G7: a windowed run to the Castle Suite (LOAD GAME from ../memcards), sound muted, F3 overlay, presents logged
# from PL seconds on (default 1), speed lines every 300 VBlanks; ends by itself at HG_MAXVB (default 3900).
# usage: [PL=SECONDS] wintest.sh TAG [env...]   then: python pacing.py $HG_SCRATCH/win_TAG.log
V=$(cd "$(dirname "$0")" && pwd); R=$(cd "$V/../.." && pwd); S=${HG_SCRATCH:-$(cd "$R/.." && pwd)/scratch}
T=$1; shift; cd $R/recomp
env HG_MAXVB=3900 HG_SPEEDLOG=300 "$@" HG_AUDIO_MUTE=1 HG_OVERLAY=1 HG_PRESENTLOG=${PL:-1} HG_MOVIES=skip HG_PADREC= \
  HG_PAD=600:cross,900:cross,1200:cross,1500:cross,1800:cross,2100:cross,2400:cross,2600:cross,2800:cross ./$HGEXE -q > $S/win_$T.log 2>&1
