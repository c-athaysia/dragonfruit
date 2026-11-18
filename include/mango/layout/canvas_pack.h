#pragma once
#include <stdbool.h>

// rectangle packing for the canvas, canvas units
// plain geometry no compositor state
// alone perimeter here means outer perimeter of the cluster each rect grown
// half a gap per side so neighbours touch, unioned, holes filled gaps and
// pockets inside the cluster dont count
typedef struct {
	double x, y, w, h;
} pack_rect;

double pack_perimeter(const pack_rect *r, int n, double gx, double gy);

// rearranges r[0..n) into a gap separated cluster with the smallest perimeter,
// sizes stay and x/y get written exact over its candidates for small n, beam
// search after that cluster top left ends up near 0,0 and the caller moves it
void pack_min_perimeter(pack_rect *r, int n, double gx, double gy);

// best spot for a new w x h rect on the border of r[anchor]
// touches it at the gap, overlaps nothing in r[0..n), smallest perimeter
// increase false if theres no spot
bool pack_place_adjacent(const pack_rect *r, int n, int anchor, double w,
						 double h, double gx, double gy, double *ox,
						 double *oy);

// best spot for a new w x h rect touching ANY rect in the cluster, smallest
// perimeter increase tie goes to the spot whose centre is nearest px,py false
// if the cluster is empty or has no free border
bool pack_place_cluster(const pack_rect *r, int n, double w, double h,
						double gx, double gy, double px, double py, double *ox,
						double *oy);

// like pack_place_cluster but looks one window ahead
// each spot is scored by the perimeter after this rect and the best next one of
// the same size new canvas windows all start the same size so the next one is
// this size plain greedy keeps stacking wide windows into a column and the
// lookahead sees an L becomes a 2x2 ties go to this steps perimeter then
// nearness to px,py
bool pack_place_compact(const pack_rect *r, int n, double w, double h,
						double gx, double gy, double px, double py, double *ox,
						double *oy);

// independent clusters
// two rects are connected if the empty space between their boxes is at most
// maxgap, 0 on an axis where they overlap clusters are the connected groups
// writes a cluster label per rect (0..count-1 in order of first appearance) and
// returns count
int pack_clusters(const pack_rect *r, int n, double maxgap, int *label);

// collision avoidance, smallest shift dx,dy of the k moving rects so none is
// closer than sep to any of the f fixed rects tries shifts that put one moving
// rect sep from one fixed rect on either axis plus pairs of the smallest ones
// for diagonals false and 0,0 if nothing clears
bool pack_avoid(const pack_rect *moving, int k, const pack_rect *fixed, int f,
				double sep, double *dx, double *dy);
