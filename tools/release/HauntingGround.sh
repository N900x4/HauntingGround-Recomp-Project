#!/bin/sh
# Haunting Ground Recomp: the Linux launcher (what HauntingGround.exe, tools/release/launcher.c, does on Windows).
# First start: asks for the player's own disc image and runs engine/src/tools/release/setup.py with the system's
# python3: the image is checked (read-only), the game files are copied out and the game is translated and compiled with
# the bundled gcc into bin/hgboot (a few minutes; logs/launcher.log). Then it writes a default hg.ini (the GPU
# renderer; the game falls back to the software renderer by itself if Vulkan fails) and starts the game in bin/ with
# HG_INI=../hg.ini and HG_PADREC=auto, its output in logs/play.log (the one before in logs/play.prev.log). Later
# starts go straight to the game; a new release unpacked over the old one (engine/version.txt differs from
# bin/build.txt) builds again, reusing the verified game files.
# Dialogs: kdialog (KDE, Steam Deck desktop mode) or zenity (GNOME and others), else the terminal.
# Needs: python3 (3.8 or later), the SDL2 library (libSDL2-2.0.so.0: installed with most desktops and with Steam's
# dependencies; Debian/Ubuntu: libsdl2-2.0-0, Fedora: SDL2, Arch: sdl2 or sdl2-compat) and a Vulkan driver for the GPU
# renderer.
# Options: --setup (build again), --iso PATH (use this image, no file picker), --no-start, --batch (no dialogs;
# exit status 0 built/played, 1 failed, 2 the game stopped unexpectedly).
ROOT=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
cd "$ROOT" || exit 1
mkdir -p logs 2>/dev/null
LOG=logs/launcher.log
log() { printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$LOG"; }
BATCH=0; FORCE=0; NOSTART=0; ISO=
while [ $# -gt 0 ]; do
  case "$1" in
    --batch) BATCH=1;; --setup) FORCE=1;; --no-start) NOSTART=1;;
    --iso) shift; ISO=$1; FORCE=1;;
  esac; shift
done
TITLE="Haunting Ground Recomp"
have() { command -v "$1" >/dev/null 2>&1; }
ui() {   # ui KIND TEXT   (KIND: info | error)
  [ $BATCH = 1 ] && { echo "$2"; return; }
  if have kdialog; then [ "$1" = error ] && kdialog --title "$TITLE" --error "$2" || kdialog --title "$TITLE" --msgbox "$2"
  elif have zenity; then zenity --"$1" --title="$TITLE" --no-wrap --text="$2" 2>/dev/null
  else printf '\n%s\n' "$2"; [ -t 0 ] && { printf 'Press Enter. '; read -r _; }; fi
}
pick_iso() {   # prints the chosen path
  if have kdialog; then kdialog --title "Choose your Haunting Ground disc image (.iso)" --getopenfilename "$HOME" "*.iso *.ISO|Disc images (*.iso)"
  elif have zenity; then zenity --file-selection --title="Choose your Haunting Ground disc image (.iso)" --file-filter="Disc images | *.iso *.ISO" 2>/dev/null
  elif [ -t 0 ]; then printf 'Path of your Haunting Ground disc image (.iso): ' >&2; read -r p; printf '%s' "$p"
  fi
}
log "launcher start in $ROOT ($(uname -sr))"
[ -w "$ROOT" ] || { ui error "The game folder is not writable:\n$ROOT\n\nPlease move the folder somewhere in your home folder."; exit 1; }
# python3 and SDL2
PY=$(command -v python3)
if [ -z "$PY" ] || ! "$PY" -c 'import sys; sys.exit(sys.version_info < (3, 8))' 2>/dev/null; then
  ui error "Python 3.8 or later is needed for the first start (it builds the game).\nPlease install python3 with your distribution's package manager."; log "no python3"; exit 1
fi
sdl_ok() { ldconfig -p 2>/dev/null | grep -q 'libSDL2-2.0.so.0' && return 0
  for d in /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/local/lib; do [ -e "$d/libSDL2-2.0.so.0" ] && return 0; done; return 1; }
if ! sdl_ok; then
  ui error "The SDL2 library is missing (libSDL2-2.0.so.0).\nPlease install it: Debian/Ubuntu: libsdl2-2.0-0, Fedora: SDL2, Arch: sdl2 (or sdl2-compat)."; log "no SDL2"; exit 1
fi
# build when needed
VER=$(cat engine/version.txt 2>/dev/null)
need_build() { [ $FORCE = 1 ] && return 0; [ -x bin/hgboot ] || return 0; grep -q "^release=$VER\$" bin/build.txt 2>/dev/null || return 0; return 1; }
run_setup() {   # run_setup [ISO]
  set -- "$PY" -u -I engine/src/tools/release/setup.py --root "$ROOT" ${1:+--iso "$1"}
  log "setup: $*"
  if [ $BATCH = 0 ] && have kdialog && [ -z "$NOTERM" ]; then
    # progress: kdialog's progress bar fed from setup.py's @progress / @stage lines
    DB=$(kdialog --title "$TITLE" --progressbar "Preparing the game..." 100 2>/dev/null)
    "$@" 2>&1 | while IFS= read -r l; do
      printf '%s\n' "$l" >> "$LOG"
      case "$l" in
        @progress\ *) [ -n "$DB" ] && qdbus $DB Set "" value "${l#@progress }" >/dev/null 2>&1;;
        @stage\ *) [ -n "$DB" ] && qdbus $DB setLabelText "${l#@stage }" >/dev/null 2>&1;;
        @need-iso|@error\ *|@done) printf '%s\n' "$l" > logs/setup.result;;
      esac
    done
    [ -n "$DB" ] && qdbus $DB close >/dev/null 2>&1
  elif [ $BATCH = 0 ] && have zenity; then
    "$@" 2>&1 | while IFS= read -r l; do
      printf '%s\n' "$l" >> "$LOG"
      case "$l" in
        @progress\ *) echo "${l#@progress }";; @stage\ *) echo "# ${l#@stage }";;
        @need-iso|@error\ *|@done) printf '%s\n' "$l" > logs/setup.result;;
      esac
    done | zenity --progress --title="$TITLE" --text="Preparing the game..." --auto-close --no-cancel 2>/dev/null
  else
    "$@" 2>&1 | while IFS= read -r l; do
      printf '%s\n' "$l" >> "$LOG"
      case "$l" in
        @stage\ *) echo "${l#@stage }" >&2;;   # (to the terminal: stdout is the result)
        @need-iso|@error\ *|@done) printf '%s\n' "$l" > logs/setup.result;;
      esac
    done
  fi
  cat logs/setup.result 2>/dev/null
}
if need_build; then
  rm -f logs/setup.result
  res=$(run_setup "$ISO")
  while [ "$res" = "@need-iso" ]; do
    [ $BATCH = 1 ] && { echo "the disc image is needed: --iso PATH"; exit 1; }
    ui info "Welcome! The first start builds the game from your own copy of Haunting Ground.\n\nPlease choose the disc image (.iso) you dumped from your North American (NTSC-U, SLUS-21075) disc.\nIt is only read, never changed."
    ISO=$(pick_iso)
    [ -z "$ISO" ] && { log "no image chosen"; exit 1; }
    rm -f logs/setup.result; res=$(run_setup "$ISO")
  done
  case "$res" in
    @done) log "setup done";;
    @error*) ui error "${res#@error }\n\nThe details are in $ROOT/logs/launcher.log."; log "setup failed: $res"; exit 1;;
    *) ui error "The setup stopped unexpectedly.\nThe details are in $ROOT/logs/launcher.log."; log "setup ended without a result"; exit 1;;
  esac
fi
[ $NOSTART = 1 ] && exit 0
# hg.ini: the GPU renderer by default
if [ ! -e hg.ini ]; then
  printf '# Haunting Ground Recomp settings (the game'"'"'s Options menu writes the rest)\nrenderer = gpu        # the GPU (Vulkan); the game falls back to software if it cannot start it\n' > hg.ini
  log "hg.ini written: renderer gpu"
fi
# the game
[ -e logs/play.log ] && mv -f logs/play.log logs/play.prev.log
log "game start"
( cd bin && HG_INI=../hg.ini HG_PADREC=${HG_PADREC:-auto} exec ./hgboot -q ) > logs/play.log 2>&1
code=$?
stopline=$(grep -E '^(==== )?STOP: ' logs/play.log | tail -1 | sed 's/^==== //; s/^STOP: //')
crash=$(grep -E '^==== CRASH' logs/play.log | tail -1)
log "game ended: status $code; stop: ${stopline:-none}; ${crash}"
case "$stopline" in "window closed"*) exit 0;; esac
why=${crash:-${stopline:-"the game ended with status $code"}}
ui error "The game stopped unexpectedly: $why\n\nIf you report it, please attach logs/play.log and logs/launcher.log from the folder\n$ROOT"
[ $BATCH = 1 ] && exit 2
exit 0
