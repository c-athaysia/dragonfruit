#include "mango/draw/shader_bg.h"
#include "mango/common/log.h"
#include "mango/common/server.h"
#include "mango/manage/monitor.h"
#include "mango/layout/canvas.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <GLES2/gl2.h>
#ifndef SHADER_DIR
#define SHADER_DIR "/usr/share/dragonfruit/shaders"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <scenefx/render/fx_renderer/fx_renderer.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/pass.h>
#include <xf86drm.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

// fullscreen triangle strip in clip space
// v_coords is made in here so user shaders just declare it
static const char *VERT_SRC = "attribute vec2 a_pos;\n"
							  "varying vec2 v_coords;\n"
							  "void main() {\n"
							  "  v_coords = a_pos * 0.5 + 0.5;\n"
							  "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
							  "}\n";

static GLuint program;
static GLuint vbo;
static GLint loc_pos, loc_size, loc_camera, loc_zoom;
static bool gl_ready;

static GLuint compile_stage(GLenum type, const char *src, const char *label) {
	GLuint sh = glCreateShader(type);
	if (!sh)
		return 0;
	glShaderSource(sh, 1, &src, NULL);
	glCompileShader(sh);

	GLint ok = GL_FALSE;
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024] = {0};
		glGetShaderInfoLog(sh, sizeof(log) - 1, NULL, log);
		mango_error(true, WLR_ERROR, "shader_bg: %s failed to compile: %s", label, log);
		glDeleteShader(sh);
		return 0;
	}
	return sh;
}

static void gl_teardown(void) {
	if (program) {
		glDeleteProgram(program);
		program = 0;
	}
	if (vbo) {
		glDeleteBuffers(1, &vbo);
		vbo = 0;
	}
	gl_ready = false;
}

// gl only runs with the private context current so compile waits for the first
// shader_bg_update not config parse
static char *pending_src;
static bool pending_valid;

bool shader_bg_load(const char *path) {
	free(pending_src);
	pending_src = NULL;
	pending_valid = false;

	if (!path || !*path) {
		// disabling drops the program on the next update since thats when
		// theres a context only mark it if theres a program or every shaderless
		// session does a pointless pass
		pending_valid = program != 0;
		return true;
	}

	// no slash means stock shader name like grain dots plus
	// looked up in install dir after $DRAGONFRUIT_SHADER_DIR so build tree
	// works slash means its a path and used as is
	char *expanded = NULL;
	if (path[0] == '~' && path[1] == '/') {
		const char *home = getenv("HOME");
		if (home) {
			size_t n = strlen(home) + strlen(path);
			expanded = calloc(1, n);
			if (expanded)
				snprintf(expanded, n, "%s%s", home, path + 1);
		}
	}
	const char *real = expanded ? expanded : path;

	FILE *f = NULL;
	char stockbuf[1024];
	if (!strchr(real, '/')) {
		const char *dirs[2] = {getenv("DRAGONFRUIT_SHADER_DIR"), SHADER_DIR};
		for (size_t i = 0; i < 2 && !f; i++) {
			if (!dirs[i] || !*dirs[i])
				continue;
			snprintf(stockbuf, sizeof(stockbuf), "%s/%s.glsl", dirs[i], real);
			f = fopen(stockbuf, "rb");
			if (f)
				real = stockbuf;
		}
		if (!f) {
			mango_error(true, WLR_ERROR,
						"shader_bg: no stock shader named '%s' (looked in %s). "
						"Stock names: grain, dots, plus. Use a path with '/' "
						"for your own.",
						path, SHADER_DIR);
			free(expanded);
			return false;
		}
	} else {
		f = fopen(real, "rb");
	}
	if (!f) {
		mango_error(true, WLR_ERROR, "shader_bg: cannot open %s", real);
		free(expanded);
		return false;
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len <= 0 || len > (1 << 20)) {
		mango_error(true, WLR_ERROR, "shader_bg: %s is empty or implausibly large", real);
		fclose(f);
		free(expanded);
		return false;
	}
	char *src = calloc(1, (size_t)len + 1);
	if (!src || fread(src, 1, (size_t)len, f) != (size_t)len) {
		mango_error(true, WLR_ERROR, "shader_bg: failed reading %s", real);
		free(src);
		fclose(f);
		free(expanded);
		return false;
	}
	fclose(f);
	free(expanded);

	pending_src = src;
	pending_valid = true;
	return true;
}

// builds the program, needs a current gl context
static void gl_build_if_pending(void) {
	if (!pending_valid)
		return;
	pending_valid = false;

	gl_teardown();

	if (!pending_src)
		return; // off

	GLuint vs = compile_stage(GL_VERTEX_SHADER, VERT_SRC, "vertex shader");
	GLuint fs =
		compile_stage(GL_FRAGMENT_SHADER, pending_src, "background shader");
	free(pending_src);
	pending_src = NULL;
	if (!vs || !fs) {
		if (vs)
			glDeleteShader(vs);
		if (fs)
			glDeleteShader(fs);
		return;
	}

	program = glCreateProgram();
	glAttachShader(program, vs);
	glAttachShader(program, fs);
	glBindAttribLocation(program, 0, "a_pos");
	glLinkProgram(program);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint ok = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[1024] = {0};
		glGetProgramInfoLog(program, sizeof(log) - 1, NULL, log);
		mango_error(true, WLR_ERROR, "shader_bg: link failed: %s", log);
		glDeleteProgram(program);
		program = 0;
		return;
	}

	loc_pos = glGetAttribLocation(program, "a_pos");
	loc_size = glGetUniformLocation(program, "size");
	loc_camera = glGetUniformLocation(program, "u_camera");
	loc_zoom = glGetUniformLocation(program, "u_zoom");

	static const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	gl_ready = true;
	mango_error(true, WLR_INFO, "shader_bg: background shader ready");
}

void shader_bg_mark_dirty(void) {
	Monitor *m = NULL;
	wl_list_for_each(m, &server.monitors, link) {
		m->bg_dirty = true;
	}
}

// two ways to get the shader output into the scene, picked once per run:
// direct: render into a gbm buffer inside a scenefx pass, the scene samples it
// copy: render in a private egl context and hand the scene a cpu copy
// the nvidia blob (its userspace, open kernel module or not) leaked vram on
// every scenefx pass into the gbm buffer and never showed it, so it gets copy
// copy stalls on glReadPixels once per redraw (camera moves only)
static bool use_copy(void) {
	static int copy = -1;
	if (copy < 0) {
		const char *e = getenv("DRAGONFRUIT_SHADER_COPY"); // 0/1 overrides
		drmVersion *v = drmGetVersion(wlr_renderer_get_drm_fd(server.renderer));
		copy = e && *e ? *e == '1' : v && !strcmp(v->name, "nvidia-drm");
		drmFreeVersion(v);
		mango_error(true, WLR_INFO, "shader_bg: %s path",
					copy ? "copy (nvidia)" : "direct");
	}
	return copy;
}

struct cpu_buffer {
	struct wlr_buffer base;
	uint8_t *data;
};

static void cpu_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct cpu_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	free(buf->data);
	free(buf);
}

static bool cpu_buffer_begin(struct wlr_buffer *wlr_buffer, uint32_t flags,
							 void **data, uint32_t *format, size_t *stride) {
	struct cpu_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
		return false;
	*data = buf->data;
	*format = DRM_FORMAT_ABGR8888; // GL_RGBA bytes
	*stride = (size_t)wlr_buffer->width * 4;
	return true;
}

static void cpu_buffer_end(struct wlr_buffer *wlr_buffer) {}

static const struct wlr_buffer_impl cpu_buffer_impl = {
	.destroy = cpu_buffer_destroy,
	.begin_data_ptr_access = cpu_buffer_begin,
	.end_data_ptr_access = cpu_buffer_end,
};

// needs the private context current
static void gl_free_target(Monitor *m) {
	if (m->bg_fbo)
		glDeleteFramebuffers(1, &m->bg_fbo);
	if (m->bg_tex)
		glDeleteTextures(1, &m->bg_tex);
	m->bg_fbo = m->bg_tex = 0;
	m->bg_w = m->bg_h = 0;
}

// needs the private context current
static bool gl_ensure_target(Monitor *m, int32_t w, int32_t h) {
	if (m->bg_fbo && m->bg_w == w && m->bg_h == h)
		return true;
	gl_free_target(m);
	glGenTextures(1, &m->bg_tex);
	glBindTexture(GL_TEXTURE_2D, m->bg_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
				 NULL);
	glBindTexture(GL_TEXTURE_2D, 0);
	glGenFramebuffers(1, &m->bg_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, m->bg_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
						   m->bg_tex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		mango_error(true, WLR_ERROR, "shader_bg: %dx%d framebuffer incomplete",
					w, h);
		gl_free_target(m);
		gl_teardown(); // turn the shader off instead of retrying every frame
		return false;
	}
	m->bg_w = w;
	m->bg_h = h;
	m->bg_dirty = true;
	return true;
}

// the shader gets its own egl context on its own gbm device, nothing is shared
// with scenefx (the output is a cpu copy anyway). borrowing scenefx's context
// meant a render pass on a gbm buffer per redraw, and the nvidia blob leaks
// ~8 MiB/s of vram on every such pass no matter what is drawn
static struct gbm_device *gbm;
static EGLDisplay egl_dpy = EGL_NO_DISPLAY;
static EGLContext egl_ctx = EGL_NO_CONTEXT;
static bool ctx_failed; // dont retry every frame

struct egl_saved {
	EGLDisplay dpy;
	EGLContext ctx;
	EGLSurface draw, read;
};

static bool ctx_create(void) {
	int fd = wlr_renderer_get_drm_fd(server.renderer);
	if (fd < 0 || !(gbm = gbm_create_device(fd))) {
		mango_error(true, WLR_ERROR, "shader_bg: no gbm device for the renderer");
		return false;
	}
	static const EGLint attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	egl_dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
	if (egl_dpy == EGL_NO_DISPLAY || !eglInitialize(egl_dpy, NULL, NULL) ||
		!eglBindAPI(EGL_OPENGL_ES_API) ||
		(egl_ctx = eglCreateContext(egl_dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT,
									attribs)) == EGL_NO_CONTEXT) {
		mango_error(true, WLR_ERROR, "shader_bg: egl context failed (0x%x)",
					eglGetError());
		return false;
	}
	return true;
}

static void ctx_destroy(void) {
	if (egl_ctx != EGL_NO_CONTEXT)
		eglDestroyContext(egl_dpy, egl_ctx);
	if (egl_dpy != EGL_NO_DISPLAY)
		eglTerminate(egl_dpy);
	if (gbm)
		gbm_device_destroy(gbm);
	egl_ctx = EGL_NO_CONTEXT;
	egl_dpy = EGL_NO_DISPLAY;
	gbm = NULL;
}

// makes the private context current, ctx_end puts back whatever was current
static bool ctx_begin(struct egl_saved *sv) {
	sv->dpy = eglGetCurrentDisplay();
	sv->ctx = eglGetCurrentContext();
	sv->draw = eglGetCurrentSurface(EGL_DRAW);
	sv->read = eglGetCurrentSurface(EGL_READ);
	if (egl_ctx == EGL_NO_CONTEXT && !ctx_failed && !ctx_create()) {
		ctx_destroy();
		ctx_failed = true;
		return false;
	}
	return egl_ctx != EGL_NO_CONTEXT &&
		   eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_ctx);
}

static void ctx_end(struct egl_saved *sv) {
	if (sv->dpy != EGL_NO_DISPLAY)
		eglMakeCurrent(sv->dpy, sv->draw, sv->read, sv->ctx);
	else
		eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

static bool ensure_buffer(Monitor *m, int32_t w, int32_t h) {
	if (m->bg_buf && m->bg_w == w && m->bg_h == h)
		return true;

	if (m->bg_buf) {
		wlr_buffer_drop(m->bg_buf);
		m->bg_buf = NULL;
	}

	// wlroots 0.20 has no way to ask for render formats so the format is
	// hardcoded modifier list cant be empty or the gbm allocator asserts len >
	// 0 and the whole compositor dies DRM_FORMAT_MOD_INVALID is how you say
	// implicit
	uint64_t mods[] = {DRM_FORMAT_MOD_INVALID};
	struct wlr_drm_format fmt = {
		.format = DRM_FORMAT_ARGB8888,
		.len = 1,
		.capacity = 0,
		.modifiers = mods,
	};

	m->bg_buf = wlr_allocator_create_buffer(server.allocator, w, h, &fmt);
	if (!m->bg_buf) {
		mango_error(true, WLR_ERROR, "shader_bg: could not allocate a %dx%d buffer", w, h);
		return false;
	}
	m->bg_w = w;
	m->bg_h = h;
	m->bg_dirty = true;
	return true;
}

static void release_buffer(Monitor *m) {
	if (m->bg_node) {
		wlr_scene_buffer_set_buffer(m->bg_node, NULL);
		wlr_scene_node_set_enabled(&m->bg_node->node, false);
	}
	if (m->bg_buf) {
		wlr_buffer_drop(m->bg_buf);
		m->bg_buf = NULL;
	}
	m->bg_w = m->bg_h = 0;
}

// caller binds the target fbo
static void draw(Monitor *m, int32_t w, int32_t h) {
	glViewport(0, 0, w, h);
	glDisable(GL_BLEND);
	glClearColor(0.f, 0.f, 0.f, 0.f);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(program);
	if (loc_size >= 0)
		glUniform2f(loc_size, (GLfloat)w, (GLfloat)h);
	if (loc_camera >= 0)
		glUniform2f(loc_camera, (GLfloat)m->cam_x, (GLfloat)m->cam_y);
	if (loc_zoom >= 0)
		glUniform1f(loc_zoom, (GLfloat)(m->zoom > 0.0 ? m->zoom : 1.0));

	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glEnableVertexAttribArray((GLuint)loc_pos);
	glVertexAttribPointer((GLuint)loc_pos, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray((GLuint)loc_pos);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glUseProgram(0);
}

// returns m->bg_buf, still owned by the monitor
static struct wlr_buffer *render_direct(Monitor *m, int32_t w, int32_t h) {
	if (!server.allocator || !ensure_buffer(m, w, h))
		return NULL;
	// the pass makes the gl context current and binds this buffers fbo
	// all the gl below needs that including the deferred compile
	struct wlr_render_pass *pass =
		wlr_renderer_begin_buffer_pass(server.renderer, m->bg_buf, NULL);
	if (!pass)
		return NULL;
	gl_build_if_pending();
	if (gl_ready && program) {
		glBindFramebuffer(GL_FRAMEBUFFER,
						  fx_renderer_get_buffer_fbo(server.renderer, m->bg_buf));
		draw(m, w, h);
	}
	wlr_render_pass_submit(pass);
	return m->bg_buf;
}

// returns a new cpu buffer, caller drops it
static struct wlr_buffer *render_copy(Monitor *m, int32_t w, int32_t h) {
	// all the gl below needs the context including the deferred compile
	struct egl_saved sv;
	if (!ctx_begin(&sv)) {
		pending_valid = false; // no context, no shader
		return NULL;
	}
	gl_build_if_pending();

	struct cpu_buffer *out = NULL;
	if (gl_ready && program && gl_ensure_target(m, w, h) &&
		(out = calloc(1, sizeof(*out))) &&
		(out->data = malloc((size_t)w * h * 4))) {
		glBindFramebuffer(GL_FRAMEBUFFER, m->bg_fbo);
		draw(m, w, h);
		// gl row 0 is the bottom, it was the first row of the direct path
		// buffer too so orientation matches what the shaders were written for
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out->data);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		wlr_buffer_init(&out->base, &cpu_buffer_impl, w, h);
	} else if (out) {
		free(out);
		out = NULL;
	}
	if (!gl_ready || !program)
		gl_free_target(m);
	ctx_end(&sv);
	return out ? &out->base : NULL;
}

struct wlr_buffer *shader_bg_render_view(Monitor *m, int32_t w, int32_t h,
										 double cam_x, double cam_y,
										 double zoom) {
	if (!m || !server.renderer || w <= 0 || h <= 0 ||
		!(pending_valid || (gl_ready && program)))
		return NULL;
	double sx = m->cam_x, sy = m->cam_y, sz = m->zoom;
	m->cam_x = cam_x, m->cam_y = cam_y, m->zoom = zoom;
	struct wlr_buffer *out = NULL;
	if (use_copy()) {
		// the copy target resizes to fit, the next update sees the size
		// change and redraws the screen one
		out = render_copy(m, w, h);
	} else {
		// a buffer of its own the monitors one stays on screen
		struct wlr_buffer *live = m->bg_buf;
		int32_t lw = m->bg_w, lh = m->bg_h;
		m->bg_buf = NULL;
		bool drew = render_direct(m, w, h) != NULL;
		out = m->bg_buf;
		if (!drew && out) {
			wlr_buffer_drop(out);
			out = NULL;
		}
		m->bg_buf = live, m->bg_w = lw, m->bg_h = lh;
	}
	m->cam_x = sx, m->cam_y = sy, m->zoom = sz;
	if (out && !(gl_ready && program)) {
		wlr_buffer_drop(out);
		out = NULL;
	}
	return out;
}

static bool shader_bg_update_at(Monitor *m);

// draws at the camera canvas_bg_camera gives, the live one unless the
// background is still gliding after a tag switch
bool shader_bg_update(Monitor *m) {
	if (!m || !m->wlr_output || !server.renderer)
		return false;
	double x, y, z, sx = m->cam_x, sy = m->cam_y, sz = m->zoom;
	bool glide = canvas_bg_camera(m, &x, &y, &z);
	m->cam_x = x, m->cam_y = y, m->zoom = z;
	bool drew = shader_bg_update_at(m);
	m->cam_x = sx, m->cam_y = sy, m->zoom = sz;
	if (glide)
		wlr_output_schedule_frame(m->wlr_output);
	return drew;
}

static bool shader_bg_update_at(Monitor *m) {

	// no shader and none pending means nothing to draw so no buffer and no pass
	// it used to allocate a full buffer anyway and run an empty pass every
	// frame after the first pan
	if (!pending_valid && !(gl_ready && program)) {
		if (m->bg_node && m->bg_node->buffer)
			release_buffer(m);
		m->bg_dirty = false;
		return false;
	}

	int32_t w = m->m.width, h = m->m.height;
	if (w <= 0 || h <= 0)
		return false;

	bool cam_moved = m->bg_cam_x != m->cam_x || m->bg_cam_y != m->cam_y ||
					 m->bg_zoom != m->zoom;
	bool need = m->bg_dirty || cam_moved || m->bg_w != w || m->bg_h != h ||
				pending_valid;
	if (!need)
		return false;

	bool copy = use_copy();
	struct wlr_buffer *out = copy ? render_copy(m, w, h) : render_direct(m, w, h);

	if (!gl_ready || !program) {
		// shader got disabled or failed so drop the old frame
		if (out && copy)
			wlr_buffer_drop(out);
		release_buffer(m);
		m->bg_dirty = false;
		return false;
	}
	if (!out)
		return false;

	if (!m->bg_node) {
		m->bg_node = wlr_scene_buffer_create(&server.scene->tree, NULL);
		if (!m->bg_node) {
			if (copy)
				wlr_buffer_drop(out);
			return false;
		}
		// goes above the wallpaper layer and below the rest
		// these shaders are see through so they have to draw over the wallpaper
		// under LyrBg swaybg would just cover it
		// use the wrapper not layers[LyrBg] because place_above needs a sibling
		// since mango fa9a0877
		wlr_scene_node_place_above(&m->bg_node->node,
								   &server.layers_wrap[LyrBg]->node);
	}

	wlr_scene_buffer_set_buffer_with_damage(m->bg_node, out, NULL);
	if (copy)
		wlr_buffer_drop(out); // the scene holds its own lock
	wlr_scene_buffer_set_dest_size(m->bg_node, w, h);
	wlr_scene_node_set_position(&m->bg_node->node, m->m.x, m->m.y);
	wlr_scene_node_set_enabled(&m->bg_node->node, true);

	m->bg_cam_x = m->cam_x;
	m->bg_cam_y = m->cam_y;
	m->bg_zoom = m->zoom;
	m->bg_dirty = false;
	return true;
}

void shader_bg_finish_monitor(Monitor *m) {
	if (!m)
		return;
	if (m->bg_node) {
		wlr_scene_node_destroy(&m->bg_node->node);
		m->bg_node = NULL;
	}
	if (m->bg_buf) {
		wlr_buffer_drop(m->bg_buf);
		m->bg_buf = NULL;
	}
	struct egl_saved sv;
	if (m->bg_fbo && egl_ctx != EGL_NO_CONTEXT && ctx_begin(&sv)) {
		gl_free_target(m);
		ctx_end(&sv);
	}
	m->bg_w = m->bg_h = 0;
}

void shader_bg_finish(void) {
	struct egl_saved sv;
	if (egl_ctx == EGL_NO_CONTEXT)
		gl_teardown(); // direct path, as before
	else if (ctx_begin(&sv)) {
		gl_teardown();
		ctx_end(&sv);
	}
	ctx_destroy();
	free(pending_src);
	pending_src = NULL;
	pending_valid = false;
}
