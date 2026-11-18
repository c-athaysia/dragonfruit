#include "mango/draw/shader_bg.h"
#include "mango/common/log.h"
#include "mango/common/server.h"
#include "mango/manage/monitor.h"

#include <drm_fourcc.h>
#include <scenefx/render/fx_renderer/fx_renderer.h>
#ifndef SHADER_DIR
#define SHADER_DIR "/usr/share/dragonfruit/shaders"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/pass.h>
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

// gl stuff needs a current context and wlroots only gives one inside a render
// pass so compile waits for the first shader_bg_update not config parse
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

bool shader_bg_update(Monitor *m) {
	if (!m || !m->wlr_output || !server.renderer || !server.allocator)
		return false;

	// no shader and none pending means nothing to draw so no buffer and no pass
	// it used to allocate a full buffer anyway and run an empty pass every
	// frame after the first pan
	if (!pending_valid && !(gl_ready && program)) {
		if (m->bg_buf)
			release_buffer(m);
		m->bg_dirty = false;
		return false;
	}

	int32_t w = m->m.width, h = m->m.height;
	if (w <= 0 || h <= 0)
		return false;

	bool cam_moved = m->bg_cam_x != m->cam_x || m->bg_cam_y != m->cam_y ||
					 m->bg_zoom != m->zoom;
	bool need = m->bg_dirty || cam_moved || !m->bg_buf || m->bg_w != w ||
				m->bg_h != h || pending_valid;
	if (!need)
		return false;

	if (!ensure_buffer(m, w, h))
		return false;

	// the pass makes the gl context current and binds this buffers fbo
	// all the gl below needs that including the deferred compile
	struct wlr_render_pass *pass =
		wlr_renderer_begin_buffer_pass(server.renderer, m->bg_buf, NULL);
	if (!pass)
		return false;

	gl_build_if_pending();

	if (gl_ready && program) {
		GLuint fbo = fx_renderer_get_buffer_fbo(server.renderer, m->bg_buf);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
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

	wlr_render_pass_submit(pass);

	if (!gl_ready || !program) {
		// shader got disabled or failed so drop the old frame and give the
		// buffer back
		release_buffer(m);
		m->bg_dirty = false;
		return false;
	}

	if (!m->bg_node) {
		m->bg_node = wlr_scene_buffer_create(&server.scene->tree, NULL);
		if (!m->bg_node)
			return false;
		// goes above the wallpaper layer and below the rest
		// these shaders are see through so they have to draw over the wallpaper
		// under LyrBg swaybg would just cover it
		// use the wrapper not layers[LyrBg] because place_above needs a sibling
		// since mango fa9a0877
		wlr_scene_node_place_above(&m->bg_node->node,
								   &server.layers_wrap[LyrBg]->node);
	}

	wlr_scene_buffer_set_buffer_with_damage(m->bg_node, m->bg_buf, NULL);
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
	m->bg_w = m->bg_h = 0;
}

void shader_bg_finish(void) {
	gl_teardown();
	free(pending_src);
	pending_src = NULL;
	pending_valid = false;
}
