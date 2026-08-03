#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "bench.h"

/*
 * A rotating triangle, drawn into a framebuffer object.  Deliberately trivial:
 * the render cost has to stay small enough that the readback is what the
 * benchmark actually measures.
 */
static const char vert_src[] =
	"attribute vec2 a_pos;\n"
	"attribute vec3 a_color;\n"
	"uniform float u_angle;\n"
	"uniform float u_aspect;\n"
	"varying vec3 v_color;\n"
	"void main()\n"
	"{\n"
	"	float s = sin(u_angle);\n"
	"	float c = cos(u_angle);\n"
	"	vec2 p = vec2(a_pos.x * c - a_pos.y * s,\n"
	"		      a_pos.x * s + a_pos.y * c);\n"
	"	p.x /= u_aspect;\n"
	"	gl_Position = vec4(p, 0.0, 1.0);\n"
	"	v_color = a_color;\n"
	"}\n";

static const char frag_src[] =
	"precision mediump float;\n"
	"varying vec3 v_color;\n"
	"void main()\n"
	"{\n"
	"	gl_FragColor = vec4(v_color, 1.0);\n"
	"}\n";

/*
 * Interleaved position (x, y) and colour (r, g, b).  The circumradius is 0.6
 * so that the triangle stays inside the default centred crop as it rotates,
 * which makes the mjpeg output actually worth looking at.
 */
static const GLfloat vertices[] = {
	 0.0000f,  0.60f,	1.0f, 0.2f, 0.2f,
	-0.5196f, -0.30f,	0.2f, 1.0f, 0.2f,
	 0.5196f, -0.30f,	0.2f, 0.4f, 1.0f,
};

static GLuint compile_shader(GLenum type, const char *src)
{
	GLuint shader = glCreateShader(type);
	GLint ok = GL_FALSE;

	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024] = "";

		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		die("%s shader compilation failed: %s",
		    type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
	}
	return shader;
}

static GLuint build_program(void)
{
	GLuint prog = glCreateProgram();
	GLuint vs = compile_shader(GL_VERTEX_SHADER, vert_src);
	GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src);
	GLint ok = GL_FALSE;

	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glBindAttribLocation(prog, 0, "a_pos");
	glBindAttribLocation(prog, 1, "a_color");
	glLinkProgram(prog);
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[1024] = "";

		glGetProgramInfoLog(prog, sizeof(log), NULL, log);
		die("program link failed: %s", log);
	}
	glDeleteShader(vs);
	glDeleteShader(fs);
	return prog;
}

void renderer_init(struct renderer *r, int width, int height)
{
	GLenum status;

	r->width = width;
	r->height = height;

	/*
	 * GL_RGBA8_OES needs GL_OES_rgb8_rgba8.  ES 2.0 core only guarantees
	 * GL_RGBA4 and friends for renderbuffers, and reading 8 bits per
	 * channel out of a 4 bit per channel buffer would not be a meaningful
	 * benchmark.
	 */
	if (!epoxy_has_gl_extension("GL_OES_rgb8_rgba8"))
		die("GL_OES_rgb8_rgba8 is required for an 8 bit render target");

	glGenRenderbuffers(1, &r->color_rb);
	glBindRenderbuffer(GL_RENDERBUFFER, r->color_rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, width, height);

	glGenFramebuffers(1, &r->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				  GL_RENDERBUFFER, r->color_rb);
	gl_check("render target creation");

	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		die("render framebuffer incomplete (0x%04x)", status);

	r->prog = build_program();
	r->u_angle = glGetUniformLocation(r->prog, "u_angle");
	r->u_aspect = glGetUniformLocation(r->prog, "u_aspect");

	glGenBuffers(1, &r->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
	gl_check("vertex buffer creation");
}

void renderer_draw(struct renderer *r, double t)
{
	glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
	glViewport(0, 0, r->width, r->height);

	glClearColor(0.08f, 0.09f, 0.12f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(r->prog);
	glUniform1f(r->u_angle, (GLfloat)fmod(t * 2.0, 2.0 * M_PI));
	glUniform1f(r->u_aspect, (GLfloat)r->width / (GLfloat)r->height);

	glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
			      (const void *)0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
			      (const void *)(2 * sizeof(GLfloat)));

	glDrawArrays(GL_TRIANGLES, 0, 3);

	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
}

void renderer_fini(struct renderer *r)
{
	glDeleteBuffers(1, &r->vbo);
	glDeleteProgram(r->prog);
	glDeleteFramebuffers(1, &r->fbo);
	glDeleteRenderbuffers(1, &r->color_rb);
}
