// wrapl - RAPL energy counters as one-second average watts, readable without root.
//
// Since Platypus (CVE-2020-8694, CVE-2020-8695) the powercap energy counters are root-only: sampled at a fine grain,
// they leak what the CPU computes. wrapl reads them once a second and publishes only the average power of each zone
// over that second, in a world-readable JSON file that is replaced atomically. A sample that does not cover about
// one second (the process stalled, the machine was suspended) is dropped, never published.

#define _GNU_SOURCE	// CLOCK_BOOTTIME

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define WRAPL_VERSION "0.2.0"
#define MAX_ZONES 64
#define PATH_LEN 512
#define NAME_LEN 48
#define KEY_LEN (2 * NAME_LEN + 16)

// The only spans published: anything shorter would be a finer-grained sample than the one-second promise, anything
// longer is a gap (stall, suspend) whose counters may have been reset.
#define MIN_INTERVAL 0.9
#define MAX_INTERVAL 1.5
// No RAPL zone draws this much; a larger reading is a counter reset (e.g. across suspend), not a wrap.
#define MAX_WATTS 10000.0

// Ticks and intervals count time spent suspended, so a suspend shows up as a gap and is dropped.
#define CLOCK CLOCK_BOOTTIME

// Exit codes: 1 no counters at start (not worth restarting), 2 usage, 3 every counter became unreadable.
enum { EXIT_NO_ZONES = 1, EXIT_USAGE = 2, EXIT_LOST = 3 };

struct zone {
	char dir[PATH_LEN];	// .../intel-rapl:0:1
	char key[KEY_LEN];	// package-0, core-0, uncore-0, dram-0, psys
	uint64_t max_uj;	// the counter wraps to 0 past this
	uint64_t last_uj;
	bool have_last;
	bool unreadable;	// logged once when it goes, once when it comes back
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
	if (snprintf(path, sizeof(path), "%s/%s", dir, file) >= (int)sizeof(path))
		return -1;
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
	if (errno || end == buf || *end != '\0' || buf[0] == '-')
		return -1;
	*out = v;
	return 0;
}

// Anything but [A-Za-z0-9_-] becomes '_', so a name can go into JSON as is.
static void sanitize(char *s)
{
	for (; *s; s++)
		if (!(*s == '-' || *s == '_' || (*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') ||
		      (*s >= 'A' && *s <= 'Z')))
			*s = '_';
}

// A zone's JSON key: its name, and for a subzone the name of its package after "package-" ("core" under
// "package-0" is "core-0", under "package-0-die-1" it is "core-0-die-1"), so subzones pair up with their parent.
static int zone_key(const char *root, const char *entry, const char *dir, char *key, size_t len)
{
	char name[NAME_LEN], parent[NAME_LEN], pdir[PATH_LEN];
	unsigned pkg, sub;
	if (read_line(dir, "name", name, sizeof(name)) != 0)
		return -1;
	sanitize(name);
	if (sscanf(entry, "intel-rapl:%u:%u", &pkg, &sub) != 2) {
		snprintf(key, len, "%s", name);
		return 0;
	}
	snprintf(pdir, sizeof(pdir), "%s/intel-rapl:%u", root, pkg);
	if (read_line(pdir, "name", parent, sizeof(parent)) == 0) {
		sanitize(parent);
		const char *suffix = strncmp(parent, "package-", 8) == 0 ? parent + 8 : parent;
		snprintf(key, len, "%s-%s", name, suffix);
	} else {
		snprintf(key, len, "%s-%u", name, pkg);
	}
	return 0;
}

static bool key_taken(const struct zone *z, int n, const char *key)
{
	for (int i = 0; i < n; i++)
		if (strcmp(z[i].key, key) == 0)
			return true;
	return false;
}

// Two zones never share a key: a clash gets "_2", "_3", ... appended, and is reported.
static void make_unique(struct zone *z, int n)
{
	if (!key_taken(z, n, z[n].key))
		return;
	char base[KEY_LEN];
	snprintf(base, sizeof(base), "%s", z[n].key);
	for (int i = 2; key_taken(z, n, z[n].key); i++)
		snprintf(z[n].key, sizeof(z[n].key), "%.*s_%d", KEY_LEN - 12, base, i);
	fprintf(stderr, "wrapl: %s: key %s is taken, publishing it as %s\n", z[n].dir, base, z[n].key);
}

// Every intel-rapl:* zone with a readable counter, packages before their subzones.
static int find_zones(const char *root, struct zone *z)
{
	struct dirent **list;
	int n = scandir(root, &list, NULL, alphasort);
	if (n < 0)
		return -1;
	int count = 0, ignored = 0;
	for (int i = 0; i < n; i++) {
		const char *entry = list[i]->d_name;
		if (strncmp(entry, "intel-rapl:", 11) != 0) {
			free(list[i]);
			continue;
		}
		if (count == MAX_ZONES) {
			ignored++;
			free(list[i]);
			continue;
		}
		struct zone *x = &z[count];
		uint64_t probe;
		memset(x, 0, sizeof(*x));
		if (snprintf(x->dir, sizeof(x->dir), "%s/%s", root, entry) >= (int)sizeof(x->dir))
			fprintf(stderr, "wrapl: skipping %s/%s: path too long\n", root, entry);
		else if (zone_key(root, entry, x->dir, x->key, sizeof(x->key)) == 0 &&
			 read_u64(x->dir, "max_energy_range_uj", &x->max_uj) == 0 &&
			 read_u64(x->dir, "energy_uj", &probe) == 0) {
			make_unique(z, count);
			count++;
		} else {
			fprintf(stderr, "wrapl: skipping %s: no readable counter\n", x->dir);
		}
		free(list[i]);
	}
	free(list);
	if (ignored)
		fprintf(stderr, "wrapl: more than %d zones, ignoring %d of them\n", MAX_ZONES, ignored);
	return count;
}

static double seconds(clockid_t clock)
{
	struct timespec t;
	clock_gettime(clock, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

// Appends to a fixed buffer. Once something does not fit, full is set and later appends do nothing.
struct buf {
	char s[128 + MAX_ZONES * (KEY_LEN + 24)];
	size_t len;
	bool full;
};

static void append(struct buf *b, const char *fmt, ...)
{
	if (b->full)
		return;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b->s + b->len, sizeof(b->s) - b->len, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= sizeof(b->s) - b->len)
		b->full = true;
	else
		b->len += (size_t)n;
}

// Writes a fresh file (unpredictable name, O_EXCL, so no symlink is followed) beside the target, then renames it
// over the target: readers see the old file or the new one, never half of one.
static int publish(const char *out, const char *json)
{
	char tmp[PATH_LEN + 8];
	if (snprintf(tmp, sizeof(tmp), "%s.XXXXXX", out) >= (int)sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	int fd = mkostemp(tmp, O_CLOEXEC);
	if (fd < 0)
		return -1;
	FILE *f = fchmod(fd, 0644) == 0 ? fdopen(fd, "w") : NULL;
	if (!f) {
		int e = errno;
		close(fd);
		unlink(tmp);
		errno = e;
		return -1;
	}
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
		default: usage(stderr); return EXIT_USAGE;
		}
	}
	if (optind != argc) {
		usage(stderr);
		return EXIT_USAGE;
	}
	// room for the zone entries under root, and for the temporary name beside out
	if (strlen(root) > PATH_LEN - 64 || strlen(out) > PATH_LEN - 1) {
		fprintf(stderr, "wrapl: %s path too long\n", strlen(root) > PATH_LEN - 64 ? "powercap" : "output");
		return EXIT_USAGE;
	}

	static struct zone zones[MAX_ZONES];
	int nzones = find_zones(root, zones);
	if (nzones <= 0) {
		fprintf(stderr, "wrapl: no RAPL counters under %s\n", root);
		return EXIT_NO_ZONES;
	}
	for (int i = 0; i < nzones; i++)
		fprintf(stderr, "wrapl: %s (%s)\n", zones[i].key, zones[i].dir);

	struct sigaction sa = { .sa_handler = on_signal };	// no SA_RESTART: a signal ends the sleep
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	umask(022);

	// Tick on absolute one-second marks, so the time the reads take does not add up. After an overrun (a stall, a
	// suspend) the marks restart from now: catching up would mean ticks microseconds apart.
	struct timespec next;
	clock_gettime(CLOCK, &next);
	double last_t = 0;
	bool have_t = false;
	int last_err = 0;
	while (!stop) {
		double t = seconds(CLOCK), dt = t - last_t;
		bool span_ok = have_t && dt >= MIN_INTERVAL && dt <= MAX_INTERVAL;
		if (have_t && !span_ok)
			fprintf(stderr, "wrapl: dropping a %.3f s sample: not a one-second average\n", dt);
		last_t = t;
		have_t = true;

		struct buf json = { .len = 0 };
		int fields = 0, readable = 0;
		append(&json, "{\"time\": %lld, \"interval\": %.3f, \"watts\": {", (long long)time(NULL), dt);
		for (int i = 0; i < nzones; i++) {
			struct zone *z = &zones[i];
			uint64_t e;
			if (read_u64(z->dir, "energy_uj", &e) != 0) {
				if (!z->unreadable)
					fprintf(stderr, "wrapl: %s: counter unreadable\n", z->key);
				z->unreadable = true;
				z->have_last = false;
				continue;
			}
			if (z->unreadable)
				fprintf(stderr, "wrapl: %s: counter readable again\n", z->key);
			z->unreadable = false;
			readable++;
			if (z->have_last && span_ok) {
				// a wrap only if the old reading was in range; otherwise the subtraction itself would wrap
				bool sane = e >= z->last_uj || z->last_uj <= z->max_uj;
				uint64_t delta = e >= z->last_uj ? e - z->last_uj : e + (z->max_uj - z->last_uj) + 1;
				double w = (double)delta / 1e6 / dt;
				if (!sane || w > MAX_WATTS)
					fprintf(stderr, "wrapl: %s: dropping an implausible reading (counter reset?)\n", z->key);
				else
					append(&json, "%s\"%s\": %.2f", fields++ ? ", " : "", z->key, w);
			}
			z->last_uj = e;
			z->have_last = true;
		}
		append(&json, "}}\n");

		if (readable == 0) {
			// nothing left to publish from: take the numbers down and let the service manager start over
			fprintf(stderr, "wrapl: every counter is unreadable, stopping\n");
			unlink(out);
			return EXIT_LOST;
		}
		if (json.full)
			fprintf(stderr, "wrapl: sample too large, not published\n");
		else if (fields > 0) {
			int err = publish(out, json.s) == 0 ? 0 : errno;
			if (err && err != last_err)	// say it once, not every second
				fprintf(stderr, "wrapl: cannot write %s: %s\n", out, strerror(err));
			else if (!err && last_err)
				fprintf(stderr, "wrapl: writing %s again\n", out);
			last_err = err;
		}

		next.tv_sec += 1;
		struct timespec now;
		clock_gettime(CLOCK, &now);
		if (now.tv_sec > next.tv_sec || (now.tv_sec == next.tv_sec && now.tv_nsec >= next.tv_nsec)) {
			next = now;
			next.tv_sec += 1;
		}
		while (!stop && clock_nanosleep(CLOCK, TIMER_ABSTIME, &next, NULL) == EINTR)
			;
	}
	unlink(out);	// no stale numbers once wrapl stops
	return 0;
}
