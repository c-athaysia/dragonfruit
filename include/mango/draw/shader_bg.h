#ifndef __DRAW_SHADER_BG_H__
#define __DRAW_SHADER_BG_H__ 1

#include "mango/common/types.h"
#include <stdbool.h>
#include <stdint.h>

// procedural canvas background
// user glsl fragment shader gets drawn to an offscreen buffer at the bottom of
// the scene it gets the camera position so grain or dots show you moving
// uniforms same as driftwm so its shaders work as is
// varying vec2 v_coords   0..1 across the output
// uniform vec2 size       output size in pixels
// uniform vec2 u_camera   camera position in pixels
// only redraws when the camera moves or output resizes
// redraw every frame and the gpu never sleeps so dont

// compiles a fragment shader from a file, null or empty path turns the
// background off false on read or compile fail and the old shader stays
bool shader_bg_load(const char *path);

// redraw on next update, call it when the camera moves
void shader_bg_mark_dirty(void);

// draws if it needs to, true if it drew
bool shader_bg_update(Monitor *m);

// the background as a camera at cam_x cam_y zoom would see it into a new
// w by h buffer, for previews null when there is no shader caller drops it
struct wlr_buffer *shader_bg_render_view(Monitor *m, int32_t w, int32_t h,
										 double cam_x, double cam_y,
										 double zoom);
// frees this monitors buffer and scene node
void shader_bg_finish_monitor(Monitor *m);

// frees the compiled program
void shader_bg_finish(void);

#endif
