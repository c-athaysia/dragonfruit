// rectangle packing for the canvas
// perimeter of a cluster, smallest perimeter arrangement for cascade, best spot
// next to a window for auto placement see canvas_pack.h
#include "mango/layout/canvas_pack.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// edges closer than this are the same edge
// windows a gap apart are off by float noise and the sliver between them
// shouldnt count
#define PACK_EPS 0.5
#define PACK_MAX 64

static int cmp_double(const void *a, const void *b) {
	double d = *(const double *)a - *(const double *)b;
	return (d > 0) - (d < 0);
}

// sorts values and merges ones closer than PACK_EPS, returns the new count
static int merge_coords(double *v, int n) {
	qsort(v, n, sizeof(*v), cmp_double);
	int k = 0;
	for (int i = 0; i < n; i++)
		if (k == 0 || v[i] - v[k - 1] > PACK_EPS)
			v[k++] = v[i];
	return k;
}

static int nearest(const double *v, int n, double x) {
	int best = 0;
	for (int i = 1; i < n; i++)
		if (fabs(v[i] - x) < fabs(v[best] - x))
			best = i;
	return best;
}

double pack_perimeter(const pack_rect *r, int n, double gx, double gy) {
	if (n <= 0)
		return 0.0;
	double xs[2 * PACK_MAX], ys[2 * PACK_MAX];
	if (n > PACK_MAX)
		n = PACK_MAX;
	for (int i = 0; i < n; i++) {
		xs[2 * i] = r[i].x - gx / 2;
		xs[2 * i + 1] = r[i].x + r[i].w + gx / 2;
		ys[2 * i] = r[i].y - gy / 2;
		ys[2 * i + 1] = r[i].y + r[i].h + gy / 2;
	}
	int nx = merge_coords(xs, 2 * n), ny = merge_coords(ys, 2 * n);
	int cw = nx - 1, ch = ny - 1;
	if (cw <= 0 || ch <= 0)
		return 0.0;

	// 0 empty, 1 covered, 2 outside
	unsigned char *cell = calloc((size_t)cw * ch, 1);
	int *stack = malloc(sizeof(int) * (size_t)cw * ch);
	if (!cell || !stack) {
		free(cell);
		free(stack);
		return 0.0;
	}
	for (int i = 0; i < n; i++) {
		int x0 = nearest(xs, nx, r[i].x - gx / 2);
		int x1 = nearest(xs, nx, r[i].x + r[i].w + gx / 2);
		int y0 = nearest(ys, ny, r[i].y - gy / 2);
		int y1 = nearest(ys, ny, r[i].y + r[i].h + gy / 2);
		for (int y = y0; y < y1; y++)
			for (int x = x0; x < x1; x++)
				cell[y * cw + x] = 1;
	}
	// flood the outside in from the border
	// any empty cell it cant reach is a hole inside the cluster and counts as
	// cluster
	int sp = 0;
	for (int y = 0; y < ch; y++)
		for (int x = 0; x < cw; x++)
			if ((x == 0 || y == 0 || x == cw - 1 || y == ch - 1) &&
				cell[y * cw + x] == 0) {
				cell[y * cw + x] = 2;
				stack[sp++] = y * cw + x;
			}
	while (sp) {
		int c = stack[--sp], x = c % cw, y = c / cw;
		int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
		for (int k = 0; k < 4; k++) {
			int ax = nb[k][0], ay = nb[k][1];
			if (ax < 0 || ay < 0 || ax >= cw || ay >= ch ||
				cell[ay * cw + ax] != 0)
				continue;
			cell[ay * cw + ax] = 2;
			stack[sp++] = ay * cw + ax;
		}
	}
	// perimeter is every side of a cluster cell that faces outside
	double p = 0.0;
	for (int y = 0; y < ch; y++)
		for (int x = 0; x < cw; x++) {
			if (cell[y * cw + x] == 2)
				continue;
			double w = xs[x + 1] - xs[x], h = ys[y + 1] - ys[y];
			if (x == 0 || cell[y * cw + x - 1] == 2)
				p += h;
			if (x == cw - 1 || cell[y * cw + x + 1] == 2)
				p += h;
			if (y == 0 || cell[(y - 1) * cw + x] == 2)
				p += w;
			if (y == ch - 1 || cell[(y + 1) * cw + x] == 2)
				p += w;
		}
	free(cell);
	free(stack);
	return p;
}

// overlap length of the inflated spans a and b, negative means apart
static double span_overlap(double a0, double a1, double b0, double b1) {
	return fmin(a1, b1) - fmax(a0, b0);
}

// each rect grown by half the gap, do they overlap (1), touch on an edge (2) or
// neither (0)
static int relation(const pack_rect *a, const pack_rect *b, double gx,
					double gy) {
	double ox = span_overlap(a->x - gx / 2, a->x + a->w + gx / 2, b->x - gx / 2,
							 b->x + b->w + gx / 2);
	double oy = span_overlap(a->y - gy / 2, a->y + a->h + gy / 2, b->y - gy / 2,
							 b->y + b->h + gy / 2);
	if (ox > PACK_EPS && oy > PACK_EPS)
		return 1;
	if ((fabs(ox) <= PACK_EPS && oy > PACK_EPS) ||
		(fabs(oy) <= PACK_EPS && ox > PACK_EPS))
		return 2;
	return 0;
}

// candidate spots for a w x h rect next to the placed ones
// each edge of it sits against or lines up with an edge of a placed rect
// keeps only spots that overlap nothing and touch the cluster
// or touch rect anchor if anchor >= 0
static int candidates(const pack_rect *p, int k, double w, double h, double gx,
					  double gy, int anchor, double *cx, double *cy, int max) {
	double xs[4 * PACK_MAX], ys[4 * PACK_MAX];
	int nxs = 0, nys = 0;
	for (int i = 0; i < k; i++) {
		xs[nxs++] = p[i].x + p[i].w + gx;
		xs[nxs++] = p[i].x - w - gx;
		xs[nxs++] = p[i].x;
		xs[nxs++] = p[i].x + p[i].w - w;
		ys[nys++] = p[i].y + p[i].h + gy;
		ys[nys++] = p[i].y - h - gy;
		ys[nys++] = p[i].y;
		ys[nys++] = p[i].y + p[i].h - h;
	}
	nxs = merge_coords(xs, nxs);
	nys = merge_coords(ys, nys);
	int n = 0;
	for (int a = 0; a < nxs; a++)
		for (int b = 0; b < nys && n < max; b++) {
			pack_rect c = {xs[a], ys[b], w, h};
			bool touches = false, ok = true;
			for (int i = 0; i < k && ok; i++) {
				int rel = relation(&c, &p[i], gx, gy);
				if (rel == 1)
					ok = false;
				else if (rel == 2 && (anchor < 0 || anchor == i))
					touches = true;
			}
			if (ok && touches) {
				cx[n] = c.x;
				cy[n] = c.y;
				n++;
			}
		}
	return n;
}

// tie break between equal perimeters, smaller bounding box wins
static double bbox_area(const pack_rect *r, int n) {
	double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
	for (int i = 0; i < n; i++) {
		x0 = fmin(x0, r[i].x);
		y0 = fmin(y0, r[i].y);
		x1 = fmax(x1, r[i].x + r[i].w);
		y1 = fmax(y1, r[i].y + r[i].h);
	}
	return (x1 - x0) * (y1 - y0);
}

typedef struct {
	double per, area;
	int idx; // into the child array
} pack_key;

static void origin(const pack_rect *r, int k, double *x0, double *y0) {
	*x0 = 1e18, *y0 = 1e18;
	for (int i = 0; i < k; i++)
		*x0 = fmin(*x0, r[i].x), *y0 = fmin(*y0, r[i].y);
}

// is arrangement t already in the first ncur states
static bool seen(const pack_rect *cur, int ncur, int n, const pack_rect *t,
				 int k) {
	double tx, ty;
	origin(t, k, &tx, &ty);
	for (int s = 0; s < ncur; s++) {
		const pack_rect *u = &cur[(size_t)s * n];
		double ux, uy;
		origin(u, k, &ux, &uy);
		bool same = true;
		for (int i = 0; i < k && same; i++)
			same = fabs((t[i].x - tx) - (u[i].x - ux)) < PACK_EPS &&
				   fabs((t[i].y - ty) - (u[i].y - uy)) < PACK_EPS;
		if (same)
			return true;
	}
	return false;
}

static int cmp_key(const void *a, const void *b) {
	const pack_key *s = a, *t = b;
	if (fabs(s->per - t->per) > 1e-6)
		return s->per < t->per ? -1 : 1;
	return (s->area > t->area) - (s->area < t->area);
}

// beam search over one insertion order
// returns the best perimeter and writes that arrangement to out in insertion
// order states are runs of n rects sized to the cluster not PACK_MAX, child
// array grows when it needs to
static double beam(const pack_rect *in, const int *order, int n, double gx,
				   double gy, int width, pack_rect *out) {
	enum { MAXC = 4096 };
	size_t cap = 256;
	pack_rect *cur = malloc(sizeof(pack_rect) * (size_t)width * n);
	pack_rect *kids = malloc(sizeof(pack_rect) * cap * n);
	pack_key *keys = malloc(sizeof(pack_key) * cap);
	double *cx = malloc(sizeof(double) * MAXC),
		   *cy = malloc(sizeof(double) * MAXC);
	double best = INFINITY;
	if (!cur || !kids || !keys || !cx || !cy)
		goto out;
	int ncur = 1;
	cur[0] = (pack_rect){0, 0, in[order[0]].w, in[order[0]].h};
	for (int step = 1; step < n; step++) {
		double w = in[order[step]].w, h = in[order[step]].h;
		size_t nk = 0;
		for (int s = 0; s < ncur; s++) {
			const pack_rect *st = &cur[(size_t)s * n];
			int nc = candidates(st, step, w, h, gx, gy, -1, cx, cy, MAXC);
			for (int c = 0; c < nc; c++) {
				if (nk == cap) {
					cap *= 2;
					pack_rect *k2 = realloc(kids, sizeof(pack_rect) * cap * n);
					pack_key *y2 = realloc(keys, sizeof(pack_key) * cap);
					if (!k2 || !y2) {
						kids = k2 ? k2 : kids;
						keys = y2 ? y2 : keys;
						goto out;
					}
					kids = k2, keys = y2;
				}
				pack_rect *t = &kids[nk * n];
				memcpy(t, st, sizeof(pack_rect) * step);
				t[step] = (pack_rect){cx[c], cy[c], w, h};
				keys[nk] = (pack_key){pack_perimeter(t, step + 1, gx, gy),
									  bbox_area(t, step + 1), (int)nk};
				nk++;
			}
		}
		if (nk == 0)
			goto out;
		qsort(keys, nk, sizeof(pack_key), cmp_key);
		// keep the best width DIFFERENT arrangements
		// most children are the same layout shifted or came from another parent
		// and dupes starve the beam compared after moving each to its top left
		ncur = 0;
		for (size_t q = 0; q < nk && ncur < width; q++) {
			const pack_rect *t = &kids[(size_t)keys[q].idx * n];
			if (seen(cur, ncur, n, t, step + 1))
				continue;
			memcpy(&cur[(size_t)ncur * n], t, sizeof(pack_rect) * n);
			ncur++;
		}
	}
	best = pack_perimeter(cur, n, gx, gy);
	memcpy(out, cur, sizeof(pack_rect) * n);
out:
	free(cur), free(kids), free(keys), free(cx), free(cy);
	return best;
}

// indices of r sorted biggest first by key 0 area, 1 height, 2 width
// hand written insertion sort, no qsort because the packer runs on a worker
// thread and the main thread so no static state allowed
static void sort_order(const pack_rect *r, int n, int key, int *order) {
	for (int i = 0; i < n; i++) {
		const pack_rect *p = &r[i];
		double k = key == 0 ? p->w * p->h : key == 1 ? p->h : p->w;
		int j = i;
		for (; j > 0; j--) {
			const pack_rect *q = &r[order[j - 1]];
			double kq = key == 0 ? q->w * q->h : key == 1 ? q->h : q->w;
			if (kq >= k)
				break;
			order[j] = order[j - 1];
		}
		order[j] = i;
	}
}

static bool next_perm(int *a, int n) {
	int i = n - 2;
	while (i >= 0 && a[i] >= a[i + 1])
		i--;
	if (i < 0)
		return false;
	int j = n - 1;
	while (a[j] <= a[i])
		j--;
	int t = a[i];
	a[i] = a[j], a[j] = t;
	for (int l = i + 1, r = n - 1; l < r; l++, r--)
		t = a[l], a[l] = a[r], a[r] = t;
	return true;
}

void pack_min_perimeter(pack_rect *r, int n, double gx, double gy) {
	if (n <= 0)
		return;
	if (n > PACK_MAX)
		n = PACK_MAX;
	int order[PACK_MAX];
	pack_rect best_r[PACK_MAX], tmp[PACK_MAX];
	int best_order[PACK_MAX];
	double best = INFINITY;

	// exact min perimeter is np hard so this is a search that has to finish
	// inside a keypress up to 4 windows every order with beam 64, up to 8 three
	// orders (area height width) with beam 32 up to 16 beam 8, past that one
	// order and greedy, fixed beam widths tuned by timing,
	// widen them if big clusters pack badly
	if (n <= 4) {
		for (int i = 0; i < n; i++)
			order[i] = i;
		do {
			double p = beam(r, order, n, gx, gy, 64, tmp);
			if (p < best - 1e-6) {
				best = p;
				memcpy(best_r, tmp, sizeof(pack_rect) * n);
				memcpy(best_order, order, sizeof(int) * n);
			}
		} while (next_perm(order, n));
	} else {
		// past 16 windows one order and greedy, each eval is O(n^2)
		int orders = n <= 16 ? 3 : 1;
		int width = n <= 8 ? 32 : n <= 16 ? 8 : 1;
		for (int key = 0; key < orders; key++) {
			sort_order(r, n, key, order);
			double p = beam(r, order, n, gx, gy, width, tmp);
			if (p < best - 1e-6) {
				best = p;
				memcpy(best_r, tmp, sizeof(pack_rect) * n);
				memcpy(best_order, order, sizeof(int) * n);
			}
		}
	}
	if (!isfinite(best))
		return;
	for (int i = 0; i < n; i++) {
		r[best_order[i]].x = best_r[i].x;
		r[best_order[i]].y = best_r[i].y;
	}
}

bool pack_place_adjacent(const pack_rect *r, int n, int anchor, double w,
						 double h, double gx, double gy, double *ox,
						 double *oy) {
	enum { MAXC = 4096 };
	if (n <= 0 || anchor < 0 || anchor >= n || n >= PACK_MAX)
		return false;
	double cx[MAXC], cy[MAXC];
	int nc = candidates(r, n, w, h, gx, gy, anchor, cx, cy, MAXC);
	if (nc == 0)
		return false;
	pack_rect t[PACK_MAX];
	memcpy(t, r, sizeof(pack_rect) * n);
	double best = INFINITY, best_area = INFINITY;
	for (int c = 0; c < nc; c++) {
		t[n] = (pack_rect){cx[c], cy[c], w, h};
		double p = pack_perimeter(t, n + 1, gx, gy);
		double a = bbox_area(t, n + 1);
		if (p < best - 1e-6 || (fabs(p - best) <= 1e-6 && a < best_area)) {
			best = p;
			best_area = a;
			*ox = cx[c];
			*oy = cy[c];
		}
	}
	return true;
}

bool pack_place_cluster(const pack_rect *r, int n, double w, double h,
						double gx, double gy, double px, double py, double *ox,
						double *oy) {
	enum { MAXC = 4096 };
	if (n <= 0 || n >= PACK_MAX)
		return false;
	static _Thread_local double cx[MAXC], cy[MAXC];
	int nc = candidates(r, n, w, h, gx, gy, -1, cx, cy, MAXC);
	if (nc == 0)
		return false;
	pack_rect t[PACK_MAX];
	memcpy(t, r, sizeof(pack_rect) * n);
	double best = INFINITY, best_d = INFINITY;
	for (int c = 0; c < nc; c++) {
		t[n] = (pack_rect){cx[c], cy[c], w, h};
		double p = pack_perimeter(t, n + 1, gx, gy);
		double dx = cx[c] + w / 2 - px, dy = cy[c] + h / 2 - py;
		double d = dx * dx + dy * dy;
		if (p < best - 1e-6 || (fabs(p - best) <= 1e-6 && d < best_d)) {
			best = p;
			best_d = d;
			*ox = cx[c];
			*oy = cy[c];
		}
	}
	return true;
}

bool pack_place_compact(const pack_rect *r, int n, double w, double h,
						double gx, double gy, double px, double py, double *ox,
						double *oy) {
	enum { MAXC = 4096 };
	if (n <= 0 || n + 1 >= PACK_MAX)
		return false;
	static _Thread_local double cx[MAXC], cy[MAXC], nx[MAXC], ny[MAXC];
	int nc = candidates(r, n, w, h, gx, gy, -1, cx, cy, MAXC);
	if (nc == 0)
		return false;
	pack_rect t[PACK_MAX];
	memcpy(t, r, sizeof(pack_rect) * n);
	double best2 = INFINITY, best1 = INFINITY, best_d = INFINITY;
	for (int c = 0; c < nc; c++) {
		t[n] = (pack_rect){cx[c], cy[c], w, h};
		double p1 = pack_perimeter(t, n + 1, gx, gy);
		// best follow up of the same size
		double p2 = p1;
		int nn = candidates(t, n + 1, w, h, gx, gy, -1, nx, ny, MAXC);
		if (nn > 0) {
			p2 = INFINITY;
			for (int k = 0; k < nn; k++) {
				t[n + 1] = (pack_rect){nx[k], ny[k], w, h};
				double q = pack_perimeter(t, n + 2, gx, gy);
				if (q < p2)
					p2 = q;
			}
		}
		double dx = cx[c] + w / 2 - px, dy = cy[c] + h / 2 - py;
		double d = dx * dx + dy * dy;
		if (p2 < best2 - 1e-6 ||
			(fabs(p2 - best2) <= 1e-6 &&
			 (p1 < best1 - 1e-6 || (fabs(p1 - best1) <= 1e-6 && d < best_d)))) {
			best2 = p2, best1 = p1, best_d = d;
			*ox = cx[c];
			*oy = cy[c];
		}
	}
	return true;
}

static int uf_find(int *p, int i) {
	while (p[i] != i)
		i = p[i] = p[p[i]];
	return i;
}

int pack_clusters(const pack_rect *r, int n, double maxgap, int *label) {
	int parent[PACK_MAX];
	if (n > PACK_MAX)
		n = PACK_MAX;
	for (int i = 0; i < n; i++)
		parent[i] = i;
	for (int i = 0; i < n; i++)
		for (int j = i + 1; j < n; j++) {
			double dx = fmax(0.0, fmax(r[j].x - (r[i].x + r[i].w),
									   r[i].x - (r[j].x + r[j].w)));
			double dy = fmax(0.0, fmax(r[j].y - (r[i].y + r[i].h),
									   r[i].y - (r[j].y + r[j].h)));
			if (dx * dx + dy * dy <= maxgap * maxgap)
				parent[uf_find(parent, i)] = uf_find(parent, j);
		}
	int count = 0, map[PACK_MAX];
	for (int i = 0; i < n; i++)
		map[i] = -1;
	for (int i = 0; i < n; i++) {
		int root = uf_find(parent, i);
		if (map[root] < 0)
			map[root] = count++;
		label[i] = map[root];
	}
	return count;
}

static double box_dist(const pack_rect *a, const pack_rect *b) {
	double dx = fmax(0.0, fmax(b->x - (a->x + a->w), a->x - (b->x + b->w)));
	double dy = fmax(0.0, fmax(b->y - (a->y + a->h), a->y - (b->y + b->h)));
	return sqrt(dx * dx + dy * dy);
}

static bool clears(const pack_rect *moving, int k, const pack_rect *fixed,
				   int f, double sep, double dx, double dy) {
	for (int i = 0; i < k; i++) {
		pack_rect m = {moving[i].x + dx, moving[i].y + dy, moving[i].w,
					   moving[i].h};
		for (int j = 0; j < f; j++)
			if (box_dist(&m, &fixed[j]) < sep - 1e-6)
				return false;
	}
	return true;
}

static int cmp_abs(const void *a, const void *b) {
	double x = fabs(*(const double *)a), y = fabs(*(const double *)b);
	return (x > y) - (x < y);
}

bool pack_avoid(const pack_rect *moving, int k, const pack_rect *fixed, int f,
				double sep, double *dx, double *dy) {
	*dx = *dy = 0.0;
	if (clears(moving, k, fixed, f, sep, 0, 0))
		return true;
	enum { MAXS = 2 * PACK_MAX * PACK_MAX };
	static _Thread_local double xs[MAXS], ys[MAXS];
	int nx = 0, ny = 0;
	for (int i = 0; i < k && i < PACK_MAX; i++)
		for (int j = 0; j < f && j < PACK_MAX; j++) {
			const pack_rect *a = &moving[i], *o = &fixed[j];
			xs[nx++] = (o->x - sep) - (a->x + a->w); // left of it
			xs[nx++] = (o->x + o->w + sep) - a->x;	 // right of it
			ys[ny++] = (o->y - sep) - (a->y + a->h); // above it
			ys[ny++] = (o->y + o->h + sep) - a->y;	 // below it
		}
	qsort(xs, nx, sizeof(double), cmp_abs);
	qsort(ys, ny, sizeof(double), cmp_abs);
	double best = INFINITY;
	// one axis shifts, smallest first
	for (int i = 0; i < nx; i++)
		if (fabs(xs[i]) < best && clears(moving, k, fixed, f, sep, xs[i], 0)) {
			best = fabs(xs[i]), *dx = xs[i], *dy = 0;
			break;
		}
	for (int i = 0; i < ny; i++)
		if (fabs(ys[i]) < best && clears(moving, k, fixed, f, sep, 0, ys[i])) {
			best = fabs(ys[i]), *dx = 0, *dy = ys[i];
			break;
		}
	// diagonal, pairs of the smallest candidates on each axis
	int lx = nx < 16 ? nx : 16, ly = ny < 16 ? ny : 16;
	for (int i = 0; i < lx; i++)
		for (int j = 0; j < ly; j++) {
			double d = hypot(xs[i], ys[j]);
			if (d < best && clears(moving, k, fixed, f, sep, xs[i], ys[j]))
				best = d, *dx = xs[i], *dy = ys[j];
		}
	return isfinite(best);
}
