#include "mango/layout/canvas.h"
#include "mango/animation/client.h"
#include "mango/animation/common.h"
#include "mango/common/log.h"
#include "mango/common/server.h"
#include "mango/common/types.h"
#include "mango/common/util.h"
#include "mango/input/pointer.h"
#include "mango/layout/arrange.h"
#include "mango/layout/canvas_pack.h"
#include "mango/layout/canvas_precompute.h"
#include "mango/layout/layout.h"
#include "mango/manage/client.h"
#include "mango/manage/monitor.h"
#include "mango/workspace_overview/workspace_overview.h"
#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>

// true while the camera drives the layout
// windows follow it stiffly with no animation
static bool camera_rigid;
static bool canvas_overview_fit(Monitor *m);
static void canvas_layout_pos(Monitor *m, Client *c, double *x, double *y);

// moves camera and zoom anchor together
// leave the anchor behind and the next zoom tick yanks the view back
static void canvas_shift(Monitor *m, double dx, double dy) {
	m->cam_x += dx;
	m->cam_y += dy;
	m->zoom_anchor_cx += dx;
	m->zoom_anchor_cy += dy;
	m->anchor_from_cx += dx;
	m->anchor_from_cy += dy;
}

bool canvas_active(Monitor *m) {
	if (!m || !m->pertag)
		return false;
	const Layout *lt = m->pertag->ltidxs[get_mon_curtag(m)];
	return lt && lt->id == CANVAS;
}

// gap between two windows same as tile() uses
// gappih side by side gappiv stacked and zero when gaps are off
// snap and placement use it so it looks tiled
static double canvas_gap_h(Monitor *m) {
	return server.enable_gaps && m ? (double)m->gappih : 0.0;
}

static double canvas_gap_v(Monitor *m) {
	return server.enable_gaps && m ? (double)m->gappiv : 0.0;
}

// view is the usable area m->w but the camera maps to the monitor origin m->m
// any middle of the view goes through these two or a top bar puts it too high
// anchor c* is the canvas point at this middle
static double view_off_x(Monitor *m) {
	return (double)(m->w.x - m->m.x) + (double)m->w.width / 2.0;
}

static double view_off_y(Monitor *m) {
	return (double)(m->w.y - m->m.y) + (double)m->w.height / 2.0;
}

void canvas_view_centre(Monitor *m, double *cx, double *cy) {
	*cx = m->cam_x + view_off_x(m) / m->zoom;
	*cy = m->cam_y + view_off_y(m) / m->zoom;
}

// camera position that puts canvas point cx cy in the middle of the view
static void canvas_cam_for_centre(Monitor *m, double cx, double cy,
								  double zoom) {
	m->cam_x = cx - view_off_x(m) / zoom;
	m->cam_y = cy - view_off_y(m) / zoom;
}

// usable area in canvas units
void canvas_viewport(Monitor *m, double *x, double *y, double *w, double *h) {
	double z = m->zoom > 0.0 ? m->zoom : 1.0;
	*x = m->cam_x + (double)(m->w.x - m->m.x) / z;
	*y = m->cam_y + (double)(m->w.y - m->m.y) / z;
	*w = (double)m->w.width / z;
	*h = (double)m->w.height / z;
}

// visible windows for cascade placement and precompute
// ids and canvas boxes minus exclude
int canvas_cluster(Monitor *m, Client *exclude, Client **ids, pack_rect *r) {
	int n = 0;
	Client *c = NULL;
	wl_list_for_each(c, &server.clients, link) {
		if (c == exclude || !VISIBLEON(c, m) || !ISFAKETILED(c) ||
			!c->canvas_placed || c->isminimized || c->iskilling || n >= 64)
			continue;
		ids[n] = c;
		r[n] = (pack_rect){c->canvas_geom.x, c->canvas_geom.y,
						   c->canvas_geom.width, c->canvas_geom.height};
		n++;
	}
	return n;
}

// biggest empty gap that still links two windows into one cluster
// with snap its just the tiling gap plus a hair for float noise
double canvas_cluster_reach(Monitor *m) {
	if (config.canvas_cluster_snap)
		return fmax(canvas_gap_h(m), canvas_gap_v(m)) + 1.0;
	return config.canvas_cluster_distance;
}

// labels each window with a cluster number
// big enough groups get 0 to count-1 and small groups are loose and get -1
// returns cluster count
// cascade overview and precompute all use this so they agree
int canvas_cluster_labels(Monitor *m, const pack_rect *r, int n, int *label) {
	int raw[64], size[64] = {0};
	int ng = pack_clusters(r, n, canvas_cluster_reach(m), raw);
	for (int i = 0; i < n; i++)
		size[raw[i]]++;
	int map[64], count = 0;
	for (int g = 0; g < ng; g++)
		map[g] = size[g] >= config.canvas_cluster_min_size ? count++ : -1;
	for (int i = 0; i < n; i++)
		label[i] = map[raw[i]];
	return count;
}

// on the plane means placed and not floating
// a floating window keeps its old canvas_geom but isnt drawn there
// so dont navigate to it or avoid it
bool canvas_on_plane(Client *c) {
	return c && c->canvas_placed && ISFAKETILED(c);
}

// auto-recent remembers the window you work in
// a focus right after placement is the auto focus on map not a choice so skip
// it else a burst of new windows clusters around the wrong one
void canvas_note_focus(Client *c) {
	if (!c || !c->mon || !canvas_on_plane(c) ||
		get_now_in_ms() - c->canvas_seeded_ms < 300)
		return;
	c->mon->canvas_work_anchor = c;
}

// true if a canvas rect overlaps any placed client on m
static bool canvas_spot_free(Monitor *m, Client *skip, double x, double y,
							 double w, double h) {
	// free means at least a tiling gap from everything
	// half a unit less so a spot exactly one gap away isnt touching
	const double pad_h = canvas_gap_h(m) - 0.5, pad_v = canvas_gap_v(m) - 0.5;
	Client *o = NULL;
	wl_list_for_each(o, &server.clients, link) {
		// not gated on VISIBLEON on purpose
		// a client being seeded has no tags yet so every window looked
		// invisible and new windows piled on one spot
		if (o == skip || o->mon != m || !o->canvas_placed || !ISFAKETILED(o))
			continue;
		if (x < o->canvas_geom.x + o->canvas_geom.width + pad_h &&
			o->canvas_geom.x < x + w + pad_h &&
			y < o->canvas_geom.y + o->canvas_geom.height + pad_v &&
			o->canvas_geom.y < y + h + pad_v)
			return false;
	}
	return true;
}

void canvas_seed_client(Client *c, Monitor *m) {
	if (!c || !m)
		return;

	// size for a new window
	// placeholder c->geom is the whole screen so use half the viewport instead
	// clients that need their own size float and arent laid out here
	double z = m->zoom > 0.0 ? m->zoom : 1.0;
	int32_t w = (int32_t)((double)m->w.width / z / 2.0);
	int32_t h = (int32_t)((double)m->w.height / z / 2.0);
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	// client still has the old size so canvas() must not learn from it until it
	// catches up
	c->canvas_resize_pending = true;

	// centred in the view in canvas units
	double cx, cy;
	canvas_view_centre(m, &cx, &cy);
	cx -= w / 2.0;
	cy -= h / 2.0;

	c->canvas_geom.width = w;
	c->canvas_geom.height = h;
	c->canvas_placed = true;
	c->canvas_seeded_ms = get_now_in_ms();

	// auto modes put the window where the cluster outline grows least see
	// canvas_pack.c focused is on the focused one and recent on the one you
	// were working in and compact on any with a one step lookahead usually
	// precomputed and no free border means best spot by any window then the
	// spiral below
	int mode = config.canvas_placement;
	if (mode != CANVAS_PLACE_CENTER) {
		Client *anchor =
			m->sel && m->sel != c && canvas_on_plane(m->sel) ? m->sel : NULL;
		if (mode == CANVAS_PLACE_RECENT) {
			Client *w0 = m->canvas_work_anchor, *o = NULL;
			bool alive = false;
			wl_list_for_each(o, &server.clients, link) alive |= o == w0;
			if (alive && w0 != c && canvas_on_plane(w0) && VISIBLEON(w0, m))
				anchor = w0;
			else
				m->canvas_work_anchor = anchor; // this bursts base
		}
		double px, py;
		if (anchor && canvas_precompute_spot(m, c, anchor, w, h, &px, &py)) {
			mango_error(true, WLR_DEBUG, "canvas: placement from precompute");
			c->canvas_geom.x = px;
			c->canvas_geom.y = py;
			return;
		}
		Client *ids[64];
		pack_rect r[65];
		int n = canvas_cluster(m, c, ids, r);
		double gh = canvas_gap_h(m), gv = canvas_gap_v(m);
		int a = 0;
		while (anchor && a < n && ids[a] != anchor)
			a++;
		bool found = false;
		if (mode == CANVAS_PLACE_COMPACT) {
			double fx, fy;
			canvas_view_centre(m, &fx, &fy);
			if (anchor && a < n)
				fx = r[a].x + r[a].w / 2, fy = r[a].y + r[a].h / 2;
			found = pack_place_compact(r, n, w, h, gh, gv, fx, fy, &px, &py);
		} else if (anchor && a < n) {
			found = pack_place_adjacent(r, n, a, w, h, gh, gv, &px, &py);
		}
		// anchor border is full so best spot beside any window
		if (!found && n > 0 && anchor && a < n)
			found = pack_place_cluster(r, n, w, h, gh, gv, r[a].x + r[a].w / 2,
									   r[a].y + r[a].h / 2, &px, &py);
		if (found) {
			mango_error(true, WLR_DEBUG,
						"canvas: placement from a live search");
			c->canvas_geom.x = px;
			c->canvas_geom.y = py;
			return;
		}
	}

	// center mode is the view middle then a spiral out in steps of window size
	// step must scale with the window
	// a fixed 48px nudge is smaller than any window so it all piled on the
	// fallback
	double sx = w + canvas_gap_h(m), sy = h + canvas_gap_v(m);
	for (int ring = 0; ring < 6; ring++) {
		for (int dy = -ring; dy <= ring; dy++) {
			for (int dx = -ring; dx <= ring; dx++) {
				// only the ring edge the inside was covered by a smaller ring
				if (ring > 0 && abs(dx) != ring && abs(dy) != ring)
					continue;
				double x = cx + dx * sx, y = cy + dy * sy;
				if (canvas_spot_free(m, c, x, y, w, h)) {
					c->canvas_geom.x = x;
					c->canvas_geom.y = y;
					return;
				}
			}
		}
	}
	c->canvas_geom.x = cx;
	c->canvas_geom.y = cy;
}

// canvas_cascade packs every visible window into a tidy group around the view
// middle biggest first sizes kept no layout to fall back on here so this is the
// tidy up
int32_t canvas_cascade(const Arg *arg) {
	(void)arg;
	Monitor *m = server.selected_monitor;
	if (!m || !canvas_active(m))
		return 0;

	// repack into the cluster with the smallest outline sizes kept see
	// canvas_pack.c usually already computed by the precompute worker so the
	// key just applies it
	Client *all[64], *list[64];
	pack_rect ra[64], r[64];
	int na = canvas_cluster(m, NULL, all, ra);
	if (na == 0)
		return 0;

	// scope is the focused windows cluster
	// but everything when theres nothing to tell apart
	// scope=all or one cluster or none or every window alone or focused one is
	// loose no focused window means the one nearest the view middle stands in
	int lab[64], n = 0;
	double vcx, vcy;
	canvas_view_centre(m, &vcx, &vcy);
	int nc = canvas_cluster_labels(m, ra, na, lab);
	int focus = -1;
	for (int i = 0; i < na; i++)
		if (all[i] == m->sel)
			focus = i;
	if (focus < 0) {
		double cx = vcx, cy = vcy, bd = INFINITY;
		for (int i = 0; i < na; i++) {
			double dx = ra[i].x + ra[i].w / 2 - cx;
			double dy = ra[i].y + ra[i].h / 2 - cy;
			if (dx * dx + dy * dy < bd)
				bd = dx * dx + dy * dy, focus = i;
		}
	}
	bool everything = config.canvas_cascade_scope == CANVAS_SCOPE_ALL ||
					  nc <= 1 || nc == na || lab[focus] < 0;
	for (int i = 0; i < na; i++)
		if (everything || lab[i] == lab[focus])
			list[n] = all[i], r[n++] = ra[i];

	// where the cluster is now so it can stay there with anchor=cluster
	double ox0 = 1e18, oy0 = 1e18, ox1 = -1e18, oy1 = -1e18;
	for (int i = 0; i < n; i++) {
		ox0 = fmin(ox0, r[i].x), oy0 = fmin(oy0, r[i].y);
		ox1 = fmax(ox1, r[i].x + r[i].w), oy1 = fmax(oy1, r[i].y + r[i].h);
	}

	bool cached = canvas_precompute_packed(m, list, n, r, everything);
	if (!cached)
		pack_min_perimeter(r, n, canvas_gap_h(m), canvas_gap_v(m));
	mango_error(true, WLR_DEBUG, "canvas: cascade of %d (%s) from %s", n,
				everything ? "everything" : "one cluster",
				cached ? "precompute" : "a live search");

	// center the result on the old cluster center or the viewport
	double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
	for (int i = 0; i < n; i++) {
		x0 = fmin(x0, r[i].x), y0 = fmin(y0, r[i].y);
		x1 = fmax(x1, r[i].x + r[i].w), y1 = fmax(y1, r[i].y + r[i].h);
	}
	double ccx = (ox0 + ox1) / 2, ccy = (oy0 + oy1) / 2;
	if (config.canvas_cascade_anchor == CANVAS_ANCHOR_VIEWPORT)
		ccx = vcx, ccy = vcy;
	double dx = ccx - (x0 + x1) / 2, dy = ccy - (y0 + y1) / 2;
	for (int i = 0; i < n; i++)
		r[i].x += dx, r[i].y += dy;

	// a repacked cluster can change shape and hit a neighbour
	// keep every other window a cluster distance away
	// move the repacked one the smallest amount that does it
	if (!everything) {
		pack_rect fixed[64];
		int f = 0;
		for (int i = 0; i < na; i++)
			if (lab[i] != lab[focus])
				fixed[f++] = ra[i];
		double ax, ay;
		if (pack_avoid(r, n, fixed, f, canvas_cluster_reach(m) + 1.0, &ax, &ay))
			for (int i = 0; i < n; i++)
				r[i].x += ax, r[i].y += ay;
	}
	for (int i = 0; i < n; i++) {
		// glide from wherever its drawn now even mid glide
		double fx, fy;
		canvas_layout_pos(m, list[i], &fx, &fy);
		list[i]->canvas_from_x = fx;
		list[i]->canvas_from_y = fy;
		list[i]->canvas_layout_moving = !m->canvas_overview;
		list[i]->canvas_geom.x = r[i].x;
		list[i]->canvas_geom.y = r[i].y;
	}

	// in the overview the layout changed under it
	// refit the view to the new layout or the cluster animates around an old
	// zoom and aim the way back at the cluster leaving used the old camera
	// which looked at nothing here
	if (m->canvas_overview) {
		double rx0 = 1e18, ry0 = 1e18, rx1 = -1e18, ry1 = -1e18;
		for (int i = 0; i < n; i++) {
			rx0 = fmin(rx0, r[i].x), ry0 = fmin(ry0, r[i].y);
			rx1 = fmax(rx1, r[i].x + r[i].w), ry1 = fmax(ry1, r[i].y + r[i].h);
		}
		m->ov_restore_cx = (rx0 + rx1) / 2;
		m->ov_restore_cy = (ry0 + ry1) / 2;
		canvas_overview_fit(m);
	}

	int32_t dur = config.animation_duration_move > 0
					  ? config.animation_duration_move
					  : 300;
	if (m->canvas_overview) {
		// in overview the card positions come from the targets so the glide is
		// mango per window animation camera_rigid has to stay off or windows
		// teleport
		m->layout_anim_until = get_now_in_ms() + dur;
		arrange(m, true, false);
	} else {
		// canvas glide through canvas_layout_pos()
		// camera tick rearranges every frame until it lands and stays rigid
		// window animation would ease again each frame and lag
		m->canvas_layout_start = get_now_in_ms();
		m->canvas_layout_dur = dur;
		m->canvas_layout_animating = true;
		bool was = camera_rigid;
		camera_rigid = true;
		arrange(m, false, false);
		camera_rigid = was;
	}
	wlr_output_schedule_frame(m->wlr_output);
	return 0;
}

// edge snap like mango apply_window_snap() but rests at canvas_gap_h/v and not
// flush closes the smallest facing gap under snap_distance and runs once on
// release a drop up to snap_distance into a neighbour gets pushed back out
// unlike mango canvas units so zoom doesnt matter and no monitor edges because
// the plane has none a neighbour only counts if the two windows face each other
// across the other axis
// o moves with c - c itself, or both are in the multi select on c's monitor
static bool canvas_in_group(Client *c, Client *o) {
	return o == c ||
		   (c->canvas_selected && o->canvas_selected && o->mon == c->mon &&
			VISIBLEON(o, c->mon) && ISFAKETILED(o));
}

// nearest gap each way (left right up down in s) from one window at bx by to
// the windows outside its group - only lowers what s already holds
// snap reach multiplier, the workspace overview previews are drawn so small
// that snap_distance canvas units is a few pixels there
static double snap_reach_scale = 1.0;

static void canvas_snap_dists(Client *c, Monitor *m, double bx, double by,
							  double s[4]) {
	double thr = (double)config.snap_distance * snap_reach_scale;
	double w = c->canvas_geom.width, h = c->canvas_geom.height;
	double gh = canvas_gap_h(m), gv = canvas_gap_v(m);
	const double reach_h = gh + thr, reach_v = gv + thr;
	double *snap_left = &s[0], *snap_right = &s[1], *snap_up = &s[2],
		   *snap_down = &s[3];

	Client *o = NULL;
	wl_list_for_each(o, &server.clients, link) {
		if (o == c || o->mon != m || !o->canvas_placed || !ISFAKETILED(o) ||
			!VISIBLEON(o, m))
			continue;
		if (c->canvas_selected && o->canvas_selected)
			continue; // its own group moves with it
		double ox = o->canvas_geom.x, oy = o->canvas_geom.y;
		double ow = o->canvas_geom.width, oh = o->canvas_geom.height;

		// dropped slightly into a neighbour so push it back out to the gap
		// along the shallowest side only if that side is within snap_distance
		// mango ignores negative gaps so never snaps an overlap
		// only for real intersections
		// a window stacked below and shifted sideways must not get shoved
		if (bx < ox + ow && ox < bx + w && by < oy + oh && oy < by + h) {
			double pen[4] = {bx - (ox + ow), ox - (bx + w), by - (oy + oh),
							 oy - (by + h)}; // all negative l r u d
			int k = 0;
			for (int i = 1; i < 4; i++)
				if (pen[i] > pen[k])
					k = i;
			if (pen[k] > -thr && pen[k] < s[k])
				s[k] = pen[k];
			continue;
		}

		bool vspan = by < oy + oh + reach_v && oy < by + h + reach_v;
		bool hspan = bx < ox + ow + reach_h && ox < bx + w + reach_h;

		if (vspan) {
			double gap_l = bx - (ox + ow); // my left to their right
			double gap_r = ox - (bx + w);  // my right to their left
			if (gap_l >= 0.0 && gap_l < *snap_left)
				*snap_left = gap_l;
			if (gap_r >= 0.0 && gap_r < *snap_right)
				*snap_right = gap_r;
		}
		if (hspan) {
			double gap_u = by - (oy + oh); // my top to their bottom
			double gap_d = oy - (by + h);  // my bottom to their top
			if (gap_u >= 0.0 && gap_u < *snap_up)
				*snap_up = gap_u;
			if (gap_d >= 0.0 && gap_d < *snap_down)
				*snap_down = gap_d;
		}
	}
}

// c dropped at x y snaps - a multi select snaps as one shape, every member's
// gaps count and they all shift by the same amount
static void canvas_snap_edges(Client *c, Monitor *m, double *x, double *y) {
	if (!config.enable_floating_snap)
		return;
	double thr = (double)config.snap_distance * snap_reach_scale;
	if (thr <= 0.0)
		return;

	double bx = *x, by = *y;
	double gh = canvas_gap_h(m), gv = canvas_gap_v(m);
	const double reach_h = gh + thr, reach_v = gv + thr;
	double s[4] = {reach_h, reach_h, reach_v, reach_v};
	double dx = bx - c->canvas_geom.x, dy = by - c->canvas_geom.y;
	Client *g = NULL;
	wl_list_for_each(g, &server.clients, link) if (canvas_in_group(c, g))
		canvas_snap_dists(g, m, g->canvas_geom.x + dx, g->canvas_geom.y + dy,
						  s);
	double snap_left = s[0], snap_right = s[1], snap_up = s[2],
		   snap_down = s[3];

	// closer side wins like mango and ties go to the second test like its <=
	// each leaves exactly the tiling gap
	if (snap_left < snap_right && snap_left < reach_h)
		*x = bx - (snap_left - gh);
	else if (snap_right <= snap_left && snap_right < reach_h)
		*x = bx + (snap_right - gh);

	if (snap_up < snap_down && snap_up < reach_v)
		*y = by - (snap_up - gv);
	else if (snap_down <= snap_up && snap_down < reach_v)
		*y = by + (snap_down - gv);
}

// snaps a layout space box to neighbour edges
// has to hit the box given to resize() and not just canvas_geom or the drag
// overwrites it call it after canvas_commit_box() or the rounded box undoes the
// exact position where a frame would rest if released now in canvas units and
// false if no snap
static bool canvas_snap_point(Client *c, Monitor *m, double *nx, double *ny);
static bool canvas_snap_target(Client *c, struct wlr_box box, double *nx,
							   double *ny) {
	if (!c || !c->mon || !canvas_active(c->mon) || !ISFAKETILED(c))
		return false;
	Monitor *m = c->mon;
	if (m->canvas_overview)
		return false; // overview positions are fake
	double z = m->zoom > 0.0 ? m->zoom : 1.0;
	*nx = (double)(box.x - m->m.x) / z + m->cam_x;
	*ny = (double)(box.y - m->m.y) / z + m->cam_y;
	return canvas_snap_point(c, m, nx, ny);
}

// snaps canvas position nx ny for c against windows visible on m
static bool canvas_snap_point(Client *c, Monitor *m, double *nx, double *ny) {
	double ox = *nx, oy = *ny;
	canvas_snap_edges(c, m, nx, ny);
	if (*nx == ox && *ny == oy)
		return false;
	// canvas_snap_no_overlap refuses a snap that would land on another window
	// touching at the gap is fine
	// window stays where dropped and no preview shows
	// the box only promises spots it can deliver
	// every member of a multi select has to land clear
	if (config.canvas_snap_no_overlap) {
		double dx = *nx - c->canvas_geom.x, dy = *ny - c->canvas_geom.y;
		Client *g = NULL, *o = NULL;
		wl_list_for_each(g, &server.clients, link) {
			if (!canvas_in_group(c, g))
				continue;
			double gx = g->canvas_geom.x + dx, gy = g->canvas_geom.y + dy;
			double w = g->canvas_geom.width, h = g->canvas_geom.height;
			wl_list_for_each(o, &server.clients, link) {
				if (canvas_in_group(c, o) || o->mon != m || !o->canvas_placed ||
					!ISFAKETILED(o) || !VISIBLEON(o, m))
					continue;
				if (gx < o->canvas_geom.x + o->canvas_geom.width &&
					o->canvas_geom.x < gx + w &&
					gy < o->canvas_geom.y + o->canvas_geom.height &&
					o->canvas_geom.y < gy + h)
					return false;
			}
		}
	}
	return true;
}

// workspace overview drop
// snaps against the windows of the tag it lands on not the shown one
// z is the preview's screen pixels per canvas unit, reach is snap_distance
// screen pixels there like it is on the canvas at zoom 1
bool canvas_snap_on_tags(Client *c, uint32_t tags, double z, double *nx,
						 double *ny) {
	if (!c || !c->mon || !canvas_active(c->mon) || !ISFAKETILED(c))
		return false;
	Monitor *m = c->mon;
	uint32_t saved = m->tagset[m->seltags];
	m->tagset[m->seltags] = tags;
	snap_reach_scale = z > 0.0 && z < 1.0 ? 1.0 / z : 1.0;
	bool snapped = canvas_snap_point(c, m, nx, ny);
	snap_reach_scale = 1.0;
	m->tagset[m->seltags] = saved;
	return snapped;
}

void canvas_snap_layout(Client *c, struct wlr_box *box) {
	double nx, ny;
	if (!box || !canvas_snap_target(c, *box, &nx, &ny))
		return;
	Monitor *m = c->mon;
	double z = m->zoom > 0.0 ? m->zoom : 1.0;
	// store the snapped position itself not one from the pixel box
	// box->x is rounded to a whole pixel so at 50 percent zoom a 5 unit gap
	// came out 6 callers commit the box first and snap after so this wins
	box->x = m->m.x + (int32_t)lround((nx - m->cam_x) * z);
	box->y = m->m.y + (int32_t)lround((ny - m->cam_y) * z);
	c->canvas_geom.x = nx;
	c->canvas_geom.y = ny;
}

// drop preview is mango dropcolor box but for the canvas
// shows where the release snap lands and only while a snap would happen
// made from the same scenefx bits as a window and sits just above the dragged
// one canvas_drop_* options default to the window ones one tree for the whole
// server and off when not dragging
typedef struct {
	struct wlr_scene_tree *tree;
	struct wlr_scene_shadow *shadow;
	struct wlr_scene_blur *blur;
	struct wlr_scene_rect *fill, *border, *inner;
} canvas_box;

static canvas_box selbox;
static canvas_box *drops; // one per window in the dragged group, grown on use
static size_t ndrops;

static void canvas_box_hide(canvas_box *b) {
	if (b->tree)
		wlr_scene_node_set_enabled(&b->tree->node, false);
}

static void canvas_drops_hide_from(size_t i) {
	for (; i < ndrops; i++)
		canvas_box_hide(&drops[i]);
}

void canvas_drop_preview_hide(void) { canvas_drops_hide_from(0); }

static canvas_box *canvas_drop_box(size_t i) {
	if (i >= ndrops) {
		canvas_box *d = realloc(drops, (i + 1) * sizeof(*d));
		if (!d)
			return NULL;
		for (drops = d; ndrops <= i; ndrops++)
			drops[ndrops] = (canvas_box){0};
	}
	return &drops[i];
}

// make it on first use and keep it a child of parent
// creation order is stacking order shadow blur fill border
static void canvas_box_ensure(canvas_box *b, struct wlr_scene_tree *parent) {
	if (!b->tree) {
		b->tree = wlr_scene_tree_create(parent);
		b->shadow = wlr_scene_shadow_create(b->tree, 0, 0, 0, 0.0f,
											config.shadowscolor);
		b->blur = wlr_scene_blur_create(b->tree, 0, 0);
		b->fill = wlr_scene_rect_create(b->tree, 0, 0, config.dropcolor);
		b->border = wlr_scene_rect_create(b->tree, 0, 0, config.focuscolor);
		b->inner = wlr_scene_rect_create(b->tree, 0, 0, config.focuscolor);
	} else if (b->tree->node.parent != parent) {
		wlr_scene_node_reparent(&b->tree->node, parent);
	}
}

// wlr_scene_rect blends premultiplied colours see HANDOFF 4.7 but config
// colours are straight unconverted a 30 percent border came out brighter than
// its own colour measured c594a3 for 0xA67C8E4D over 0x2C221E when 513d40 is
// right text-node.c does the same
static void premultiply(const float in[4], float out[4]) {
	out[0] = in[0] * in[3];
	out[1] = in[1] * in[3];
	out[2] = in[2] * in[3];
	out[3] = in[3];
}

// draws a box of w x h screen pixels in a canvas_box_style
// same scenefx pieces as a window a fill a border ring a shadow and a backdrop
// blur placed and clipped like client_draw_border() and client_draw_shadow()
// unset style fields use the window option and border width and radius follow
// zoom
static void canvas_box_decorate(canvas_box *b, const canvas_box_style *st,
								const float fill[4], double zoom, int32_t w,
								int32_t h) {
	double z = zoom > 0.0 && zoom < 1.0 ? zoom : 1.0;
	// border_width is the outer ring like border_px, inner_border_px and
	// inner_border_gap add a second ring inside it the same as on a window
	int32_t bw = st->border_width, iw = 0, gap = 0;
	if (bw > 0) {
		bw = MANGO_MAX(1, (int32_t)lround(bw * z));
		if (config.inner_borderpx > 0) {
			iw = MANGO_MAX(1, (int32_t)lround(config.inner_borderpx * z));
			gap = (int32_t)lround(config.inner_border_gap * z);
		}
	}
	int32_t r = st->radius >= 0 ? st->radius : (int32_t)config.border_radius;
	r = (int32_t)lround(r * z);
	if (bw + gap + iw > MANGO_MIN(w, h) / 2)
		iw = gap = 0; // too small for two rings, one border like a window
	bw = MANGO_MIN(bw, MANGO_MIN(w, h) / 2);
	int32_t tw = bw + gap + iw; // fill starts inside both rings
	struct fx_corner_radii corners = corner_radii_all(r);

	float col[4];
	premultiply(fill, col);
	wlr_scene_rect_set_color(b->fill, col);
	wlr_scene_node_set_position(&b->fill->node, tw, tw);
	wlr_scene_rect_set_size(b->fill, w - 2 * tw, h - 2 * tw);
	wlr_scene_rect_set_corner_radii(b->fill, corners);

	const float *outer =
		st->border_color_set ? st->border_color : config.focuscolor;
	wlr_scene_node_set_enabled(&b->border->node, bw > 0);
	if (bw > 0) {
		premultiply(outer, col);
		wlr_scene_rect_set_color(b->border, col);
		wlr_scene_rect_set_size(b->border, w, h);
		wlr_scene_rect_set_corner_radii(b->border, corners);
		wlr_scene_rect_set_clipped_region(
			b->border, (struct clipped_region){
						   .area = {bw, bw, w - 2 * bw, h - 2 * bw},
						   .corners = corners,
					   });
	}

	wlr_scene_node_set_enabled(&b->inner->node, iw > 0);
	if (iw > 0) {
		int32_t in = bw + gap;
		premultiply(st->inner_border_color_set	  ? st->inner_border_color
					: config.inner_focuscolor_set ? config.inner_focuscolor
												  : outer,
					col);
		wlr_scene_rect_set_color(b->inner, col);
		wlr_scene_node_set_position(&b->inner->node, in, in);
		wlr_scene_rect_set_size(b->inner, w - 2 * in, h - 2 * in);
		wlr_scene_rect_set_corner_radii(b->inner, corners);
		wlr_scene_rect_set_clipped_region(
			b->inner, (struct clipped_region){
						  .area = {iw, iw, w - 2 * tw, h - 2 * tw},
						  .corners = corners,
					  });
	}

	wlr_scene_node_set_enabled(&b->shadow->node, st->shadow);
	if (st->shadow) {
		int32_t ss =
			st->shadow_size >= 0 ? st->shadow_size : config.shadows_size;
		int32_t px = st->shadow_position_x != INT32_MIN
						 ? st->shadow_position_x
						 : config.shadows_position_x;
		int32_t py = st->shadow_position_y != INT32_MIN
						 ? st->shadow_position_y
						 : config.shadows_position_y;
		struct wlr_box box = {0, 0, w, h};
		struct wlr_box sbox = {px, py, w + 2 * ss, h + 2 * ss};
		struct wlr_box hole;
		wlr_box_intersection(&hole, &box, &sbox);
		hole.x -= px;
		hole.y -= py;
		wlr_scene_shadow_set_color(b->shadow, st->shadow_color_set
												  ? st->shadow_color
												  : config.shadowscolor);
		wlr_scene_shadow_set_blur_sigma(b->shadow, st->shadow_blur >= 0.0f
													   ? st->shadow_blur
													   : config.shadows_blur);
		wlr_scene_shadow_set_corner_radius(b->shadow, r);
		wlr_scene_node_set_position(&b->shadow->node, px, py);
		wlr_scene_shadow_set_size(b->shadow, sbox.width, sbox.height);
		wlr_scene_shadow_set_clipped_region(
			b->shadow,
			(struct clipped_region){.area = hole, .corners = corners});
	}

	wlr_scene_node_set_enabled(&b->blur->node, st->blur);
	if (st->blur) {
		wlr_scene_blur_set_size(b->blur, w, h);
		wlr_scene_blur_set_corner_radii(b->blur, corners);
		wlr_scene_blur_set_strength(b->blur, 1.0f);
		wlr_scene_blur_set_alpha(b->blur, 1.0f);
	}
}

// resize snap with canvas_resize_snap=edge
// only the dragged edges snap each to the nearest facing neighbour edge and
// rest a tiling gap away same reach and overlap rule as a move and the window
// never gets too small call it after canvas_commit_box() since it works on
// canvas_geom false means no snap
static bool canvas_resize_snap_target(Client *c, double *rx, double *ry,
									  double *rw, double *rh) {
	if (!c || !c->mon || !canvas_active(c->mon) || c->mon->canvas_overview ||
		!config.enable_floating_snap || config.snap_distance <= 0)
		return false;
	Monitor *m = c->mon;
	double thr = (double)config.snap_distance;
	double gh = canvas_gap_h(m), gv = canvas_gap_v(m);
	double x0 = c->canvas_geom.x, y0 = c->canvas_geom.y;
	double x1 = x0 + c->canvas_geom.width, y1 = y0 + c->canvas_geom.height;
	// a border grab drags one edge, a drag_corner grab two - only snap those
	uint32_t e = server.resize_edge;
	bool right = e & WLR_EDGE_RIGHT, bottom = e & WLR_EDGE_BOTTOM;
	bool hx = e & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
	bool vy = e & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM);
	double bx = INFINITY, by = INFINITY, ex = 0, ey = 0;

	Client *o = NULL;
	wl_list_for_each(o, &server.clients, link) {
		if (o == c || o->mon != m || !o->canvas_placed || o->isminimized ||
			o->iskilling || !VISIBLEON(o, m))
			continue;
		double ox0 = o->canvas_geom.x, oy0 = o->canvas_geom.y;
		double ox1 = ox0 + o->canvas_geom.width;
		double oy1 = oy0 + o->canvas_geom.height;
		// facing across the dragged vertical edge
		if (hx && y0 < oy1 + gv + thr && oy0 < y1 + gv + thr) {
			double d = right ? ox0 - x1 : x0 - ox1;
			bool ahead = right ? ox0 > x0 : ox1 < x1;
			if (ahead && d > -thr && d < gh + thr && fabs(d - gh) < bx)
				bx = fabs(d - gh), ex = right ? ox0 - gh : ox1 + gh;
		}
		// facing across the dragged horizontal edge
		if (vy && x0 < ox1 + gh + thr && ox0 < x1 + gh + thr) {
			double d = bottom ? oy0 - y1 : y0 - oy1;
			bool ahead = bottom ? oy0 > y0 : oy1 < y1;
			if (ahead && d > -thr && d < gv + thr && fabs(d - gv) < by)
				by = fabs(d - gv), ey = bottom ? oy0 - gv : oy1 + gv;
		}
	}
	if (!isfinite(bx) && !isfinite(by))
		return false;
	if (isfinite(bx)) {
		if (right)
			x1 = ex;
		else
			x0 = ex;
	}
	if (isfinite(by)) {
		if (bottom)
			y1 = ey;
		else
			y0 = ey;
	}
	double minw = 2.0 * config.borderpx + 32.0;
	if (x1 - x0 < minw || y1 - y0 < minw)
		return false;
	if (config.canvas_snap_no_overlap) {
		wl_list_for_each(o, &server.clients, link) {
			if (o == c || o->mon != m || !o->canvas_placed || o->isminimized ||
				o->iskilling || !VISIBLEON(o, m))
				continue;
			if (x0 < o->canvas_geom.x + o->canvas_geom.width &&
				o->canvas_geom.x < x1 &&
				y0 < o->canvas_geom.y + o->canvas_geom.height &&
				o->canvas_geom.y < y1)
				return false;
		}
	}
	*rx = x0, *ry = y0, *rw = x1 - x0, *rh = y1 - y0;
	return true;
}

// canvas rect with unscaled border as the layout frame canvas() would make
static struct wlr_box canvas_rect_to_layout(Monitor *m, double x, double y,
											double w, double h) {
	double z = m->zoom > 0.0 ? m->zoom : 1.0;
	int32_t bw2 = 2 * (int32_t)canvas_border_width(m);
	double cb2 = 2.0 * config.borderpx;
	return (struct wlr_box){
		.x = m->m.x + (int32_t)lround((x - m->cam_x) * z),
		.y = m->m.y + (int32_t)lround((y - m->cam_y) * z),
		.width = (int32_t)lround((w - cb2) * z) + bw2,
		.height = (int32_t)lround((h - cb2) * z) + bw2,
	};
}

// release of a resize with canvas_resize_snap=edge
// applies the edge snap to canvas_geom exact and float_geom for the animation
// false if nothing snaps
bool canvas_resize_snap_apply(Client *c) {
	double x, y, w, h;
	if (!canvas_resize_snap_target(c, &x, &y, &w, &h))
		return false;
	c->canvas_geom.x = x, c->canvas_geom.y = y;
	c->canvas_geom.width = w, c->canvas_geom.height = h;
	c->float_geom = canvas_rect_to_layout(c->mon, x, y, w, h);
	return true;
}

// box b in layout pixels on top of window c
static void canvas_drop_show(size_t i, Client *c, Monitor *m,
							 struct wlr_box b) {
	canvas_box *d = canvas_drop_box(i);
	if (!d || !c->scene)
		return;
	canvas_box_ensure(d, c->scene->node.parent);
	canvas_box_decorate(d, &config.canvas_drop, config.dropcolor, m->zoom,
						b.width, b.height);
	wlr_scene_node_set_position(&d->tree->node, b.x, b.y);
	wlr_scene_node_place_above(&d->tree->node, &c->scene->node);
	wlr_scene_node_set_enabled(&d->tree->node, true);
}

// workspace overview drop preview, box b in screen pixels at preview zoom z
// parented next to the overview so it outlives a rebuild of the overview tree
void canvas_wsov_drop_show(struct wlr_scene_tree *parent, struct wlr_box b,
						   double z) {
	canvas_box *d = canvas_drop_box(0);
	if (!d)
		return;
	canvas_box_ensure(d, parent);
	canvas_box_decorate(d, &config.canvas_drop, config.dropcolor, z, b.width,
						b.height);
	wlr_scene_node_set_position(&d->tree->node, b.x, b.y);
	wlr_scene_node_raise_to_top(&d->tree->node);
	wlr_scene_node_set_enabled(&d->tree->node, true);
}

// snap highlight for whatever release will do
// move snaps the position canvas_snap_target
// resize does what canvas_resize_snap says move beside a neighbour or snap the
// dragged edges or nothing
void canvas_drop_preview(Client *c) {
	if (!c || !c->scene || !c->mon || !config.enable_floating_snap) {
		canvas_drop_preview_hide();
		return;
	}
	Monitor *m = c->mon;
	bool resizing = server.cursor_mode == CurResize;
	struct wlr_box b;
	double nx, ny, nw, nh;
	size_t n = 0; // boxes past the first
	if (resizing && config.canvas_resize_snap == CANVAS_RESIZE_SNAP_OFF) {
		canvas_drop_preview_hide();
		return;
	} else if (resizing &&
			   config.canvas_resize_snap == CANVAS_RESIZE_SNAP_EDGE) {
		if (!canvas_resize_snap_target(c, &nx, &ny, &nw, &nh)) {
			canvas_drop_preview_hide();
			return;
		}
		b = canvas_rect_to_layout(m, nx, ny, nw, nh);
	} else {
		if (!canvas_snap_target(c, c->float_geom, &nx, &ny)) {
			canvas_drop_preview_hide();
			return;
		}
		double z = m->zoom > 0.0 ? m->zoom : 1.0;
		b = (struct wlr_box){m->m.x + (int32_t)lround((nx - m->cam_x) * z),
							 m->m.y + (int32_t)lround((ny - m->cam_y) * z),
							 c->float_geom.width, c->float_geom.height};
		// the rest of a multi select lands shifted the same as c
		double dx = nx - c->canvas_geom.x, dy = ny - c->canvas_geom.y;
		Client *g = NULL;
		wl_list_for_each(g, &server.clients, link) {
			if (g == c || !canvas_in_group(c, g))
				continue;
			canvas_drop_show(++n, g, m,
							 canvas_rect_to_layout(m, g->canvas_geom.x + dx,
												   g->canvas_geom.y + dy,
												   g->canvas_geom.width,
												   g->canvas_geom.height));
		}
	}
	canvas_drop_show(0, c, m, b);
	canvas_drops_hide_from(n + 1);
}

// multi select
// a move bind on empty canvas draws a rubber band
// windows it touches join live or only fully inside with
// canvas_select_mode=contain dragging a selected window moves them all and a
// plain click clears it
static struct {
	Monitor *m;
	double x0, y0, x1, y1; // layout screen coordinates
} sel;

bool canvas_select_begin(double x, double y) {
	Monitor *m = monitor_at_point(x, y);
	if (!config.canvas_select_enabled || !m || !canvas_active(m) ||
		m->canvas_overview)
		return false;
	sel.m = m;
	sel.x0 = sel.x1 = x;
	sel.y0 = sel.y1 = y;
	server.cursor_mode = CurCanvasSelect;
	wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, "crosshair");
	return true;
}

void canvas_select_motion(double x, double y) {
	Monitor *m = sel.m;
	if (!m)
		return;
	sel.x1 = x, sel.y1 = y;
	int32_t bx = (int32_t)fmin(sel.x0, sel.x1),
			by = (int32_t)fmin(sel.y0, sel.y1);
	int32_t bw = (int32_t)fabs(sel.x1 - sel.x0),
			bh = (int32_t)fabs(sel.y1 - sel.y0);
	if (bw < 2 || bh < 2) {
		canvas_box_hide(&selbox);
		return;
	}
	canvas_box_ensure(&selbox, server.layers[LyrFloat]);
	canvas_box_decorate(&selbox, &config.canvas_select,
						config.canvas_select.fill_set
							? config.canvas_select.fill
							: config.dropcolor,
						m->zoom, bw, bh);
	wlr_scene_node_set_position(&selbox.tree->node, bx, by);
	wlr_scene_node_raise_to_top(&selbox.tree->node);
	wlr_scene_node_set_enabled(&selbox.tree->node, true);

	// live what is in the band right now is the selection
	Client *c = NULL;
	wl_list_for_each(c, &server.clients, link) {
		if (!VISIBLEON(c, m) || !ISFAKETILED(c) || !c->canvas_placed)
			continue;
		struct wlr_box g = c->geom;
		bool hit = config.canvas_select_mode == CANVAS_SELECT_CONTAIN
					   ? g.x >= bx && g.y >= by && g.x + g.width <= bx + bw &&
							 g.y + g.height <= by + bh
					   : g.x < bx + bw && bx < g.x + g.width && g.y < by + bh &&
							 by < g.y + g.height;
		if (hit != c->canvas_selected) {
			c->canvas_selected = hit;
			client_update_border_color(c);
		}
	}
}

void canvas_select_end(void) {
	server.cursor_mode = CurNormal;
	wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, "default");
	canvas_box_hide(&selbox);
	// click without a drag clears the selection
	if (fabs(sel.x1 - sel.x0) < 2 && fabs(sel.y1 - sel.y0) < 2)
		canvas_select_clear();
	sel.m = NULL;
}

void canvas_select_clear(void) {
	Client *c = NULL;
	wl_list_for_each(c, &server.clients, link) if (c->canvas_selected) {
		c->canvas_selected = false;
		client_update_border_color(c);
	}
}

// click on empty canvas clears the selection canvas_select_clear_on_click
// only a click though
// a press that turns into a drag is a pan and leaves it alone so wait for
// release
static struct {
	bool armed;
	double x, y;
} click;

void canvas_select_press(bool on_empty_canvas, double x, double y) {
	click.armed = on_empty_canvas && config.canvas_select_clear_on_click;
	click.x = x, click.y = y;
}

void canvas_select_release(double x, double y) {
	if (click.armed && fabs(x - click.x) < 3 && fabs(y - click.y) < 3)
		canvas_select_clear();
	click.armed = false;
}

// group move
// moves every other selected visible window on the monitor by the same dx dy
// does nothing unless the dragged one is selected
// glide=false while dragging or they trail the pointer measured 88px behind
// glide=true for the release snap
void canvas_group_follow(Client *c, double dx, double dy, bool glide) {
	if (!c || !c->canvas_selected || !c->mon || (dx == 0.0 && dy == 0.0))
		return;
	Client *o = NULL;
	bool any = false;
	wl_list_for_each(o, &server.clients, link) {
		if (o == c || !canvas_in_group(c, o))
			continue;
		o->canvas_geom.x += dx;
		o->canvas_geom.y += dy;
		any = true;
	}
	if (!any)
		return;
	bool was = camera_rigid;
	camera_rigid = !glide;
	arrange(c->mon, false, false);
	camera_rigid = was;
}

void canvas_commit_box(Client *c, struct wlr_box layout_geom) {
	// floating isnt on the plane
	// it stays put on screen while the camera moves so a canvas position for it
	// is just a stale obstacle
	if (!c || !c->mon || !canvas_active(c->mon) || !ISFAKETILED(c))
		return;
	Monitor *m = c->mon;
	double z = m->zoom > 0.0 ? m->zoom : 1.0;
	// keep it double
	// rounding to ints makes drags step by 1/zoom pixels and at 200 percent it
	// jumps two pixels at a time
	c->canvas_geom.x = (double)(layout_geom.x - m->m.x) / z + m->cam_x;
	c->canvas_geom.y = (double)(layout_geom.y - m->m.y) / z + m->cam_y;
	// extents are screen space and the border in them is the scaled one
	// canvas_border_width() canvas_geom keeps content plus the unscaled border
	// so take the screen border off then unscale only the content then put the
	// canvas border back get it wrong and every drag resizes the client through
	// resize_apply() thats how dragging while zoomed out grew a window drag
	// after drag
	double bw2 = 2.0 * (double)canvas_frame_bw(c);
	double cbw2 = 2.0 * (double)config.borderpx;
	// a pure move gives back the frame canvas() built so keep that size
	// canvas() rounds content * zoom to whole pixels so the round trip loses
	// half a pixel / zoom at 20 percent 632 wide became 630 and every drag
	// shrank it measured 640x360 to 638x358 in one drag only an axis that
	// really changed is solved backwards
	int32_t fw =
		(int32_t)lround((c->canvas_geom.width - cbw2) * z) + (int32_t)bw2;
	int32_t fh =
		(int32_t)lround((c->canvas_geom.height - cbw2) * z) + (int32_t)bw2;
	if (layout_geom.width != fw)
		c->canvas_geom.width = ((double)layout_geom.width - bw2) / z + cbw2;
	if (layout_geom.height != fh)
		c->canvas_geom.height = ((double)layout_geom.height - bw2) / z + cbw2;
	c->canvas_placed = true;
	c->canvas_layout_moving = false; // the pointer has it now
}

void canvas_pan(Monitor *m, double dx, double dy) {
	if (!m)
		return;
	canvas_shift(m, dx, dy);
	arrange(m, false, false);
}

// where a window is drawn on the canvas right now
// normally canvas_geom but while a cascade glides it its a point on the move
// curve toward canvas_geom cascade animates in canvas space and the camera
// makes screen positions each frame so they compose screen space animation like
// mango per window made windows chase a moving target that was mod+t during a
// follow focus flight
static void canvas_layout_pos(Monitor *m, Client *c, double *x, double *y) {
	*x = c->canvas_geom.x, *y = c->canvas_geom.y;
	if (!c->canvas_layout_moving || !m->canvas_layout_animating)
		return;
	double t = (double)(get_now_in_ms() - m->canvas_layout_start) /
			   (double)(m->canvas_layout_dur > 0 ? m->canvas_layout_dur : 1);
	if (t >= 1.0)
		return;
	double e = find_animation_curve_at(t < 0.0 ? 0.0 : t, MOVE);
	*x = c->canvas_from_x + (c->canvas_geom.x - c->canvas_from_x) * e;
	*y = c->canvas_from_y + (c->canvas_geom.y - c->canvas_from_y) * e;
}

// centres a windows jump letter on it, the label is a child of c->scene so
// the position is relative to the frame
static void canvas_jump_label_place(Client *c) {
	if (!c->mon || !c->mon->is_jump_mode || !c->jump_char ||
		!c->jump_label_node)
		return;
	struct wlr_scene_node *label = &c->jump_label_node->scene->node;
	int32_t lw = c->jump_label_node->logical_width;
	int32_t lh = c->jump_label_node->logical_height;
	if (lw <= 0 || lh <= 0 || lw > c->geom.width || lh > c->geom.height) {
		wlr_scene_node_set_enabled(label, false);
		return;
	}
	wlr_scene_node_set_position(label, (c->geom.width - lw) / 2,
								(c->geom.height - lh) / 2);
	wlr_scene_node_set_enabled(label, true);
	wlr_scene_node_raise_to_top(label);
}

static bool canvas_follow_fly(Client *c);

void canvas(Monitor *m) {
	Client *c = NULL;
	struct wlr_box geom;

	if (!m)
		return;

	canvas_tag_camera_sync(m);

	// overview positions are made for the whole set at once
	// relaxation is pairwise so no doing it one client at a time
	if (m->canvas_overview || m->ov_blend > 0.0)
		canvas_overview_layout(m);

	Client *seeded = NULL; // newest window placed this pass
	wl_list_for_each(c, &server.clients, link) {
		if (!VISIBLEON(c, m) || !ISFAKETILED(c))
			continue;

		// fullscreen or maximized isnt on the canvas while it is one
		// skip the write-back below or the monitor size gets saved as its
		// canvas size nothing restores it and 748x564 came back 1284x724
		if (c->isfullscreen || c->ismaximizescreen)
			continue;

		if (!c->canvas_placed) {
			canvas_seed_client(c, m);
			seeded = c;
		}

		double z = m->zoom > 0.0 ? m->zoom : 1.0;

		// chrome scales with the camera border shadow position
		// in overview position is log compressed toward the centroid
		// canvas_geom is never touched so leaving restores the real layout
		double px, py;
		canvas_layout_pos(m, c, &px, &py);
		if (m->ov_blend > 0.0) {
			// blend dont snap
			// switching straight from compressed to true sent every window off
			// screen while the camera took 300ms to catch up
			px += (c->ov_x - px) * m->ov_blend;
			py += (c->ov_y - py) * m->ov_blend;
		}
		geom.x = m->m.x + (int32_t)lround((px - m->cam_x) * z);
		geom.y = m->m.y + (int32_t)lround((py - m->cam_y) * z);

		// size the frame from the content not the other way
		// surface is scaled to window geometry * zoom
		// making the frame separately from canvas_geom drifts when a client
		// picks its own size like kitty rounding to cells then content sat a
		// few pixels inside the border on the right and bottom use the same
		// number as the content and the border hugs it at any zoom
		struct wlr_box wingeo = {0};
		client_get_geometry(c, &wingeo);
		if (wingeo.width > 0 && wingeo.height > 0) {
			int32_t cbw = (int32_t)canvas_frame_bw(c);
			// while a restore is pending the client size means nothing its just
			// what fullscreen left so build the frame from the stored canvas
			// size and let resize_apply() hand it back learning starts again
			// when the client catches up
			if (c->canvas_resize_pending) {
				double want_w = c->canvas_geom.width - 2.0 * config.borderpx;
				double want_h = c->canvas_geom.height - 2.0 * config.borderpx;
				if (fabs((double)wingeo.width - want_w) < 2.0 &&
					fabs((double)wingeo.height - want_h) < 2.0)
					c->canvas_resize_pending = false;
				geom.width = (int32_t)lround(want_w * z) + 2 * cbw;
				geom.height = (int32_t)lround(want_h * z) + 2 * cbw;
			} else {
				geom.width =
					(int32_t)lround((double)wingeo.width * z) + 2 * cbw;
				geom.height =
					(int32_t)lround((double)wingeo.height * z) + 2 * cbw;
				// keep canvas_geom in step so drags and seeding agree
				c->canvas_geom.width =
					(double)wingeo.width + 2.0 * config.borderpx;
				c->canvas_geom.height =
					(double)wingeo.height + 2.0 * config.borderpx;
			}
		} else {
			geom.width = (int32_t)lround(c->canvas_geom.width * z);
			geom.height = (int32_t)lround(c->canvas_geom.height * z);
		}

		client_tile_resize(c, geom, 0);
		canvas_jump_label_place(c);
	}
	// the overview closed by click or canvas_overview while letters were up
	// mangos grid overview arranges with its own layout so this never sees it
	if (m->is_jump_mode && !m->canvas_overview)
		finish_jump_mode(m);
	// canvas_follow_new_windows pans to a window that just landed even with
	// canvas_follow_focus off, canvas_follow_focus already covers it when on
	if (seeded && config.canvas_follow_new_windows && !m->canvas_overview)
		canvas_follow_fly(seeded);
	canvas_precompute_note(m);
}

// interactive pan

// pan anchor in cursor coordinates
// has to be double
// an int differenced against the double cursor leaves a sub pixel delta that
// never goes to zero
static double pan_anchor_x, pan_anchor_y;
static int64_t pan_anchor_ms;

// camera physics constants
// camera is not run through mango per window easing
// that eases each window alone so a pan looks like windows sliding on the plane
// and not the plane moving camera has its own velocity and windows are placed
// rigidly see canvas_camera_is_rigid
#define CAM_FRICTION 6.0	 // exponential decay per second
#define CAM_MIN_VELOCITY 8.0 // px/s below this the camera is at rest
#define CAM_KEY_ACCEL                                                          \
	6000.0 // px/s^2 while a pan key is held and top speed is this over
		   // CAM_FRICTION so 1000 px/s
#define CANVAS_PAN_REF_MAG 5.0 // the canvas_pan magnitude that means normal
#define CAM_MAX_DT 0.1 // clamp so a stalled frame cant teleport the camera
#define CAM_FLING_STALE_MS                                                     \
	50 // a release this long after the last drag motion doesnt fling

// pan keys push with constant force while held not a one shot
// holding an arrow should speed up smooth to a cruise not stutter on the key
// repeat held keys are tracked by keycode canvas_pan_action() records it from
// server.binding_press_keycode and canvas_pan_key_released() clears it tracking
// a set is what makes diagonals work
#define CANVAS_MAX_HELD 4
static struct {
	uint32_t keycode;
	double nx, ny;
	double speed; // multiple of base pan speed and 1.0 is normal
	bool active;
} pan_held[CANVAS_MAX_HELD];

// true when this starts a new hold and false when it just refreshes one already
// down caller must not restart the camera clock on a key repeat
static bool pan_held_add(uint32_t keycode, double nx, double ny, double speed) {
	int free_slot = -1;
	for (int i = 0; i < CANVAS_MAX_HELD; i++) {
		if (pan_held[i].active && pan_held[i].keycode == keycode) {
			pan_held[i].nx =
				nx; // key repeat refreshes it so adding twice is fine
			pan_held[i].ny = ny;
			pan_held[i].speed = speed;
			return false;
		}
		if (!pan_held[i].active && free_slot < 0)
			free_slot = i;
	}
	if (free_slot < 0)
		return false;
	pan_held[free_slot] = (typeof(pan_held[0])){
		.keycode = keycode, .nx = nx, .ny = ny, .speed = speed, .active = true};
	return true;
}

void canvas_pan_key_released(uint32_t keycode) {
	for (int i = 0; i < CANVAS_MAX_HELD; i++)
		if (pan_held[i].active && pan_held[i].keycode == keycode)
			pan_held[i].active = false;
}

// adds up the held directions into one force and gives the speed to use along
// it direction is normalised so diagonals arent faster speed is the biggest of
// the held keys own magnitudes fast key plus normal key gives a fast diagonal
// and not some average nobody wanted
static bool pan_held_force(double *fx, double *fy, double *speed) {
	*fx = 0.0;
	*fy = 0.0;
	*speed = 0.0;
	bool any = false;
	for (int i = 0; i < CANVAS_MAX_HELD; i++) {
		if (!pan_held[i].active)
			continue;
		*fx += pan_held[i].nx;
		*fy += pan_held[i].ny;
		if (pan_held[i].speed > *speed)
			*speed = pan_held[i].speed;
		any = true;
	}
	if (!any)
		return false;
	double len = sqrt((*fx) * (*fx) + (*fy) * (*fy));
	if (len > 0.0) { // normalise so diagonals arent faster
		*fx /= len;
		*fy /= len;
	}
	if (*speed <= 0.0)
		*speed = 1.0;
	return true;
}

// re-entry guard
// arrange() ends by calling pointer_process_motion() which comes straight back
// here canvas_pan() to arrange() to motion to canvas_pan() until the stack dies
// the guard makes that impossible instead of hoping the delta hits zero
static bool pan_in_progress;

// camera is moving
// plane grabbed or pan key held or coasting
// window positions on the plane cant change from that alone
bool canvas_camera_moving(Monitor *m) {
	double hfx, hfy, hsp;
	double rest = CAM_MIN_VELOCITY / (m->zoom > 0.0 ? m->zoom : 1.0);
	return server.cursor_mode == CurCanvasPan ||
		   pan_held_force(&hfx, &hfy, &hsp) || fabs(m->cam_vx) >= rest ||
		   fabs(m->cam_vy) >= rest;
}

bool canvas_camera_is_rigid(void) {
	return camera_rigid || server.cursor_mode == CurCanvasPan;
}

// monitor whose plane is grabbed
// fixed for the whole drag
// with sloppy focus selected_monitor follows the pointer to another output mid
// drag and gave it the rest of the drag and the release the grabbed one coasted
// on its last velocity looked up in the list each time so an unplugged output
// reads NULL
static Monitor *pan_mon;

static Monitor *pan_monitor(void) {
	Monitor *m = NULL;
	wl_list_for_each(m, &server.monitors, link) if (m == pan_mon) return m;
	return NULL;
}

void canvas_pan_begin(double cx, double cy) {
	pan_anchor_x = cx;
	pan_anchor_y = cy;
	pan_anchor_ms = get_now_in_ms();

	// grabbing the plane kills any fling still going
	Monitor *m = pan_mon = server.selected_monitor;
	if (m) {
		m->cam_vx = 0.0;
		m->cam_vy = 0.0;
	}
}

// releasing the plane
// velocity is the last motion sample so after holding still its a movement from
// long ago drag stop hold release and the camera flung anyway only a release
// while the pointer still moves flings
void canvas_pan_end(void) {
	Monitor *m = pan_monitor();
	pan_mon = NULL;
	if (m && get_now_in_ms() - pan_anchor_ms > CAM_FLING_STALE_MS) {
		m->cam_vx = 0.0;
		m->cam_vy = 0.0;
	}
}

void canvas_pan_motion(double cx, double cy) {
	if (pan_in_progress)
		return;

	Monitor *m = pan_monitor();
	if (!m || !canvas_active(m))
		return;

	double dx = cx - pan_anchor_x;
	double dy = cy - pan_anchor_y;
	pan_anchor_x = cx;
	pan_anchor_y = cy;

	if (dx == 0.0 && dy == 0.0)
		return;

	// camera is in canvas units and the cursor moves in screen pixels
	// one pixel is 1/zoom canvas units so divide by zoom to track the pointer
	// 1:1 without it a drag at 200 percent moved the canvas twice as far as the
	// cursor
	double z = m->zoom > 0.0 ? m->zoom : 1.0;
	dx /= z;
	dy /= z;

	// track 1:1 with the cursor while dragging so it feels direct not eased
	// velocity is only recorded so release can fling
	int64_t now = get_now_in_ms();
	double dt = (double)(now - pan_anchor_ms) / 1000.0;
	pan_anchor_ms = now;
	// no upper bound on dt
	// a slow move after a pause is a slow velocity and skipping it kept the old
	// fast one
	if (dt > 0.0) {
		m->cam_vx = -dx / dt;
		m->cam_vy = -dy / dt;
	}

	pan_in_progress = true;
	camera_rigid = true;
	canvas_pan(m, -dx, -dy);
	camera_rigid = false;
	pan_in_progress = false;
}

// steps the camera one frame
// true while moving so frames keep coming and false when it settles or it burns
// frames forever overview transitions stay rigid on purpose windows eased on
// their own lag the background shader and the grid slides out from under them
// ov_blend is already eased on the zoom clock so only real layout changes like
// canvas_cascade() want layout_anim_until
bool canvas_camera_tick(Monitor *m) {
	if (!m || !canvas_active(m))
		return false;

	bool busy = false;
	int64_t now = get_now_in_ms();
	// a cascade gliding in canvas space needs a rigid arrange every frame until
	// it lands the last one puts every window on target
	if (m->canvas_layout_animating) {
		busy = true;
		if (now - m->canvas_layout_start >= m->canvas_layout_dur) {
			m->canvas_layout_animating = false;
			Client *c = NULL;
			wl_list_for_each(c, &server.clients, link) if (c->mon == m)
				c->canvas_layout_moving = false;
		}
	}
	double dt = (double)(now - m->cam_last_tick_ms) / 1000.0;
	// no time passed means nothing to integrate
	// this used to fake a 1/60 s here and a same millisecond frame lurched the
	// camera forward v/60 fast overlapping arrow presses hit it easily
	if (dt <= 0.0)
		dt = 0.0;
	else if (dt > CAM_MAX_DT)
		dt = 1.0 / 60.0;

	// translation
	// runs always even during a zoom because the two are separate and have to
	// compose
	bool dragging = (server.cursor_mode == CurCanvasPan);
	double fx, fy, fspeed;
	bool forced = pan_held_force(&fx, &fy, &fspeed);
	double z = m->zoom > 0.0 ? m->zoom : 1.0;
	double rest = CAM_MIN_VELOCITY / z;

	if (dragging) {
		// motion events drive the camera directly so nothing to integrate
		// keep frames coming so the drag stays smooth
		busy = true;
	} else if (forced || fabs(m->cam_vx) >= rest || fabs(m->cam_vy) >= rest) {
		if (forced) {
			// the constant is a screen feel so turn it into canvas units at the
			// current zoom speed scales the force so it scales terminal
			// velocity CAM_KEY_ACCEL * speed / CAM_FRICTION a magnitude of 10
			// cruises twice as fast as 5
			m->cam_vx += fx * (CAM_KEY_ACCEL * fspeed / z) * dt;
			m->cam_vy += fy * (CAM_KEY_ACCEL * fspeed / z) * dt;
		}
		canvas_shift(m, m->cam_vx * dt, m->cam_vy * dt);
		double decay = exp(-CAM_FRICTION * dt);
		m->cam_vx *= decay;
		m->cam_vy *= decay;
		busy = true;
	} else {
		m->cam_vx = 0.0;
		m->cam_vy = 0.0;
	}

	// zoom
	// eased through mango curves unlike translation which is physical
	// recomputing the camera from the anchor keeps the focal point fixed
	// panning shifted the anchor too so a pan mid zoom survives
	if (m->zoom_animating) {
		double t = m->zoom_anim_duration > 0
					   ? (double)(now - m->zoom_anim_start) /
							 (double)m->zoom_anim_duration
					   : 1.0;
		if (t >= 1.0) {
			t = 1.0;
			m->zoom_animating = false;
		}
		double f = find_animation_curve_at(t, MOVE);
		m->zoom = m->zoom_from + (m->zoom_target - m->zoom_from) * f;
		m->ov_blend =
			m->ov_blend_from + (m->ov_blend_to - m->ov_blend_from) * f;
		// interpolating the anchor too lets this do a pure camera move
		// with zoom_from == zoom_target the view glides to a new centre and
		// doesnt snap
		double ax =
			m->anchor_from_cx + (m->zoom_anchor_cx - m->anchor_from_cx) * f;
		double ay =
			m->anchor_from_cy + (m->zoom_anchor_cy - m->anchor_from_cy) * f;
		canvas_cam_for_centre(m, ax, ay, m->zoom);
		busy = true;
	}

	m->cam_last_tick_ms = now;
	if (!busy)
		return false;

	// rigid is right for a plain camera move
	// wrong while a layout change eases in because no per window animation
	// makes cascade and overview teleport
	bool rigid = now >= m->layout_anim_until;

	pan_in_progress = true;
	camera_rigid = rigid;
	arrange(m, !rigid, false);
	camera_rigid = false;
	pan_in_progress = false;
	return true;
}

// eases the camera to a canvas point at the current zoom through mango
// animation curves used by fly-to centre-on-window and overview exit
void canvas_fly_to(Monitor *m, double cx, double cy, bool animate_layout) {
	if (!m)
		return;
	int64_t now = get_now_in_ms();
	int32_t dur = config.animation_duration_move > 0
					  ? config.animation_duration_move
					  : 300;
	// compose with an animation already running dont cancel it
	// fly-to restarts the shared animation clock so always writing zoom_target
	// = m->zoom threw away the target mid flight thats how leaving overview
	// lost its zoom restore exit animation still ran when arrange() to
	// pointer_process_motion() to client_focus() to canvas_follow_focus() made
	// a fly-to that pinned zoom at the overview scale same for the overview
	// blend so keep its destination and restart its clock from where it is
	bool inflight = m->zoom_animating;
	canvas_view_centre(m, &m->anchor_from_cx, &m->anchor_from_cy);
	m->zoom_anchor_cx = cx;
	m->zoom_anchor_cy = cy;
	m->zoom_from = m->zoom;
	if (!inflight)
		m->zoom_target = m->zoom;
	m->ov_blend_from = m->ov_blend;
	if (!inflight)
		m->ov_blend_to = m->ov_blend;
	m->zoom_anim_start = now;
	m->zoom_anim_duration = dur;
	m->zoom_animating = true;
	m->cam_last_tick_ms = now;
	if (animate_layout)
		m->layout_anim_until = now + dur;
	wlr_output_schedule_frame(m->wlr_output);
}

// dispatch actions

// canvas_pan,dx,dy pans while the key is held
// direction is the ratio and magnitude is the speed 5 is normal
int32_t canvas_pan_action(const Arg *arg) {
	Monitor *m = server.selected_monitor;
	if (!m || !canvas_active(m))
		return 0;

	// registers a held direction
	// force is applied per frame in canvas_camera_tick() while the key is down
	double nx = (double)arg->i, ny = (double)arg->i2;
	double len = sqrt(nx * nx + ny * ny);
	if (len == 0.0)
		return 0;
	// the magnitude is the speed so canvas_pan,-9,0 is faster than
	// canvas_pan,-5,0 CANVAS_PAN_REF_MAG 5 is normal because old bindings
	// carried 5 so canvas_pan,-1,0 is a fifth of normal now watch that when
	// copying old binds direction is normalised below so a diagonal never beats
	// an axis
	double speed = len / CANVAS_PAN_REF_MAG;
	// only a new hold from rest restarts the camera clock
	// restarting on every key repeat or overlapping press dropped part of a
	// frame and the speed wobbled once auto repeat kicked in a fresh hold still
	// resets it so a pan after a long idle skips a huge stale dt
	double hfx, hfy, hsp;
	double rest = CAM_MIN_VELOCITY / (m->zoom > 0.0 ? m->zoom : 1.0);
	bool moving = pan_held_force(&hfx, &hfy, &hsp) || m->zoom_animating ||
				  fabs(m->cam_vx) >= rest || fabs(m->cam_vy) >= rest;
	// only a key bind press can be held because release ends it
	// from dfmsg or a mouse or axis or gesture bind there is no key and a hold
	// under a stale keycode never ended so those get one push to this pans
	// cruise speed never above what it had and friction stops it
	uint32_t key = server.binding_press_keycode;
	if (!key) {
		double v = CAM_KEY_ACCEL * speed / CAM_FRICTION /
				   (m->zoom > 0.0 ? m->zoom : 1.0);
		double cap = fmax(v, hypot(m->cam_vx, m->cam_vy));
		m->cam_vx += nx / len * v;
		m->cam_vy += ny / len * v;
		double now = hypot(m->cam_vx, m->cam_vy);
		if (now > cap) {
			m->cam_vx *= cap / now;
			m->cam_vy *= cap / now;
		}
	} else if (!pan_held_add(key, nx / len, ny / len, speed)) {
		moving = true; // a key repeat so the clock is live
	}
	if (!moving)
		m->cam_last_tick_ms = get_now_in_ms();
	wlr_output_schedule_frame(m->wlr_output);
	return 0;
}

// canvas_center_window puts the focused window in the view middle
// camera moves and the window doesnt
int32_t canvas_center_window(const Arg *arg) {
	(void)arg;
	Monitor *m = server.selected_monitor;
	if (!m || !canvas_active(m))
		return 0;
	Client *c = m->sel;
	if (!canvas_on_plane(c))
		return 0;
	canvas_fly_to(m, c->canvas_geom.x + c->canvas_geom.width / 2.0,
				  c->canvas_geom.y + c->canvas_geom.height / 2.0, false);
	return 0;
}

// canvas_focus_follows_camera pans to whatever the pointer focuses so the view
// follows you without a drag called from the sloppy focus path and does nothing
// unless the option is on the flight itself split out so the debounce timer can
// reach it everything is worked out when it runs not when focus changed the
// camera may have moved or the window may already be centred by then
static bool canvas_follow_fly(Client *c) {
	if (!c || !c->mon)
		return false;
	Monitor *m = c->mon;

	double z = m->zoom > 0.0 ? m->zoom : 1.0;
	double vw = (double)m->w.width / z, vh = (double)m->w.height / z;
	double wcx = c->canvas_geom.x + c->canvas_geom.width / 2.0;
	double wcy = c->canvas_geom.y + c->canvas_geom.height / 2.0;
	double vcx, vcy;
	canvas_view_centre(m, &vcx, &vcy);

	// small dead zone only so the camera doesnt jitter when the window is
	// already centred
	if (fabs(wcx - vcx) < vw * 0.05 && fabs(wcy - vcy) < vh * 0.05)
		return false;

	// already on the way there
	// restarting the clock only stretches the flight out one frame at a time
	if (m->zoom_animating && fabs(m->zoom_anchor_cx - wcx) < 1.0 &&
		fabs(m->zoom_anchor_cy - wcy) < 1.0)
		return false;

	canvas_fly_to(m, wcx, wcy, false);
	return true;
}

// debounce on the leading edge
// first focus change after a quiet time flies right away and only ones within
// the delay get held sweeping a row flies once then lands where the pointer
// stops only a real flight opens a quiet period so passing a centred window
// doesnt eat it client is kept by id since a window can close during the wait
// and zero delay arms nothing
static struct wl_event_source *follow_timer;
static uint32_t follow_pending_id;
static int64_t follow_last_fly_ms;

static int32_t canvas_follow_timer_fire(void *data) {
	(void)data;
	uint32_t want = follow_pending_id;
	follow_pending_id = 0;
	if (!want)
		return 0;

	Monitor *m = server.selected_monitor;
	if (!m || !canvas_active(m) || m->canvas_overview ||
		server.cursor_mode == CurCanvasPan || server.grab_client)
		return 0;

	Client *c = NULL, *found = NULL;
	wl_list_for_each(c, &server.clients, link) {
		if (c->id == want) {
			found = c;
			break;
		}
	}
	// focus has to still be where it was when the timer was armed
	if (found && found == m->sel && canvas_on_plane(found) &&
		VISIBLEON(found, m) && canvas_follow_fly(found))
		follow_last_fly_ms = get_now_in_ms();
	return 0;
}

void canvas_follow_focus(Client *c) {
	if (!c || !c->mon || !config.canvas_follow_focus || !canvas_active(c->mon))
		return;
	Monitor *m = c->mon;
	if (m->canvas_overview || server.cursor_mode == CurCanvasPan ||
		server.grab_client)
		return; // never fight a drag or hijack the overview
	// only windows the canvas positions
	// floating isnt on the plane and canvas() skips it so its canvas_geom is
	// stale or nothing flying at one sent the view to a bad coordinate
	if (!canvas_on_plane(c))
		return;

	// re-entry guard
	// arrange() ends in pointer_process_motion() to client_focus() to
	// canvas_follow_focus the camera tick calls arrange() every frame so each
	// frame re-armed the flight it was running camera never moved and it
	// rendered nonstop
	if (pan_in_progress)
		return;

	// small dead zone only so the camera doesnt jitter when the window is
	// already centred it was a quarter of the viewport and never fired to focus
	// a window with the pointer you can see it so its centre is nearly always
	// inside a quarter viewport of the middle the guard against twitching
	// blocked the feature
	if (config.canvas_follow_focus_delay > 0) {
		int64_t now = get_now_in_ms();
		if (now - follow_last_fly_ms >= config.canvas_follow_focus_delay) {
			// quiet long enough so this is the leading edge so go now
			// drop any trailing fire still armed or it yanks the camera again
			// after this lands
			follow_pending_id = 0;
			if (follow_timer)
				wl_event_source_timer_update(follow_timer, 0);
			if (canvas_follow_fly(c))
				follow_last_fly_ms = now;
			return;
		}

		// inside the quiet period so hold it for the trailing edge
		follow_pending_id = c->id;
		if (!follow_timer)
			follow_timer = wl_event_loop_add_timer(
				server.event_loop, canvas_follow_timer_fire, NULL);
		if (follow_timer) {
			wl_event_source_timer_update(follow_timer,
										 config.canvas_follow_focus_delay);
			return;
		}
		// no timer to be had so act like theres no delay
	}

	canvas_follow_fly(c);
}

// canvas_focus_dir,dx,dy focuses the nearest window in a direction
// nearest means in that direction or a far left window a pixel up wins a press
// of Up drop whats behind or outside a 45 degree cone then score along + 2 *
// across starts from the focused window or the view centre if none
int32_t canvas_focus_dir(const Arg *arg) {
	Monitor *m = server.selected_monitor;
	if (!m || !canvas_active(m))
		return 0;

	double dx = (double)arg->i, dy = (double)arg->i2;
	double len = sqrt(dx * dx + dy * dy);
	if (len == 0.0)
		return 0;
	dx /= len;
	dy /= len;

	double vx, vy, vw, vh;
	canvas_viewport(m, &vx, &vy, &vw, &vh);

	Client *cur = m->sel;
	// canvas_focus_dir_from=center steps from the window nearest the view
	// middle, so a camera panned away from focus navigates from what you see
	if (config.canvas_focus_dir_from) {
		// mid flight the view middle is where the camera is going, so quick
		// presses step on from the window just picked not one passed on the way
		double mx = vx + vw / 2.0, my = vy + vh / 2.0, bd = INFINITY;
		if (m->zoom_animating)
			mx = m->zoom_anchor_cx, my = m->zoom_anchor_cy;
		Client *c = NULL;
		cur = NULL;
		wl_list_for_each(c, &server.clients, link) {
			if (!VISIBLEON(c, m) || !ISFAKETILED(c) || !c->canvas_placed ||
				c->isminimized || !canvas_on_plane(c))
				continue;
			double ox = c->canvas_geom.x + c->canvas_geom.width / 2.0 - mx;
			double oy = c->canvas_geom.y + c->canvas_geom.height / 2.0 - my;
			if (ox * ox + oy * oy < bd)
				bd = ox * ox + oy * oy, cur = c;
		}
	}
	double rx, ry;
	if (canvas_on_plane(cur) && VISIBLEON(cur, m)) {
		rx = cur->canvas_geom.x + cur->canvas_geom.width / 2.0;
		ry = cur->canvas_geom.y + cur->canvas_geom.height / 2.0;
	} else {
		cur = NULL;
		rx = vx + vw / 2.0;
		ry = vy + vh / 2.0;
	}

	Client *best = NULL;
	double best_score = 0.0;
	Client *c = NULL;
	wl_list_for_each(c, &server.clients, link) {
		if (c == cur || !VISIBLEON(c, m) || !ISFAKETILED(c) ||
			!c->canvas_placed || c->isminimized)
			continue;
		double cx = c->canvas_geom.x + c->canvas_geom.width / 2.0;
		double cy = c->canvas_geom.y + c->canvas_geom.height / 2.0;
		double ox = cx - rx, oy = cy - ry;
		double along = ox * dx + oy * dy;
		if (along <= 0.0)
			continue; // behind or dead abeam
		double across = fabs(ox * -dy + oy * dx);
		if (across > along)
			continue; // outside the cone
		double score = along + 2.0 * across;
		if (!best || score < best_score) {
			best_score = score;
			best = c;
		}
	}
	if (!best)
		return 0;

	client_focus(best, 1);

	// bring it into view if its not already
	// follow focus may be off but a directional focus that leaves you on empty
	// canvas did half the job so only fires when the target is off screen and
	// windows already in view stay put
	// except with canvas_focus_dir_from=center which steps from the window in
	// the middle, so the target has to become that window or the next press
	// starts from the old one
	double l = vx, t = vy, r = l + vw, b = t + vh;
	if (config.canvas_focus_dir_from || best->canvas_geom.x < l || best->canvas_geom.y < t ||
		best->canvas_geom.x + best->canvas_geom.width > r ||
		best->canvas_geom.y + best->canvas_geom.height > b)
		canvas_fly_to(m, best->canvas_geom.x + best->canvas_geom.width / 2.0,
					  best->canvas_geom.y + best->canvas_geom.height / 2.0,
					  false);
	return 0;
}

// canvas_reset_camera sends the camera to the canvas origin
// goes through canvas_fly_to() not a write to cam_x/cam_y
// a zoom or fly in flight recomputes the camera from its anchor next tick and a
// fling keeps integrating so a direct write got undone at once and the view
// snapped back or coasted off target is the view centre that leaves the camera
// at 0,0 at the zoom being flown to
int32_t canvas_reset_camera(const Arg *arg) {
	(void)arg;
	Monitor *m = server.selected_monitor;
	if (!m || !canvas_active(m))
		return 0;
	m->cam_vx = 0.0;
	m->cam_vy = 0.0;
	double z = m->zoom_animating ? m->zoom_target : m->zoom;
	canvas_fly_to(m, view_off_x(m) / z, view_off_y(m) / z, false);
	return 0;
}

// canvas_drag begins a camera pan
// bind it as a mousebind like mousebind=SUPER,btn_left,canvas_drag
// pointer grabs the plane and motion is handled in pointer_process_motion
int32_t canvas_drag(const Arg *arg) {
	(void)arg;
	Monitor *m = server.selected_monitor;
	if (!m || !canvas_active(m))
		return 0;

	server.grab_client = NULL;
	canvas_pan_begin(server.cursor->x, server.cursor->y);
	server.cursor_mode = CurCanvasPan;
	wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, "grabbing");
	return 0;
}

// zoom

#define CANVAS_ZOOM_MIN 0.2
// 1.0 is native resolution and no magnification same as driftwm
// above 1.0 the compositor upsamples a 1x render so text is soft however you
// scale fixing that means clients re-render and re-layout every zoom step so no
// zooming out downsamples and seeing more is the point anyway
#define CANVAS_ZOOM_MAX 1.0

// scales one scene buffer to the camera zoom
// base size has to be stable across frames
// scaling buffer->dst_width compounds when the clip covers the whole surface
// and 7x4 became 1x1 and the window vanished so use the clip rect for the root
// surface and the surfaces own size for subsurfaces
struct canvas_zoom_ctx {
	double zoom;
	struct wlr_surface *root;
	int32_t root_w, root_h;
};

// the surface a subsurface scene subtree draws
// wlr_scene_subsurface_tree_create() gives every surface its own tree with its
// buffer plus a tree per child subsurface so the first scene_surface directly
// inside a tree is the surface that tree is for
static struct wlr_surface *canvas_subtree_surface(struct wlr_scene_tree *tree) {
	struct wlr_scene_node *n = NULL;
	wl_list_for_each(n, &tree->children, link) {
		if (n->type != WLR_SCENE_NODE_BUFFER)
			continue;
		struct wlr_scene_buffer *buf = wlr_scene_buffer_from_node(n);
		struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(buf);
		if (ss && ss->surface)
			return ss->surface;
	}
	return NULL;
}

// scale subsurface positions not just buffers
// wlroots offsets are unscaled so a subsurface 200px in stayed 200px and slid
// out of place terminals dont show it but browsers and CSD apps do done every
// frame from the output handler because a commit puts the old offset back
// (ox, oy) is the clip origin, the CSD margin. the xdg tree and the root
// buffer carry it unscaled and cancel, so a top level subsurface scales from
// there and not from the surface origin. scaling from 0 put librewolf's page
// 20 * (1 - z) up and left, and wlroots works out a subsurface clip from its
// tree position, so a root commit (focus change) and a page commit each left
// a different offset - the page jumped 6px at 73% until its next commit
// one that starts in the margin gets cut by wlroots, canvas_scale_buffer()
// scales that cut offset so it lands right either way
static void canvas_scale_subsurfaces(struct wlr_scene_node *node, double z,
									 int32_t ox, int32_t oy) {
	if (!node || node->type != WLR_SCENE_NODE_TREE)
		return;
	struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
	struct wlr_scene_node *child = NULL;
	wl_list_for_each(child, &tree->children, link) {
		if (child->type != WLR_SCENE_NODE_TREE)
			continue;
		struct wlr_scene_tree *ct = wlr_scene_tree_from_node(child);
		struct wlr_surface *surf = canvas_subtree_surface(ct);
		if (surf) {
			struct wlr_subsurface *sub =
				wlr_subsurface_try_from_wlr_surface(surf);
			if (sub) {
				wlr_scene_node_set_position(
					child,
					ox + (int32_t)lround((double)(sub->current.x - ox) * z),
					oy + (int32_t)lround((double)(sub->current.y - oy) * z));
				// nested ones are relative to their parent surface
				canvas_scale_subsurfaces(child, z, 0, 0);
				continue;
			}
		}
		canvas_scale_subsurfaces(child, z, ox, oy);
	}
}

// the part of a surface a scene buffer still shows after the wlroots clip, in
// surface px. wlroots crops a subsurface that runs past the window clip by
// shrinking src_box, so a dest size from the whole surface stretched what was
// left over the cut part (100px of a subsurface hanging off the right edge
// drew 300px wide, past the border)
static struct wlr_fbox
scene_buffer_visible_box(struct wlr_scene_buffer *buffer,
						 struct wlr_surface *surface) {
	struct wlr_fbox vis = {0, 0, surface->current.width,
						   surface->current.height};
	struct wlr_fbox full, cur = buffer->src_box;
	wlr_surface_get_buffer_source_box(surface, &full);
	if (wlr_fbox_empty(&cur) || wlr_fbox_empty(&full))
		return vis;
	// same buffer to surface orientation wlroots uses for the crop
	int32_t bw = surface->current.buffer_width;
	int32_t bh = surface->current.buffer_height;
	wlr_fbox_transform(&full, &full, surface->current.transform, bw, bh);
	wlr_fbox_transform(&cur, &cur, surface->current.transform, bw, bh);
	vis.x = (cur.x - full.x) / full.width * vis.width;
	vis.y = (cur.y - full.y) / full.height * vis.height;
	vis.width *= cur.width / full.width;
	vis.height *= cur.height / full.height;
	return vis;
}

static void canvas_scale_buffer(struct wlr_scene_buffer *buffer, int32_t sx,
								int32_t sy, void *data) {
	(void)sx;
	(void)sy;
	const struct canvas_zoom_ctx *ctx = data;

	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(buffer);
	if (!scene_surface || !scene_surface->surface)
		return;

	struct wlr_surface *surface = scene_surface->surface;
	double base_w, base_h;
	if (surface == ctx->root) {
		base_w = ctx->root_w;
		base_h = ctx->root_h;
	} else {
		// a subsurface the clip cut keeps only its visible part, and wlroots
		// leaves the node at the cut offset in surface px so that scales too.
		// the root's offset cancels the xdg tree's unscaled one and stays
		struct wlr_fbox vis = scene_buffer_visible_box(buffer, surface);
		base_w = vis.width;
		base_h = vis.height;
		wlr_scene_node_set_position(&buffer->node,
									(int32_t)lround(vis.x * ctx->zoom),
									(int32_t)lround(vis.y * ctx->zoom));
	}

	// ceil not lround
	// frame inner edge is lround(base * zoom) and if content rounds down it
	// falls a pixel short the blend between black content and a light border
	// reads as a dark seam rounding outward makes it meet or overlap by 1px at
	// most and the border draws on top so you cant see it
	int32_t w = (int32_t)ceil(base_w * ctx->zoom - 1e-6);
	int32_t h = (int32_t)ceil(base_h * ctx->zoom - 1e-6);
	if (w > 0 && h > 0)
		wlr_scene_buffer_set_dest_size(buffer, w, h);
}

// zoom a wayland clients surfaces
// the clip has to stay because its offset is the CSD margin and dropping it
// shifted content down and right the clip size has to be un-zoomed because
// c->geom is scaled and the surface isnt or it crops instead of shrinks
// set_clip() sets dest_size as a side effect so scale dest_size after it not
// before
void canvas_apply_zoom_scale(Client *c) {
	if (!c || !c->mon || !c->scene || !c->scene_surface)
		return;
	// xwayland content is scaled by client_update_xwayland_clip() instead
	// it owns the source box and dest_size for the X11 root buffer and redoes
	// them after every commit scaling dest_size here too makes them fight over
	// the same buffer every frame
	if (client_is_x11(c))
		return;
	// also run while a window animation is going at zoom 1.0
	// frame still eases to its new size so content has to scale with it even at
	// native scale both conditions are false at rest so an idle canvas stays
	// free
	if (!canvas_zoomed(c->mon) &&
		!(canvas_active(c->mon) && c->animation.running))
		return;

	double z = c->mon->zoom;
	if (z <= 0.0)
		return;

	struct wlr_box clip;
	client_get_clip(c, &clip);

	// client_get_clip() takes width and height from c->geom which is already
	// scaled but the surface isnt right unscaled size is the clients own
	// logical window geometry not rebuilt from canvas_geom which drifts when a
	// client picks its own size like kitty rounding to cells that left content
	// inset from the frame at the bottom and right when zoomed in
	struct wlr_box wingeo = {0};
	client_get_geometry(c, &wingeo);
	if (wingeo.width > 0 && wingeo.height > 0) {
		clip.width = wingeo.width;
		clip.height = wingeo.height;
	}

	// never let the clip run past the surface or the window goes empty
	struct wlr_surface *surf = client_surface(c);
	if (surf) {
		if (clip.x + clip.width > surf->current.width)
			clip.width = surf->current.width - clip.x;
		if (clip.y + clip.height > surf->current.height)
			clip.height = surf->current.height - clip.y;
	}
	if (clip.width <= 0 || clip.height <= 0)
		return;

	wlr_scene_subsurface_tree_set_clip(&c->scene_surface->node, &clip);

	// scale the content to the frame actually on screen not the camera zoom
	// the border draws from c->animation.current which eases and trails its
	// target scaling by m->zoom put content at full size at once and it spilled
	// past the border during overview exit and fly to focus width only so two
	// axes rounding differently cant squash the aspect ratio
	int32_t frame_bw = 2 * (int32_t)canvas_frame_bw(c);
	if (wingeo.width > 0 && c->animation.current.width > frame_bw)
		z = (double)(c->animation.current.width - frame_bw) /
			(double)wingeo.width;

	struct canvas_zoom_ctx ctx = {
		.zoom = z,
		.root = surf,
		.root_w = clip.width,
		.root_h = clip.height,
	};
	wlr_scene_node_for_each_buffer(&c->scene->node, canvas_scale_buffer, &ctx);
	canvas_scale_subsurfaces(&c->scene_surface->node, z, clip.x, clip.y);
}

// corner radius for a canvas client scaled by the camera
// it can scale safely because its one number given to the renderer from
// set_client_corner_location() unscaled a 12px radius at 25 percent zoom looked
// like 48px rounded and floored at zero and never bigger than the unzoomed
// radius
uint32_t canvas_corner_radius(Monitor *m) {
	if (!m || !canvas_active(m))
		return config.border_radius;
	double z = m->zoom > 0.0 ? m->zoom : 1.0;
	if (z >= 1.0)
		return config.border_radius;
	double r = (double)config.border_radius * z;
	return r > 0.0 ? (uint32_t)lround(r) : 0;
}

// border width for a canvas client is borderpx scaled by the camera
// so frame border and content shrink together and gaps stay the same on screen
// every write of c->bw goes through here and its plain borderpx off the canvas
// or at 100 percent worked out fresh every call so a stale scaled value cant
// stick or get clobbered floored at 1px so focus stays visible
uint32_t canvas_border_width(Monitor *m) {
	if (!m || !canvas_active(m) || m->zoom >= 1.0 || m->zoom <= 0.0 ||
		config.borderpx == 0)
		return config.borderpx;
	long b = lround((double)config.borderpx * m->zoom);
	return b < 1 ? 1 : (uint32_t)b;
}

// border width for one client. a floating window isnt on the plane, the
// camera doesnt scale it, so neither does its border (it went 4px to 2px at
// 50% while the window kept its size)
uint32_t canvas_client_border_width(Client *c) {
	if (c && c->isfloating)
		return config.borderpx;
	return canvas_border_width(c ? c->mon : NULL);
}

// border a canvas client is actually drawn with on screen
// 0 when it has none by the same tests resize_apply() uses to zero c->bw
// using canvas_border_width() anyway made a borderless window at 100 percent
// grow 2*borderpx every arrange resize_apply() configured it at the whole frame
// and the client committed that and the next arrange added the border again
// canvas_geom keeps the nominal border either way like the zoomed configure
// path that subtracts config.borderpx
uint32_t canvas_frame_bw(Client *c) {
	if (c->no_border || c->iskilling || check_hit_no_border(c))
		return 0;
	return canvas_client_border_width(c);
}

bool canvas_zoomed(Monitor *m) {
	return m && m->zoom != 1.0 && canvas_active(m);
}

// canvas_zoom,relative,percent
// canvas_zoom,0,100 is absolute 100 percent
// canvas_zoom,1,125 is relative 125 percent of now
// applies a relative zoom step clamped and eased and pct is a percentage
static void canvas_zoom_by(Monitor *m, double pct);

// canvas_zoom_in and canvas_zoom_out step by config.canvas_zoom_step
int32_t canvas_zoom_in(const Arg *arg) {
	(void)arg;
	Monitor *m = server.selected_monitor;
	if (!m || !canvas_active(m))
		return 0;
	double step =
		config.canvas_zoom_step > 0.0 ? config.canvas_zoom_step : 112.0;
	canvas_zoom_by(m, step);
	return 0;
}

int32_t canvas_zoom_out(const Arg *arg) {
	(void)arg;
	Monitor *m = server.selected_monitor;
	if (!m || !canvas_active(m))
		return 0;
	double step =
		config.canvas_zoom_step > 0.0 ? config.canvas_zoom_step : 112.0;
	// inverse of the in step so in then out ends up exactly where it started
	canvas_zoom_by(m, 100.0 * 100.0 / step);
	return 0;
}

int32_t canvas_zoom_action(const Arg *arg) {
	Monitor *m = server.selected_monitor;
	if (!m || !canvas_active(m))
		return 0;
	double pct = (double)arg->i2;
	if (pct <= 0.0)
		return 0;
	if (arg->i) {
		canvas_zoom_by(m, pct);
	} else {
		// absolute so turn it into the relative step that gets there
		double base = m->zoom_animating ? m->zoom_target : m->zoom;
		if (base > 0.0)
			canvas_zoom_by(m, pct / base);
	}
	return 0;
}

static void canvas_zoom_by(Monitor *m, double pct) {
	if (!m || pct <= 0.0)
		return;

	bool inflight = m->zoom_animating;
	double base = inflight ? m->zoom_target : m->zoom;
	double target = base * (pct / 100.0);

	if (target < CANVAS_ZOOM_MIN)
		target = CANVAS_ZOOM_MIN;
	if (target > CANVAS_ZOOM_MAX)
		target = CANVAS_ZOOM_MAX;
	if (target == m->zoom && !m->zoom_animating)
		return;

	if (!m->zoom_animating) {
		// anchor only when starting fresh
		// re-deriving mid animation drifts the centre every notch
		canvas_view_centre(m, &m->zoom_anchor_cx, &m->zoom_anchor_cy);
	}
	// where the camera is starting from which isnt where its heading
	// mid animation zoom_anchor is where the running transition is going not
	// where the camera is copying it into anchor_from made the camera jump to
	// the overview centre on a scroll notch so start from where the camera
	// really is and keep heading where it was
	if (inflight) {
		canvas_view_centre(m, &m->anchor_from_cx, &m->anchor_from_cy);
	} else {
		m->anchor_from_cx = m->zoom_anchor_cx;
		m->anchor_from_cy = m->zoom_anchor_cy;
	}
	// hold the overview blend across a zoom step
	// ov_blend uses this same animation clock
	// leaving ov_blend_from/to at whatever the last overview transition wrote
	// made every notch replay that transition windows snapped to true positions
	// at t=0 and eased back to compressed a twitch every step for as long as a
	// stale ov_blend_to sat there
	m->ov_blend_from = m->ov_blend;
	if (!inflight)
		m->ov_blend_to = m->ov_blend;
	m->zoom_from = m->zoom;
	m->zoom_target = target;
	m->zoom_anim_start = get_now_in_ms();
	m->zoom_anim_duration = config.animation_duration_move > 0
								? config.animation_duration_move
								: 300;
	m->zoom_animating = true;
	m->cam_last_tick_ms = get_now_in_ms();
	wlr_output_schedule_frame(m->wlr_output);
}

// overview

// log distance compression
// zoom to fit is useless on an infinite plane because one far window makes the
// rest specks so remap each windows distance from the centroid r' = k * ln(1 +
// r/k) near identity for small r and a window ten times further lands only
// about 2.4x further out direction is untouched and canvas_geom is never
// written so leaving restores the real layout canvas_overview_compression is k
// default 1200 canvas units

static void canvas_overview_centroid(Monitor *m, double *ox, double *oy) {
	double sx = 0.0, sy = 0.0;
	int n = 0;
	Client *c = NULL;
	wl_list_for_each(c, &server.clients, link) {
		if (!VISIBLEON(c, m) || !ISFAKETILED(c))
			continue;
		sx += c->canvas_geom.x + c->canvas_geom.width / 2.0;
		sy += c->canvas_geom.y + c->canvas_geom.height / 2.0;
		n++;
	}
	*ox = n ? sx / n : m->cam_x;
	*oy = n ? sy / n : m->cam_y;
}

// works out every windows overview position for this frame compress then push
// apart compression alone stacks windows because pulling everything to a
// centroid is what makes far things collide so after it a few relax passes push
// overlapping pairs apart along the line between their centres results are
// cached on the client for the frame canvas_geom is never touched so leaving
// restores the real layout
static void canvas_overview_layout_windows(Monitor *m) {
	Client *list[64];
	int n = 0;
	Client *c = NULL;
	wl_list_for_each(c, &server.clients, link) {
		if (!VISIBLEON(c, m) || !ISFAKETILED(c) || n >= 64)
			continue;
		canvas_overview_point(m, c->canvas_geom.x, c->canvas_geom.y, &c->ov_x,
							  &c->ov_y);
		list[n++] = c;
	}
	if (n < 2)
		return;

	const double gap = config.canvas_overview_spacing;
	for (int pass = 0; pass < 12; pass++) {
		bool moved = false;
		for (int i = 0; i < n; i++) {
			for (int j = i + 1; j < n; j++) {
				Client *a = list[i], *b = list[j];
				double aw = a->canvas_geom.width, ah = a->canvas_geom.height;
				double bw = b->canvas_geom.width, bh = b->canvas_geom.height;
				double acx = a->ov_x + aw / 2.0, acy = a->ov_y + ah / 2.0;
				double bcx = b->ov_x + bw / 2.0, bcy = b->ov_y + bh / 2.0;
				double dx = bcx - acx, dy = bcy - acy;
				double ox = (aw + bw) / 2.0 + gap - fabs(dx);
				double oy = (ah + bh) / 2.0 + gap - fabs(dy);
				if (ox <= 0.0 || oy <= 0.0)
					continue; // not overlapping
				// separate along the shallower axis
				// smaller correction disturbs the remembered arrangement least
				if (ox < oy) {
					double push = (dx >= 0 ? ox : -ox) / 2.0;
					a->ov_x -= push;
					b->ov_x += push;
				} else {
					double push = (dy >= 0 ? oy : -oy) / 2.0;
					a->ov_y -= push;
					b->ov_y += push;
				}
				moved = true;
			}
		}
		if (!moved)
			break;
	}
}

// canvas_overview_mode=clusters
// compress the distances between clusters with the log but keep each clusters
// own layout linear each window moves by its cluster offset so an island looks
// like it does on the canvas just nearer the others clusters that end up
// overlapping get pushed apart as rigid blocks canvas_overview_spacing apart
// along the shallower axis
static void canvas_overview_layout_clusters(Monitor *m) {
	Client *ids[64];
	pack_rect r[64];
	int n = canvas_cluster(m, NULL, ids, r);
	if (n == 0)
		return;
	// true clusters keep their layout
	// loose windows groups below canvas_cluster_min_size are each spaced out
	// alone
	int lab[64];
	int nl = canvas_cluster_labels(m, r, n, lab);
	for (int i = 0; i < n; i++)
		if (lab[i] < 0)
			lab[i] = nl++;
	double bx0[64], by0[64], bx1[64], by1[64], offx[64], offy[64];
	for (int l = 0; l < nl; l++)
		bx0[l] = by0[l] = 1e18, bx1[l] = by1[l] = -1e18;
	for (int i = 0; i < n; i++) {
		int l = lab[i];
		bx0[l] = fmin(bx0[l], r[i].x), by0[l] = fmin(by0[l], r[i].y);
		bx1[l] = fmax(bx1[l], r[i].x + r[i].w);
		by1[l] = fmax(by1[l], r[i].y + r[i].h);
	}
	for (int l = 0; l < nl; l++) {
		double cx = (bx0[l] + bx1[l]) / 2, cy = (by0[l] + by1[l]) / 2, px, py;
		canvas_overview_point(m, cx, cy, &px, &py);
		offx[l] = px - cx, offy[l] = py - cy;
	}
	const double gap = config.canvas_overview_spacing;
	for (int pass = 0; pass < 12; pass++) {
		bool moved = false;
		for (int a = 0; a < nl; a++)
			for (int b = a + 1; b < nl; b++) {
				double aw = bx1[a] - bx0[a], ah = by1[a] - by0[a];
				double bw = bx1[b] - bx0[b], bh = by1[b] - by0[b];
				double acx = bx0[a] + offx[a] + aw / 2,
					   acy = by0[a] + offy[a] + ah / 2;
				double bcx = bx0[b] + offx[b] + bw / 2,
					   bcy = by0[b] + offy[b] + bh / 2;
				double dx = bcx - acx, dy = bcy - acy;
				double ox = (aw + bw) / 2 + gap - fabs(dx);
				double oy = (ah + bh) / 2 + gap - fabs(dy);
				if (ox <= 0.0 || oy <= 0.0)
					continue;
				if (ox < oy) {
					double push = (dx >= 0 ? ox : -ox) / 2;
					offx[a] -= push, offx[b] += push;
				} else {
					double push = (dy >= 0 ? oy : -oy) / 2;
					offy[a] -= push, offy[b] += push;
				}
				moved = true;
			}
		if (!moved)
			break;
	}
	for (int i = 0; i < n; i++) {
		ids[i]->ov_x = r[i].x + offx[lab[i]];
		ids[i]->ov_y = r[i].y + offy[lab[i]];
	}
}

void canvas_overview_layout(Monitor *m) {
	if (config.canvas_overview_mode == CANVAS_OVERVIEW_WINDOWS)
		canvas_overview_layout_windows(m);
	else
		canvas_overview_layout_clusters(m);
}

// leaves overview and goes to one window the click target
// frames it at the zoom the overview was entered from so picking a window puts
// you back at the scale you were working at
void canvas_overview_pick(Client *c) {
	if (!c || !c->mon || !c->mon->canvas_overview)
		return;
	Monitor *m = c->mon;
	m->canvas_overview = false;
	m->ov_blend_from = m->ov_blend;
	m->ov_blend_to = 0.0;
	canvas_view_centre(m, &m->anchor_from_cx, &m->anchor_from_cy);
	m->zoom_anchor_cx = c->canvas_geom.x + c->canvas_geom.width / 2.0;
	m->zoom_anchor_cy = c->canvas_geom.y + c->canvas_geom.height / 2.0;
	m->zoom_from = m->zoom;
	m->zoom_target = m->ov_restore_zoom;
	m->zoom_anim_start = get_now_in_ms();
	m->zoom_anim_duration = config.animation_duration_move > 0
								? config.animation_duration_move
								: 300;
	m->zoom_animating = true;
	m->cam_last_tick_ms = get_now_in_ms();
	wlr_output_schedule_frame(m->wlr_output);
}

void canvas_overview_point(Monitor *m, double x, double y, double *outx,
						   double *outy) {
	double cx, cy;
	canvas_overview_centroid(m, &cx, &cy);
	double dx = x - cx, dy = y - cy;
	double r = sqrt(dx * dx + dy * dy);
	if (r < 1e-6) {
		*outx = x;
		*outy = y;
		return;
	}
	double k = config.canvas_overview_compression;
	double rr = k * log(1.0 + r / k);
	double f = rr / r;
	*outx = cx + dx * f;
	*outy = cy + dy * f;
}

// frame the overview on the layout it actually shows compressed then relaxed
// measuring only the compressed positions framed the wrong box and cards left
// the screen edges measured 2 windows spanning x -6..1286 on 1280px starts the
// zoom and camera animation and is used on entry and after any layout change
// while open false when theres nothing to show
static bool canvas_overview_fit(Monitor *m) {
	double minx = 1e18, miny = 1e18, maxx = -1e18, maxy = -1e18;
	int n = 0;
	Client *c = NULL;
	canvas_overview_layout(m);
	wl_list_for_each(c, &server.clients, link) {
		if (!VISIBLEON(c, m) || !ISFAKETILED(c))
			continue;
		double px = c->ov_x, py = c->ov_y;
		if (px < minx)
			minx = px;
		if (py < miny)
			miny = py;
		if (px + c->canvas_geom.width > maxx)
			maxx = px + c->canvas_geom.width;
		if (py + c->canvas_geom.height > maxy)
			maxy = py + c->canvas_geom.height;
		n++;
	}
	if (n == 0)
		return false;

	const double margin = 1.0 + config.canvas_overview_margin / 100.0;
	double bw = (maxx - minx) * margin, bh = (maxy - miny) * margin;
	double zx = bw > 0 ? (double)m->w.width / bw : 1.0;
	double zy = bh > 0 ? (double)m->w.height / bh : 1.0;
	double target = zx < zy ? zx : zy;
	if (target > 1.0)
		target = 1.0; // never magnify
	if (target < CANVAS_ZOOM_MIN)
		target = CANVAS_ZOOM_MIN;

	canvas_view_centre(m, &m->anchor_from_cx, &m->anchor_from_cy);
	m->zoom_anchor_cx = (minx + maxx) / 2.0;
	m->zoom_anchor_cy = (miny + maxy) / 2.0;
	m->zoom_from = m->zoom;
	m->zoom_target = target;
	m->zoom_anim_start = get_now_in_ms();
	m->zoom_anim_duration = config.animation_duration_move > 0
								? config.animation_duration_move
								: 300;
	m->zoom_animating = true;
	m->cam_last_tick_ms = get_now_in_ms();
	wlr_output_schedule_frame(m->wlr_output);
	return true;
}

// each tag keeps its own camera
// what gets saved is where the camera is going, not where it is mid flight,
// and with the overview open the camera to come back to
static void canvas_tag_cam_save(Monitor *m) {
	uint32_t t = m->canvas_cam_tag;
	if (t >= PERTAG_SLOTS)
		return;
	struct Pertag *p = m->pertag;
	if (m->canvas_overview) {
		p->cam_cx[t] = m->ov_restore_cx;
		p->cam_cy[t] = m->ov_restore_cy;
		p->cam_zoom[t] = m->ov_restore_zoom;
	} else if (m->zoom_animating) {
		p->cam_cx[t] = m->zoom_anchor_cx;
		p->cam_cy[t] = m->zoom_anchor_cy;
		p->cam_zoom[t] = m->zoom_target;
	} else {
		canvas_view_centre(m, &p->cam_cx[t], &p->cam_cy[t]);
		p->cam_zoom[t] = m->zoom;
	}
	p->cam_saved[t] = true;
}

static void canvas_tag_cam_load(Monitor *m) {
	uint32_t t = m->canvas_cam_tag;
	struct Pertag *p = m->pertag;
	if (t >= PERTAG_SLOTS || !p->cam_saved[t])
		return;
	m->cam_vx = m->cam_vy = 0.0;
	m->zoom_animating = false;
	if (m->canvas_overview) {
		// the overview stays open and reframes the new tag
		m->ov_restore_cx = p->cam_cx[t];
		m->ov_restore_cy = p->cam_cy[t];
		m->ov_restore_zoom = p->cam_zoom[t];
		m->ov_blend_from = m->ov_blend;
		m->ov_blend_to = 1.0;
		canvas_overview_fit(m);
		return;
	}
	m->ov_blend = m->ov_blend_from = m->ov_blend_to = 0.0;
	m->zoom = m->zoom_target = p->cam_zoom[t];
	canvas_cam_for_centre(m, p->cam_cx[t], p->cam_cy[t], m->zoom);
	wlr_output_schedule_frame(m->wlr_output);
}

static bool wsov_capturing;
static uint32_t wsov_real_tag = UINT32_MAX;

// a tag switch moves the camera at once, the background glides from where it
// was last drawn to the new camera over the tag animation instead of jumping
static void canvas_bg_glide_start(Monitor *m) {
	// the workspace overview already drew the shader at the new camera
	if (wsov_capturing || workspace_overview_is_active() || !config.animations ||
		!config.animation_duration_tag || m->bg_w <= 0 || m->bg_zoom <= 0.0)
		return;
	m->bg_glide_cx = m->bg_cam_x + view_off_x(m) / m->bg_zoom;
	m->bg_glide_cy = m->bg_cam_y + view_off_y(m) / m->bg_zoom;
	m->bg_glide_zoom = m->bg_zoom;
	m->bg_glide_start = get_now_in_ms();
	m->bg_glide = true;
}

// camera the background is drawn with, true while it is still gliding
bool canvas_bg_camera(Monitor *m, double *cam_x, double *cam_y, double *zoom) {
	*cam_x = m->cam_x, *cam_y = m->cam_y, *zoom = m->zoom;
	if (!m->bg_glide)
		return false;
	double t = (double)(get_now_in_ms() - m->bg_glide_start) /
			   config.animation_duration_tag;
	if (t >= 1.0 || !config.animation_duration_tag) {
		m->bg_glide = false;
		return false;
	}
	double e = find_animation_curve_at(t, TAG);
	double cx, cy;
	canvas_view_centre(m, &cx, &cy);
	cx = m->bg_glide_cx + (cx - m->bg_glide_cx) * e;
	cy = m->bg_glide_cy + (cy - m->bg_glide_cy) * e;
	*zoom = m->bg_glide_zoom + (m->zoom - m->bg_glide_zoom) * e;
	*cam_x = cx - view_off_x(m) / *zoom;
	*cam_y = cy - view_off_y(m) / *zoom;
	return true;
}

// called from canvas() so every path that changes curtag is covered
// a tag never visited starts from the camera you leave
void canvas_tag_camera_sync(Monitor *m) {
	uint32_t t = m->pertag->curtag;
	uint32_t old = m->canvas_cam_tag;
	if (t == old || t >= PERTAG_SLOTS)
		return;
	canvas_tag_cam_save(m);
	struct Pertag *p = m->pertag;
	if (!p->cam_saved[t] && old < PERTAG_SLOTS) {
		p->cam_cx[t] = p->cam_cx[old];
		p->cam_cy[t] = p->cam_cy[old];
		p->cam_zoom[t] = p->cam_zoom[old];
		p->cam_saved[t] = true;
	}
	m->canvas_cam_tag = t;
	if (!m->canvas_overview)
		canvas_bg_glide_start(m);
	canvas_tag_cam_load(m);
}

// workspace overview capture
// it arranges every tag in turn, framed like the canvas overview so no window
// is off its preview, then puts the real camera back
void canvas_wsov_begin(Monitor *m) {
	canvas_tag_camera_sync(m);
	wsov_capturing = true;
	canvas_tag_cam_save(m);
	wsov_real_tag = m->canvas_cam_tag;
	// framing lays every tag out at preview zoom keep the real boxes so
	// the restore arrange doesnt configure hidden windows at preview size
	Client *c;
	wl_list_for_each(c, &server.clients, link) {
		if (c->mon == m)
			c->canvas_wsov_geom = c->geom;
	}
}

void canvas_wsov_frame(Monitor *m) {
	m->canvas_cam_tag = m->pertag->curtag;
	m->ov_blend = m->ov_blend_from = m->ov_blend_to = 1.0;
	m->cam_vx = m->cam_vy = 0.0;
	if (canvas_overview_fit(m)) {
		m->zoom_animating = false;
		m->zoom = m->zoom_target;
		canvas_cam_for_centre(m, m->zoom_anchor_cx, m->zoom_anchor_cy,
							  m->zoom);
	} else {
		m->zoom_animating = false;
		m->zoom = 1.0;
		canvas_cam_for_centre(m, 0.0, 0.0, 1.0);
	}
}

// box c has in a wsov preview framed with that camera
// same math as canvas() so recaptures between full captures agree with them,
// animation.current only knows the real camera
// e 0 is the real layout and 1 the overview one, opening the overview blends it
struct wlr_box canvas_preview_box(Client *c, double ov_x, double ov_y,
								  double cam_x, double cam_y, double z,
								  double e) {
	Monitor *m = c->mon;
	double saved = m->zoom;
	m->zoom = z; // border width scales with the camera
	int32_t cbw = (int32_t)canvas_frame_bw(c);
	m->zoom = saved;
	double inner = 2.0 * config.borderpx;
	double px = c->canvas_geom.x + (ov_x - c->canvas_geom.x) * e;
	double py = c->canvas_geom.y + (ov_y - c->canvas_geom.y) * e;
	return (struct wlr_box){
		.x = m->m.x + (int32_t)lround((px - cam_x) * z),
		.y = m->m.y + (int32_t)lround((py - cam_y) * z),
		.width = (int32_t)lround((c->canvas_geom.width - inner) * z) + 2 * cbw,
		.height =
			(int32_t)lround((c->canvas_geom.height - inner) * z) + 2 * cbw,
	};
}

// view centre and zoom tag t is shown with, a tag never visited gets the
// camera you are on like canvas_tag_camera_sync gives it
void canvas_tag_camera(Monitor *m, uint32_t t, double *cx, double *cy,
					   double *zoom) {
	struct Pertag *p = m->pertag;
	if (!wsov_capturing && t == m->canvas_cam_tag)
		canvas_tag_cam_save(m);
	uint32_t s = t < PERTAG_SLOTS && p->cam_saved[t]
					 ? t
					 : (wsov_capturing ? wsov_real_tag : m->canvas_cam_tag);
	if (s < PERTAG_SLOTS && p->cam_saved[s]) {
		*cx = p->cam_cx[s], *cy = p->cam_cy[s], *zoom = p->cam_zoom[s];
		return;
	}
	canvas_view_centre(m, cx, cy);
	*zoom = m->zoom;
}

void canvas_cam_at(Monitor *m, double cx, double cy, double zoom,
				   double *cam_x, double *cam_y) {
	*cam_x = cx - view_off_x(m) / zoom;
	*cam_y = cy - view_off_y(m) / zoom;
}

// tag t centred on c at the zoom the tag was left at, picking a window in the
// workspace overview lands like canvas_overview_pick
void canvas_tag_camera_focus(Monitor *m, uint32_t t, Client *c) {
	if (t >= PERTAG_SLOTS || !c || !c->canvas_placed)
		return;
	double cx, cy, z;
	canvas_tag_camera(m, t, &cx, &cy, &z);
	struct Pertag *p = m->pertag;
	p->cam_cx[t] = c->canvas_geom.x + c->canvas_geom.width / 2.0;
	p->cam_cy[t] = c->canvas_geom.y + c->canvas_geom.height / 2.0;
	p->cam_zoom[t] = z;
	p->cam_saved[t] = true;
	if (t == m->canvas_cam_tag)
		canvas_tag_cam_load(m);
}

// a window sent to an empty tag takes that tag's camera with it, so the tag
// opens on the window and not on the empty spot it was left at
void canvas_client_tags_changed(Client *c, uint32_t old_tags) {
	Monitor *m = c->mon;
	if (!m || !c->canvas_placed)
		return;
	uint32_t added = c->tags & ~old_tags & TAGMASK;
	for (uint32_t t = 1; added && t < PERTAG_SLOTS; t++, added >>= 1) {
		if (!(added & 1) || !m->pertag->cam_saved[t])
			continue;
		uint32_t bit = 1u << (t - 1);
		bool empty = true;
		Client *o;
		wl_list_for_each(o, &server.clients, link) {
			if (o != c && o->mon == m && (o->tags & bit)) {
				empty = false;
				break;
			}
		}
		if (empty)
			canvas_tag_camera_focus(m, t, c);
	}
}

void canvas_wsov_end(Monitor *m) {
	Client *c;
	wl_list_for_each(c, &server.clients, link) {
		if (c->mon == m && c->canvas_wsov_geom.width > 0)
			c->geom = c->canvas_wsov_geom;
	}
	wsov_capturing = false;
	m->canvas_cam_tag = m->pertag->curtag;
	if (m->canvas_cam_tag < PERTAG_SLOTS && m->pertag->cam_saved[m->canvas_cam_tag])
		canvas_tag_cam_load(m);
	else
		m->ov_blend = m->ov_blend_from = m->ov_blend_to = 0.0;
}

// canvas_overview toggles
// on entry compress distances and pick the zoom that frames the compressed
// extent on exit put the old camera back
int32_t canvas_overview(const Arg *arg) {
	(void)arg;
	Monitor *m = server.selected_monitor;
	if (!m || !canvas_active(m) || workspace_overview_is_active())
		return 0;

	if (m->canvas_overview) {
		m->canvas_overview = false;
		// aim the blend back at the true layout
		// without it toggling off inherited ov_blend_to = 1.0 from entry and
		// replayed the compression so Mod+Tab left windows stacked where the
		// overview put them
		m->ov_blend_from = m->ov_blend;
		m->ov_blend_to = 0.0;
		m->zoom_anchor_cx = m->ov_restore_cx;
		m->zoom_anchor_cy = m->ov_restore_cy;
		canvas_view_centre(m, &m->anchor_from_cx, &m->anchor_from_cy);
		m->zoom_from = m->zoom;
		m->zoom_target = m->ov_restore_zoom;
		m->zoom_anim_start = get_now_in_ms();
		m->zoom_anim_duration = config.animation_duration_move > 0
									? config.animation_duration_move
									: 300;
		m->zoom_animating = true;
		m->cam_last_tick_ms = get_now_in_ms();
		wlr_output_schedule_frame(m->wlr_output);
		return 0;
	}

	m->canvas_overview = true; // so the layout compresses while we measure
	m->ov_blend_from = m->ov_blend;
	m->ov_blend_to = 1.0;
	canvas_view_centre(m, &m->ov_restore_cx, &m->ov_restore_cy);
	m->ov_restore_zoom = m->zoom;
	if (!canvas_overview_fit(m)) {
		m->canvas_overview = false;
		m->ov_blend_to = 0.0;
	}
	return 0;
}

// the canvas overview with a letter on every window, press one to go to it
// same overview as canvas_overview, clicking still works while the letters are
// up and escape or pressing this again leaves
void begin_jump_mode(Monitor *m) { m->is_jump_mode = 1; }

void finish_jump_mode(Monitor *m) {
	if (!m->is_jump_mode)
		return;

	Client *c;
	wl_list_for_each(c, &server.clients, link) {
		if (c->mon != m)
			continue;

		c->jump_char = '\0';
		c->jump_keycodes = (MultiKeycode){0};

		if (c->jump_label_node && c->jump_label_node->scene->node.enabled) {
			wlr_scene_node_set_enabled(&c->jump_label_node->scene->node, false);
		}
	}
	m->is_jump_mode = 0;
}

int32_t canvas_jump(const Arg *arg) {
	Monitor *m = server.selected_monitor;
	if (!m || !canvas_active(m))
		return 0;
	if (m->canvas_overview && m->is_jump_mode)
		return canvas_overview(arg);
	if (!m->canvas_overview)
		canvas_overview(arg);
	if (!m->canvas_overview)
		return 0;

	const char *labels =
		config.jump_labels ? config.jump_labels : default_jump_labels;
	size_t n = labels ? strlen(labels) : 0, i = 0;
	begin_jump_mode(m);
	Client *c = NULL;
	wl_list_for_each(c, &server.clients, link) {
		if (i >= n)
			break;
		if (!VISIBLEON(c, m) || c->isunglobal || client_is_x11_popup(c) ||
			!c->jump_label_node)
			continue;
		c->jump_char = labels[i++];
		find_keycodes_for_char(c->jump_char, &c->jump_keycodes);
		char text[2] = {c->jump_char, '\0'};
		mango_jump_label_node_update(c->jump_label_node, text,
									 m->wlr_output->scale);
		canvas_jump_label_place(c);
	}
	return 0;
}
