#ifndef __LAYOUT_CANVAS_H__
#define __LAYOUT_CANVAS_H__ 1

#include "mango/common/types.h"
#include "mango/layout/canvas_pack.h"
#include <stdbool.h>
#include <stdint.h>

// the canvas layout, windows sit at fixed spots on an infinite 2d plane instead
// of being packed into the monitor each monitor has a camera
// (Monitor.cam_x/cam_y) saying which canvas point is at its top left so
// layout_x = m->m.x + (canvas_x - cam_x)
// Monitor.zoom scales it all down from there and never goes past 1.0

// layout entry point
void canvas(Monitor *m);

// is this monitors current tag on the canvas layout
bool canvas_active(Monitor *m);

// gives a client its first canvas spot, centred in the viewport
void canvas_seed_client(Client *c, Monitor *m);

// writes a layout space box back to the clients canvas coordinates
// call it after any move or resize by hand so the result survives re-arranges
// takes the box as an argument because resize() goes through the animation path
// and c->geom isnt always what was just applied
void canvas_commit_box(Client *c, struct wlr_box layout_geom);

// snaps a layout space box to neighbouring window edges
// give it the box that goes to resize() not just the stored canvas position
void canvas_snap_layout(Client *c, struct wlr_box *box);
void canvas_drop_preview(Client *c);
bool canvas_resize_snap_apply(Client *c);
void canvas_drop_preview_hide(void);
int canvas_cluster(Monitor *m, Client *exclude, Client **ids, pack_rect *r);
void canvas_note_focus(Client *c);
int canvas_cluster_labels(Monitor *m, const pack_rect *r, int n, int *label);
double canvas_cluster_reach(Monitor *m);
bool canvas_select_begin(double x, double y);
void canvas_select_motion(double x, double y);
void canvas_select_end(void);
void canvas_select_clear(void);
void canvas_select_press(bool on_empty_canvas, double x, double y);
void canvas_select_release(double x, double y);
void canvas_group_follow(Client *c, double dx, double dy, bool glide);

// pans the camera by a canvas space delta
void canvas_pan(Monitor *m, double dx, double dy);

// dispatch actions you can bind in config
int32_t canvas_pan_action(const Arg *arg);
int32_t canvas_center_window(const Arg *arg);
int32_t canvas_reset_camera(const Arg *arg);
int32_t canvas_drag(const Arg *arg);

// pan by hand, driven from pointer_process_motion
// anchors stay double and re-entry is guarded, see canvas.c
void canvas_pan_begin(double cx, double cy);
void canvas_pan_motion(double cx, double cy);
void canvas_pan_end(void);
bool canvas_camera_moving(Monitor *m);
// on the canvas and not floating, drawn at its canvas_geom
bool canvas_on_plane(Client *c);

// moves the camera one frame, true while its still moving
// so the frame handler keeps going and stops when it settles
bool canvas_camera_tick(Monitor *m);

// true while the camera drives the layout, windows follow it rigidly with no
// per window animation
bool canvas_camera_is_rigid(void);

// true when this monitors zoom isnt 1.0
bool canvas_zoomed(Monitor *m);

// canvas_zoom,<relative>,<percent> - phase 3b spike, no animation
int32_t canvas_zoom_action(const Arg *arg);
int32_t canvas_zoom_in(const Arg *arg);
int32_t canvas_zoom_out(const Arg *arg);

// retiles every visible window into a grid around the viewport centre, each
// window keeps its size
int32_t canvas_cascade(const Arg *arg);

// overview, toggles a view where distances are squashed logarithmically and
// frames it canvas_geom isnt touched so leaving restores the real layout
int32_t canvas_overview(const Arg *arg);
void canvas_overview_point(Monitor *m, double x, double y, double *outx,
						   double *outy);
void canvas_overview_layout(Monitor *m);

// leaves overview and goes to one window
void canvas_overview_pick(Client *c);

// canvas_overview with jump letters on the windows
int32_t canvas_jump(const Arg *arg);

// eases the camera to a canvas point at the current zoom
void canvas_fly_to(Monitor *m, double cx, double cy, bool animate_layout);

// focuses the nearest window in a direction, canvas_focus_dir,<dx>,<dy>
int32_t canvas_focus_dir(const Arg *arg);

// pans to a window the pointer just focused when canvas_follow_focus is on
void canvas_follow_focus(Client *c);

// border width a canvas client should have at this monitors zoom
// one place for it, layout and resize_apply() both use it
// canvas point at the centre of the usable area (monitor minus bars)
void canvas_view_centre(Monitor *m, double *cx, double *cy);
// usable area as a canvas space rect
void canvas_viewport(Monitor *m, double *x, double *y, double *w, double *h);

uint32_t canvas_border_width(Monitor *m);
uint32_t canvas_client_border_width(Client *c);
// border a client is drawn with, the one above or 0 if borderless
uint32_t canvas_frame_bw(Client *c);

// corner radius for a canvas client, scaled by the camera
uint32_t canvas_corner_radius(Monitor *m);

// scales a wayland clients surfaces to the zoom instead of cropping them
// has to be redone every frame because a surface commit resets dest_size
void canvas_apply_zoom_scale(Client *c);

// clears a held pan direction, called from the keyboard handler on release
// pan keys push for as long as theyre held
void canvas_pan_key_released(uint32_t keycode);

void begin_jump_mode(Monitor *m);
void finish_jump_mode(Monitor *m);
#endif
