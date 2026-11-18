// background precompute for cascade and auto placement, see canvas_precompute.h
// one worker thread, one job slot that only holds the newest snapshot so a
// burst of changes is one job, one result slot worker sleeps on a condition
// variable when idle so no wakeups results get read under the mutex so the main
// loop never gets woken either clients and monitors only cross threads as
// opaque ids for matching, the worker never dereferences them
#include "mango/layout/canvas_precompute.h"
#include "mango/common/server.h"
#include "mango/config/parse.h"
#include "mango/input/pointer.h"
#include "mango/layout/canvas.h"
#include "mango/manage/client.h"
#include "mango/manage/monitor.h"
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#define PC_MAX 64

typedef struct {
	Monitor *mon;
	uint64_t pack_sig, place_sig;
	int n;
	Client *ids[PC_MAX];
	pack_rect r[PC_MAX];
	double gx, gy, seed_w, seed_h;
	int mode;		   // config.canvas_placement it was made for
	int scope;		   // config.canvas_cascade_scope
	int label[PC_MAX]; // cluster of each window, all 0 for scope all
} pc_snapshot;

typedef struct {
	pc_snapshot in;
	pack_rect packed[PC_MAX];	  // each cluster on its own
	pack_rect packed_all[PC_MAX]; // everything at once
	bool spot_ok[PC_MAX];
	double spot_x[PC_MAX], spot_y[PC_MAX];
} pc_result;

static pthread_mutex_t pc_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pc_wake = PTHREAD_COND_INITIALIZER;
static bool pc_started, pc_pending;
static pc_snapshot pc_job, pc_last_sent;
static pc_result pc_done;
static bool pc_have_result;

// fnv-1a over values rounded to 1/100 of a canvas unit
static uint64_t mix(uint64_t h, double v) {
	int64_t q = llround(v * 100.0);
	for (int i = 0; i < 8; i++) {
		h ^= (uint64_t)((q >> (8 * i)) & 0xff);
		h *= 1099511628211ull;
	}
	return h;
}

static void seed_size(Monitor *m, double *w, double *h) {
	// has to match canvas_seed_client, half the viewport in canvas units
	double z = m->zoom > 0.0 ? m->zoom : 1.0;
	*w = (double)(int32_t)((double)m->w.width / z / 2.0);
	*h = (double)(int32_t)((double)m->w.height / z / 2.0);
	if (*w < 1)
		*w = 1;
	if (*h < 1)
		*h = 1;
}

static void snapshot(Monitor *m, Client *exclude, pc_snapshot *s) {
	s->mon = m;
	s->n = canvas_cluster(m, exclude, s->ids, s->r);
	s->gx = server.enable_gaps ? m->gappih : 0;
	s->gy = server.enable_gaps ? m->gappiv : 0;
	seed_size(m, &s->seed_w, &s->seed_h);
	s->mode = config.canvas_placement;
	s->scope = config.canvas_cascade_scope;
	if (s->scope == CANVAS_SCOPE_CLUSTER)
		canvas_cluster_labels(m, s->r, s->n, s->label);
	else
		memset(s->label, 0, sizeof(s->label));
	// cascade depends on which windows and their sizes
	// placement also depends on positions and the size a new window would get
	uint64_t h = 1469598103934665603ull;
	h = mix(h, (double)(uintptr_t)m);
	h = mix(h, s->gx);
	h = mix(h, s->gy);
	// and per cluster on which windows are in it
	// moving a window inside its cluster keeps the arrangement, moving it to
	// another one doesnt
	h = mix(h, s->scope);
	for (int i = 0; i < s->n; i++) {
		h = mix(h, (double)(uintptr_t)s->ids[i]);
		h = mix(h, s->r[i].w);
		h = mix(h, s->r[i].h);
		h = mix(h, s->label[i]);
	}
	s->pack_sig = h;
	for (int i = 0; i < s->n; i++) {
		h = mix(h, s->r[i].x);
		h = mix(h, s->r[i].y);
	}
	h = mix(h, s->seed_w);
	h = mix(h, s->seed_h);
	h = mix(h, s->mode);
	s->place_sig = h;
}

static void compute(const pc_snapshot *in, pc_result *out) {
	out->in = *in;
	// each cluster packed on its own, one cluster is everything for scope all
	// positions are relative to that clusters packing
	int nl = 0;
	for (int i = 0; i < in->n; i++)
		nl = in->label[i] + 1 > nl ? in->label[i] + 1 : nl;
	memcpy(out->packed_all, in->r, sizeof(pack_rect) * in->n);
	pack_min_perimeter(out->packed_all, in->n, in->gx, in->gy);
	for (int l = 0; l < nl; l++) {
		pack_rect sub[PC_MAX];
		int idx[PC_MAX], k = 0;
		for (int i = 0; i < in->n; i++)
			if (in->label[i] == l)
				idx[k] = i, sub[k++] = in->r[i];
		pack_min_perimeter(sub, k, in->gx, in->gy);
		for (int j = 0; j < k; j++)
			out->packed[idx[j]] = sub[j];
	}
	// per window, where a new one goes if THAT window is the anchor
	// (auto-focused / auto-recent, on its border) or the focused one
	// (auto-compact, anywhere, ties toward it) same fallback as
	// canvas_seed_client
	for (int i = 0; i < in->n; i++) {
		const pack_rect *a = &in->r[i];
		double cx = a->x + a->w / 2, cy = a->y + a->h / 2;
		bool ok = false;
		if (in->mode == CANVAS_PLACE_COMPACT)
			ok = pack_place_compact(in->r, in->n, in->seed_w, in->seed_h,
									in->gx, in->gy, cx, cy, &out->spot_x[i],
									&out->spot_y[i]);
		else
			ok = pack_place_adjacent(in->r, in->n, i, in->seed_w, in->seed_h,
									 in->gx, in->gy, &out->spot_x[i],
									 &out->spot_y[i]);
		if (!ok)
			ok = pack_place_cluster(in->r, in->n, in->seed_w, in->seed_h,
									in->gx, in->gy, cx, cy, &out->spot_x[i],
									&out->spot_y[i]);
		out->spot_ok[i] = ok;
	}
}

static void *worker(void *arg) {
	(void)arg;
	static pc_snapshot job; // worker only copies, too big for the stack
	static pc_result res;
	pthread_mutex_lock(&pc_lock);
	for (;;) {
		while (!pc_pending)
			pthread_cond_wait(&pc_wake, &pc_lock);
		job = pc_job;
		pc_pending = false;
		pthread_mutex_unlock(&pc_lock);
		compute(&job, &res);
		pthread_mutex_lock(&pc_lock);
		pc_done = res;
		pc_have_result = true;
	}
	return NULL;
}

void canvas_precompute_note(Monitor *m) {
	if (!config.canvas_precompute || !m || !canvas_active(m))
		return;
	// skip while dragging, the drop arrange takes the snapshot, same for a zoom
	// in flight pan fling and fly rearrange every frame without moving anything
	// on the plane each of those frames rebuilt the whole snapshot (clusters,
	// O(n^2) pack_clusters, the hash) and the signature was the same not
	// camera_rigid though, cascade arranges rigidly AFTER moving canvas_geom
	// its glide frames are skipped too since canvas_geom is already final
	// the camera tick clears canvas_layout_animating before the landing arrange
	// and that one takes the snapshot
	if (server.cursor_mode == CurMove || server.cursor_mode == CurResize ||
		m->zoom_animating || m->canvas_layout_animating ||
		canvas_camera_moving(m))
		return;
	pc_snapshot s;
	snapshot(m, NULL, &s);
	if (s.mon == pc_last_sent.mon && s.place_sig == pc_last_sent.place_sig)
		return;
	pc_last_sent = s;
	pthread_mutex_lock(&pc_lock);
	pc_job = s;
	pc_pending = true;
	if (!pc_started) {
		// thread is never joined, it dies with the process
		pthread_t t;
		pc_started = pthread_create(&t, NULL, worker, NULL) == 0;
		if (pc_started)
			pthread_detach(t);
	}
	pthread_cond_signal(&pc_wake);
	pthread_mutex_unlock(&pc_lock);
}

bool canvas_precompute_packed(Monitor *m, Client **list, int n, pack_rect *out,
							  bool all) {
	if (!config.canvas_precompute || !pc_started)
		return false;
	pc_snapshot s;
	snapshot(m, NULL, &s);
	bool ok = false;
	pthread_mutex_lock(&pc_lock);
	if (pc_have_result && pc_done.in.mon == m &&
		pc_done.in.pack_sig == s.pack_sig) {
		ok = true;
		for (int i = 0; i < n && ok; i++) {
			int j = 0;
			while (j < pc_done.in.n && pc_done.in.ids[j] != list[i])
				j++;
			if (j == pc_done.in.n)
				ok = false;
			else
				out[i] = all ? pc_done.packed_all[j] : pc_done.packed[j];
		}
	}
	pthread_mutex_unlock(&pc_lock);
	return ok;
}

bool canvas_precompute_spot(Monitor *m, Client *exclude, Client *anchor,
							double w, double h, double *x, double *y) {
	if (!config.canvas_precompute || !pc_started)
		return false;
	pc_snapshot s;
	snapshot(m, exclude, &s);
	bool ok = false;
	pthread_mutex_lock(&pc_lock);
	if (pc_have_result && pc_done.in.mon == m && pc_done.in.mode == s.mode &&
		pc_done.in.place_sig == s.place_sig &&
		fabs(pc_done.in.seed_w - w) < 0.5 && fabs(pc_done.in.seed_h - h) < 0.5)
		for (int j = 0; j < pc_done.in.n; j++)
			if (pc_done.in.ids[j] == anchor && pc_done.spot_ok[j]) {
				*x = pc_done.spot_x[j];
				*y = pc_done.spot_y[j];
				ok = true;
			}
	pthread_mutex_unlock(&pc_lock);
	return ok;
}
