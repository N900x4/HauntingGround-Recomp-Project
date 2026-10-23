#!/bin/sh
export HG_INI=${HG_INI:-off}   # gates never read the user's hg.ini (set HG_INI=FILE to use one)
# Saving from our build (release 0.1), scripted and headless. Needs the user's card with save 02 at the Castle Suite clock
# (the card the G2 route uses: ../memcards, or SAVETEST_CARD=DIR holding port0/); that card is only ever copied.
# The game's clock is fixed (HG_CLOCK), so the saves and their dates repeat from run to run.
#   s1  LOAD GAME (save 02) -> Circle at the clock -> Save current game? YES -> slot 1 -> slot 03 (new) -> saved
#   s2  the same, but the card's save directory is deleted while Fiona stands in the room (after the load, before the
#       save): the game asks "Create new data file?", YES, and writes the directory, icon.sys, icon00.ico and the save
#       file into slot 01 on what is now an empty card
#   s3  from s1's card: LOAD GAME (newest, 03) -> clock -> slot 03 again -> "Game data already present. Overwrite?" YES
#   l0  LOAD GAME of the user's own save 02 (an existing save) from a copy of the user's card
#   l1, l2, l3  LOAD GAME (newest) from the cards s1, s2, s3 wrote
# Pass: every save says "successfully saved" (checked by the files: written, 77,168 bytes, icon.sys and icon00.ico
# byte-identical to the user's card, which PCSX2 wrote); after loading, every frame in the room (VBlank 2610 on) of l1, l2,
# l3 is identical to l0, and Fiona's stage and position are those at the moment of saving; the card files' md5 sums
# match ref/save.md5 (the save data is deterministic with the fixed clock).
# usage: savetest.sh TAG        outputs in $HG_SCRATCH/save_TAG (default HG_SCRATCH D:/ps2-work/scratch); run from the
#                               MSYS2 UCRT64 shell; about 1-2 minutes. HG_GSVK=0 for the software renderer.
V=$(cd "$(dirname "$0")" && pwd); R=$(cd "$V/../.." && pwd); S=${HG_SCRATCH:-/d/ps2-work/scratch}
T=${1:-x}; CARD=${SAVETEST_CARD:-$R/memcards}; W=$S/save_$T
[ -f "$CARD/port0/BASLUS-21075HG/BASLUS-21075HG" ] || { echo "savetest: no Haunting Ground save on $CARD/port0"; exit 1; }
rm -rf $W; mkdir -p $W; cd $R/recomp
LOAD=600:cross,900:cross,1200:cross,1500:cross,1800:cross,2100:cross,2400:cross,2600:cross,2800:cross
CLOCK=1791997665
card() { rm -rf $W/c_$1; mkdir -p $W/c_$1; cp -a $2/port0 $W/c_$1/; }
run() {   # NAME MAXVB PAD: card $W/c_NAME, frames every 10 VBlanks hashed into $W/NAME.md5
  n=$1; mv=$2; p=$3; mkdir -p $W/f_$n
  env HG_GAME_DIR="$R/game" HG_MOVIES=skip HG_MEMCARD_DIR=$W/c_$n HG_INI=off HG_CLOCK=$CLOCK HG_GSVK=${HG_GSVK:-1} \
      HG_PAD=$LOAD$p HG_POSLOG=300 HG_MAXVB=$mv HG_FRAMES=$W/f_$n HG_FRAME_EVERY=10 HG_WINDOW=0 ./hgboot.exe -q -t 6000 > $W/$n.log 2>&1
  (cd $W/f_$n && md5sum *.ppm) | sort -k2 > $W/$n.md5; rm -rf $W/f_$n
}
SAVE=,3060:circle,3150:cross,3250:cross,3450:cross
card s1 $CARD; card s2 $CARD; card l0 $CARD
run s1 4300 $SAVE,3550:down,3620:cross,3800:cross &
run s2 13200 ,12060:circle,12150:cross,12250:cross,12450:cross,12500:left,12550:cross,12800:cross,13000:cross &
# s2: delete the save directory once Fiona is in the room (VBlank 3000-9900; the save starts at 12060)
while ! grep -qE "^pos: VBlank [3-9][0-9]00 stage 21 " $W/s2.log 2>/dev/null; do sleep 0.2; done
rm -rf $W/c_s2/port0/BASLUS-21075HG; DEL=$(grep "^pos:" $W/s2.log | tail -1 | awk '{print $3}')
[ -e $W/c_s2/port0/BASLUS-21075HG ] && DEL=failed
wait
card s3 $W/c_s1; card l1 $W/c_s1; card l2 $W/c_s2
run s3 4300 $SAVE,3620:cross,3700:left,3750:cross,3950:cross & run l0 3600 "" & wait
card l3 $W/c_s3
run l1 3600 "" & run l2 3600 "" & wait
run l3 3600 "" & wait
F=0; fail() { echo "FAIL: $*"; F=1; }
echo "s2: save directory deleted at VBlank $DEL"; case $DEL in failed|"") fail "s2: the save directory could not be deleted";; esac
for c in s1 s2 s3; do
  d=$W/c_$c/port0/BASLUS-21075HG
  [ -f $d/BASLUS-21075HG ] || { fail "$c: no save file"; continue; }
  [ $(wc -c < $d/BASLUS-21075HG) = 77168 ] || fail "$c: save file size $(wc -c < $d/BASLUS-21075HG)"
  for i in icon.sys icon00.ico; do
    [ "$(md5sum < $d/$i)" = "$(md5sum < $CARD/port0/BASLUS-21075HG/$i)" ] || fail "$c: $i differs from the user's card"
  done
  [ "$(md5sum < $d/BASLUS-21075HG)" = "$(md5sum < $CARD/port0/BASLUS-21075HG/BASLUS-21075HG)" ] && fail "$c: save file unchanged"
done
for c in s1 s2 s3; do for i in BASLUS-21075HG icon.sys icon00.ico; do
  echo "$(md5sum < $W/c_$c/port0/BASLUS-21075HG/$i | cut -c1-32) $c/$i"; done; done > $W/cards.md5
awk '{ split($2, a, /[_.]/); if (a[2] + 0 >= 2610) print }' $W/l0.md5 > $W/room_l0.md5
for l in l1 l2 l3; do
  awk '{ split($2, a, /[_.]/); if (a[2] + 0 >= 2610) print }' $W/$l.md5 > $W/room_$l.md5
  nd=$(sort $W/room_l0.md5 $W/room_$l.md5 | uniq -u | wc -l)
  printf "%s: %d frames in the room after loading, " $l $(wc -l < $W/room_$l.md5)
  if [ $nd = 0 ]; then echo "identical to loading the user's save 02 (l0)"; else echo "DIFFERENT ($nd lines)"; F=1; fi
done
at() { grep "^pos: VBlank $2 " $W/$1.log | sed 's/.*stage/stage/'; }
P0=$(at s1 3600); echo "s1 at the save (VBlank 3600): $P0"
for l in l0 l1 l2 l3; do p=$(at $l 3300); echo "$l after loading (VBlank 3300): $p"; [ "$p" = "$P0" ] || fail "$l position"; done
[ "$(at s2 12300)" = "$P0" ] || fail "s2 position at the save: $(at s2 12300)"
[ "$(at s3 3600)" = "$P0" ] || fail "s3 position at the save: $(at s3 3600)"
cat $W/cards.md5
if [ -f $V/ref/save.md5 ]; then
  nd=$(sort $V/ref/save.md5 $W/cards.md5 | uniq -u | wc -l)
  if [ $nd = 0 ]; then echo "card files identical to ref/save.md5"; else echo "card files DIFFERENT from ref/save.md5 ($nd lines)"; F=1; fi
fi
[ $F = 0 ] && echo "savetest $T: PASS" || echo "savetest $T: FAIL"
