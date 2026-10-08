#!/usr/bin/env bash
# Discover entry points the static analysis misses by playing the game natively:
# coin + start, then shoot around. Every run that stops on an unknown target
# appends it to data/seeds.txt and rebuilds. (run inside WSL)
#   runtime/seed-play.sh [iterations] [frames] [build dir]
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
iters="${1:-40}"
frames="${2:-6000}"
out="${3:-$HOME/hotd2/hb2}"
script="1300:coin,1330:start,1336:-start"
for f in $(seq 1500 30 "$frames"); do
	x=$(( (f * 37) % 600 + 20 )); y=$(( (f * 23) % 440 + 20 ))
	script="$script,$f:aim=$x/$y,$((f+1)):fire,$((f+4)):-fire"
	if (( f % 300 == 0 )); then script="$script,$((f+8)):reload,$((f+12)):-reload"; fi
done
cd "$out"
for i in $(seq 1 "$iters"); do
	[ -f missing.txt ] && grep -v "^00000000" missing.txt >> "$here/data/seeds.txt"
	rm -f missing.txt
	bash "$here/runtime/build-harness.sh" "$HOME/hotd2/roms/hotd2.zip" "$out" 2>&1 | grep -E "rror:"
	HOTD2_INTERP=1 HOTD2_TRACE=0 HOTD2_FRAMES="$frames" HOTD2_INPUT="$script" timeout 1200 ./harness ic22.bin > run.log 2>&1
	echo "iter $i: $(grep "^stop" run.log | cut -c1-40) | $(grep "^--- frames" run.log | cut -c5-60) | new targets $(cat missing.txt 2>/dev/null | wc -l)"
	[ -f missing.txt ] || break
	if grep -q "^00000000" missing.txt; then echo "NULL CALL:"; cat missing.txt; break; fi
done
