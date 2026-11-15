#!/bin/sh
# Runs wrapl against simulated counters, no root needed. Every sample wrapl publishes is collected and checked, so
# the edge cases below are checked in the very samples that cover them:
#   package-0        50 W
#   core-0           30 W, its counter wraps past max_energy_range_uj 2 s in
#   package-0-die-1  20 W, a second die: its subzone must be keyed core-0-die-1
#   core-0-die-1     10 W, its counter resets to 0 at 3 s (as across suspend): that reading must be dropped
# and wrapl is stopped (SIGSTOP) for 3 s: no sample may cover anything but about one second.
# Values must match within 5 %: the simulated counters advance every ~20-30 ms.
set -eu
export LC_ALL=C   # awk reads "9.96" as 9 under a decimal-comma locale

writer= daemon= poller=
dir=$(mktemp -d)
trap 'kill $writer $daemon $poller 2>/dev/null || true; rm -rf "$dir"' EXIT
fail() { echo "FAIL $*"; exit 1; }

p=$dir/powercap
zone() {   # entry, name, max_energy_range_uj
	mkdir -p "$p/$1" && echo "$2" >"$p/$1/name" && echo "$3" >"$p/$1/max_energy_range_uj"
}
zone intel-rapl:0 package-0 262143328850
zone intel-rapl:0:0 core 1000000000
zone intel-rapl:1 package-0-die-1 262143328850
zone intel-rapl:1:0 core 262143328850

now_us() { echo $(($(date +%s%N) / 1000)); }
put() { echo "$2" >"$p/$1/energy_uj.new" && mv "$p/$1/energy_uj.new" "$p/$1/energy_uj"; }

# counters follow the clock, not the loop, so the rates are exact whatever the loop's timing
start=$(now_us)
(
	while :; do
		el=$(($(now_us) - start))
		put intel-rapl:0 $((1000000 + el * 50))
		put intel-rapl:0:0 $(((940000000 + el * 30) % 1000000001))
		put intel-rapl:1 $((el * 20))
		if [ $el -lt 3000000 ]; then put intel-rapl:1:0 $((100000000 + el * 10)); else put intel-rapl:1:0 $(((el - 3000000) * 10)); fi
		sleep 0.01
	done
) &
writer=$!
sleep 0.2

./wrapl -p "$p" -o "$dir/power.json" 2>"$dir/log" &
daemon=$!
(while :; do cat "$dir/power.json" 2>/dev/null || true; sleep 0.05; done) >"$dir/polled" &
poller=$!

sleep 4.3
kill -STOP "$daemon"
sleep 3
cont=$(now_us)
kill -CONT "$daemon"
sleep 2.7

kill "$daemon"
wait "$daemon" || fail "wrapl exited with $?"
daemon=
kill "$poller"
poller=
[ ! -e "$dir/power.json" ] || fail "output left behind after stop"

sort -u "$dir/polled" >"$dir/samples"
cat "$dir/samples"
awk -v start="$start" -v cont="$cont" '
function within(z, w, g) { g += 0; return g >= w * 0.95 && g <= w * 1.05 }
function bad(msg) { print "FAIL " msg; failed = 1 }
{
	line = $0
	gsub(/[{}"]/, "")
	n = split($0, item, ", ")
	delete got
	for (i = 1; i <= n; i++) { m = split(item[i], kv, ": "); got[kv[m - 1]] = kv[m] }
	t = got["time"] + 0; iv = got["interval"] + 0
	samples++
	if (iv < 0.9 || iv > 1.5) bad("interval " iv " in " line)
	for (z in want) {
		if (!(z in got)) { if (z != "core-0-die-1" || ++resets > 1) bad(z " missing from " line); continue }
		if (!within(z, want[z], got[z])) bad(z ": " got[z] " W, expected " want[z] " W, in " line)
	}
	for (k in got) if (k != "time" && k != "interval" && !(k in want)) bad("unexpected key " k)
	# windows that must have been published, and checked above
	# time is whole seconds: a window really ends somewhere in [t, t + 1)
	if (t - iv < start / 1e6 + 2 && t + 1 > start / 1e6 + 2) wrap = 1
	if (t - iv < start / 1e6 + 3 && t + 1 > start / 1e6 + 3) reset = 1
	if (t > cont / 1e6 + 1) resumed = 1
}
BEGIN { want["package-0"] = 50; want["core-0"] = 30; want["package-0-die-1"] = 20; want["core-0-die-1"] = 10 }
END {
	if (samples < 6) bad("only " samples " samples")
	if (!wrap) bad("no sample covers the core-0 wrap")
	if (!reset) bad("no sample covers the core-0-die-1 reset")
	if (!resumed) bad("no sample after the stall")
	if (failed) exit 1
	printf "ok   %d samples: all one-second, all within 5 %%, wrap covered, keys per die\n", samples
}' "$dir/samples" || fail "samples"
drops=$(grep -c 'dropping a .* s sample' "$dir/log" || true)
[ "$drops" -eq 1 ] || fail "$drops samples dropped around the stall, expected 1 (more is a catch-up burst)"
echo "ok   stall dropped, no catch-up burst"
grep -q 'core-0-die-1: dropping an implausible reading' "$dir/log" || fail "the counter reset was not dropped"
echo "ok   counter reset dropped"
echo "ok   output removed on stop"

# startup: limits, duplicates, bad paths
q=$dir/many
for i in $(seq 0 65); do
	mkdir -p "$q/intel-rapl:$i" && echo psys >"$q/intel-rapl:$i/name"
	echo 1000 >"$q/intel-rapl:$i/max_energy_range_uj" && echo 0 >"$q/intel-rapl:$i/energy_uj"
done
./wrapl -p "$q" -o "$dir/many.json" 2>"$dir/log" &
daemon=$!
sleep 0.3
kill "$daemon"
wait "$daemon" || true
daemon=
grep -q 'more than 64 zones, ignoring 2' "$dir/log" || fail "zones past the limit not reported"
grep -q 'key psys is taken, publishing it as psys_2' "$dir/log" || fail "duplicate key not made unique"
echo "ok   zone limit reported, duplicate keys made unique"

rc=0; ./wrapl -p "$(printf "%0600d" 0)" 2>/dev/null || rc=$?
[ $rc -eq 2 ] || fail "too long a path: exit $rc, expected 2"
rc=0; ./wrapl -p "$dir/none" 2>/dev/null || rc=$?
[ $rc -eq 1 ] || fail "no counters: exit $rc, expected 1"
echo "ok   bad paths refused"

# every counter gone (driver unloaded, permissions reset): wrapl takes the file down and exits 3
r=$dir/lost
mkdir -p "$r/intel-rapl:0" && echo package-0 >"$r/intel-rapl:0/name" && echo 1000000000 >"$r/intel-rapl:0/max_energy_range_uj"
echo 0 >"$r/intel-rapl:0/energy_uj"
./wrapl -p "$r" -o "$dir/lost.json" 2>/dev/null &
daemon=$!
sleep 1.5
[ -e "$dir/lost.json" ] || fail "nothing published before the counter went"
rm "$r/intel-rapl:0/energy_uj"
rc=0; wait "$daemon" || rc=$?
daemon=
[ $rc -eq 3 ] || fail "counters lost: exit $rc, expected 3"
[ ! -e "$dir/lost.json" ] || fail "output left behind after the counters went"
echo "ok   lost counters: output removed, exit 3"

# wrapl-perms: grants group read, and puts back exactly the owner, group and mode each counter had
w=$dir/perms
mkdir -p "$w/intel-rapl:0" "$w/intel-rapl:0:0"
touch "$w/intel-rapl:0/energy_uj" "$w/intel-rapl:0:0/energy_uj"
chmod 0400 "$w/intel-rapl:0/energy_uj"
chmod 0444 "$w/intel-rapl:0:0/energy_uj"
grant_to=$(id -g)
orig=$(id -G | tr ' ' '\n' | grep -vx "$grant_to" | head -n1 || true)   # another group of ours, if there is one
[ -z "$orig" ] || chgrp "$orig" "$w/intel-rapl:0/energy_uj"
before=$(stat -c '%u %g %a' "$w"/intel-rapl:*/energy_uj)
perms() { WRAPL_POWERCAP=$w WRAPL_PERMS_STATE=$dir/perms.state WRAPL_GROUP=$grant_to ./wrapl-perms "$1"; }
perms grant
perms grant   # twice, as after a crash: must still restore the originals
[ "$(stat -c '%g %a' "$w/intel-rapl:0/energy_uj")" = "$grant_to 440" ] || fail "grant: $(stat -c '%g %a' "$w/intel-rapl:0/energy_uj")"
[ "$(stat -c '%a' "$w/intel-rapl:0:0/energy_uj")" = 444 ] || fail "grant changed a world-readable mode"
[ "$(stat -c '%a' "$dir/perms.state")" = 600 ] || fail "state file is not private"
perms revoke
[ "$(stat -c '%u %g %a' "$w"/intel-rapl:*/energy_uj)" = "$before" ] || fail "revoke did not restore the originals"
[ ! -e "$dir/perms.state" ] || fail "state file left behind"
perms revoke   # nothing recorded: nothing to do
echo "ok   wrapl-perms grants group read and restores the originals${orig:+ (group too)}"
