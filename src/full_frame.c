#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"

/*
 * Full-frame readback benchmark.  Each frame is rendered and then read back
 * whole into a malloc'd buffer, either straight through glReadPixels or by
 * way of pixel pack buffers, synchronously or with two of them alternating so
 * that the frame being copied out is the one read back on the previous
 * iteration.  The unaligned variants read back to a one byte offset inside
 * the pixel pack buffer, see pbo_init() in capture.c for why.
 */
enum ff_method {
	FF_READPIXELS,
	FF_PBO_SYNC,
	FF_PBO_ASYNC,
	FF_PBO_SYNC_UNALIGNED,
	FF_PBO_ASYNC_UNALIGNED,
	FF_METHOD_COUNT
};

struct ff_options {
	int width;
	int height;
	double duration;	/* seconds */
	int warmup;		/* frames discarded before measuring */
	int quality;		/* jpeg quality */
	bool encode;
	const char *outdir;
	unsigned method_mask;
};

struct ff_capture {
	enum ff_method method;
	int width;
	int height;
	size_t pitch;
	size_t frame_bytes;
	GLuint pbo[2];
	int npbo;
	size_t offset;		/* where the frame starts inside a pbo */
	int cur;		/* the pbo the next readback goes into */
	bool pending;		/* the other pbo holds a frame not yet copied */
	unsigned char *dst;	/* malloc'd destination, the thing we time */
	uint64_t completed;
};

struct ff_result {
	enum ff_method method;
	uint64_t frames;
	double elapsed;
	struct load load;
	char output[512];
};

/* see capture_touch() in capture.c */
static volatile uint64_t ff_checksum;

static const char *ff_method_name(enum ff_method m)
{
	switch (m) {
	case FF_READPIXELS:
		return "readpixels";
	case FF_PBO_SYNC:
		return "pbo-sync";
	case FF_PBO_ASYNC:
		return "pbo-async";
	case FF_PBO_SYNC_UNALIGNED:
		return "pbo-sync-unaligned";
	case FF_PBO_ASYNC_UNALIGNED:
		return "pbo-async-unaligned";
	default:
		return "unknown";
	}
}

static const char *ff_method_description(enum ff_method m)
{
	switch (m) {
	case FF_READPIXELS:
		return "one full-frame glReadPixels straight into the "
		       "destination buffer";
	case FF_PBO_SYNC:
		return "full-frame glReadPixels into a single pixel pack "
		       "buffer, then mapping that same buffer and a memcpy "
		       "out of it";
	case FF_PBO_ASYNC:
		return "full-frame glReadPixels into one of two alternating "
		       "pixel pack buffers, then mapping the other one, which "
		       "holds the previous frame, and a memcpy out of it";
	case FF_PBO_SYNC_UNALIGNED:
		return "as pbo-sync, but reading back to a one byte offset "
		       "inside the pixel pack buffer, which keeps Mesa off its "
		       "shader based pack buffer download";
	case FF_PBO_ASYNC_UNALIGNED:
		return "as pbo-async, but reading back to a one byte offset "
		       "inside the pixel pack buffers, which keeps Mesa off its "
		       "shader based pack buffer download";
	default:
		return "unknown";
	}
}

/*
 * GL_OES_mapbuffer on its own only offers write-only mappings, so reading the
 * result back needs glMapBufferRangeEXT.
 */
static void pbo_check_extensions(void)
{
	if (!epoxy_has_gl_extension("GL_NV_pixel_buffer_object"))
		die("pbo methods need GL_NV_pixel_buffer_object");
	if (!epoxy_has_gl_extension("GL_EXT_map_buffer_range"))
		die("pbo methods need GL_EXT_map_buffer_range to map for reading");
	if (!epoxy_has_gl_extension("GL_OES_mapbuffer"))
		die("pbo methods need GL_OES_mapbuffer for glUnmapBufferOES");
}

static void ff_capture_init(struct ff_capture *c, enum ff_method m,
			    const struct ff_options *o)
{
	int i;

	memset(c, 0, sizeof(*c));

	c->method = m;
	c->width = o->width;
	c->height = o->height;
	c->pitch = (size_t)c->width * 4;
	c->frame_bytes = c->pitch * (size_t)c->height;

	c->dst = xmalloc(c->frame_bytes);
	memset(c->dst, 0, c->frame_bytes);

	switch (m) {
	case FF_READPIXELS:
		return;
	case FF_PBO_SYNC:
		c->npbo = 1;
		break;
	case FF_PBO_ASYNC:
		c->npbo = 2;
		break;
	case FF_PBO_SYNC_UNALIGNED:
		c->npbo = 1;
		c->offset = 1;
		break;
	case FF_PBO_ASYNC_UNALIGNED:
		c->npbo = 2;
		c->offset = 1;
		break;
	default:
		die("unknown full-frame method");
	}

	pbo_check_extensions();

	glGenBuffers(c->npbo, c->pbo);
	for (i = 0; i < c->npbo; i++) {
		glBindBuffer(GL_PIXEL_PACK_BUFFER_NV, c->pbo[i]);

		/*
		 * ES 2.0 has only the *_DRAW usage hints and
		 * NV_pixel_buffer_object adds none, so DYNAMIC_DRAW is the
		 * closest legal description of a buffer rewritten every
		 * frame.
		 */
		glBufferData(GL_PIXEL_PACK_BUFFER_NV,
			     (GLsizeiptr)(c->frame_bytes + c->offset), NULL,
			     GL_DYNAMIC_DRAW);
	}
	glBindBuffer(GL_PIXEL_PACK_BUFFER_NV, 0);
	gl_check("pixel pack buffer creation");
}

static void ff_capture_fini(struct ff_capture *c)
{
	if (c->npbo)
		glDeleteBuffers(c->npbo, c->pbo);
	free(c->dst);
	memset(c, 0, sizeof(*c));
}

/*
 * With a pixel pack buffer bound the last glReadPixels argument is an offset
 * into that buffer rather than a client pointer.
 */
static void pbo_read(const struct ff_capture *c, GLuint pbo)
{
	glBindBuffer(GL_PIXEL_PACK_BUFFER_NV, pbo);
	glReadPixels(0, 0, c->width, c->height, GL_RGBA, GL_UNSIGNED_BYTE,
		     (void *)(uintptr_t)c->offset);
	glBindBuffer(GL_PIXEL_PACK_BUFFER_NV, 0);
}

/*
 * There is no fence here: mapping the buffer is what waits for the readback
 * into it to finish.
 */
static void pbo_copy_out(struct ff_capture *c, GLuint pbo)
{
	void *map;

	glBindBuffer(GL_PIXEL_PACK_BUFFER_NV, pbo);

	map = glMapBufferRangeEXT(GL_PIXEL_PACK_BUFFER_NV, (GLintptr)c->offset,
				  (GLsizeiptr)c->frame_bytes,
				  GL_MAP_READ_BIT_EXT);
	if (!map)
		die("glMapBufferRangeEXT of the pixel pack buffer failed");

	memcpy(c->dst, map, c->frame_bytes);

	if (!glUnmapBufferOES(GL_PIXEL_PACK_BUFFER_NV))
		die("the pixel pack buffer contents were lost while mapped");
	glBindBuffer(GL_PIXEL_PACK_BUFFER_NV, 0);
}

static void ff_capture_touch(const struct ff_capture *c)
{
	uint64_t sum = 0;
	int row;

	for (row = 0; row < c->height; row++)
		sum += c->dst[(size_t)row * c->pitch];

	ff_checksum += sum;
}

/*
 * Reads back the frame just rendered, and returns whether c->dst now holds a
 * new frame.  For FF_PBO_ASYNC that frame is the one rendered on the previous
 * call, and the very first call yields nothing.
 */
static bool ff_capture_frame(struct ff_capture *c)
{
	bool had_pending;

	switch (c->method) {
	case FF_READPIXELS:
		glReadPixels(0, 0, c->width, c->height, GL_RGBA,
			     GL_UNSIGNED_BYTE, c->dst);
		break;

	case FF_PBO_SYNC:
	case FF_PBO_SYNC_UNALIGNED:
		pbo_read(c, c->pbo[0]);
		pbo_copy_out(c, c->pbo[0]);
		break;

	case FF_PBO_ASYNC:
	case FF_PBO_ASYNC_UNALIGNED:
		pbo_read(c, c->pbo[c->cur]);

		had_pending = c->pending;
		c->pending = true;
		c->cur ^= 1;

		/* c->cur is now the buffer holding the previous frame */
		if (!had_pending)
			return false;

		pbo_copy_out(c, c->pbo[c->cur]);
		break;

	default:
		die("unknown full-frame method");
	}

	ff_capture_touch(c);
	c->completed++;
	return true;
}

static void usage(FILE *f, const char *argv0)
{
	int i;

	fprintf(f,
"usage: %s [options]\n"
"\n"
"Measures how many rendered frames can be read back whole into a malloc'd\n"
"buffer.  Results go to stdout as json, everything else to stderr.\n"
"\n"
"  -d, --duration SEC    measurement duration per test (default 1.0)\n"
"  -W, --width PX        frame width (default 1920)\n"
"  -H, --height PX       frame height (default 1080)\n"
"  -m, --method NAME     run only this method, may be repeated, 'all' or\n"
"                        one of the names below\n"
"  -w, --warmup N        warmup frames per test (default 10)\n"
"  -e, --encode          also encode captured frames to mjpeg, for\n"
"                        verification only; this costs frames\n"
"  -q, --quality N       jpeg quality, 1 to 100 (default 85)\n"
"  -o, --output-dir DIR  where to write mjpeg files (default .)\n"
"  -h, --help            show this help\n"
"\n"
"methods:\n",
		argv0);

	for (i = 0; i < FF_METHOD_COUNT; i++)
		fprintf(f, "  %s\n", ff_method_name(i));
}

static unsigned method_bit_from_name(const char *name)
{
	int i;

	if (!strcmp(name, "all"))
		return (1u << FF_METHOD_COUNT) - 1;

	for (i = 0; i < FF_METHOD_COUNT; i++)
		if (!strcmp(name, ff_method_name(i)))
			return 1u << i;

	die("unknown method '%s'", name);
	return 0;
}

static int parse_int(const char *s, const char *what)
{
	char *end;
	long v = strtol(s, &end, 10);

	if (*end || end == s)
		die("%s: '%s' is not an integer", what, s);
	if (v < 0 || v > 1 << 20)
		die("%s: %ld out of range", what, v);
	return (int)v;
}

static double parse_double(const char *s, const char *what)
{
	char *end;
	double v = strtod(s, &end);

	if (*end || end == s)
		die("%s: '%s' is not a number", what, s);
	return v;
}

static void parse_args(int argc, char **argv, struct ff_options *o)
{
	static const struct option longopts[] = {
		{ "duration",	required_argument, NULL, 'd' },
		{ "width",	required_argument, NULL, 'W' },
		{ "height",	required_argument, NULL, 'H' },
		{ "method",	required_argument, NULL, 'm' },
		{ "warmup",	required_argument, NULL, 'w' },
		{ "encode",	no_argument,	   NULL, 'e' },
		{ "quality",	required_argument, NULL, 'q' },
		{ "output-dir",	required_argument, NULL, 'o' },
		{ "help",	no_argument,	   NULL, 'h' },
		{ NULL,		0,		   NULL, 0 }
	};
	int c;

	o->width = 1920;
	o->height = 1080;
	o->duration = 1.0;
	o->warmup = 10;
	o->quality = 85;
	o->encode = false;
	o->outdir = ".";
	o->method_mask = 0;

	while ((c = getopt_long(argc, argv, "d:W:H:m:w:eq:o:h", longopts,
				NULL)) != -1) {
		switch (c) {
		case 'd':
			o->duration = parse_double(optarg, "--duration");
			break;
		case 'W':
			o->width = parse_int(optarg, "--width");
			break;
		case 'H':
			o->height = parse_int(optarg, "--height");
			break;
		case 'm':
			o->method_mask |= method_bit_from_name(optarg);
			break;
		case 'w':
			o->warmup = parse_int(optarg, "--warmup");
			break;
		case 'e':
			o->encode = true;
			break;
		case 'q':
			o->quality = parse_int(optarg, "--quality");
			break;
		case 'o':
			o->outdir = optarg;
			break;
		case 'h':
			usage(stdout, argv[0]);
			exit(EXIT_SUCCESS);
		default:
			usage(stderr, argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (optind < argc)
		die("unexpected argument '%s'", argv[optind]);

	if (!o->method_mask)
		o->method_mask = (1u << FF_METHOD_COUNT) - 1;

	if (o->width < 1 || o->height < 1)
		die("frame size must be at least 1x1");
	if (o->duration <= 0.0)
		die("duration must be positive");
	if (o->quality < 1 || o->quality > 100)
		die("jpeg quality must be between 1 and 100");
}

static void run_test(struct renderer *r, enum ff_method m,
		     const struct ff_options *o, struct ff_result *res)
{
	struct load_sample load_start, load_end;
	struct ff_capture cap;
	struct mjpeg *mj = NULL;
	double t0;
	int i;

	memset(res, 0, sizeof(*res));
	res->method = m;

	ff_capture_init(&cap, m, o);

	if (o->encode) {
		snprintf(res->output, sizeof(res->output),
			 "%s/fullframe-%s-%dx%d.mjpg", o->outdir,
			 ff_method_name(m), o->width, o->height);
		mj = mjpeg_open(res->output, o->width, o->height, o->quality);
	}

	log_msg("running %s: %s", ff_method_name(m), ff_method_description(m));

	/*
	 * Warm up so that buffer allocation and the first readback of a
	 * fresh surface land outside the measurement.  It also primes the
	 * async method, so that every measured iteration yields a frame.
	 */
	for (i = 0; i < o->warmup; i++) {
		renderer_draw(r, (double)i * 0.01);
		ff_capture_frame(&cap);
	}
	gl_check("warmup");
	cap.completed = 0;

	load_sample(&load_start);
	t0 = now_s();
	while (now_s() - t0 < o->duration) {
		renderer_draw(r, now_s() - t0);

		if (ff_capture_frame(&cap) && mj)
			mjpeg_write(mj, cap.dst, cap.pitch);
	}

	res->elapsed = now_s() - t0;
	load_sample(&load_end);
	res->frames = cap.completed;
	gl_check("capture loop");

	load_diff(&load_start, &load_end, &res->load);

	log_msg("  %llu frames in %.3f s (%.1f fps)",
		(unsigned long long)res->frames, res->elapsed,
		res->frames / res->elapsed);
	log_load(&res->load);

	if (mj)
		mjpeg_close(mj);
	ff_capture_fini(&cap);
}

static json_object *result_json(const struct ff_result *res)
{
	json_object *j = json_object_new_object();

	json_object_object_add(j, "test",
		json_object_new_string(ff_method_name(res->method)));
	json_object_object_add(j, "method",
		json_object_new_string(ff_method_name(res->method)));
	json_object_object_add(j, "description",
		json_object_new_string(ff_method_description(res->method)));
	json_object_object_add(j, "frames",
			       json_object_new_int64((int64_t)res->frames));
	json_add_double(j, "elapsed_s", res->elapsed, 6);
	json_add_double(j, "fps", res->elapsed > 0.0 ?
			res->frames / res->elapsed : 0.0, 2);
	json_add_load(j, &res->load);

	if (res->output[0])
		json_object_object_add(j, "output",
				       json_object_new_string(res->output));

	return j;
}

static json_object *config_json(const struct ff_options *o)
{
	json_object *j = json_object_new_object();

	json_object_object_add(j, "width", json_object_new_int(o->width));
	json_object_object_add(j, "height", json_object_new_int(o->height));
	json_add_double(j, "duration_s", o->duration, 3);
	json_object_object_add(j, "warmup_frames",
			       json_object_new_int(o->warmup));
	json_object_object_add(j, "format", json_object_new_string("RGBA8888"));
	json_object_object_add(j, "encode", json_object_new_boolean(o->encode));
	if (o->encode)
		json_object_object_add(j, "jpeg_quality",
				       json_object_new_int(o->quality));
	return j;
}

int main(int argc, char **argv)
{
	struct ff_options opts;
	struct gl_ctx gl;
	struct renderer renderer;
	struct ff_result results[FF_METHOD_COUNT];
	json_object *root, *array;
	int i;
	int nresults = 0;

	parse_args(argc, argv, &opts);

	gl_ctx_init(&gl);
	renderer_init(&renderer, opts.width, opts.height);

	log_msg("full frame %dx%d, %.3f s per test", opts.width, opts.height,
		opts.duration);

	for (i = 0; i < FF_METHOD_COUNT; i++) {
		if (!(opts.method_mask & (1u << i)))
			continue;
		run_test(&renderer, i, &opts, &results[nresults++]);
	}

	log_msg("copy checksum %016llx", (unsigned long long)ff_checksum);

	root = json_object_new_object();
	json_object_object_add(root, "gl", json_gl_info());
	json_object_object_add(root, "config", config_json(&opts));

	array = json_object_new_array();
	for (i = 0; i < nresults; i++)
		json_object_array_add(array, result_json(&results[i]));
	json_object_object_add(root, "results", array);

	puts(json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY));
	json_object_put(root);

	renderer_fini(&renderer);
	gl_ctx_fini(&gl);
	return EXIT_SUCCESS;
}
