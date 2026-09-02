#!/bin/zsh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

fail() { print -r -- "NOT VERIFIED: $*"; exit 1; }

APP="$ROOT/dist/SW-Doom.app"
DMG="$ROOT/dist/SW-Doom.dmg"
MACOS="$APP/Contents/MacOS"
FRAMEWORKS="$APP/Contents/Frameworks"
RESOURCES="$APP/Contents/Resources"
ENGINE="$MACOS/swdoom"
LAUNCHER="$MACOS/SW-Doom"

[[ -d "$APP" ]] || fail "missing $APP"
[[ -f "$DMG" ]] || fail "missing $DMG"

otool -L "$ENGINE" | grep -E '/opt/homebrew|/usr/local/opt' && fail "swdoom still links brew paths" || true
otool -L "$ENGINE" | grep -q '@executable_path/../Frameworks/libSDL2-2.0.0.dylib' \
  || fail "swdoom missing rewritten SDL2 install name"

for lib in "$FRAMEWORKS"/libSDL2-2.0.0.dylib "$FRAMEWORKS"/libSDL3.0.dylib; do
  [[ -f "$lib" ]] || fail "missing $lib"
  otool -L "$lib" | grep -E '/opt/homebrew|/usr/local/opt' && fail "$lib still references brew" || true
  otool -l "$lib" | awk '/cmd LC_RPATH/{p=1} p&&/path/{sub(/^.*path /,""); sub(/ \(offset.*$/,""); print; p=0}' \
    | grep -E 'homebrew|Cellar|/opt/homebrew|/usr/local/opt|opt/sdl3' \
    && fail "$lib still has cellar rpath" || true
done

fw=("$FRAMEWORKS"/*(N))
(( ${#fw[@]} == 3 )) || fail "Frameworks has ${#fw[@]} entries, want 3"
[[ -f "$FRAMEWORKS/libSDL2-2.0.0.dylib" ]] || fail "missing libSDL2-2.0.0.dylib"
[[ -f "$FRAMEWORKS/libSDL3.0.dylib" ]] || fail "missing libSDL3.0.dylib"
[[ -L "$FRAMEWORKS/libSDL3.dylib" ]] || fail "libSDL3.dylib is not a symlink"
[[ "$(readlink "$FRAMEWORKS/libSDL3.dylib")" == "libSDL3.0.dylib" ]] \
  || fail "libSDL3.dylib must point at libSDL3.0.dylib"

[[ -f "$RESOURCES/freedoom1.wad" ]] || fail "missing Resources/freedoom1.wad"
[[ -f "$RESOURCES/COPYING.txt" ]] || fail "missing Resources/COPYING.txt"
[[ -f "$RESOURCES/LICENSE.TXT" ]] || fail "missing Resources/LICENSE.TXT"

IWAD_PATH="$RESOURCES/freedoom1.wad" python3 - <<'PY' || fail "bundled IWAD is not Freedoom with E1M1"
import os, struct, pathlib, sys
b = pathlib.Path(os.environ["IWAD_PATH"]).read_bytes()
ident, num, ofs = struct.unpack("<4sii", b[:12])
if ident != b"IWAD":
    sys.exit(1)
names = []
for i in range(num):
    name = struct.unpack("<ii8s", b[ofs+i*16:ofs+(i+1)*16])[2].split(b"\0",1)[0]
    names.append(name)
if b"FREEDOOM" not in names or b"E1M1" not in names:
    sys.exit(1)
print("bundled IWAD Freedoom with E1M1")
PY

codesign -dv "$APP" >/dev/null 2>&1 || fail "codesign -dv failed on app"

LOG="$ROOT/dist/bundle-launch.log"
: > "$LOG"
DYLD_PRINT_LIBRARIES=1 "$LAUNCHER" -nomonsters -warp 1 1 \
  >"$LOG" 2>&1 &
LPID=$!
sleep 4
kill "$LPID" 2>/dev/null || true
wait "$LPID" 2>/dev/null || true

grep -q 'Frameworks/libSDL2-2.0.0.dylib' "$LOG" || fail "dyld did not load bundled SDL2"
grep -q 'Frameworks/libSDL3' "$LOG" || fail "dyld did not load bundled SDL3"
grep -E '/opt/homebrew/.*/(libSDL|sdl)' "$LOG" && fail "dyld loaded brew SDL" || true
grep -q ' adding .*freedoom1.wad' "$LOG" || fail "engine did not add freedoom1.wad"

ATTACH_OUT="$(hdiutil attach -nobrowse "$DMG")"
DEV="$(print -r -- "$ATTACH_OUT" | awk '/\/Volumes\//{print $1; exit}')"
MNT="$(print -r -- "$ATTACH_OUT" | awk '/\/Volumes\//{print $NF; exit}')"
[[ -n "$MNT" && -d "$MNT" ]] || fail "hdiutil attach produced no mount"
trap 'hdiutil detach "$MNT" >/dev/null 2>&1 || hdiutil detach "$DEV" >/dev/null 2>&1 || true' EXIT
[[ -d "$MNT/SW-Doom.app" ]] || fail "dmg missing SW-Doom.app"
[[ -L "$MNT/Applications" ]] || fail "dmg missing Applications symlink"
hdiutil detach "$MNT" >/dev/null 2>&1 || hdiutil detach "$DEV" >/dev/null 2>&1 || fail "detach failed"
trap - EXIT

print -r -- "verify-macos-bundle.sh: ok"
