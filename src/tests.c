#include <stdio.h>

#include "bench.h"

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
	case CAPTURE_PBO_UNALIGNED:
		return "pbo-unaligned";
	case CAPTURE_UDMABUF:
		return "udmabuf";
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
	case CAPTURE_PBO_UNALIGNED:
		return "glReadPixels of the crop into a pixel pack buffer at a "
		       "one byte offset, which keeps Mesa off its shader based "
		       "pack buffer download, then glMapBufferRangeEXT and a "
		       "memcpy out of the mapping, using "
		       "GL_NV_pixel_buffer_object";
	case CAPTURE_UDMABUF:
		return "the frame rendered straight into a linear host memory "
		       "buffer made with /dev/udmabuf, imported through "
		       "EGL_EXT_image_dma_buf_import and attached with "
		       "GL_OES_EGL_image, then glFinish and a per-row memcpy "
		       "of the crop out of the mapping, no glReadPixels";
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
