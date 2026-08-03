#include <stdio.h>
#include <string.h>

#include "bench.h"

/*
 * A headless OpenGL ES 2.0 context.  EGL_MESA_platform_surfaceless gives us a
 * context without any window system, which is all we need since every frame is
 * rendered into a framebuffer object.
 */
void gl_ctx_init(struct gl_ctx *g)
{
	static const EGLint config_attrs[] = {
		EGL_SURFACE_TYPE,	EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE,	EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE,		8,
		EGL_GREEN_SIZE,		8,
		EGL_BLUE_SIZE,		8,
		EGL_ALPHA_SIZE,		8,
		EGL_NONE
	};
	static const EGLint ctx_attrs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	const char *client_exts;
	EGLConfig config;
	EGLint major, minor;
	EGLint num_configs = 0;

	client_exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
	if (!client_exts || !strstr(client_exts, "EGL_MESA_platform_surfaceless"))
		die("EGL_MESA_platform_surfaceless is not available");

	g->dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA,
					  EGL_DEFAULT_DISPLAY, NULL);
	if (g->dpy == EGL_NO_DISPLAY)
		die("eglGetPlatformDisplayEXT failed (0x%04x)", eglGetError());

	if (!eglInitialize(g->dpy, &major, &minor))
		die("eglInitialize failed (0x%04x)", eglGetError());

	if (!eglBindAPI(EGL_OPENGL_ES_API))
		die("eglBindAPI(EGL_OPENGL_ES_API) failed (0x%04x)", eglGetError());

	if (!eglChooseConfig(g->dpy, config_attrs, &config, 1, &num_configs) ||
	    num_configs < 1)
		die("no EGL config with an ES 2.0 renderable type");

	g->ctx = eglCreateContext(g->dpy, config, EGL_NO_CONTEXT, ctx_attrs);
	if (g->ctx == EGL_NO_CONTEXT)
		die("eglCreateContext failed (0x%04x)", eglGetError());

	if (!eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g->ctx))
		die("eglMakeCurrent failed (0x%04x)", eglGetError());

	log_msg("EGL %d.%d, %s", major, minor, eglQueryString(g->dpy, EGL_VENDOR));
	log_msg("GL_VENDOR   %s", glGetString(GL_VENDOR));
	log_msg("GL_RENDERER %s", glGetString(GL_RENDERER));
	log_msg("GL_VERSION  %s", glGetString(GL_VERSION));
}

void gl_ctx_fini(struct gl_ctx *g)
{
	eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(g->dpy, g->ctx);
	eglTerminate(g->dpy);
}
