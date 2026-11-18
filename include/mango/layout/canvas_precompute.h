#pragma once
#include "mango/common/types.h"
#include "mango/layout/canvas_pack.h"
#include <stdbool.h>

// background precompute for cascade and auto placement (canvas_precompute, on
// by default) packing search is O(n^2) per candidate so a worker thread keeps
// the answers ready instead of running on keypress or window map two things it
// keeps best min perimeter arrangement of visible windows, only depends on
// sizes so moving a window doesnt break it best spot for a new window beside
// each visible window, depends on positions too so a drop refreshes it users
// take the cached answer if it matches the current state and compute it right
// there if not same as with the option off

// called at the end of every canvas() arrange
// snapshots the cluster and hands it to the worker if its different from the
// last one
void canvas_precompute_note(Monitor *m);

// fills out[i] (indexed like list, one cluster or everything for scope all)
// with the cached cascade arrangement if its current same windows sizes cluster
// split and scope
bool canvas_precompute_packed(Monitor *m, Client **list, int n, pack_rect *out,
							  bool all);

// cached best spot for a new w x h window beside anchor, ignoring exclude (the
// window being placed) if its current
bool canvas_precompute_spot(Monitor *m, Client *exclude, Client *anchor,
							double w, double h, double *x, double *y);
