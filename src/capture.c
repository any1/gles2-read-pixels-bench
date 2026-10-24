#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gbm.h>

#include "bench.h"

/*
 * The one pixel format used throughout, as a DRM fourcc: little endian
 * a:b:g:r in a 32 bit word, which is r, g, b, a in memory, the same as
 * GL_RGBA with GL_UNSIGNED_BYTE.  gbm.h carries the fourcc values, so libdrm
 * is not needed for them; the modifier constant it does not carry is
 * DRM_FORMAT_MOD_INVALID from drm_fourcc.h, spelled out below.
 */
#define FOURCC_ABGR8888 GBM_FORMAT_ABGR8888
#define FORMAT_MOD_INVALID 0x00ffffffffffffffULL

/* EGL_EXT_device_drm_render_node is newer than the epoxy headers here */
#ifndef EGL_DRM_RENDER_NODE_FILE_EXT
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#endif

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
 * Attaches c->image to a framebuffer as a renderbuffer and points the
 * renderer at it, so that every frame from here on is drawn into whatever
 * memory that image was made from.
 */
static void image_target_init(struct capture *c, struct renderer *r)
{
	GLenum status;

	if (!epoxy_has_gl_extension("GL_OES_EGL_image"))
		die("rendering into an EGLImage needs GL_OES_EGL_image");

	glGenRenderbuffers(1, &c->image_rb);
	glBindRenderbuffer(GL_RENDERBUFFER, c->image_rb);
	glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, c->image);

	glGenFramebuffers(1, &c->image_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, c->image_fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				  GL_RENDERBUFFER, c->image_rb);
	gl_check("EGLImage render target creation");

	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		die("EGLImage framebuffer incomplete (0x%04x)", status);

	c->renderer = r;
	c->saved_fbo = r->fbo;
	r->fbo = c->image_fbo;
	c->src_fbo = c->image_fbo;
}

static void image_target_free(struct capture *c)
{
	if (c->renderer) {
		c->renderer->fbo = c->saved_fbo;
		c->renderer = NULL;
	}
	if (c->image_fbo)
		glDeleteFramebuffers(1, &c->image_fbo);
	if (c->image_rb)
		glDeleteRenderbuffers(1, &c->image_rb);
	if (c->image != EGL_NO_IMAGE_KHR)
		eglDestroyImageKHR(eglGetCurrentDisplay(), c->image);
}

/*
 * The render node behind the EGL display, when EGL will say which it is, or
 * else the first one that opens.
 */
static int open_render_node(EGLDisplay dpy)
{
	EGLDeviceEXT dev = EGL_NO_DEVICE_EXT;
	EGLAttrib attr = 0;
	const char *exts, *path;
	char buf[32];
	int i, fd;

	exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
	if (exts && strstr(exts, "EGL_EXT_device_query") &&
	    eglQueryDisplayAttribEXT(dpy, EGL_DEVICE_EXT, &attr))
		dev = (EGLDeviceEXT)attr;

	if (dev != EGL_NO_DEVICE_EXT) {
		exts = eglQueryDeviceStringEXT(dev, EGL_EXTENSIONS);
		path = exts && strstr(exts, "EGL_EXT_device_drm_render_node") ?
			eglQueryDeviceStringEXT(dev, EGL_DRM_RENDER_NODE_FILE_EXT) :
			NULL;
		if (path) {
			fd = open(path, O_RDWR | O_CLOEXEC);
			if (fd < 0)
				die("opening %s failed: %s", path, strerror(errno));
			log_msg("gbm on %s, named by EGL", path);
			return fd;
		}
	}

	for (i = 128; i < 192; i++) {
		snprintf(buf, sizeof(buf), "/dev/dri/renderD%d", i);
		fd = open(buf, O_RDWR | O_CLOEXEC);
		if (fd >= 0) {
			log_msg("gbm on %s, the first render node found", buf);
			return fd;
		}
	}

	die("no DRM render node could be opened");
	return -1;
}

/*
 * No glReadPixels: the frame is a GBM buffer object created for rendering,
 * with the modifiers EGL is willing to import, so the driver puts it wherever
 * and tiles it however it renders best.  It is imported as an EGLImage and
 * attached to a framebuffer that the renderer is pointed at, and the crop is
 * read through gbm_bo_map(), which leaves it to the driver to get those
 * pixels into something the CPU can read.  Mesa copies through a linear
 * staging buffer the same way glReadPixels does, so what this measures is
 * that copy without the GL pixel path on top of it.
 */
static void gbm_init(struct capture *c, struct renderer *r)
{
	static const EGLint fd_attr[4] = {
		EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
		EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT,
	};
	static const EGLint offset_attr[4] = {
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
		EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT,
	};
	static const EGLint pitch_attr[4] = {
		EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
		EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
	};
	static const EGLint mod_lo_attr[4] = {
		EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
		EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
		EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
		EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
	};
	static const EGLint mod_hi_attr[4] = {
		EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
		EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
		EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
		EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
	};
	EGLDisplay dpy = eglGetCurrentDisplay();
	EGLuint64KHR mods[64];
	EGLint nmods = 0;
	EGLint attrs[64];
	uint64_t modifier;
	int fds[4];
	int planes, p;
	int n = 0;

	if (!epoxy_has_egl_extension(dpy, "EGL_EXT_image_dma_buf_import"))
		die("the gbm method needs EGL_EXT_image_dma_buf_import");

	c->drm_fd = open_render_node(dpy);
	c->gbm = gbm_create_device(c->drm_fd);
	if (!c->gbm)
		die("gbm_create_device failed");

	/*
	 * Given the choice, let GBM pick among the layouts EGL can import.
	 * Without modifiers both sides fall back to what the driver does by
	 * default for the format, which has to agree too.
	 */
	if (epoxy_has_egl_extension(dpy,
				    "EGL_EXT_image_dma_buf_import_modifiers") &&
	    !eglQueryDmaBufModifiersEXT(dpy, FOURCC_ABGR8888, 64, mods, NULL,
					&nmods))
		nmods = 0;

	if (nmods > 0)
		c->bo = gbm_bo_create_with_modifiers2(c->gbm, c->fb_width,
						      c->fb_height,
						      FOURCC_ABGR8888, mods,
						      nmods,
						      GBM_BO_USE_RENDERING);
	if (!c->bo)
		c->bo = gbm_bo_create(c->gbm, c->fb_width, c->fb_height,
				      FOURCC_ABGR8888, GBM_BO_USE_RENDERING);
	if (!c->bo)
		die("gbm_bo_create failed: %s", strerror(errno));

	modifier = gbm_bo_get_modifier(c->bo);
	planes = gbm_bo_get_plane_count(c->bo);
	if (planes < 1 || planes > 4)
		die("gbm buffer with %d planes", planes);

	log_msg("gbm buffer: modifier 0x%016llx, %d plane%s, stride %u",
		(unsigned long long)modifier, planes, planes == 1 ? "" : "s",
		gbm_bo_get_stride(c->bo));

	attrs[n++] = EGL_WIDTH;
	attrs[n++] = c->fb_width;
	attrs[n++] = EGL_HEIGHT;
	attrs[n++] = c->fb_height;
	attrs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
	attrs[n++] = (EGLint)FOURCC_ABGR8888;

	for (p = 0; p < planes; p++) {
		fds[p] = gbm_bo_get_fd_for_plane(c->bo, p);
		if (fds[p] < 0)
			die("gbm_bo_get_fd_for_plane(%d) failed: %s", p,
			    strerror(errno));

		attrs[n++] = fd_attr[p];
		attrs[n++] = fds[p];
		attrs[n++] = offset_attr[p];
		attrs[n++] = (EGLint)gbm_bo_get_offset(c->bo, p);
		attrs[n++] = pitch_attr[p];
		attrs[n++] = (EGLint)gbm_bo_get_stride_for_plane(c->bo, p);

		if (modifier != FORMAT_MOD_INVALID) {
			attrs[n++] = mod_lo_attr[p];
			attrs[n++] = (EGLint)(modifier & 0xffffffff);
			attrs[n++] = mod_hi_attr[p];
			attrs[n++] = (EGLint)(modifier >> 32);
		}
	}
	attrs[n++] = EGL_NONE;

	c->image = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
				     NULL, attrs);
	if (c->image == EGL_NO_IMAGE_KHR)
		die("eglCreateImageKHR of the gbm buffer failed (0x%04x)",
		    eglGetError());

	/* the image holds its own references to the planes */
	for (p = 0; p < planes; p++)
		close(fds[p]);

	image_target_init(c, r);
}

static void gbm_free(struct capture *c)
{
	image_target_free(c);
	if (c->bo)
		gbm_bo_destroy(c->bo);
	if (c->gbm)
		gbm_device_destroy(c->gbm);
	if (c->drm_fd >= 0)
		close(c->drm_fd);
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
 * The gbm buffer is the render target itself, so unlike a staging buffer it
 * cannot be built after the frame has been rendered and freed after the
 * capture; that method has no per-capture variant.
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
	c->drm_fd = -1;
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
	case CAPTURE_GBM:
	case CAPTURE_GBM_FULL:
		gbm_init(c, r);
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
	void *map, *map_data;
	uint32_t stride;
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

	case CAPTURE_GBM:
		/*
		 * gbm_bo_map() has the driver deliver just the crop, which
		 * for a tiled buffer means a copy into a linear staging
		 * buffer of its choosing.  It also waits for the GPU, but
		 * through a context of its own, so finish here first.  The
		 * map data has to come in clear or mesa refuses the map.
		 */
		glFinish();
		map_data = NULL;
		map = gbm_bo_map(c->bo, c->x, c->y, c->w, c->h,
				 GBM_BO_TRANSFER_READ, &stride, &map_data);
		if (!map)
			die("gbm_bo_map failed: %s", strerror(errno));

		for (row = 0; row < c->h; row++)
			memcpy(c->dst + (size_t)row * c->row_bytes,
			       (const unsigned char *)map + (size_t)row * stride,
			       c->row_bytes);

		gbm_bo_unmap(c->bo, map_data);
		break;

	case CAPTURE_GBM_FULL:
		/*
		 * The same, except that the whole buffer is mapped and the
		 * crop is picked out of it by the CPU, so the driver's copy
		 * covers the full frame rather than just the crop.
		 */
		glFinish();
		map_data = NULL;
		map = gbm_bo_map(c->bo, 0, 0, c->fb_width, c->fb_height,
				 GBM_BO_TRANSFER_READ, &stride, &map_data);
		if (!map)
			die("gbm_bo_map failed: %s", strerror(errno));

		for (row = 0; row < c->h; row++)
			memcpy(c->dst + (size_t)row * c->row_bytes,
			       (const unsigned char *)map +
			       (size_t)(c->y + row) * stride +
			       (size_t)c->x * 4,
			       c->row_bytes);

		gbm_bo_unmap(c->bo, map_data);
		break;

	default:
		die("unknown capture method");
	}

	if (c->realloc_staging)
		staging_free(c);

	c->completed++;
}

void capture_fini(struct capture *c)
{
	if (c->method == CAPTURE_DIRECT) {
		glPixelStorei(GL_PACK_ROW_LENGTH, 0);
		glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
		glPixelStorei(GL_PACK_SKIP_ROWS, 0);
	}
	if (c->method == CAPTURE_GBM || c->method == CAPTURE_GBM_FULL)
		gbm_free(c);

	staging_free(c);
	free(c->dst);
	memset(c, 0, sizeof(*c));
}
