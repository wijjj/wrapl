// wrapl - RAPL energy counters as one-second average watts, readable without root.
//
// Since Platypus (CVE-2020-8694, CVE-2020-8695) the powercap energy counters are root-only: sampled at a fine grain,
// they leak what the CPU computes. wrapl reads them as root once a second and publishes only the average power of
// each zone over that second, in a world-readable JSON file that is replaced atomically.

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define WRAPL_VERSION "1.0.0"
#define MAX_ZONES 32
#define PATH_LEN 512

struct zone {
	char dir[PATH_LEN];	// .../intel-rapl:0:1
	char key[64];		// package-0, core-0, uncore-0, dram-0, psys
	uint64_t max_uj;	// the counter wraps to 0 past this
	uint64_t last_uj;
	bool have_last;
};

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static int read_line(const char *dir, const char *file, char *buf, size_t len)
{
	char path[PATH_LEN + 64];
	snprintf(path, sizeof(path), "%s/%s", dir, file);
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	bool ok = fgets(buf, (int)len, f) != NULL;
	fclose(f);
	if (!ok)
		return -1;
	buf[strcspn(buf, "\n")] = '\0';
	return 0;
}

static int read_u64(const char *dir, const char *file, uint64_t *out)
{
	char buf[32], *end;
	if (read_line(dir, file, buf, sizeof(buf)) != 0)
		return -1;
	errno = 0;
	unsigned long long v = strtoull(buf, &end, 10);
	if (errno || end == buf || *end != '\0')
		return -1;
	*out = v;
	return 0;
}

// A zone's JSON key: its name, with the package number appended for subzones ("core" of package 0 is "core-0"),
// so the keys stay unique on machines with several packages. Anything but [A-Za-z0-9_-] becomes '_'.
static int zone_key(const char *dir, const char *entry, char *key, size_t len)
{
	char name[48];
	unsigned pkg, sub;
	if (read_line(dir, "name", name, sizeof(name)) != 0)
		return -1;
	for (char *c = name; *c; c++)
		if (!(*c == '-' || *c == '_' || (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') ||
		      (*c >= 'A' && *c <= 'Z')))
			*c = '_';
	if (sscanf(entry, "intel-rapl:%u:%u", &pkg, &sub) == 2)
		snprintf(key, len, "%s-%u", name, pkg);
	else
		snprintf(key, len, "%s", name);
	return 0;
}

// Every intel-rapl:* zone with a readable counter, packages before their subzones.
static int find_zones(const char *root, struct zone *z)
{
	struct dirent **list;
	int n = scandir(root, &list, NULL, alphasort);
	if (n < 0)
		return -1;
	int count = 0;
	for (int i = 0; i < n; i++) {
		const char *entry = list[i]->d_name;
		if (count < MAX_ZONES && strncmp(entry, "intel-rapl:", 11) == 0) {
			struct zone *x = &z[count];
			uint64_t probe;
			memset(x, 0, sizeof(*x));
			snprintf(x->dir, sizeof(x->dir), "%s/%s", root, entry);
			if (zone_key(x->dir, entry, x->key, sizeof(x->key)) == 0 &&
			    read_u64(x->dir, "max_energy_range_uj", &x->max_uj) == 0 &&
			    read_u64(x->dir, "energy_uj", &probe) == 0)
				count++;
			else
				fprintf(stderr, "wrapl: skipping %s: no readable counter\n", x->dir);
		}
		free(list[i]);
	}
	free(list);
	return count;
}

static double seconds(clockid_t clock)
{
	struct timespec t;
	clock_gettime(clock, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

// Writes the file beside its target, then renames it over the target: readers see the old file or the new one.
static int publish(const char *out, const char *tmp, const char *json)
{
	FILE *f = fopen(tmp, "w");
	if (!f)
		return -1;
	bool ok = fputs(json, f) >= 0;
	ok = (fclose(f) == 0) && ok;
	if (!ok || rename(tmp, out) != 0) {
		int e = errno;
		unlink(tmp);
		errno = e;
		return -1;
	}
	return 0;
}

static void usage(FILE *to)
{
	fputs("usage: wrapl [-p DIR] [-o FILE]\n"
	      "Publishes the RAPL energy counters as one-second average watts.\n\n"
	      "  -p DIR   powercap root (default /sys/class/powercap)\n"
	      "  -o FILE  output file (default /run/wrapl/power.json)\n"
	      "  -V       print the version\n"
	      "  -h       print this help\n",
	      to);
}

int main(int argc, char **argv)
{
	const char *root = "/sys/class/powercap";
	const char *out = "/run/wrapl/power.json";
	int opt;
	while ((opt = getopt(argc, argv, "p:o:Vh")) != -1) {
		switch (opt) {
		case 'p': root = optarg; break;
		case 'o': out = optarg; break;
		case 'V': puts("wrapl " WRAPL_VERSION); return 0;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}
	if (optind != argc) {
		usage(stderr);
		return 2;
	}

	char tmp[PATH_LEN];
	if (snprintf(tmp, sizeof(tmp), "%s.tmp", out) >= (int)sizeof(tmp)) {
		fprintf(stderr, "wrapl: output path too long\n");
		return 2;
	}

	struct zone zones[MAX_ZONES];
	int nzones = find_zones(root, zones);
	if (nzones <= 0) {
		fprintf(stderr, "wrapl: no RAPL counters under %s\n", root);
		return 1;
	}
	for (int i = 0; i < nzones; i++)
		fprintf(stderr, "wrapl: %s (%s)\n", zones[i].key, zones[i].dir);

	struct sigaction sa = { .sa_handler = on_signal };	// no SA_RESTART: a signal ends the sleep
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	umask(022);

	// tick on absolute one-second marks, so the time the reads take does not add up
	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);
	double last_t = 0;
	int last_err = 0;
	while (!stop) {
		double t = seconds(CLOCK_MONOTONIC), dt = last_t > 0 ? t - last_t : 0;
		char json[4096];
		size_t len = 0;
		int fields = 0;
		len += (size_t)snprintf(json, sizeof(json), "{\"time\": %.3f, \"interval\": %.3f, \"watts\": {",
					seconds(CLOCK_REALTIME), dt);
		for (int i = 0; i < nzones; i++) {
			struct zone *z = &zones[i];
			uint64_t e;
			if (read_u64(z->dir, "energy_uj", &e) != 0) {
				z->have_last = false;
				continue;
			}
			if (z->have_last && dt > 0 && len < sizeof(json)) {
				uint64_t delta = e >= z->last_uj ? e - z->last_uj : e + (z->max_uj - z->last_uj) + 1;
				len += (size_t)snprintf(json + len, sizeof(json) - len, "%s\"%s\": %.2f",
							fields++ ? ", " : "", z->key, (double)delta / 1e6 / dt);
			}
			z->last_uj = e;
			z->have_last = true;
		}
		if (len < sizeof(json))
			len += (size_t)snprintf(json + len, sizeof(json) - len, "}}\n");
		last_t = t;

		if (fields > 0 && len < sizeof(json)) {
			int err = publish(out, tmp, json) == 0 ? 0 : errno;
			if (err != last_err)	// say it once, not every second
				fprintf(stderr, err ? "wrapl: cannot write %s: %s\n" : "wrapl: writing %s again%s\n", out,
					err ? strerror(err) : "");
			last_err = err;
		}

		next.tv_sec += 1;
		while (!stop && clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR)
			;
	}
	unlink(out);	// no stale numbers once wrapl stops
	return 0;
}
