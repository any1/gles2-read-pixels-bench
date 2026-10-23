#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/dma-buf.h>
#include <linux/udmabuf.h>

#include "bench.h"

/*
 * DRM_FORMAT_ABGR8888 from drm_fourcc.h, spelled out here so that libdrm is
 * not a dependency for the sake of one constant.  Little endian a:b:g:r in a
 * 32 bit word, which is r, g, b, a in memory, the same as GL_RGBA with
 * GL_UNSIGNED_BYTE.
 */
#define FOURCC_ABGR8888 ((uint32_t)'A' | (uint32_t)'B' << 8 | \
			 (uint32_t)'2' << 16 | (uint32_t)'4' << 24)

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
 * No readback at all: the frame is rendered straight into a linear buffer in
 * host memory that this process owns, and the crop is copied out of it with
 * the CPU once the GPU is done.  The buffer is a memfd turned into a dma-buf
 * by /dev/udmabuf, imported as an EGLImage and attached to a framebuffer as a
 * renderbuffer, which the renderer is then pointed at.
 *
 * This is the one way to keep a driver's copy engines and shaders out of a
 * capture entirely, since every form of glReadPixels has to move pixels out
 * of a render target the driver placed and tiled as it saw fit.  The cost
 * moves into the render instead: every pixel the GPU draws goes to host
 * memory, and the frame is linear, so the measured rate is that of
 * rendering and capturing together, which is what the benchmark counts
 * anyway.
 */
static void udmabuf_init(struct capture *c, struct renderer *r)
{
	EGLDisplay dpy = eglGetCurrentDisplay();
	struct udmabuf_create create = { 0 };
	EGLint attrs[32];
	GLenum status;
	long page;
	int dev;
	int n = 0;

	if (!epoxy_has_egl_extension(dpy, "EGL_EXT_image_dma_buf_import"))
		die("the udmabuf method needs EGL_EXT_image_dma_buf_import");
	if (!epoxy_has_gl_extension("GL_OES_EGL_image"))
		die("the udmabuf method needs GL_OES_EGL_image");

	/* udmabuf wants whole pages and a memfd that cannot shrink */
	page = sysconf(_SC_PAGESIZE);
	c->map_size = (size_t)c->fb_width * c->fb_height * 4;
	c->map_size = (c->map_size + page - 1) / page * page;

	c->memfd = memfd_create("gles2-read-pixels-bench frame",
				MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (c->memfd < 0)
		die("memfd_create failed: %s", strerror(errno));
	if (ftruncate(c->memfd, (off_t)c->map_size) < 0)
		die("ftruncate of the frame memfd failed: %s", strerror(errno));
	if (fcntl(c->memfd, F_ADD_SEALS, F_SEAL_SHRINK) < 0)
		die("sealing the frame memfd failed: %s", strerror(errno));

	dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
	if (dev < 0)
		die("opening /dev/udmabuf failed: %s", strerror(errno));

	create.memfd = (uint32_t)c->memfd;
	create.flags = UDMABUF_FLAGS_CLOEXEC;
	create.offset = 0;
	create.size = c->map_size;
	c->dmabuf = ioctl(dev, UDMABUF_CREATE, &create);
	if (c->dmabuf < 0)
		die("UDMABUF_CREATE failed: %s", strerror(errno));
	close(dev);

	c->map = mmap(NULL, c->map_size, PROT_READ, MAP_SHARED, c->memfd, 0);
	if (c->map == MAP_FAILED)
		die("mmap of the frame memfd failed: %s", strerror(errno));

	attrs[n++] = EGL_WIDTH;
	attrs[n++] = c->fb_width;
	attrs[n++] = EGL_HEIGHT;
	attrs[n++] = c->fb_height;
	attrs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
	attrs[n++] = (EGLint)FOURCC_ABGR8888;
	attrs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;
	attrs[n++] = c->dmabuf;
	attrs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
	attrs[n++] = 0;
	attrs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
	attrs[n++] = c->fb_width * 4;

	/* say linear outright where that can be said */
	if (epoxy_has_egl_extension(dpy,
				    "EGL_EXT_image_dma_buf_import_modifiers")) {
		attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
		attrs[n++] = 0;
		attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
		attrs[n++] = 0;
	}
	attrs[n++] = EGL_NONE;

	c->image = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
				     NULL, attrs);
	if (c->image == EGL_NO_IMAGE_KHR)
		die("eglCreateImageKHR of the udmabuf failed (0x%04x)",
		    eglGetError());

	glGenRenderbuffers(1, &c->udma_rb);
	glBindRenderbuffer(GL_RENDERBUFFER, c->udma_rb);
	glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, c->image);

	glGenFramebuffers(1, &c->udma_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, c->udma_fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				  GL_RENDERBUFFER, c->udma_rb);
	gl_check("udmabuf render target creation");

	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		die("udmabuf framebuffer incomplete (0x%04x)", status);

	c->renderer = r;
	c->saved_fbo = r->fbo;
	r->fbo = c->udma_fbo;
	c->src_fbo = c->udma_fbo;
}

static void udmabuf_free(struct capture *c)
{
	if (c->renderer) {
		c->renderer->fbo = c->saved_fbo;
		c->renderer = NULL;
	}
	if (c->udma_fbo)
		glDeleteFramebuffers(1, &c->udma_fbo);
	if (c->udma_rb)
		glDeleteRenderbuffers(1, &c->udma_rb);
	if (c->image != EGL_NO_IMAGE_KHR)
		eglDestroyImageKHR(eglGetCurrentDisplay(), c->image);
	if (c->map && c->map != MAP_FAILED)
		munmap(c->map, c->map_size);
	if (c->dmabuf >= 0)
		close(c->dmabuf);
	if (c->memfd >= 0)
		close(c->memfd);
}

/*
 * dma-buf asks for CPU access to be bracketed like this, so that exporters
 * on machines where device writes are not coherent with the CPU caches can
 * do what is needed.  Here it is cheap and there is no reason to skip it.
 */
static void udmabuf_sync(const struct capture *c, uint64_t flags)
{
	struct dma_buf_sync sync = { .flags = flags | DMA_BUF_SYNC_READ };

	if (ioctl(c->dmabuf, DMA_BUF_IOCTL_SYNC, &sync) < 0)
		die("DMA_BUF_IOCTL_SYNC failed: %s", strerror(errno));
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

/*
 * The udmabuf buffer is the render target itself, so unlike a staging buffer
 * it cannot be built after the frame has been rendered and freed after the
 * capture; it has no per-capture variant.
 */
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
		  const struct options *o, struct renderer *r)
{
	enum capture_method m = t->method;

	memset(c, 0, sizeof(*c));
	c->memfd = -1;
	c->dmabuf = -1;
	c->image = EGL_NO_IMAGE_KHR;

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
	case CAPTURE_UDMABUF:
		udmabuf_init(c, r);
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

	case CAPTURE_UDMABUF:
		/*
		 * The frame is already where the CPU can read it; all that
		 * is left is to wait for the GPU to finish drawing it.  Row
		 * 0 of the buffer is GL row 0, the bottom, the same order
		 * glReadPixels delivers.
		 */
		glFinish();
		udmabuf_sync(c, DMA_BUF_SYNC_START);

		for (row = 0; row < c->h; row++) {
			const unsigned char *src = c->map +
				((size_t)(c->y + row) * c->fb_width + c->x) * 4;

			memcpy(c->dst + (size_t)row * c->row_bytes, src,
			       c->row_bytes);
		}

		udmabuf_sync(c, DMA_BUF_SYNC_END);
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
	if (c->method == CAPTURE_UDMABUF)
		udmabuf_free(c);

	staging_free(c);
	free(c->dst);
	memset(c, 0, sizeof(*c));
}
