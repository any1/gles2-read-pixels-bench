#include <dirent.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bench.h"

/*
 * CPU load is the CPU time of the whole process, which includes the threads
 * the GL driver runs behind our back, as a share of every CPU this process
 * is allowed to run on.
 *
 * GPU load comes from the DRM client usage stats the kernel publishes in
 * /proc/self/fdinfo for every open DRM file: drm-engine-<name> is the time
 * in ns that engine spent busy with this client's work, and
 * drm-engine-capacity-<name> says how many instances of the engine there are
 * when there is more than one.  Duplicated file descriptors show the same
 * client, so each drm-client-id is only counted once.
 */

static int cpu_count(void)
{
	cpu_set_t set;
	long n;

	if (sched_getaffinity(0, sizeof(set), &set) == 0)
		return CPU_COUNT(&set);

	n = sysconf(_SC_NPROCESSORS_ONLN);
	return n > 0 ? (int)n : 1;
}

static double cpu_time_s(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) < 0)
		die("clock_gettime(CLOCK_PROCESS_CPUTIME_ID) failed");
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static struct load_engine *engine_get(struct load_sample *s, const char *name)
{
	struct load_engine *e;
	int i;

	for (i = 0; i < s->n_engines; i++)
		if (!strcmp(s->engines[i].name, name))
			return &s->engines[i];

	if (s->n_engines == LOAD_ENGINE_MAX)
		return NULL;

	e = &s->engines[s->n_engines++];
	memset(e, 0, sizeof(*e));
	snprintf(e->name, sizeof(e->name), "%s", name);
	e->capacity = 1;
	return e;
}

/*
 * Adds the engine counters of one DRM fd to the sample, unless its client
 * has been seen already.  Returns false for anything that isn't a DRM client.
 */
static bool read_fdinfo(struct load_sample *s, const char *fd,
			unsigned long long *clients, int *n_clients)
{
	char path[288], line[256], name[LOAD_ENGINE_NAME_MAX];
	unsigned long long client = 0, value;
	struct load_engine *e;
	bool is_drm = false;
	FILE *f;
	int i;

	snprintf(path, sizeof(path), "/proc/self/fdinfo/%s", fd);
	f = fopen(path, "r");
	if (!f)
		return false;

	/* drm-client-id comes before the engine lines */
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "drm-client-id: %llu", &client) == 1) {
			is_drm = true;
			for (i = 0; i < *n_clients; i++)
				if (clients[i] == client)
					goto out;
			if (*n_clients < LOAD_CLIENT_MAX)
				clients[(*n_clients)++] = client;
		} else if (is_drm &&
			   sscanf(line, "drm-engine-capacity-%31[^:]: %llu",
				  name, &value) == 2) {
			e = engine_get(s, name);
			if (e && value > 0)
				e->capacity = (unsigned)value;
		} else if (is_drm &&
			   sscanf(line, "drm-engine-%31[^:]: %llu ns",
				  name, &value) == 2) {
			e = engine_get(s, name);
			if (e)
				e->busy_ns += value;
		}
	}

out:
	fclose(f);
	return is_drm;
}

void load_sample(struct load_sample *s)
{
	unsigned long long clients[LOAD_CLIENT_MAX];
	int n_clients = 0;
	struct dirent *ent;
	char link[288];
	char target[256];
	ssize_t len;
	DIR *dir;

	memset(s, 0, sizeof(*s));
	s->wall = now_s();
	s->cpu = cpu_time_s();

	dir = opendir("/proc/self/fd");
	if (!dir)
		return;

	while ((ent = readdir(dir))) {
		if (ent->d_name[0] == '.')
			continue;

		snprintf(link, sizeof(link), "/proc/self/fd/%s", ent->d_name);
		len = readlink(link, target, sizeof(target) - 1);
		if (len < 0)
			continue;
		target[len] = '\0';
		if (strncmp(target, "/dev/dri/", 9))
			continue;

		if (read_fdinfo(s, ent->d_name, clients, &n_clients))
			s->has_drm = true;
	}

	closedir(dir);
}

static const struct load_engine *engine_find(const struct load_sample *s,
					     const char *name)
{
	int i;

	for (i = 0; i < s->n_engines; i++)
		if (!strcmp(s->engines[i].name, name))
			return &s->engines[i];
	return NULL;
}

void load_diff(const struct load_sample *a, const struct load_sample *b,
	       struct load *out)
{
	const struct load_engine *ea, *eb;
	double wall = b->wall - a->wall;
	uint64_t busy;
	double pct;
	int i;

	memset(out, 0, sizeof(*out));
	if (wall <= 0.0)
		return;

	out->cpus = cpu_count();
	out->cpu_s = b->cpu - a->cpu;
	out->cpu_pct = 100.0 * out->cpu_s / (wall * out->cpus);

	out->has_gpu = b->has_drm && b->n_engines > 0;

	/* an engine first used during the test starts from zero */
	for (i = 0; i < b->n_engines; i++) {
		eb = &b->engines[i];
		ea = engine_find(a, eb->name);
		busy = eb->busy_ns;
		if (ea)
			busy = busy > ea->busy_ns ? busy - ea->busy_ns : 0;

		pct = 100.0 * (busy * 1e-9) / (wall * eb->capacity);
		if (pct > 100.0)
			pct = 100.0;

		snprintf(out->engines[i].name, sizeof(out->engines[i].name),
			 "%s", eb->name);
		out->engines[i].pct = pct;
		if (pct > out->gpu_pct)
			out->gpu_pct = pct;
	}
	out->n_engines = b->n_engines;
}
