# wrapl

Windowed RAPL (wrapl) gives unprivileged users the power draw of their CPU. It reads the root-only RAPL (Running
Average Power Limit) energy counters once a second and publishes the average power of each zone over that second in
a world-readable file.

```console
$ cat /run/wrapl/power.json
{"time": 1794743001, "interval": 1.000, "watts": {"package-0": 96.42, "core-0": 81.17, "uncore-0": 3.08}}
```

## Why

Since the Platypus attack ([CVE-2020-8694](https://nvd.nist.gov/vuln/detail/CVE-2020-8694),
[CVE-2020-8695](https://nvd.nist.gov/vuln/detail/CVE-2020-8695)), Linux restricts the counters in
`/sys/class/powercap/intel-rapl:*/energy_uj` to root: sampled at a fine grain (the hardware updates them about every
millisecond), they leak what the CPU computes, down to key material. A one-second average is a much coarser signal,
but still a signal: see [Security](#security) before installing this on a machine shared with people you do not
trust.

## Install

Needs an Intel CPU (Sandy Bridge or later) and a kernel that exposes `/sys/class/powercap/intel-rapl:*`, systemd,
make and a C compiler. Tested on a 13th-gen Core (Raptor Lake). AMD Zen exposes the same interface on current kernels,
but is untested.

```sh
git clone https://github.com/wijjj/wrapl.git
cd wrapl
make
sudo make install          # also creates the wrapl system user
sudo systemctl daemon-reload
sudo systemctl enable --now wrapl
```

Check that it runs:

```sh
cat /run/wrapl/power.json
systemctl status wrapl
```

This installs `/usr/local/sbin/wrapl`, its helper `/usr/local/sbin/wrapl-perms`,
`/etc/systemd/system/wrapl.service` and `/etc/sysusers.d/wrapl.conf`.
`PREFIX`, `SBINDIR`, `UNITDIR`, `SYSUSERSDIR` and `DESTDIR` work as usual; with `DESTDIR` set, run
`systemd-sysusers` on the target system yourself. On a machine without RAPL the service is skipped, not failed.

## Output

| Field | Meaning |
| --- | --- |
| `time` | Unix time of the sample, in whole seconds |
| `interval` | the seconds the averages cover, between 0.9 and 1.5, normally 1.000 |
| `watts` | average power per zone |

Zones are named after the kernel's: `package-N` per package, its subzones as `core-N`, `uncore-N` and `dram-N`, and
`psys` where the platform has it. On parts with several dies per package, the subzones carry the die too:
`core-0-die-1` belongs to `package-0-die-1`. Should two zones end up with the same name, the second gets `_2`.

Only the MSR zones (`intel-rapl:*`) are read. Some laptop CPUs also have `intel-rapl-mmio:*` zones: a second way
into the same package counter, mainly for power limits. Reading both would publish the package twice, so wrapl
leaves them out.

The file is replaced atomically, so a reader never sees half of it. Samples that cannot be one-second averages are
dropped rather than published: after wrapl was stalled or the machine suspended, and readings that only a reset
counter can explain. A zone whose counter cannot be read is left out until it can be again. Treat a missing file, or
a `time` more than a few seconds old, as no data: the file is removed when wrapl stops and when every counter becomes
unreadable.

```sh
jq '.watts["package-0"]' /run/wrapl/power.json
```

## Usage

```
wrapl [-p DIR] [-o FILE]
  -p DIR   powercap root (default /sys/class/powercap)
  -o FILE  output file (default /run/wrapl/power.json)
  -V       print the version
  -h       print this help
```

Exits 1 when there are no counters to read, 2 on a usage error, and 3 when every counter has become unreadable
while running (the unit restarts it then, which grants the permissions again).

## Security

What wrapl guarantees:

- Only samples covering 0.9 to 1.5 seconds leave the process. Stalls and suspends do not produce shorter ones.
- wrapl does not run as root. While the service runs, `wrapl-perms` gives the `wrapl` group read access to the
  `energy_uj` files (mode 0440). When the service stops, it puts back the owner, group and mode each file had
  before; a reboot resets them too. The daemon runs as the `wrapl` user without capabilities, network, devices,
  write access outside `/run/wrapl`, or system calls outside `@system-service`. `systemd-analyze security wrapl` rates it 0.2, SAFE.
- The output is written to a new file with an unpredictable name and renamed over the old one, so a symlink planted
  where the file goes is replaced, not followed.

What it does not protect against, so weigh it for machines with untrusted local users:

- **Averaging over many samples.** An attacker who can make a victim repeat the same secret-dependent operation for
  a long time, and compare it against seconds where the victim idles, can still pull small power differences out of
  many one-second samples. Much slower than the original attack, but possible.
- **Activity.** Anyone can see when the CPU is busy and how busy, second by second, which says something about what
  other users run.
- **Covert channel.** Two processes that are not allowed to talk (separate users, containers, sandboxes) can signal
  each other at roughly a bit per second by loading the CPU or not.

## Logs

wrapl logs to the journal only when something changes: a counter going or coming back, a dropped sample (after a
stall or a suspend, or a counter reset), a write error.

```sh
journalctl -u wrapl -f                    # follow
journalctl -u wrapl -b | grep -v '(/sys'  # this boot, without the startup list of zones
```

## Test

```sh
make test
```

Runs wrapl against simulated counters, without root, and checks every sample it publishes: four zones over two dies,
a counter that wraps, a counter that resets, a 3-second stall, more zones than the limit, duplicate names, bad paths,
counters that disappear, and that `wrapl-perms` restores the original permissions. Takes about 15 seconds.

## Uninstall

```sh
sudo systemctl disable --now wrapl
sudo make uninstall
sudo systemctl daemon-reload
```

The `wrapl` user stays; remove it with `sudo userdel wrapl` if you like.

## TODO

- [x] Only one-second averages are published, even after stalls and suspends
- [x] Runs without root; the counters get their original permissions back on stop
- [x] Tests check every published sample
- [ ] Get `wrapl-perms` (runs as root) and `test.sh` clean under shellcheck
- [ ] Start on a udev event when the RAPL zone appears, so boot order is guaranteed
- [ ] CI: run `make test` and shellcheck on every push
- [ ] AMD Zen: test on real hardware

## License

[MIT](LICENSE)
