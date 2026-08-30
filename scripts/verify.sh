#!/bin/zsh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

fail() { print -r -- "NOT VERIFIED: $*"; exit 1; }

[[ -x "$ROOT/swdoom" ]] || fail "no swdoom binary in $ROOT"
file "$ROOT/swdoom" | grep -q 'Mach-O' || fail "swdoom is not Mach-O"
if strings "$ROOT/swdoom" | grep -qi 'chocolate doom'; then
  fail "binary looks like Chocolate Doom"
fi
[[ -f "$ROOT/wads/freedoom1.wad" ]] || fail "missing wads/freedoom1.wad"
python3 - <<'PY' || fail "IWAD is not Freedoom"
import struct, pathlib, sys
b = pathlib.Path("/Users/stevenwoolery/SW-Doom/wads/freedoom1.wad").read_bytes()
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
print "verify.sh: binary and IWAD gates ok"
