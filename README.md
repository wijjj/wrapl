# wrapl

CPU power for unprivileged users. wrapl reads the RAPL energy counters as root, once a second, and publishes the
average power of each zone over that second in a world-readable file.

```console
$ cat /run/wrapl/power.json
{"time": 1794743001.002, "interval": 1.000, "watts": {"package-0": 96.42, "core-0": 81.17, "uncore-0": 3.08}}
```

## Why

Since the Platypus attack ([CVE-2020-8694](https://nvd.nist.gov/vuln/detail/CVE-2020-8694),
[CVE-2020-8695](https://nvd.nist.gov/vuln/detail/CVE-2020-8695)), Linux restricts the counters in
`/sys/class/powercap/intel-rapl:*/energy_uj` to root: sampled at a fine grain, they leak what the CPU computes, down
to key material. A one-second average over a whole package carries no such signal. wrapl publishes that average and
nothing else; the counters stay root-only.

## Install

Needs a CPU and kernel that expose `/sys/class/powercap/intel-rapl:*` (Intel since Sandy Bridge, AMD Zen on current
kernels), systemd, make and a C compiler.

```sh
git clone https://github.com/wijjj/wrapl.git
cd wrapl
make
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable --now wrapl
```

Check that it runs:

```sh
cat /run/wrapl/power.json
systemctl status wrapl
```

This installs `/usr/local/sbin/wrapl` and `/etc/systemd/system/wrapl.service`. `PREFIX`, `SBINDIR`, `UNITDIR` and
`DESTDIR` work as usual.

## Output

| Field | Meaning |
| --- | --- |
| `time` | Unix time of the sample, in seconds |
| `interval` | the seconds the averages cover, normally 1.000 |
| `watts` | average power per zone |

Zones are named after the kernel's: `package-N` per package, its subzones as `core-N`, `uncore-N` and `dram-N`, and
`psys` where the platform has it. The file is replaced atomically, so a reader never sees half of it. It is removed
when wrapl stops: treat a missing file, or a `time` more than a few seconds old, as no data.

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

## Security

- Only one-second averages leave the process. The counters stay root-only.
- wrapl runs as root because the counters are owned by root with mode 0400. It needs no capability for that, and the
  unit takes all of them away, along with the network, write access outside `/run/wrapl`, devices and every system
  call outside `@system-service`. `systemd-analyze security wrapl` rates it 0.7, SAFE; what remains is running as root
  and writing a world-readable file, which is the job.

## Test

```sh
make test
```

Runs wrapl against simulated counters, without root: a package at 50 W, and a core zone whose counter wraps during
the run.

## Uninstall

```sh
sudo systemctl disable --now wrapl
sudo make uninstall
sudo systemctl daemon-reload
```

## License

[MIT](LICENSE)
