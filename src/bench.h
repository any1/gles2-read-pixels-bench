#ifndef BENCH_H
#define BENCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <epoxy/egl.h>
#include <epoxy/gl.h>

/*
 * The ways of getting a cropped region of the rendered frame into a buffer
 * allocated with malloc().  The first three stay within OpenGL ES 2.0 core,
 * which offers no pixel pack buffers, no buffer mapping and no packing
 * skip/row-length state.  CAPTURE_DIRECT needs GL_NV_pack_subimage, because
 * ES 2.0 core PixelStore only has PACK_ALIGNMENT and so cannot describe a
 * destination row stride wider than the region being read.
 */
enum capture_method {
	CAPTURE_GPU_STAGING,
	CAPTURE_CPU_STAGING,
	CAPTURE_PER_ROW,
	CAPTURE_DIRECT,
	CAPTURE_PBO,
	CAPTURE_METHOD_COUNT
};

struct options {
	int width;
	int height;
	int crop_x;		/* top-left origin, as the user sees it */
	int crop_y;
	int crop_w;
	int crop_h;
	double duration;	/* seconds */
	int warmup;		/* frames discarded before measuring */
	int quality;		/* jpeg quality */
	bool encode;
	const char *outdir;
	unsigned method_mask;
};

/* util.c */
void log_msg(const char *fmt, ...);
void die(const char *fmt, ...);
double now_s(void);
void gl_check(const char *what);
const char *method_name(enum capture_method m);
const char *method_description(enum capture_method m);
void *xmalloc(size_t size);

/* gl_context.c */
struct gl_ctx {
	EGLDisplay dpy;
	EGLContext ctx;
};

void gl_ctx_init(struct gl_ctx *g);
void gl_ctx_fini(struct gl_ctx *g);

/* render.c */
struct renderer {
	GLuint fbo;
	GLuint color_rb;
	GLuint prog;
	GLuint vbo;
	GLint u_angle;
	GLint u_aspect;
	int width;
	int height;
};

void renderer_init(struct renderer *r, int width, int height);
void renderer_draw(struct renderer *r, double t);
void renderer_fini(struct renderer *r);

/* capture.c */
struct capture {
	enum capture_method method;
	int x;			/* crop rect in GL space, origin bottom-left */
	int y;
	int w;
	int h;
	int fb_width;
	int fb_height;
	GLuint src_fbo;		/* framebuffer the frame was rendered into */
	GLuint stage_tex;	/* GPU staging target */
	GLuint stage_fbo;
	GLuint pbo;		/* pixel pack buffer, mapped to read back */
	unsigned char *dst;	/* malloc'd destination, the thing we time */
	unsigned char *staging;	/* full frame CPU staging buffer */
	size_t dst_size;
	size_t row_bytes;	/* bytes per cropped row */
	size_t frame_bytes;	/* bytes per cropped frame */

	/*
	 * Where the crop ends up inside dst.  Tightly packed at the start of
	 * the buffer for every method but CAPTURE_DIRECT, which lands it at
	 * its natural position in a full frame worth of pixels.
	 */
	unsigned char *crop;
	size_t crop_pitch;

	uint64_t completed;
};

void capture_init(struct capture *c, enum capture_method m,
		  const struct options *o, const struct renderer *r);
void capture_frame(struct capture *c);
void capture_fini(struct capture *c);
uint64_t capture_get_checksum(void);

/* mjpeg.c */
struct mjpeg;

struct mjpeg *mjpeg_open(const char *path, int width, int height, int quality);
void mjpeg_write(struct mjpeg *m, const unsigned char *rgba, size_t pitch);
void mjpeg_close(struct mjpeg *m);

#endif /* BENCH_H */
