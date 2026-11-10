#!/usr/bin/env bash
# Observe-only pass capture (KYTY_PASS_CAPTURE) for finding a title's TAA pass. Private saves; production untouched.
# usage: run-capture.sh <demons|astro|yotei> [seconds] [extra kyty args...]
#   START/EVERY/WINDOWS/FRAMES: capture windows (defaults 45 s, 20 s, 4, 6 flips)
#   PATHS=auto: descriptor paths computed from window 0's shaders and used from window 1 on (one launch)
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")" && pwd)
which=${1:?game}; shift
secs=${1:-140}; shift || true
case "$which" in
  demons) game='/home/david/Downloads/Demons Souls (v01.007) (Deluxe Edition) – BP 4.xx – PPSA01341 – FPKG/EP9000-PPSA01341_00-DEMONSSOULS00000-files/eboot.bin' ;;
  astro)  game='/home/david/Downloads/[PS5] ASTRO BOT (2024) (PPSA21567) (EU) v1.018/[PS5] ASTRO BOT (2024) (PPSA21567) (EU) v1.018.zar!/PPSA21567' ;;
  yotei)  game='/home/david/Downloads/Ghost of Yōtei Complete Edition/UP9000-PPSA26344_00-GHOST2SHIP000000-files/eboot.bin' ;;
  *) echo "unknown game $which" >&2; exit 2 ;;
esac
exe="$root/build/${EXE:-kyty_emulator_auto_upscaler}"
if ps -eo comm= | grep -q '^kyty_emulator'; then echo 'Refusing: an emulator is already running' >&2; exit 2; fi
mkdir -p "$root/logs"
run=$(mktemp -d "$root/logs/capture-$which-XXXXXX")
while IFS= read -r name; do case "$name" in KYTY_*|ENABLE_LSFGVK*|LSFGVK_*) unset "$name";; esac; done < <(compgen -e)
unset VK_ADD_IMPLICIT_LAYER_PATH
export DISABLE_LSFGVK=1 DISABLE_LSFGVK_EXPERIMENTAL=1
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" DISPLAY=${DISPLAY:-:0} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-1} SDL_VIDEODRIVER=x11
export KYTY_NATIVE_GPU_FLIP_STATS=1
export KYTY_PASS_CAPTURE="$run" KYTY_PASS_CAPTURE_START=${START:-45} KYTY_PASS_CAPTURE_EVERY=${EVERY:-20}
export KYTY_PASS_CAPTURE_WINDOWS=${WINDOWS:-4} KYTY_PASS_CAPTURE_FRAMES=${FRAMES:-6}
if [[ -n ${PASS:-} ]]; then export $PASS; fi
# PATHS=auto: one launch for both capture steps. Window 0 collects the shaders; as soon as it is written their
# descriptor paths (isa_paths.py, from each shader's own instructions) go to $run/paths.txt, which the emulator
# re-reads as each later window opens, so those windows dump the tables and constant buffers on those paths.
if [[ ${PATHS:-} == auto ]]; then
  export KYTY_PASS_CAPTURE_PATHS="$run/paths.txt"
  tools="$(cd -- "$root/../../tools/upscaler-scan" && pwd)"
  ( for _ in $(seq 1 600); do
      if grep -q '^\[pass capture\] window 0 written' "$run/emulator.log" 2>/dev/null; then
        python3 -I "$tools/isa_paths.py" --capture-paths "$run/paths.tmp" "$run"/shader-*.bin >> "$run/paths.log" 2>&1 &&
          mv -f "$run/paths.tmp" "$run/paths.txt" && echo "$(date +%T) paths written" >> "$run/paths.log"
        break
      fi
      sleep 1
    done ) &
fi
cd "$root/runtime"
args=(--game "$game" --present-mode Fifo --vblank-frequency 60 --gpu 0
  --shader-optimization-type Performance --console-language 1 --user-name Kyty --user-id 1000 --keyboard-player 0
  --shader-log-direction Silent --printf-direction Silent --screen-width 3840 --screen-height 2160 --fullscreen
  --no-splash --skip-notice-screen --hide-cursor)
printf '%s\n' "$which" "$(sha256sum "$exe" | cut -d' ' -f1)" "${args[@]}" "$@" > "$run/argv.txt"
echo "run=$run"
set +e
timeout --signal=TERM --kill-after=8s "${secs}s" "$exe" "${args[@]}" "$@" > "$run/emulator.log" 2>&1 &
pid=$!
# SHOTS="40 80": screenshots (KDE: spectacle). KEYS="45:j 55:j 60:Return": keys sent to the focused game window.
if [[ -n ${SHOTS:-} ]]; then for t in $SHOTS; do (sleep "$t"; spectacle -b -n -f -o "$run/screen-${t}s.png" >/dev/null 2>&1 || grim "$run/screen-${t}s.png" 2>/dev/null) & done; fi
# Keys go only to the emulator's own window (found by its PID; XSendEvent), never to whatever has the focus.
if [[ -n ${KEYS:-} ]]; then for k in $KEYS; do (sleep "${k%%:*}"; emu=$(pgrep -n -x kyty_emulator_au || pgrep -n -f "$exe"); wid=$(xdotool search --pid "$emu" 2>/dev/null | tail -1); if [[ -n $wid ]]; then xdotool key --window "$wid" --delay 120 "${k#*:}" >/dev/null 2>&1; echo "$(date +%T) ${k} -> window $wid (pid $emu)" >> "$run/keys.txt"; else echo "$(date +%T) ${k}: no game window (pid $emu)" >> "$run/keys.txt"; fi) & done; fi
wait $pid; rc=$?
set -e
[[ -f flips.csv ]] && mv flips.csv "$run/flips.csv"
# Saves: the private copy only (the production saves are never opened).
echo "exit=$rc log=$run/emulator.log"
[[ $rc == 0 || $rc == 124 || $rc == 143 ]]
