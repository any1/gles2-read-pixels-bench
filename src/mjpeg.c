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
	unsigned long jpeg_capacity;
	int width;
	int height;
	int quality;
	uint64_t frames;
	uint64_t bytes;
};

struct mjpeg *mjpeg_open(const char *path, int width, int height, int quality)
{
	struct mjpeg *m = xmalloc(sizeof(*m));

	m->width = width;
	m->height = height;
	m->quality = quality;
	m->frames = 0;
	m->bytes = 0;

	m->file = fopen(path, "wb");
	if (!m->file)
		die("cannot open %s for writing", path);

	m->tj = tjInitCompress();
	if (!m->tj)
		die("tjInitCompress failed: %s", tjGetErrorStr2(NULL));

	m->jpeg_capacity = tjBufSize(width, height, TJSAMP_420);
	m->jpeg = tjAlloc((int)m->jpeg_capacity);
	if (!m->jpeg)
		die("tjAlloc of %lu bytes failed", m->jpeg_capacity);

	return m;
}

void mjpeg_write(struct mjpeg *m, const unsigned char *rgba, size_t pitch)
{
	unsigned long size = m->jpeg_capacity;

	/*
	 * glReadPixels hands back rows bottom up, so let turbojpeg walk the
	 * buffer backwards rather than flipping it ourselves.
	 *
	 * This sticks to the TurboJPEG 2 API so that it also builds against
	 * libjpeg-turbo 2.x, as shipped by Debian. TurboJPEG 3 still provides
	 * it unchanged.
	 */
	if (tjCompress2(m->tj, rgba, m->width, (int)pitch, m->height,
			TJPF_RGBA, &m->jpeg, &size, TJSAMP_420, m->quality,
			TJFLAG_BOTTOMUP | TJFLAG_NOREALLOC) < 0)
		die("tjCompress2 failed: %s", tjGetErrorStr2(m->tj));

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

	tjFree(m->jpeg);
	tjDestroy(m->tj);
	free(m);
}
