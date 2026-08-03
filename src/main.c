#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "bench.h"

#define CROP_CENTRED (-1)

struct result {
	enum capture_method method;
	uint64_t frames;
	double elapsed;
	char output[512];
};

/*
 * json-c serialises doubles with a fixed global format, which is not much use
 * when the same document holds both durations and frame rates.
 * json_object_new_double_s() lets each value carry its own text.
 */
static void add_double(json_object *obj, const char *key, double v, int prec)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "%.*f", prec, v);
	json_object_object_add(obj, key, json_object_new_double_s(v, buf));
}

static void usage(FILE *f, const char *argv0)
{
	fprintf(f,
"usage: %s [options]\n"
"\n"
"Measures how many rendered frames can have a cropped region read back into\n"
"a malloc'd buffer, using only OpenGL ES 2.0 core functionality.  Results go\n"
"to stdout as json, everything else to stderr.\n"
"\n"
"  -d, --duration SEC    measurement duration per method (default 1.0)\n"
"  -W, --width PX        frame width (default 1920)\n"
"  -H, --height PX       frame height (default 1080)\n"
"      --crop-width PX   cropped region width (default 1280)\n"
"      --crop-height PX  cropped region height (default 720)\n"
"      --crop-x PX       crop left edge, top-left origin (default centred)\n"
"      --crop-y PX       crop top edge, top-left origin (default centred)\n"
"  -m, --method NAME     run only this method, may be repeated; one of\n"
"                        gpu-staging, cpu-staging, per-row, direct, pbo,\n"
"                        all\n"
"  -w, --warmup N        warmup frames per method (default 10)\n"
"  -e, --encode          also encode captured frames to mjpeg, for\n"
"                        verification only; this costs frames\n"
"  -q, --quality N       jpeg quality, 1 to 100 (default 85)\n"
"  -o, --output-dir DIR  where to write mjpeg files (default .)\n"
"  -h, --help            show this help\n",
		argv0);
}

static unsigned method_bit_from_name(const char *name)
{
	int i;

	if (!strcmp(name, "all"))
		return (1u << CAPTURE_METHOD_COUNT) - 1;

	for (i = 0; i < CAPTURE_METHOD_COUNT; i++)
		if (!strcmp(name, method_name(i)))
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

static void parse_args(int argc, char **argv, struct options *o)
{
	enum {
		OPT_CROP_W = 1000,
		OPT_CROP_H,
		OPT_CROP_X,
		OPT_CROP_Y,
	};
	static const struct option longopts[] = {
		{ "duration",	 required_argument, NULL, 'd' },
		{ "width",	 required_argument, NULL, 'W' },
		{ "height",	 required_argument, NULL, 'H' },
		{ "crop-width",	 required_argument, NULL, OPT_CROP_W },
		{ "crop-height", required_argument, NULL, OPT_CROP_H },
		{ "crop-x",	 required_argument, NULL, OPT_CROP_X },
		{ "crop-y",	 required_argument, NULL, OPT_CROP_Y },
		{ "method",	 required_argument, NULL, 'm' },
		{ "warmup",	 required_argument, NULL, 'w' },
		{ "encode",	 no_argument,	    NULL, 'e' },
		{ "quality",	 required_argument, NULL, 'q' },
		{ "output-dir",	 required_argument, NULL, 'o' },
		{ "help",	 no_argument,	    NULL, 'h' },
		{ NULL,		 0,		    NULL, 0 }
	};
	int c;

	o->width = 1920;
	o->height = 1080;
	o->crop_w = 1280;
	o->crop_h = 720;
	o->crop_x = CROP_CENTRED;
	o->crop_y = CROP_CENTRED;
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
		case OPT_CROP_W:
			o->crop_w = parse_int(optarg, "--crop-width");
			break;
		case OPT_CROP_H:
			o->crop_h = parse_int(optarg, "--crop-height");
			break;
		case OPT_CROP_X:
			o->crop_x = parse_int(optarg, "--crop-x");
			break;
		case OPT_CROP_Y:
			o->crop_y = parse_int(optarg, "--crop-y");
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
		o->method_mask = (1u << CAPTURE_METHOD_COUNT) - 1;

	if (o->width < 1 || o->height < 1)
		die("frame size must be at least 1x1");
	if (o->crop_w < 1 || o->crop_h < 1)
		die("crop size must be at least 1x1");
	if (o->crop_w > o->width || o->crop_h > o->height)
		die("crop %dx%d is larger than the %dx%d frame",
		    o->crop_w, o->crop_h, o->width, o->height);

	if (o->crop_x == CROP_CENTRED)
		o->crop_x = (o->width - o->crop_w) / 2;
	if (o->crop_y == CROP_CENTRED)
		o->crop_y = (o->height - o->crop_h) / 2;

	if (o->crop_x + o->crop_w > o->width ||
	    o->crop_y + o->crop_h > o->height)
		die("crop %dx%d+%d+%d does not fit in a %dx%d frame",
		    o->crop_w, o->crop_h, o->crop_x, o->crop_y,
		    o->width, o->height);
	if (o->duration <= 0.0)
		die("duration must be positive");
	if (o->quality < 1 || o->quality > 100)
		die("jpeg quality must be between 1 and 100");
}

static void run_method(struct renderer *r, enum capture_method m,
		       const struct options *o, struct result *res)
{
	struct capture cap;
	struct mjpeg *mj = NULL;
	double t0;
	int i;

	memset(res, 0, sizeof(*res));
	res->method = m;

	capture_init(&cap, m, o, r);

	if (o->encode) {
		snprintf(res->output, sizeof(res->output),
			 "%s/readpixels-%s-%dx%d-crop%dx%d.mjpg",
			 o->outdir, method_name(m), o->width, o->height,
			 o->crop_w, o->crop_h);
		mj = mjpeg_open(res->output, o->crop_w, o->crop_h, o->quality);
	}

	log_msg("running %s: %s", method_name(m), method_description(m));

	/*
	 * Warm up so that shader compilation, buffer allocation and the first
	 * readback of a fresh surface land outside the measurement.
	 */
	for (i = 0; i < o->warmup; i++) {
		renderer_draw(r, (double)i * 0.01);
		capture_frame(&cap);
	}
	gl_check("warmup");
	cap.completed = 0;

	t0 = now_s();
	while (now_s() - t0 < o->duration) {
		renderer_draw(r, now_s() - t0);
		capture_frame(&cap);

		if (mj)
			mjpeg_write(mj, cap.crop, cap.crop_pitch);
	}

	res->elapsed = now_s() - t0;
	res->frames = cap.completed;
	gl_check("capture loop");

	log_msg("  %llu frames in %.3f s (%.1f fps)",
		(unsigned long long)res->frames, res->elapsed,
		res->frames / res->elapsed);

	if (mj)
		mjpeg_close(mj);
	capture_fini(&cap);
}

static json_object *result_json(const struct result *res)
{
	json_object *j = json_object_new_object();

	json_object_object_add(j, "method",
			       json_object_new_string(method_name(res->method)));
	json_object_object_add(j, "description",
		json_object_new_string(method_description(res->method)));
	json_object_object_add(j, "frames",
			       json_object_new_int64((int64_t)res->frames));
	add_double(j, "elapsed_s", res->elapsed, 6);
	add_double(j, "fps", res->elapsed > 0.0 ?
		   res->frames / res->elapsed : 0.0, 2);

	if (res->output[0])
		json_object_object_add(j, "output",
				       json_object_new_string(res->output));

	return j;
}

static json_object *config_json(const struct options *o)
{
	json_object *j = json_object_new_object();
	json_object *crop = json_object_new_object();

	json_object_object_add(j, "width", json_object_new_int(o->width));
	json_object_object_add(j, "height", json_object_new_int(o->height));

	json_object_object_add(crop, "x", json_object_new_int(o->crop_x));
	json_object_object_add(crop, "y", json_object_new_int(o->crop_y));
	json_object_object_add(crop, "width", json_object_new_int(o->crop_w));
	json_object_object_add(crop, "height", json_object_new_int(o->crop_h));
	json_object_object_add(j, "crop", crop);

	add_double(j, "duration_s", o->duration, 3);
	json_object_object_add(j, "warmup_frames",
			       json_object_new_int(o->warmup));
	json_object_object_add(j, "format", json_object_new_string("RGBA8888"));
	json_object_object_add(j, "encode", json_object_new_boolean(o->encode));
	if (o->encode)
		json_object_object_add(j, "jpeg_quality",
				       json_object_new_int(o->quality));
	return j;
}

static json_object *gl_json(void)
{
	json_object *j = json_object_new_object();

	json_object_object_add(j, "vendor",
		json_object_new_string((const char *)glGetString(GL_VENDOR)));
	json_object_object_add(j, "renderer",
		json_object_new_string((const char *)glGetString(GL_RENDERER)));
	json_object_object_add(j, "version",
		json_object_new_string((const char *)glGetString(GL_VERSION)));
	json_object_object_add(j, "shading_language_version",
		json_object_new_string((const char *)
			glGetString(GL_SHADING_LANGUAGE_VERSION)));
	return j;
}

int main(int argc, char **argv)
{
	struct options opts;
	struct gl_ctx gl;
	struct renderer renderer;
	struct result results[CAPTURE_METHOD_COUNT];
	json_object *root, *array;
	int i;
	int nresults = 0;

	parse_args(argc, argv, &opts);

	gl_ctx_init(&gl);
	renderer_init(&renderer, opts.width, opts.height);

	log_msg("frame %dx%d, crop %dx%d+%d+%d, %.3f s per method",
		opts.width, opts.height, opts.crop_w, opts.crop_h,
		opts.crop_x, opts.crop_y, opts.duration);

	for (i = 0; i < CAPTURE_METHOD_COUNT; i++) {
		if (!(opts.method_mask & (1u << i)))
			continue;
		run_method(&renderer, i, &opts, &results[nresults++]);
	}

	log_msg("copy checksum %016llx",
		(unsigned long long)capture_get_checksum());

	root = json_object_new_object();
	json_object_object_add(root, "gl", gl_json());
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
