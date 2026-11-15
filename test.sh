#!/bin/sh
# Runs wrapl against simulated counters, no root needed: a package at 50 W and a core zone at 30 W whose counter
# wraps past max_energy_range_uj during the run. Passes when the published watts match within 5 %: the simulated
# counters advance every ~10-20 ms, so a one-second sample can be off by that much at most.
set -eu

dir=$(mktemp -d)
trap 'kill "$writer" "$daemon" 2>/dev/null || true; rm -rf "$dir"' EXIT

pkg=$dir/powercap/intel-rapl:0
core=$pkg/../intel-rapl:0:0
mkdir -p "$pkg" "$core"
echo package-0 >"$pkg/name"
echo core >"$core/name"
echo 262143328850 >"$pkg/max_energy_range_uj"
echo 1000000000 >"$core/max_energy_range_uj"

now_us() { echo $(($(date +%s%N) / 1000)); }

# counters follow the clock, not the loop, so the rates are exact whatever the loop's timing
start=$(now_us)
(
	while :; do
		el=$(($(now_us) - start))
		echo $((1000000 + el * 50)) >"$pkg/energy_uj.new" && mv "$pkg/energy_uj.new" "$pkg/energy_uj"
		echo $(((970000000 + el * 30) % 1000000001)) >"$core/energy_uj.new" && mv "$core/energy_uj.new" "$core/energy_uj"
		sleep 0.01
	done
) &
writer=$!
sleep 0.2

./wrapl -p "$dir/powercap" -o "$dir/power.json" 2>/dev/null &
daemon=$!
sleep 3.5   # the core counter wraps after 1 s

out=$(cat "$dir/power.json")
echo "$out"

check() {   # zone, expected watts
	got=$(printf '%s' "$out" | sed -n "s/.*\"$1\": \([0-9.]*\).*/\1/p")
	awk -v g="$got" -v w="$2" -v z="$1" 'BEGIN {
		if (g == "" || g < w * 0.95 || g > w * 1.05) { printf "FAIL %s: %s W, expected %s W\n", z, g, w; exit 1 }
		printf "ok   %s: %s W\n", z, g }'
}
check package-0 50
check core-0 30

kill "$daemon"
wait "$daemon"
[ ! -e "$dir/power.json" ] || { echo "FAIL output left behind after stop"; exit 1; }
echo "ok   output removed on stop"
