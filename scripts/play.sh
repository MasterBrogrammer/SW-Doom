#!/bin/zsh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

mkdir -p evidence screenshots
: > evidence/pos.log

clang -o scripts/windowid scripts/windowid.c -framework CoreGraphics -framework CoreFoundation

export SWDOOM_POSLOG="$ROOT/evidence/pos.log"
export SWDOOM_PPM="$ROOT/evidence/frame.ppm"
export DOOMWADDIR="$ROOT/wads"

./swdoom -iwad "$ROOT/wads/freedoom1.wad" -warp 1 1 -skill 3 -nomonsters -walk -fire >evidence/play.log 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT

WID=""
for i in {1..40}; do
  sleep 0.25
  if ! kill -0 $PID 2>/dev/null; then
    print -- "swdoom exited early"
    cat evidence/play.log
    exit 1
  fi
  WID=$(scripts/windowid "SW-Doom" 2>/dev/null || true)
  if [[ -n "$WID" ]]; then
    break
  fi
done

if [[ -z "$WID" ]]; then
  print -- "no SW-Doom window after 10s"
  cat evidence/play.log
  exit 1
fi

sleep 3
screencapture -l "$WID" -x "$ROOT/screenshots/play.png"
print -- "window id $WID screenshot $ROOT/screenshots/play.png"

kill $PID 2>/dev/null || true
wait $PID 2>/dev/null || true
trap - EXIT

ls -la "$ROOT/screenshots/play.png" "$ROOT/evidence/pos.log" "$ROOT/evidence/frame.ppm" 2>/dev/null || true
print -- "--- pos log ---"
cat "$ROOT/evidence/pos.log" | head
print -- "--- play log tail ---"
tail -30 evidence/play.log

grep -q 'I_StartSound: id=1 ' evidence/play.log || { print -- "NOT VERIFIED: no pistol SFX (id=1)"; exit 1; }
grep -q 'I_UpdateSound: mix heard' evidence/play.log || { print -- "NOT VERIFIED: mix never heard"; exit 1; }
grep -qE 'I_StartSound: id=(18|19|34) ' evidence/play.log || { print -- "NOT VERIFIED: no world/player-channel SFX besides pistol"; exit 1; }
grep -q 'I_PlaySong: started' evidence/play.log || { print -- "NOT VERIFIED: music did not start"; exit 1; }
print -- "player pistol SFX started, mixer heard, music started"
