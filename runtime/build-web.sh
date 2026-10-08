#!/usr/bin/env bash
# Build the browser version of the recompiled game (run inside WSL).
#   runtime/build-web.sh [path/to/hotd2.zip] [build dir]
# Output: web/game/hotd2.js + hotd2.wasm (+ worker), never committed.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
zip="${1:-$HOME/hotd2/roms/hotd2.zip}"
out="${2:-$HOME/hotd2/wb}"
source "$HOME/hotd2/emsdk/emsdk_env.sh" >/dev/null 2>&1
mkdir -p "$out/gen" "$out/obj"
python3 -I -c "import sys; sys.path.insert(0, '$here/tools'); sys.argv = ['recomp', '$zip', '$out/gen']; import recomp; recomp.main()"
srcs=("$out"/gen/*.c "$here"/runtime/web.c "$here"/runtime/hle.c "$here"/runtime/jvs.c "$here"/runtime/cart.c
      "$here"/runtime/ta.c "$here"/runtime/coro.c "$here"/runtime/pvr.c "$here"/runtime/glframe.c)
cflags="-O2 -pthread -w -I$here/runtime -I$out/gen"
printf '%s\n' "${srcs[@]}" | xargs -P"$(nproc)" -I{} sh -c 'o='"$out"'/obj/$(basename {} .c).o; [ "$o" -nt {} ] || emcc '"$cflags"' -c {} -o "$o"'
mkdir -p "$here/web/game"
emcc -O2 -pthread "$out"/obj/*.o -o "$here/web/game/hotd2.js" \
	-sENVIRONMENT=web,worker -sPTHREAD_POOL_SIZE=2 \
	-sINITIAL_MEMORY=768MB -sMAXIMUM_MEMORY=2GB -sALLOW_MEMORY_GROWTH \
	-sSTACK_SIZE=1MB -sDEFAULT_PTHREAD_STACK_SIZE=8MB \
	-sEXPORTED_FUNCTIONS=_malloc \
	-sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32,HEAPF32,HEAP32 \
	-sINVOKE_RUN=0 -sEXIT_RUNTIME=0
echo "built $here/web/game/hotd2.js"
