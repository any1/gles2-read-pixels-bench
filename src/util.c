#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "bench.h"

/*
 * Everything that is not the benchmark result goes to stderr, so that stdout
 * stays a clean json document.
 */
void log_msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void die(const char *fmt, ...)
{
	va_list ap;

	fputs("fatal: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(EXIT_FAILURE);
}

void *xmalloc(size_t size)
{
	void *p = malloc(size);

	if (!p)
		die("out of memory allocating %zu bytes", size);
	return p;
}

double now_s(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
		die("clock_gettime failed");
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void gl_check(const char *what)
{
	GLenum err = glGetError();

	if (err != GL_NO_ERROR)
		die("GL error 0x%04x after %s", err, what);
}

const char *method_name(enum capture_method m)
{
	switch (m) {
	case CAPTURE_GPU_STAGING:
		return "gpu-staging";
	case CAPTURE_CPU_STAGING:
		return "cpu-staging";
	case CAPTURE_PER_ROW:
		return "per-row";
	case CAPTURE_DIRECT:
		return "direct";
	case CAPTURE_PBO:
		return "pbo";
	default:
		return "unknown";
	}
}

const char *method_description(enum capture_method m)
{
	switch (m) {
	case CAPTURE_GPU_STAGING:
		return "glCopyTexSubImage2D of the crop into a crop-sized "
		       "texture, then one full glReadPixels of that texture";
	case CAPTURE_CPU_STAGING:
		return "one full-frame glReadPixels into a staging buffer, "
		       "then a per-row memcpy of the crop";
	case CAPTURE_PER_ROW:
		return "one glReadPixels per row of the crop, straight into "
		       "the destination buffer";
	case CAPTURE_DIRECT:
		return "one glReadPixels of the crop straight into a full "
		       "frame sized destination buffer, no staging buffer, "
		       "using GL_NV_pack_subimage";
	case CAPTURE_PBO:
		return "glReadPixels of the crop into a pixel pack buffer, "
		       "then glMapBufferRangeEXT and a memcpy out of the "
		       "mapping, using GL_NV_pixel_buffer_object";
	default:
		return "unknown";
	}
}

/*
 * Every combination the benchmark runs: each method with its staging buffer
 * pre-allocated, and, for the methods that stage anything at all, once more
 * with the staging buffer allocated and freed around every capture.
 */
int test_list(struct test *tests)
{
	int i;
	int n = 0;

	for (i = 0; i < CAPTURE_METHOD_COUNT; i++) {
		tests[n].method = i;
		tests[n].realloc_staging = false;
		n++;

		if (!method_has_staging(i))
			continue;

		tests[n].method = i;
		tests[n].realloc_staging = true;
		n++;
	}

	return n;
}

void test_name(const struct test *t, char *buf, size_t size)
{
	snprintf(buf, size, "%s%s", method_name(t->method),
		 t->realloc_staging ? "-realloc" : "");
}
