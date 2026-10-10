#!/usr/bin/env bash
# Discover entry points the static analysis misses by playing the game natively:
# coin + start, then shoot around. Every run that stops on an unknown target
# appends it to data/seeds.txt and rebuilds. (run inside WSL)
# Player 1 never runs out of lives (0x0C3D0430 held at 3) and boss health (float at
# 0x0C3D11FC) is held at 0 once the first boss fight starts, so runs reach later
# stages; set HOTD2_FREEZE= (empty) to play fair.
#   runtime/seed-play.sh [iterations] [frames] [build dir]
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
iters="${1:-40}"
frames="${2:-6000}"
out="${3:-$HOME/hotd2/hb2}"
script="1300:coin,1310:coin,1330:start,1336:-start,1400:start,1410:-start,1470:start,1480:-start"
for f in $(seq 1500 30 "$frames"); do
	x=$(( (f * 37) % 600 + 20 )); y=$(( (f * 23) % 440 + 20 ))
	script="$script,$f:aim=$x/$y,$((f+1)):fire,$((f+4)):-fire"
	if (( f % 300 == 0 )); then script="$script,$((f+8)):reload,$((f+12)):-reload"; fi
done
cd "$out"
export HOTD2_FREEZE="${HOTD2_FREEZE-0C3D0430=3@1500,0C3D11FC=0@27000,0C3D11FD=0@27000,0C3D11FE=0@27000,0C3D11FF=0@27000}"
for i in $(seq 1 "$iters"); do
	[ -f missing.txt ] && grep -v "^00000000" missing.txt >> "$here/data/seeds.txt"
	rm -f missing.txt
	bash "$here/runtime/build-harness.sh" "$HOME/hotd2/roms/hotd2.zip" "$out" 2>&1 | grep -E "rror:"
	HOTD2_INTERP=1 HOTD2_TRACE=0 HOTD2_FRAMES="$frames" HOTD2_INPUT="$script" timeout 3000 ./harness ic22.bin > run.log 2>&1
	echo "iter $i: $(grep "^stop" run.log | cut -c1-40) | $(grep "^--- frames" run.log | cut -c5-60) | new targets $(cat missing.txt 2>/dev/null | wc -l)"
	[ -f missing.txt ] || break
	if grep -q "^00000000" missing.txt; then echo "NULL CALL:"; cat missing.txt; break; fi
done
