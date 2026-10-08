#!/usr/bin/env bash
# Regenerate the recompiled C and build the native harness (run inside WSL).
#   runtime/build-harness.sh [path/to/hotd2.zip] [build dir]
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
zip="${1:-$HOME/hotd2/roms/hotd2.zip}"
out="${2:-$HOME/hotd2/hb}"
mkdir -p "$out/gen"
python3 -I -c "import sys; sys.path.insert(0, '$here/tools'); sys.argv = ['recomp', '$zip', '$out/gen']; import recomp; recomp.main()"
[ -f "$out/ic22.bin" ] || python3 -I -c "import zipfile; open('$out/ic22.bin', 'wb').write(zipfile.ZipFile('$zip').read('epr-21585.ic22'))"
[ -f "$out/cart.bin" ] || python3 -I "$here/tools/cart.py" "$zip" "$out/cart.bin"
cd "$out"
srcs=("$out"/gen/*.c "$here"/runtime/harness.c "$here"/runtime/hle.c "$here"/runtime/jvs.c
      "$here"/runtime/cart.c "$here"/runtime/ta.c "$here"/runtime/coro.c "$here"/runtime/pvr.c "$here"/runtime/glframe.c)
mkdir -p obj
printf '%s\n' "${srcs[@]}" | xargs -P"$(nproc)" -I{} sh -c 'o=obj/$(basename {} .c).o; [ "$o" -nt {} ] || gcc -O1 -g -w -I'"$here"'/runtime -I'"$out"'/gen -c {} -o "$o"'
gcc obj/*.o -lm -o harness
echo "built $out/harness"
