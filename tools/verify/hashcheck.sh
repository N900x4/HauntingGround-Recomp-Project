#!/bin/sh
export HG_INI=${HG_INI:-off}   # gates never read the user's hg.ini (set HG_INI=FILE to use one)
# Gates G1/G1g (and G2 with HG_ROUTE=cs): frame hashes of the software GS and the GPU modes against the frozen
# reference (ref/hashA.md5, or ref/cs.md5 for the Castle Suite route). Runs two at a time.
# usage: hashcheck.sh TAG            (from anywhere; outputs in $HG_SCRATCH, default D:/ps2-work/scratch)
V=$(cd "$(dirname "$0")" && pwd); R=$(cd "$V/../.." && pwd); S=${HG_SCRATCH:-/d/ps2-work/scratch}; export HG_SCRATCH=$S
T=${1:-x}; cd $R/recomp
if [ "$HG_ROUTE" = cs ]; then REF=$V/ref/cs.md5; VB=4200; else REF=$V/ref/hashA.md5; VB=4000; fi
one() { n=$1; shift; env "$@" sh $V/runhash.sh ./hgboot.exe $S/hc_${T}_$n.md5 $VB > /dev/null 2>&1; }
one sw HG_GSVK=0 & one gpu HG_GSVK=1 & wait
one interp HG_GSVK=1 HG_GSVK_INTERP=1 & one x5 HG_GSVK=1 HG_GSVK_SCALE=5 & wait
for n in sw gpu interp x5; do
  printf "%-7s %4d frames: " $n $(wc -l < $S/hc_${T}_$n.md5)
  nd=$(sort $REF $S/hc_${T}_$n.md5 | uniq -u | wc -l); if [ $nd = 0 ]; then echo "identical to $(basename $REF)"; else echo "DIFFERENT ($nd lines)"; fi
done
