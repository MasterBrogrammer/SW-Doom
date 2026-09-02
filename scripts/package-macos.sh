#!/bin/zsh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

fail() { print -r -- "package-macos: $*"; exit 1; }

APP="$ROOT/dist/SW-Doom.app"
DMG="$ROOT/dist/SW-Doom.dmg"
STAGE="$ROOT/dist/dmg-stage"
CONTENTS="$APP/Contents"
MACOS="$CONTENTS/MacOS"
FRAMEWORKS="$CONTENTS/Frameworks"
RESOURCES="$CONTENTS/Resources"

rm -rf "$APP" "$STAGE"
mkdir -p "$MACOS" "$FRAMEWORKS" "$RESOURCES" "$STAGE"

[[ -x "$ROOT/swdoom" ]] || make -C "$ROOT/linuxdoom-1.10"
file "$ROOT/swdoom" | grep -q 'Mach-O' || fail "swdoom is not Mach-O"

[[ -f "$ROOT/wads/freedoom1.wad" ]] || fail "missing wads/freedoom1.wad"
IWAD_PATH="$ROOT/wads/freedoom1.wad" python3 - <<'PY' || fail "IWAD is not Freedoom with E1M1"
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
print("IWAD Freedoom with E1M1")
PY

SDL2_SRC="$(otool -L "$ROOT/swdoom" | awk '/libSDL2/{print $1; exit}')"
[[ -n "$SDL2_SRC" && -f "$SDL2_SRC" ]] || fail "cannot resolve SDL2 from swdoom"

SDL3_DIR=""
while IFS= read -r rpath; do
  [[ -z "$rpath" ]] && continue
  resolved="$rpath"
  if [[ "$rpath" == @loader_path/* ]]; then
    resolved="$(dirname "$SDL2_SRC")/${rpath#@loader_path/}"
  elif [[ "$rpath" == @executable_path/* ]]; then
    resolved="$(dirname "$ROOT/swdoom")/${rpath#@executable_path/}"
  fi
  if [[ -f "$resolved/libSDL3.0.dylib" ]]; then
    SDL3_DIR="$resolved"
    break
  fi
  if [[ -f "$resolved/libSDL3.dylib" ]]; then
    SDL3_DIR="$resolved"
    break
  fi
done < <(otool -l "$SDL2_SRC" | awk '/cmd LC_RPATH/{p=1} p&&/path/{sub(/^.*path /,""); sub(/ \(offset.*$/,""); print; p=0}')

if [[ -z "$SDL3_DIR" ]]; then
  for cand in /opt/homebrew/opt/sdl3/lib /usr/local/opt/sdl3/lib; do
    if [[ -f "$cand/libSDL3.0.dylib" ]]; then
      SDL3_DIR="$cand"
      break
    fi
  done
fi
[[ -n "$SDL3_DIR" ]] || fail "cannot resolve SDL3 lib dir from SDL2 rpaths"

if [[ -f "$SDL3_DIR/libSDL3.0.dylib" ]]; then
  SDL3_SRC="$SDL3_DIR/libSDL3.0.dylib"
elif [[ -L "$SDL3_DIR/libSDL3.dylib" || -f "$SDL3_DIR/libSDL3.dylib" ]]; then
  SDL3_SRC="$(realpath "$SDL3_DIR/libSDL3.dylib")"
else
  fail "no libSDL3.0.dylib under $SDL3_DIR"
fi
[[ -f "$SDL3_SRC" ]] || fail "SDL3 source missing: $SDL3_SRC"

# source|basename|install_name_id|optional_symlink|strip_cellar_rpaths
typeset -a DYLIBS
DYLIBS=(
  "${SDL2_SRC}|libSDL2-2.0.0.dylib|@executable_path/../Frameworks/libSDL2-2.0.0.dylib||1"
  "${SDL3_SRC}|libSDL3.0.dylib|@executable_path/../Frameworks/libSDL3.0.dylib|libSDL3.dylib|"
)

cp "$ROOT/swdoom" "$MACOS/swdoom"
chmod +w "$MACOS/swdoom"
install_name_tool -change "$SDL2_SRC" \
  "@executable_path/../Frameworks/libSDL2-2.0.0.dylib" \
  "$MACOS/swdoom"

for entry in "${DYLIBS[@]}"; do
  IFS='|' read -r src base id symlink strip_rpaths <<<"$entry"
  dest="$FRAMEWORKS/$base"
  cp "$src" "$dest"
  chmod +w "$dest"
  install_name_tool -id "$id" "$dest"
  if [[ "$strip_rpaths" == 1 ]]; then
    while IFS= read -r rpath; do
      [[ -z "$rpath" ]] && continue
      if [[ "$rpath" == *opt/sdl3* || "$rpath" == *homebrew* || "$rpath" == *Cellar* ]]; then
        install_name_tool -delete_rpath "$rpath" "$dest"
      fi
    done < <(otool -l "$dest" | awk '/cmd LC_RPATH/{p=1} p&&/path/{sub(/^.*path /,""); sub(/ \(offset.*$/,""); print; p=0}')
  fi
  if [[ -n "$symlink" ]]; then
    ln -sfn "$base" "$FRAMEWORKS/$symlink"
  fi
  codesign --force --sign - "$dest"
done

clang -O2 -o "$MACOS/SW-Doom" "$ROOT/scripts/macos-launcher.c"
codesign --force --sign - "$MACOS/swdoom"
codesign --force --sign - "$MACOS/SW-Doom"

cp "$ROOT/wads/freedoom1.wad" "$RESOURCES/freedoom1.wad"
cp "$ROOT/wads/COPYING.txt" "$RESOURCES/COPYING.txt"
cp "$ROOT/LICENSE.TXT" "$RESOURCES/LICENSE.TXT"

cat > "$CONTENTS/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key>
	<string>SW-Doom</string>
	<key>CFBundleIdentifier</key>
	<string>com.masterbrogrammer.swdoom</string>
	<key>CFBundleName</key>
	<string>SW-Doom</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleShortVersionString</key>
	<string>1.0</string>
	<key>CFBundleVersion</key>
	<string>1</string>
	<key>LSMinimumSystemVersion</key>
	<string>11.0</string>
	<key>NSHighResolutionCapable</key>
	<true/>
</dict>
</plist>
PLIST

codesign --force --deep --sign - "$APP"

fw_entries=("$FRAMEWORKS"/*(N))
(( ${#fw_entries[@]} == 3 )) || fail "Frameworks must have exactly 3 entries, got ${#fw_entries[@]}"
[[ -f "$FRAMEWORKS/libSDL2-2.0.0.dylib" ]] || fail "missing bundled SDL2"
[[ -f "$FRAMEWORKS/libSDL3.0.dylib" ]] || fail "missing bundled SDL3"
[[ -L "$FRAMEWORKS/libSDL3.dylib" ]] || fail "missing libSDL3.dylib symlink"
du -sh "$FRAMEWORKS"

cp -R "$APP" "$STAGE/SW-Doom.app"
ln -sfn /Applications "$STAGE/Applications"

hdiutil create -volname SW-Doom -srcfolder "$STAGE" -ov -format UDZO "$DMG"
rm -rf "$STAGE"

print -r -- "package-macos: wrote $APP"
print -r -- "package-macos: wrote $DMG"
