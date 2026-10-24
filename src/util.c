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

/*
 * Reads every byte of a captured image, so that the copy into it cannot be
 * optimised out and the cost of pulling it through the CPU caches counts
 * towards the measurement, as it would for a real consumer of the frame.
 */
uint8_t sum_bytes(const unsigned char *buf, size_t row_bytes, int rows,
		  size_t pitch)
{
	uint8_t sum = 0;
	size_t i;
	int row;

	for (row = 0; row < rows; row++) {
		const unsigned char *p = buf + (size_t)row * pitch;

		for (i = 0; i < row_bytes; i++)
			sum += p[i];
	}

	return sum;
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
