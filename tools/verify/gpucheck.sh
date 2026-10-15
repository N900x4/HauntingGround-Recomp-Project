#!/bin/sh
# Gate G5: the GPU against the CPU word for word over the four ranges (menus, stage 1, Castle Suite, ST_020).
# usage: gpucheck.sh TAG [batch]   (batch: HG_GSVKBATCH=1, compare after each batch; else one primitive at a time)
V=$(cd "$(dirname "$0")" && pwd); R=$(cd "$V/../.." && pwd); S=${HG_SCRATCH:-/d/ps2-work/scratch}
T=${1:-x}; B=; [ "$2" = batch ] && B=1; cd $R/recomp
P6=600:cross,900:cross,1200:cross,1500:cross,1800:cross,2100:cross
for r in a b; do rm -rf $S/mc_vk$r; mkdir -p $S/mc_vk$r; done
HG_GSVKBATCH=$B HG_GSVKCHECK=500:2400 HG_WINDOW=0 HG_MEMCARD_DIR=$S/mc_vka HG_PAD=1500:start,1800:start,2100:cross HG_MAXVB=2401 ./hgboot.exe -q -t 3000 > $S/${T}_menu.log 2>&1 &
HG_GSVKBATCH=$B HG_GSVKCHECK=3000:3060 HG_WINDOW=0 HG_MOVIES=skip HG_MEMCARD_DIR=$S/mc_vkb HG_PAD=$P6 HG_MAXVB=3061 ./hgboot.exe -q -t 3000 > $S/${T}_st1.log 2>&1 &
HG_GSVKBATCH=$B HG_GSVKCHECK=3400:3440 HG_WINDOW=0 HG_MOVIES=skip HG_PAD=$P6,2400:cross,2600:cross,2800:cross HG_MAXVB=3441 ./hgboot.exe -q -t 3000 > $S/${T}_cs.log 2>&1 &
HG_GSVKBATCH=$B HG_GSVKCHECK=7900:7940 HG_WINDOW=0 HG_PADPLAY=../recordings/20261014-200745 HG_MAXVB=7941 ./hgboot.exe -q -t 3000 > $S/${T}_st20.log 2>&1 &
wait
cd $S; for f in menu st1 cs st20; do echo "== $f"; grep -E "GSVK" ${T}_$f.log | tail -6; done
