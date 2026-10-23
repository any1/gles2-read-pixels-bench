#include <stdlib.h>
#include <string.h>

#include "bench.h"

/*
 * Reading the destination buffer through a volatile accumulator keeps the
 * compiler from deciding that the copy into it is dead.  One byte per row is
 * enough to observe every memcpy() of the row loop, and memcpy() is a library
 * call, so it is eliminated whole or not at all.
 */
static volatile uint64_t capture_checksum;

static void capture_touch(const struct capture *c)
{
	uint64_t sum = 0;
	int row;

	for (row = 0; row < c->h; row++)
		sum += c->crop[(size_t)row * c->crop_pitch];

	capture_checksum += sum;
}

uint64_t capture_get_checksum(void)
{
	return capture_checksum;
}

/*
 * The GPU-side staging buffer is a crop-sized texture.  ES 2.0 has no blit, so
 * the crop is moved with glCopyTexSubImage2D, which stays on the GPU, and the
 * readback is then an untruncated full-surface glReadPixels.
 */
static void gpu_staging_alloc(struct capture *c)
{
	GLenum status;

	glGenTextures(1, &c->stage_tex);
	glBindTexture(GL_TEXTURE_2D, c->stage_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c->w, c->h, 0, GL_RGBA,
		     GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glGenFramebuffers(1, &c->stage_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, c->stage_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, c->stage_tex, 0);

	/*
	 * Both of these queries drain the pipeline, so the same staging
	 * target is only validated the first time it is built.  Repeat
	 * allocations differ from it in nothing but their object names.
	 */
	if (!c->staging_checked) {
		gl_check("staging target creation");

		status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE)
			die("staging framebuffer incomplete (0x%04x)", status);

		c->staging_checked = true;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, c->src_fbo);
}

/*
 * No staging buffer of any kind: the crop is read straight into the position
 * it occupies in a full frame worth of pixels.  That needs a destination row
 * stride wider than the region being read, which ES 2.0 core cannot express,
 * hence GL_NV_pack_subimage.
 */
static void direct_init(struct capture *c)
{
	if (!epoxy_has_gl_extension("GL_NV_pack_subimage"))
		die("the direct method needs GL_NV_pack_subimage");

	c->dst_size = (size_t)c->fb_width * c->fb_height * 4;
	c->crop_pitch = (size_t)c->fb_width * 4;

	glPixelStorei(GL_PACK_ROW_LENGTH, c->fb_width);
	glPixelStorei(GL_PACK_SKIP_PIXELS, c->x);
	glPixelStorei(GL_PACK_SKIP_ROWS, c->y);
	gl_check("pack subimage state");
}

/*
 * The readback lands in a pixel pack buffer owned by the GL, which is then
 * mapped and copied out.  GL_OES_mapbuffer on its own only offers write-only
 * mappings, so reading the result back needs glMapBufferRangeEXT.
 *
 * The unaligned variant places the crop one byte into the buffer, which is
 * legal for GL_UNSIGNED_BYTE data.  It matters to Mesa: with a pack buffer
 * bound, its state tracker first tries to fill the buffer with a fragment
 * shader doing one image store per pixel, regardless of whether the format
 * matches the storage format.  On a discrete GPU that is a stream of
 * scattered 4 byte writes into host memory across PCIe, and it is slower than
 * every other method by an order of magnitude.  An offset that is not a
 * multiple of the pixel size is rejected by that path, and the readback falls
 * through to the same blit plus memcpy that the non-PBO methods get.
 */
static void pbo_init(struct capture *c)
{
	if (!epoxy_has_gl_extension("GL_NV_pixel_buffer_object"))
		die("the pbo method needs GL_NV_pixel_buffer_object");
	if (!epoxy_has_gl_extension("GL_EXT_map_buffer_range"))
		die("the pbo method needs GL_EXT_map_buffer_range to map for reading");
	if (!epoxy_has_gl_extension("GL_OES_mapbuffer"))
		die("the pbo method needs GL_OES_mapbuffer for glUnmapBufferOES");

	if (c->method == CAPTURE_PBO_UNALIGNED)
		c->pbo_offset = 1;
}

static void pbo_alloc(struct capture *c)
{
	glGenBuffers(1, &c->pbo);
	glBindBuffer(GL_PIXEL_PACK_BUFFER_NV, c->pbo);

	/*
	 * ES 2.0 has only the *_DRAW usage hints and NV_pixel_buffer_object
	 * adds none, so DYNAMIC_DRAW is the closest legal description of a
	 * buffer rewritten every frame.  STREAM_READ is an ES 3.0 token.
	 */
	glBufferData(GL_PIXEL_PACK_BUFFER_NV,
		     (GLsizeiptr)(c->frame_bytes + c->pbo_offset), NULL,
		     GL_DYNAMIC_DRAW);
	glBindBuffer(GL_PIXEL_PACK_BUFFER_NV, 0);

	if (!c->staging_checked) {
		gl_check("pixel pack buffer creation");
		c->staging_checked = true;
	}
}

/*
 * The staging resources of the method, if it has any.  They are built once at
 * init and torn down at fini, unless the caller has asked for them to be
 * built and torn down around every single capture, as a caller that cannot
 * hold on to them between frames would have to do.
 */
static void staging_alloc(struct capture *c)
{
	switch (c->method) {
	case CAPTURE_GPU_STAGING:
		gpu_staging_alloc(c);
		break;
	case CAPTURE_CPU_STAGING:
		c->staging = xmalloc((size_t)c->fb_width * c->fb_height * 4);
		break;
	case CAPTURE_PBO:
	case CAPTURE_PBO_UNALIGNED:
		pbo_alloc(c);
		break;
	default:
		break;
	}
}

static void staging_free(struct capture *c)
{
	if (c->stage_fbo) {
		glDeleteFramebuffers(1, &c->stage_fbo);
		c->stage_fbo = 0;
	}
	if (c->stage_tex) {
		glDeleteTextures(1, &c->stage_tex);
		c->stage_tex = 0;
	}
	if (c->pbo) {
		glDeleteBuffers(1, &c->pbo);
		c->pbo = 0;
	}

	free(c->staging);
	c->staging = NULL;
}

bool method_has_staging(enum capture_method m)
{
	switch (m) {
	case CAPTURE_GPU_STAGING:
	case CAPTURE_CPU_STAGING:
	case CAPTURE_PBO:
	case CAPTURE_PBO_UNALIGNED:
		return true;
	default:
		return false;
	}
}

void capture_init(struct capture *c, const struct test *t,
		  const struct options *o, const struct renderer *r)
{
	enum capture_method m = t->method;

	memset(c, 0, sizeof(*c));

	c->method = m;
	c->realloc_staging = t->realloc_staging;
	c->fb_width = o->width;
	c->fb_height = o->height;
	c->w = o->crop_w;
	c->h = o->crop_h;
	c->x = o->crop_x;

	/*
	 * glReadPixels and glCopyTexSubImage2D address the framebuffer from
	 * the bottom left, the crop is specified from the top left.
	 */
	c->y = o->height - o->crop_y - o->crop_h;

	c->src_fbo = r->fbo;
	c->row_bytes = (size_t)c->w * 4;
	c->frame_bytes = c->row_bytes * (size_t)c->h;

	/* the crop is tightly packed unless a method says otherwise */
	c->dst_size = c->frame_bytes;
	c->crop_pitch = c->row_bytes;

	switch (m) {
	case CAPTURE_GPU_STAGING:
	case CAPTURE_CPU_STAGING:
	case CAPTURE_PER_ROW:
		break;
	case CAPTURE_DIRECT:
		direct_init(c);
		break;
	case CAPTURE_PBO:
	case CAPTURE_PBO_UNALIGNED:
		pbo_init(c);
		break;
	default:
		die("unknown capture method");
	}

	c->dst = xmalloc(c->dst_size);
	memset(c->dst, 0, c->dst_size);

	c->crop = c->dst;
	if (m == CAPTURE_DIRECT)
		c->crop += (size_t)c->y * c->crop_pitch + (size_t)c->x * 4;

	if (!c->realloc_staging)
		staging_alloc(c);
}

/*
 * Captures the crop of whatever is currently in the render framebuffer into
 * c->dst.  Every path here is synchronous: glReadPixels into client memory
 * blocks until the pixels have arrived, which is exactly the cost being
 * measured.  So is the staging allocation when it has been asked for per
 * capture; it is part of what such a capture costs.
 */
void capture_frame(struct capture *c)
{
	void *map;
	int row;

	if (c->realloc_staging)
		staging_alloc(c);

	switch (c->method) {
	case CAPTURE_GPU_STAGING:
		/*
		 * glCopyTexSubImage2D takes its source from the bound
		 * framebuffer, so the render target has to be bound for the
		 * copy and the staging target for the readback.
		 */
		glBindFramebuffer(GL_FRAMEBUFFER, c->src_fbo);
		glBindTexture(GL_TEXTURE_2D, c->stage_tex);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, c->x, c->y,
				    c->w, c->h);

		glBindFramebuffer(GL_FRAMEBUFFER, c->stage_fbo);
		glReadPixels(0, 0, c->w, c->h, GL_RGBA, GL_UNSIGNED_BYTE,
			     c->dst);
		glBindFramebuffer(GL_FRAMEBUFFER, c->src_fbo);
		break;

	case CAPTURE_CPU_STAGING:
		glReadPixels(0, 0, c->fb_width, c->fb_height, GL_RGBA,
			     GL_UNSIGNED_BYTE, c->staging);

		for (row = 0; row < c->h; row++) {
			const unsigned char *src = c->staging +
				((size_t)(c->y + row) * c->fb_width + c->x) * 4;

			memcpy(c->dst + (size_t)row * c->row_bytes, src,
			       c->row_bytes);
		}
		break;

	case CAPTURE_PER_ROW:
		for (row = 0; row < c->h; row++)
			glReadPixels(c->x, c->y + row, c->w, 1, GL_RGBA,
				     GL_UNSIGNED_BYTE,
				     c->dst + (size_t)row * c->row_bytes);
		break;

	case CAPTURE_DIRECT:
		/*
		 * The pack state set up at init places the crop, so the
		 * destination pointer stays at the start of the buffer.
		 */
		glReadPixels(c->x, c->y, c->w, c->h, GL_RGBA,
			     GL_UNSIGNED_BYTE, c->dst);
		break;

	case CAPTURE_PBO:
	case CAPTURE_PBO_UNALIGNED:
		/*
		 * With a pixel pack buffer bound the last glReadPixels
		 * argument is an offset into that buffer rather than a
		 * client pointer.
		 */
		glBindBuffer(GL_PIXEL_PACK_BUFFER_NV, c->pbo);
		glReadPixels(c->x, c->y, c->w, c->h, GL_RGBA,
			     GL_UNSIGNED_BYTE, (void *)(uintptr_t)c->pbo_offset);

		map = glMapBufferRangeEXT(GL_PIXEL_PACK_BUFFER_NV,
					  (GLintptr)c->pbo_offset,
					  (GLsizeiptr)c->frame_bytes,
					  GL_MAP_READ_BIT_EXT);
		if (!map)
			die("glMapBufferRangeEXT of the pixel pack buffer failed");

		memcpy(c->dst, map, c->frame_bytes);

		if (!glUnmapBufferOES(GL_PIXEL_PACK_BUFFER_NV))
			die("the pixel pack buffer contents were lost while mapped");
		glBindBuffer(GL_PIXEL_PACK_BUFFER_NV, 0);
		break;

	default:
		die("unknown capture method");
	}

	if (c->realloc_staging)
		staging_free(c);

	capture_touch(c);
	c->completed++;
}

void capture_fini(struct capture *c)
{
	if (c->method == CAPTURE_DIRECT) {
		glPixelStorei(GL_PACK_ROW_LENGTH, 0);
		glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
		glPixelStorei(GL_PACK_SKIP_ROWS, 0);
	}

	staging_free(c);
	free(c->dst);
	memset(c, 0, sizeof(*c));
}
