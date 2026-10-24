#ifndef BENCH_H
#define BENCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <json-c/json.h>

/*
 * The ways of getting a cropped region of the rendered frame into a buffer
 * allocated with malloc().  The first three stay within OpenGL ES 2.0 core,
 * which offers no pixel pack buffers, no buffer mapping and no packing
 * skip/row-length state.  CAPTURE_DIRECT needs GL_NV_pack_subimage, because
 * ES 2.0 core PixelStore only has PACK_ALIGNMENT and so cannot describe a
 * destination row stride wider than the region being read.  CAPTURE_PBO and
 * CAPTURE_PBO_UNALIGNED need GL_NV_pixel_buffer_object and differ only in
 * where the crop lands inside the pack buffer, see pbo_init() in capture.c.
 * CAPTURE_GBM does not use glReadPixels at all: the frame is rendered into a
 * buffer the process allocated itself, see gbm_init() in capture.c.
 */
enum capture_method {
	CAPTURE_GPU_STAGING,
	CAPTURE_CPU_STAGING,
	CAPTURE_PER_ROW,
	CAPTURE_DIRECT,
	CAPTURE_PBO,
	CAPTURE_PBO_UNALIGNED,
	CAPTURE_GBM,
	CAPTURE_METHOD_COUNT
};

/*
 * One combination the benchmark runs: a capture method, and whether its
 * staging buffer is pre-allocated or built and torn down around every single
 * capture.  Methods that stage nothing have only the one variant.
 */
struct test {
	enum capture_method method;
	bool realloc_staging;
};

#define TEST_MAX (CAPTURE_METHOD_COUNT * 2)
#define TEST_NAME_MAX 64

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
	bool list_extensions;
	const char *outdir;
	unsigned test_mask;
};

/* util.c */
void log_msg(const char *fmt, ...);
void die(const char *fmt, ...);
double now_s(void);
void gl_check(const char *what);
uint8_t sum_bytes(const unsigned char *buf, size_t row_bytes, int rows,
		  size_t pitch);

/* tests.c */
const char *method_name(enum capture_method m);
const char *method_description(enum capture_method m);
int test_list(struct test *tests);
void test_name(const struct test *t, char *buf, size_t size);
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
struct gbm_device;
struct gbm_bo;

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
	size_t pbo_offset;	/* where the crop starts inside the pbo */
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

	bool realloc_staging;	/* staging lives for one capture only */
	bool staging_checked;	/* staging has been validated once already */

	/*
	 * CAPTURE_GBM renders into an EGLImage of its own in place of the
	 * renderer's framebuffer, which is put back at fini.
	 */
	struct renderer *renderer;
	GLuint saved_fbo;
	GLuint image_fbo;
	GLuint image_rb;
	EGLImageKHR image;

	/* gbm: the frame as a buffer object laid out the driver's way */
	int drm_fd;
	struct gbm_device *gbm;
	struct gbm_bo *bo;

	uint64_t completed;
};

bool method_has_staging(enum capture_method m);
void capture_init(struct capture *c, const struct test *t,
		  const struct options *o, struct renderer *r);
void capture_frame(struct capture *c);
void capture_fini(struct capture *c);

/* load.c */
#define LOAD_ENGINE_MAX 16
#define LOAD_ENGINE_NAME_MAX 32
#define LOAD_CLIENT_MAX 16

/*
 * Engine busy time comes in one of two forms: i915 and most drivers publish
 * drm-engine-<name> as busy ns, xe publishes drm-cycles-<name> next to the
 * drm-total-cycles-<name> that elapsed on the same clock.
 */
struct load_engine {
	char name[LOAD_ENGINE_NAME_MAX];
	uint64_t busy_ns;
	uint64_t cycles;
	uint64_t total_cycles;
	unsigned capacity;
};

struct load_sample {
	double wall;			/* seconds, CLOCK_MONOTONIC */
	double cpu;			/* process CPU seconds, all threads */
	bool has_drm;
	int n_engines;
	struct load_engine engines[LOAD_ENGINE_MAX];
};

/* load between two samples, as percentages of what was available */
struct load {
	int cpus;
	double cpu_s;
	double cpu_pct;
	bool has_gpu;
	double gpu_pct;			/* the busiest engine */
	int n_engines;
	struct {
		char name[LOAD_ENGINE_NAME_MAX];
		double pct;
	} engines[LOAD_ENGINE_MAX];
};

void load_sample(struct load_sample *s);
void load_diff(const struct load_sample *a, const struct load_sample *b,
	       struct load *out);

/* report.c */
void json_add_double(json_object *obj, const char *key, double v, int prec);
void json_add_load(json_object *res, const struct load *l);
json_object *json_gl_info(void);
void log_load(const struct load *l);

/* mjpeg.c */
struct mjpeg;

struct mjpeg *mjpeg_open(const char *path, int width, int height, int quality);
void mjpeg_write(struct mjpeg *m, const unsigned char *rgba, size_t pitch);
void mjpeg_close(struct mjpeg *m);

#endif /* BENCH_H */
