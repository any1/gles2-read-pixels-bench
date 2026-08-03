#include <stdio.h>
#include <stdlib.h>

#include <turbojpeg.h>

#include "bench.h"

/*
 * Motion JPEG here is just concatenated JPEG images, which is enough for
 * ffmpeg or mpv to play back and confirm that the captured pixels are the
 * frames we think they are.
 */
struct mjpeg {
	FILE *file;
	tjhandle tj;
	unsigned char *jpeg;
	size_t jpeg_capacity;
	int width;
	int height;
	uint64_t frames;
	uint64_t bytes;
};

struct mjpeg *mjpeg_open(const char *path, int width, int height, int quality)
{
	struct mjpeg *m = xmalloc(sizeof(*m));

	m->width = width;
	m->height = height;
	m->frames = 0;
	m->bytes = 0;

	m->file = fopen(path, "wb");
	if (!m->file)
		die("cannot open %s for writing", path);

	m->tj = tj3Init(TJINIT_COMPRESS);
	if (!m->tj)
		die("tj3Init failed");

	if (tj3Set(m->tj, TJPARAM_QUALITY, quality) < 0 ||
	    tj3Set(m->tj, TJPARAM_SUBSAMP, TJSAMP_420) < 0 ||
	    tj3Set(m->tj, TJPARAM_NOREALLOC, 1) < 0)
		die("tj3Set failed: %s", tj3GetErrorStr(m->tj));

	/*
	 * glReadPixels hands back rows bottom up, so let turbojpeg walk the
	 * buffer backwards rather than flipping it ourselves.
	 */
	if (tj3Set(m->tj, TJPARAM_BOTTOMUP, 1) < 0)
		die("tj3Set failed: %s", tj3GetErrorStr(m->tj));

	m->jpeg_capacity = tj3JPEGBufSize(width, height, TJSAMP_420);
	m->jpeg = tj3Alloc(m->jpeg_capacity);
	if (!m->jpeg)
		die("tj3Alloc of %zu bytes failed", m->jpeg_capacity);

	return m;
}

void mjpeg_write(struct mjpeg *m, const unsigned char *rgba)
{
	size_t size = m->jpeg_capacity;

	if (tj3Compress8(m->tj, rgba, m->width, m->width * 4, m->height,
			 TJPF_RGBA, &m->jpeg, &size) < 0)
		die("tj3Compress8 failed: %s", tj3GetErrorStr(m->tj));

	if (fwrite(m->jpeg, 1, size, m->file) != size)
		die("short write to mjpeg output");

	m->frames++;
	m->bytes += size;
}

void mjpeg_close(struct mjpeg *m)
{
	if (fclose(m->file) != 0)
		die("failed to close mjpeg output");

	log_msg("  wrote %llu jpeg frames, %.1f MiB",
		(unsigned long long)m->frames, m->bytes / (1024.0 * 1024.0));

	tj3Free(m->jpeg);
	tj3Destroy(m->tj);
	free(m);
}
