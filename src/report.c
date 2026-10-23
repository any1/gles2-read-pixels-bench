#include <stdio.h>

#include "bench.h"

/*
 * json-c serialises doubles with a fixed global format, which is not much use
 * when the same document holds both durations and frame rates.
 * json_object_new_double_s() lets each value carry its own text.
 */
void json_add_double(json_object *obj, const char *key, double v, int prec)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "%.*f", prec, v);
	json_object_object_add(obj, key, json_object_new_double_s(v, buf));
}

void json_add_load(json_object *res, const struct load *l)
{
	json_object *cpu = json_object_new_object();
	json_object *gpu, *engines;
	int i;

	json_add_double(cpu, "load_pct", l->cpu_pct, 2);
	json_add_double(cpu, "time_s", l->cpu_s, 6);
	json_object_object_add(cpu, "cpus", json_object_new_int(l->cpus));
	json_object_object_add(res, "cpu", cpu);

	/* no DRM usage stats, so nothing to say about the GPU */
	if (!l->has_gpu) {
		json_object_object_add(res, "gpu", NULL);
		return;
	}

	gpu = json_object_new_object();
	engines = json_object_new_object();
	json_add_double(gpu, "load_pct", l->gpu_pct, 2);
	for (i = 0; i < l->n_engines; i++)
		json_add_double(engines, l->engines[i].name,
				l->engines[i].pct, 2);
	json_object_object_add(gpu, "engines", engines);
	json_object_object_add(res, "gpu", gpu);
}

json_object *json_gl_info(void)
{
	json_object *j = json_object_new_object();

	json_object_object_add(j, "vendor",
		json_object_new_string((const char *)glGetString(GL_VENDOR)));
	json_object_object_add(j, "renderer",
		json_object_new_string((const char *)glGetString(GL_RENDERER)));
	json_object_object_add(j, "version",
		json_object_new_string((const char *)glGetString(GL_VERSION)));
	json_object_object_add(j, "shading_language_version",
		json_object_new_string((const char *)
			glGetString(GL_SHADING_LANGUAGE_VERSION)));
	return j;
}

void log_load(const struct load *l)
{
	int i;

	log_msg("  cpu load %.1f%% of %d cpus", l->cpu_pct, l->cpus);
	if (!l->has_gpu)
		log_msg("  gpu load unknown, no DRM client usage stats");
	for (i = 0; i < l->n_engines; i++)
		log_msg("  gpu load %s %.1f%%", l->engines[i].name,
			l->engines[i].pct);
}
