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
static void gpu_staging_init(struct capture *c)
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
	gl_check("staging texture creation");

	glGenFramebuffers(1, &c->stage_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, c->stage_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, c->stage_tex, 0);
	gl_check("staging framebuffer creation");

	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		die("staging framebuffer incomplete (0x%04x)", status);

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
 */
static void pbo_init(struct capture *c)
{
	if (!epoxy_has_gl_extension("GL_NV_pixel_buffer_object"))
		die("the pbo method needs GL_NV_pixel_buffer_object");
	if (!epoxy_has_gl_extension("GL_EXT_map_buffer_range"))
		die("the pbo method needs GL_EXT_map_buffer_range to map for reading");
	if (!epoxy_has_gl_extension("GL_OES_mapbuffer"))
		die("the pbo method needs GL_OES_mapbuffer for glUnmapBufferOES");

	glGenBuffers(1, &c->pbo);
	glBindBuffer(GL_PIXEL_PACK_BUFFER_NV, c->pbo);

	/*
	 * ES 2.0 has only the *_DRAW usage hints and NV_pixel_buffer_object
	 * adds none, so DYNAMIC_DRAW is the closest legal description of a
	 * buffer rewritten every frame.  STREAM_READ is an ES 3.0 token.
	 */
	glBufferData(GL_PIXEL_PACK_BUFFER_NV, (GLsizeiptr)c->frame_bytes, NULL,
		     GL_DYNAMIC_DRAW);
	glBindBuffer(GL_PIXEL_PACK_BUFFER_NV, 0);
	gl_check("pixel pack buffer creation");
}

void capture_init(struct capture *c, enum capture_method m,
		  const struct options *o, const struct renderer *r)
{
	memset(c, 0, sizeof(*c));

	c->method = m;
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
	case CAPTURE_PER_ROW:
		break;
	case CAPTURE_CPU_STAGING:
		c->staging = xmalloc((size_t)c->fb_width * c->fb_height * 4);
		break;
	case CAPTURE_DIRECT:
		direct_init(c);
		break;
	case CAPTURE_PBO:
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

	if (m == CAPTURE_GPU_STAGING)
		gpu_staging_init(c);
}

/*
 * Captures the crop of whatever is currently in the render framebuffer into
 * c->dst.  Every path here is synchronous: glReadPixels into client memory
 * blocks until the pixels have arrived, which is exactly the cost being
 * measured.
 */
void capture_frame(struct capture *c)
{
	void *map;
	int row;

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
		/*
		 * With a pixel pack buffer bound the last glReadPixels
		 * argument is an offset into that buffer rather than a
		 * client pointer.
		 */
		glBindBuffer(GL_PIXEL_PACK_BUFFER_NV, c->pbo);
		glReadPixels(c->x, c->y, c->w, c->h, GL_RGBA,
			     GL_UNSIGNED_BYTE, NULL);

		map = glMapBufferRangeEXT(GL_PIXEL_PACK_BUFFER_NV, 0,
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

	if (c->stage_fbo)
		glDeleteFramebuffers(1, &c->stage_fbo);
	if (c->stage_tex)
		glDeleteTextures(1, &c->stage_tex);
	if (c->pbo)
		glDeleteBuffers(1, &c->pbo);
	free(c->staging);
	free(c->dst);
	memset(c, 0, sizeof(*c));
}
