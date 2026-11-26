#include "mango/workspace_overview/workspace_overview_internal.h"

static bool wsov_canvas_drag(void);
static void wsov_update_canvas_drop_indicator(struct wsov_cell *cell);

bool wsov_create_drop_indicator(void) {
	if (!wsov.root)
		return false;

	wsov.drop_fill = wlr_scene_rect_create(wsov.root, 1, 1, (float[4]){0});
	if (!wsov.drop_fill)
		return false;
	wsov_set_rect_color(wsov.drop_fill, config.dropcolor);
	wsov_hide_drop_indicator();
	return true;
}

bool wsov_drag_hides_item(struct wsov_cell *cell, Client *c) {
	return wsov.moved && wsov.has_drag_client && c == wsov.drag_client &&
		   cell->tag == wsov.source_tag;
}

bool wsov_drag_hides_group(struct wsov_cell *cell, Client *group_head) {
	if (!wsov.moved || !wsov.has_drag_client || !group_head ||
		cell->tag != wsov.source_tag)
		return false;
	for (Client *cur = group_head; cur; cur = cur->group_next)
		if (cur == wsov.drag_client)
			return true;
	return false;
}

void wsov_destroy_drag_buffer(void) {
	if (wsov.drag_buffer) {
		wlr_scene_node_destroy(&wsov.drag_buffer->node);
		wsov.drag_buffer = NULL;
	}
	wsov.drag_width = 0;
	wsov.drag_height = 0;
	wsov.drag_bar_height = 0;
	wsov.drag_source_width = 0;
	wsov.drag_source_height = 0;
	wsov.drag_preview_x = 0;
	wsov.drag_preview_y = 0;
}

void wsov_begin_drag_floating(Client *c) {
	if (!c || wsov.drag_floating_changed)
		return;

	wsov.drag_was_floating = c->isfloating;
	wsov.drag_was_drag_to_tile = c->drag_to_tile;
	wsov.drag_saved_float_geom = c->float_geom;
	if (c->isfloating)
		return;
	/* The drag itself must not clear fullscreen/maximized. Leave that decision
	 * to the normal arrange path after the window is dropped on its target. */
	if (c->isfullscreen || c->ismaximizescreen)
		return;
	/* Canvas windows keep their canvas size; the drop writes their canvas
	 * position directly (wsov_canvas_drop_pos). */
	if (canvas_active(c->mon))
		return;

	/* Match the normal move-drag path: a tiled window leaves the tiling model
	 * as soon as the drag starts, then returns to it on drop or cancel. */
	c->drag_to_tile = true;
	exit_scroller_stack(c);
	client_set_floating(c, 1, false);
	c->old_stack_inner_per = 0.0f;
	c->old_master_inner_per = 0.0f;
	set_size_per(c->mon, c, false);
	wsov.drag_floating_changed = true;
}

void wsov_restore_drag_floating(void) {
	Client *c = wsov.drag_client;
	if (wsov.drag_floating_changed && c) {
		c->float_geom = wsov.drag_saved_float_geom;
		c->drag_to_tile = wsov.drag_was_drag_to_tile;
		if (c->mon && c->isfloating)
			client_set_floating(c, 0, false);
		else
			c->isfloating = 0;
	}
	wsov.drag_floating_changed = false;
	wsov.drag_was_floating = false;
	wsov.drag_was_drag_to_tile = false;
	wsov.drag_saved_float_geom = (struct wlr_box){0};
}

bool wsov_capture_drag_group_bars(Client *c, struct wsov_cell *bars) {
	if (!c || !bars || !client_is_group_member(c) || !c->group_bar)
		return false;

	Client *head = client_group_head(c);
	Client *anchor = client_group_focused(head);
	if (!anchor)
		anchor = c;
	wsov_draw_group_bar_for_capture(anchor);

	bool captured = false;
	for (Client *cur = head; cur; cur = cur->group_next) {
		if (!cur->group_bar || !cur->group_bar->scene->node.enabled)
			continue;
		wsov_capture_bar(bars, cur->group_bar);
		captured = true;
	}
	return captured;
}

struct wsov_drag_surface_data {
	struct wlr_render_pass *pass;
	pixman_region32_t *clip;
	double ox, oy, scale;
};

static void wsov_drag_render_surface(struct wlr_surface *s, int sx, int sy,
									 void *data) {
	struct wsov_drag_surface_data *d = data;
	struct wlr_texture *texture = wlr_surface_get_texture(s);
	if (!texture || s->current.width <= 0 || s->current.height <= 0)
		return;
	struct wlr_fbox src;
	wlr_surface_get_buffer_source_box(s, &src);
	wlr_render_pass_add_texture(
		d->pass,
		&(struct wlr_render_texture_options){
			.texture = texture,
			.src_box = src,
			.dst_box = {(int)lround(d->ox + sx * d->scale),
						(int)lround(d->oy + sy * d->scale),
						MANGO_MAX(1, (int)lround(s->current.width * d->scale)),
						MANGO_MAX(1, (int)lround(s->current.height * d->scale))},
			.clip = d->clip,
			.transform = s->current.transform,
			.filter_mode = WLR_SCALE_FILTER_BILINEAR,
			.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
		});
}

/* Canvas windows dragged out of a framed preview are often off the real
 * camera, where their scene surfaces are clipped away or hidden, so draw the
 * client's surfaces straight from their buffers. */
static void wsov_drag_render_canvas_content(struct wlr_render_pass *pass,
											Client *c, struct wlr_box win,
											int border, int buffer_w,
											int buffer_h) {
	struct wlr_box g;
	client_get_geometry(c, &g);
	if (g.width <= 0 || g.height <= 0 || win.width <= 2 * border)
		return;
	pixman_region32_t clip;
	pixman_region32_init_rect(&clip, 0, 0, buffer_w, buffer_h);
	double scale = (double)(win.width - 2 * border) / g.width;
	struct wsov_drag_surface_data d = {
		.pass = pass,
		.clip = &clip,
		.ox = win.x + border - g.x * scale,
		.oy = win.y + border - g.y * scale,
		.scale = scale,
	};
	wlr_surface_for_each_surface(client_surface(c), wsov_drag_render_surface,
								 &d);
	pixman_region32_fini(&clip);
}

/* Zoom of the drag buffer relative to the source preview; canvas drags
 * rebuild it at the zoom of the preview under the pointer. */
static double wsov_drag_k = 1.0;
static void wsov_drag_reframe(uint32_t tag);
/* Shown zoom eases from drag_k_from to drag_k_to; the buffer built at
 * wsov_drag_k is scaled by its dest size meanwhile. */
static double drag_k_from = 1.0, drag_k_to = 1.0, drag_k_shown = 1.0;
/* Source preview zoom at drag start: k is relative to it, and the source
 * item box is rescaled from whatever zoom its preview is at right now. */
static double drag_ref_zoom;

static double wsov_drag_box_fix(void) {
	struct wsov_cell *src = wsov_cell_for_tag(wsov.source_tag);
	return src && drag_ref_zoom > 0.0 && src->canvas_zoom > 0.0
			   ? drag_ref_zoom / src->canvas_zoom
			   : 1.0;
}
static bool drag_k_anim;
static struct timespec drag_k_start;
static int drag_built_w, drag_built_h, drag_built_bar;

static bool wsov_build_drag_buffer(struct wsov_item *item, double k) {
	wsov_destroy_drag_buffer();
	if (!item || item->box.width <= 0 || item->box.height <= 0)
		return false;
	wsov_drag_k = k;

	int width = item->client && item->client->isfloating
					? MANGO_MAX(1, item->box.width)
				: item->client && canvas_active(item->client->mon)
					? MANGO_MAX(1, (int)lround(item->box.width * k *
											   wsov_drag_box_fix()))
					: MANGO_MIN(item->box.width, 900);
	int workspace_width =
		MANGO_MAX(1, item->workspace_geom.width > 0 ? item->workspace_geom.width
													: item->box.width);
	int workspace_height = MANGO_MAX(1, item->workspace_geom.height > 0
											? item->workspace_geom.height
											: item->box.height);
	double output_scale = wsov_output_scale();
	double render_scale = (double)width / workspace_width;
	int window_height =
		MANGO_MAX(1, (int)lround(workspace_height * render_scale));
	struct wsov_cell drag_bars = {0};
	int bar_height = 0;
	if (wsov_capture_drag_group_bars(item->client, &drag_bars))
		bar_height =
			MANGO_MAX(1, (int)lround(config.group_bar_height * render_scale));
	int height = bar_height + window_height;
	int buffer_width = MANGO_MAX(1, (int)lround(width * output_scale));
	int buffer_height = MANGO_MAX(1, (int)lround(height * output_scale));

	struct wlr_buffer *buffer = wsov_create_buffer(buffer_width, buffer_height);
	if (!buffer) {
		wsov_clear_cell_bars(&drag_bars);
		free(drag_bars.bar_items);
		return false;
	}
	struct wlr_render_pass *pass =
		wlr_renderer_begin_buffer_pass(server.renderer, buffer, NULL);
	if (!pass) {
		wsov_clear_cell_bars(&drag_bars);
		free(drag_bars.bar_items);
		wlr_buffer_drop(buffer);
		return false;
	}

	float background[4] = {config.rootcolor[0], config.rootcolor[1],
						   config.rootcolor[2], 1.0f};
	wsov_add_rect(pass, (struct wlr_box){0, 0, buffer_width, buffer_height},
				  background, WLR_RENDER_BLEND_MODE_NONE);

	struct wsov_cell drag_cell = {
		.box = {.x = -wsov_border_width(),
				.y = -wsov_border_width(),
				.width = width + 2 * wsov_border_width(),
				.height = window_height + 2 * wsov_border_width()},
		.buffer_w = buffer_width,
		.buffer_h = buffer_height,
	};
	struct wsov_item drag_item = {
		.client = item->client,
		.box = {.x = 0,
				.y = bar_height,
				.width = width,
				.height = window_height},
		.render_scale = render_scale,
		.render_scale_x = render_scale,
		.render_scale_y = render_scale,
	};
	bool canvas_content = item->client && !item->client->isfloating &&
						  canvas_active(item->client->mon);
	if (canvas_content) {
		/* the frame is drawn as wide as the target zoom draws it */
		drag_item.border_width = (int)lround(item->border_width * k);
		drag_item.render_scale_x = drag_item.render_scale_y = render_scale / k;
		drag_item.frame_only = true;
	}
	struct wsov_render_resources resources = {0};
	resources.target = buffer;
	wsov_render_client(pass, &drag_cell, &drag_item, &resources, output_scale);
	if (canvas_content)
		wsov_drag_render_canvas_content(
			pass, item->client,
			(struct wlr_box){0, (int)lround(bar_height * output_scale),
							 buffer_width,
							 (int)lround(window_height * output_scale)},
			(int)lround(drag_item.border_width * render_scale / k *
						output_scale),
			buffer_width, buffer_height);
	if (bar_height > 0) {
		pixman_region32_t clip;
		int scaled_bar_height = (int)lround(bar_height * output_scale);
		pixman_region32_init_rect(&clip, 0, 0, buffer_width, scaled_bar_height);
		double scale_x =
			(double)buffer_width / MANGO_MAX(1, item->client->geom.width);
		double scale_y =
			(double)scaled_bar_height / MANGO_MAX(1, config.group_bar_height);
		int origin_y = item->client->geom.y - config.group_bar_height;
		for (int i = 0; i < drag_bars.bar_item_count; i++) {
			wsov_render_bar_item_at(pass, &drag_bars.bar_items[i], &resources,
									item->client->geom.x, origin_y, scale_x,
									scale_y, &clip);
		}
		pixman_region32_fini(&clip);
	}

	if (!wlr_render_pass_submit(pass)) {
		wsov_release_textures(&resources);
		wsov_clear_cell_bars(&drag_bars);
		free(drag_bars.bar_items);
		wlr_buffer_drop(buffer);
		return false;
	}
	wsov_release_textures(&resources);
	wsov_clear_cell_bars(&drag_bars);
	free(drag_bars.bar_items);

	wsov.drag_buffer = wlr_scene_buffer_create(wsov.root, buffer);
	wlr_buffer_drop(buffer);
	if (!wsov.drag_buffer)
		return false;
	wlr_scene_buffer_set_dest_size(wsov.drag_buffer, width, height);
	wlr_scene_buffer_set_filter_mode(wsov.drag_buffer,
									 WLR_SCALE_FILTER_BILINEAR);
	wlr_scene_node_raise_to_top(&wsov.drag_buffer->node);
	wsov.drag_bar_height = drag_built_bar = bar_height;
	wsov.drag_width = drag_built_w = width;
	wsov.drag_height = drag_built_h = height;
	wsov.drag_source_width = item->box.width;
	wsov.drag_source_height = item->box.height;
	return true;
}

bool wsov_create_drag_buffer(struct wsov_item *item) {
	drag_k_from = drag_k_to = drag_k_shown = 1.0;
	drag_k_anim = false;
	struct wsov_cell *src = wsov_cell_for_tag(wsov.source_tag);
	drag_ref_zoom = src ? src->canvas_zoom : 0.0;
	return wsov_build_drag_buffer(item, 1.0);
}

void wsov_cancel_drag(void) {
	bool had_drag =
		wsov.pressed || wsov.moved || wsov.has_drag_client || wsov.drag_buffer;
	uint32_t return_tag = wsov.drag_return_tag;
	if (wsov.mon)
		wsov_drag_reframe(0);
	wsov_hide_drop_indicator();
	wsov_restore_drag_floating();
	wsov_destroy_drag_buffer();
	wsov.pressed = false;
	wsov.moved = false;
	wsov.has_drag_client = false;
	wsov.drag_scroll_direction = 0;
	wsov.drag_scroll_frame_valid = false;
	wsov.drag_client = NULL;
	wsov.source_tag = 0;
	wsov.drag_return_tag = 0;
	if (had_drag && return_tag) {
		wsov.target_tag = return_tag;
		wsov.camera_tag = return_tag;
	}
	if (wsov.mon && !server.cursor_hidden)
		wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, "default");
}

bool wsov_item_is_drop_target(const struct wsov_item *item) {
	return item && !item->is_snapshot && item->client &&
		   item->client != wsov.drag_client && ISTILED(item->client);
}

int32_t wsov_cell_tiling_count(const struct wsov_cell *cell) {
	int32_t count = 0;
	if (!cell)
		return 0;
	for (int i = 0; i < cell->item_count; i++) {
		const struct wsov_item *item = &cell->items[i];
		if (!item->is_snapshot && item->client != wsov.drag_client &&
			ISTILED(item->client))
			count++;
	}
	return count;
}

struct wsov_item *wsov_drop_item_at(struct wsov_cell *cell, double x,
									double y) {
	struct wsov_item *hit = wsov_item_at(cell, x, y);
	if (wsov_item_is_drop_target(hit))
		return hit;
	if (!cell || !wsov.mon)
		return NULL;

	struct wsov_item *nearest = NULL;
	double best = 1e30;
	double lx = x - wsov.mon->m.x;
	double ly = y - wsov.mon->m.y;
	for (int i = 0; i < cell->item_count; i++) {
		struct wsov_item *item = &cell->items[i];
		if (!wsov_item_is_drop_target(item))
			continue;
		struct wlr_box box = wsov_item_display_box(cell, item);
		double cx = box.x + box.width / 2.0;
		double cy = box.y + box.height / 2.0;
		double distance = (cx - lx) * (cx - lx) + (cy - ly) * (cy - ly);
		if (distance < best) {
			best = distance;
			nearest = item;
		}
	}
	return nearest;
}

bool wsov_get_drop_target(double x, double y, struct wsov_cell **cell_out,
						  struct wsov_item **item_out, struct wlr_box *zone_out,
						  int32_t *direction_out) {
	struct wsov_cell *cell = wsov_drop_cell_at(x, y);
	if (!cell || !wsov.mon || !wsov.drag_client || wsov.drag_was_floating) {
		return false;
	}

	struct wsov_item *item = wsov_drop_item_at(cell, x, y);
	if (!item)
		return false;

	const Layout *layout = wsov.mon->pertag->ltidxs[cell->tag];
	struct wlr_box item_box = wsov_item_final_display_box(cell, item);
	if (item_box.width <= 0 || item_box.height <= 0)
		return false;
	double rel_x = x - wsov.mon->m.x - item_box.x;
	double rel_y = y - wsov.mon->m.y - item_box.y;
	struct wlr_box zone;
	int32_t direction = client_drop_zone(
		layout, item->client->ismaster, wsov_cell_tiling_count(cell), rel_x,
		rel_y, item_box.width, item_box.height, &zone);
	zone.x += item_box.x;
	zone.y += item_box.y;
	item->client->drop_direction = direction;

	if (cell_out)
		*cell_out = cell;
	if (item_out)
		*item_out = item;
	if (zone_out)
		*zone_out = zone;
	if (direction_out)
		*direction_out = direction;
	return true;
}

void wsov_hide_drop_indicator(void) {
	wsov.drop_indicator_visible = false;
	if (wsov.drop_fill)
		wlr_scene_node_set_enabled(&wsov.drop_fill->node, false);
	canvas_drop_preview_hide();
}

/* Screen pixels per canvas unit in a cell's preview. */
static double wsov_cell_canvas_scale(struct wsov_cell *cell) {
	struct wlr_box cell_box = wsov_cell_layout_box(cell);
	return cell->canvas_zoom *
		   MANGO_MAX(1, cell_box.width - 2 * wsov_border_width()) /
		   MANGO_MAX(1, wsov.mon->m.width);
}

void wsov_update_drop_indicator(double x, double y) {
	struct wsov_cell *cell = NULL;
	struct wlr_box zone;
	if (wsov_canvas_drag()) {
		cell = wsov_drop_cell_at(x, y);
		if (!cell) {
			wsov_drag_reframe(0);
			wsov_hide_drop_indicator();
			return;
		}
		if (cell->tag != wsov.target_tag) {
			wsov.target_tag = cell->tag;
			wsov.camera_tag = cell->tag;
			wsov_refresh_target_selection();
		}
		wsov_update_canvas_drop_indicator(cell);
		return;
	}
	if (!wsov_get_drop_target(x, y, &cell, NULL, &zone, NULL) ||
		zone.width <= 0 || zone.height <= 0) {
		wsov_hide_drop_indicator();
		return;
	}

	if (cell->tag != wsov.target_tag) {
		wsov.target_tag = cell->tag;
		wsov.camera_tag = cell->tag;
		wsov_refresh_target_selection();
	}

	int32_t width = MANGO_MAX(1, zone.width);
	int32_t height = MANGO_MAX(1, zone.height);
	wsov_place_rect_clipped(wsov.drop_fill, zone.x, zone.y, width, height,
							true);
	if (!wsov.drop_fill)
		return;

	wsov.drop_indicator_visible = true;

	wlr_scene_node_raise_to_top(&wsov.drop_fill->node);
	if (wsov.drag_buffer)
		wlr_scene_node_raise_to_top(&wsov.drag_buffer->node);
}

/* Top-left of the drag preview in monitor-local layout units of cell, as if
 * the cell were the whole monitor. */
static void wsov_drag_local_origin(struct wsov_cell *cell, double *lx,
								   double *ly) {
	struct wlr_box cell_box = wsov_cell_layout_box(cell);
	double scale_x = (double)wsov.mon->m.width /
					 MANGO_MAX(1, cell_box.width - 2 * wsov_border_width());
	double scale_y = (double)wsov.mon->m.height /
					 MANGO_MAX(1, cell_box.height - 2 * wsov_border_width());
	struct wsov_cell *camera_cell = wsov_cell_for_tag(wsov.camera_tag);
	if (!camera_cell)
		camera_cell = cell;
	struct wlr_box camera_box = wsov_cell_layout_box(camera_cell);
	double progress = fmax(0.0, fmin(1.0, wsov.progress));
	double final_viewport_x =
		wsov_scrolls_horizontally() ? wsov.scroll_offset : 0.0;
	double final_viewport_y =
		wsov_scrolls_vertically() ? wsov.scroll_offset : 0.0;
	double viewport_x =
		camera_box.x + wsov_border_width() +
		(final_viewport_x - camera_box.x - wsov_border_width()) * progress;
	double viewport_y =
		camera_box.y + wsov_border_width() +
		(final_viewport_y - camera_box.y - wsov_border_width()) * progress;
	double viewport_w =
		MANGO_MAX(1, camera_box.width - 2 * wsov_border_width());
	double viewport_h =
		MANGO_MAX(1, camera_box.height - 2 * wsov_border_width());
	viewport_w += (wsov.mon->m.width - viewport_w) * progress;
	viewport_h += (wsov.mon->m.height - viewport_h) * progress;
	double camera_scale_x =
		(double)wsov.mon->m.width / MANGO_MAX(1.0, viewport_w);
	double camera_scale_y =
		(double)wsov.mon->m.height / MANGO_MAX(1.0, viewport_h);
	double preview_strip_x =
		viewport_x + (double)wsov.drag_preview_x / camera_scale_x;
	double preview_strip_y =
		viewport_y +
		(double)(wsov.drag_preview_y + wsov.drag_bar_height) / camera_scale_y;
	double content_x = preview_strip_x - cell_box.x - wsov_border_width();
	double content_y = preview_strip_y - cell_box.y - wsov_border_width();
	*lx = content_x * scale_x;
	*ly = content_y * scale_y;
}

static bool wsov_canvas_drag(void) {
	return wsov.drag_client && wsov.mon && canvas_active(wsov.mon) &&
		   !wsov.drag_was_floating;
}

/* Canvas windows keep their canvas size, so over another tag's preview the
 * drag shows them at that preview's zoom. */
void wsov_drag_rescale(double x, double y) {
	if (!wsov_canvas_drag() || !wsov.drag_buffer)
		return;
	struct wsov_cell *src = wsov_cell_for_tag(wsov.source_tag);
	struct wsov_cell *dst = wsov_drop_cell_at(x, y);
	if (!src || !dst || src->canvas_zoom <= 0.0 || dst->canvas_zoom <= 0.0)
		return;
	struct wsov_item *item = NULL;
	for (int i = 0; i < src->item_count; i++)
		if (src->items[i].client == wsov.drag_client)
			item = &src->items[i];
	if (!item)
		return;
	// aim for where a reframe glide ends so it doesnt restart every frame
	double dz = dst->log_anim ? dst->log_to_zoom : dst->canvas_zoom;
	double k = dz / (drag_ref_zoom > 0.0 ? drag_ref_zoom : src->canvas_zoom);
	if (fabs(k - drag_k_to) >= 0.01) {
		drag_k_from = drag_k_shown;
		drag_k_to = k;
		drag_k_anim = config.animations;
		clock_gettime(CLOCK_MONOTONIC, &drag_k_start);
		if (drag_k_anim)
			wsov_attach_frame_listener();
		// build at the larger end so the ease only scales down
		double build = fmax(drag_k_from, drag_k_to);
		if (drag_k_anim && fabs(build - wsov_drag_k) >= 0.01 &&
			!wsov_build_drag_buffer(item, build))
			return;
	}
	if (drag_k_anim) {
		double t = wsov_elapsed_ms(&drag_k_start) /
				   MANGO_MAX(1.0, (double)animation_curve_duration_ms(
									  &config.animation_curve_wsov_move,
									  config.animation_duration_wsov_move));
		if (t >= 1.0) {
			drag_k_anim = false;
		} else {
			double e = animation_curve_ease(&config.animation_curve_wsov_move,
											fmax(0.0, t));
			drag_k_shown = drag_k_from + (drag_k_to - drag_k_from) * e;
		}
	}
	if (!drag_k_anim) {
		drag_k_shown = drag_k_to;
		// settled, redraw sharp at the final zoom
		if (fabs(drag_k_to - wsov_drag_k) >= 0.001)
			wsov_build_drag_buffer(item, drag_k_to);
	}
	double f = drag_k_shown / wsov_drag_k;
	wsov.drag_width = MANGO_MAX(1, (int)lround(drag_built_w * f));
	wsov.drag_height = MANGO_MAX(1, (int)lround(drag_built_h * f));
	wsov.drag_bar_height = (int)lround(drag_built_bar * f);
}

bool wsov_drag_scale_animating(void) {
	return wsov.drag_buffer && drag_k_anim;
}

/* Canvas position the drag preview would land at in cell, before snapping.
 * Previews are log-compressed overview layouts, so map through the nearest
 * window's real canvas position rather than a single cell origin. */
static bool wsov_canvas_drop_pos(struct wsov_cell *cell, double *cx,
								 double *cy) {
	Client *c = wsov.drag_client;
	double z = cell ? cell->canvas_zoom : 0.0;
	if (!c || z <= 0.0)
		return false;
	double lx, ly;
	wsov_drag_local_origin(cell, &lx, &ly);
	double mx = lx + c->canvas_geom.width * z / 2.0;
	double my = ly + c->canvas_geom.height * z / 2.0;
	struct wsov_item *anchor = NULL;
	double best = 1e30;
	for (int i = 0; i < cell->item_count; i++) {
		struct wsov_item *item = &cell->items[i];
		Client *o = item->client;
		if (item->is_snapshot || !o || o == c || !ISFAKETILED(o) ||
			!o->canvas_placed)
			continue;
		struct wlr_box b = item->workspace_geom;
		double dx = b.x - wsov.mon->m.x + b.width / 2.0 - mx;
		double dy = b.y - wsov.mon->m.y + b.height / 2.0 - my;
		if (dx * dx + dy * dy < best) {
			best = dx * dx + dy * dy;
			anchor = item;
		}
	}
	if (anchor) {
		struct wlr_box b = anchor->workspace_geom;
		*cx = anchor->client->canvas_geom.x + (lx - (b.x - wsov.mon->m.x)) / z;
		*cy = anchor->client->canvas_geom.y + (ly - (b.y - wsov.mon->m.y)) / z;
	} else {
		*cx = cell->canvas_cam_x + lx / z;
		*cy = cell->canvas_cam_y + ly / z;
	}
	return true;
}

/* Tag whose preview is zoomed out for the dragged window, and the edge it
 * made room at (0 none, 1 left, 2 right, 3 top, 4 bottom). */
static uint32_t reframed_tag;
static int reframed_edge;

/* Edge of the hovered preview the dragged window's centre is closest to.
 * Snapped onto the window furthest out that way, would the dragged window
 * leave the captured framing? Then zoom out with the opposite edge pinned
 * until it fits with canvas_overview_margin around it, else stay on the captured framing. Windows keep their spots,
 * only the camera moves, and only when the closest edge changes. tag 0 puts
 * the zoomed preview back. */
static void wsov_drag_reframe(uint32_t tag) {
	if (reframed_tag && reframed_tag != tag) {
		wsov_canvas_reframe_cell(wsov_cell_for_tag(reframed_tag), false, 0.0,
								 0.0, 0.0);
		reframed_tag = 0;
	}
	struct wsov_cell *cell = tag ? wsov_cell_for_tag(tag) : NULL;
	Client *c = wsov.drag_client;
	if (!cell || cell->canvas_zoom <= 0.0 || cell->log_base_zoom <= 0.0 || !c)
		return;
	Monitor *m = wsov.mon;
	double W = m->m.width, H = m->m.height;
	double cw = c->canvas_geom.width, ch = c->canvas_geom.height;
	// window centre's distance to each edge, in preview px as shown
	double lx, ly, zs = cell->canvas_zoom;
	wsov_drag_local_origin(cell, &lx, &ly);
	double px = lx + cw * zs / 2.0, py = ly + ch * zs / 2.0;
	double dist[5] = {1e18, px, W - px, py, H - py};
	int edge = 1;
	for (int e = 2; e <= 4; e++)
		if (dist[e] < dist[edge])
			edge = e;
	if (reframed_tag == tag && reframed_edge == edge)
		return;

	// captured framing as camera top left, and the view centre offset
	double bz = cell->log_base_zoom, bx, by, ox, oy;
	canvas_cam_at(m, cell->log_base_cx, cell->log_base_cy, bz, &bx, &by);
	canvas_cam_at(m, 0.0, 0.0, 1.0, &ox, &oy); // -offset at zoom 1
	double gx = server.enable_gaps ? m->gappih : 0.0;
	double gy = server.enable_gaps ? m->gappiv : 0.0;
	// windows' extent on the axis of edge, lo..hi
	bool horiz = edge == 1 || edge == 2;
	double lo = 1e18, hi = -1e18;
	bool any = false;
	for (int i = 0; i < cell->item_count; i++) {
		struct wsov_item *it = &cell->items[i];
		Client *o = it->client;
		if (it->is_snapshot || !o || o == c || !ISFAKETILED(o))
			continue;
		any = true;
		double a = horiz ? it->ov_base_x : it->ov_base_y;
		lo = fmin(lo, a);
		hi = fmax(hi, a + (horiz ? o->canvas_geom.width
								 : o->canvas_geom.height));
	}
	// zoom that fits the window snapped past the outermost one with the far
	// edge pinned, plus half canvas_overview_margin of the windows' span like
	// the overview fit leaves on each side
	double z = bz, x0 = bx, y0 = by;
	if (any) {
		double pad = config.canvas_overview_margin / 200.0;
		if (edge == 1) {
			double need = lo - 2 * gx - cw;
			z = W / (bx + W / bz - need + (hi - need) * pad);
		} else if (edge == 2) {
			double need = hi + 2 * gx + cw;
			z = W / (need + (need - lo) * pad - bx);
		} else if (edge == 3) {
			double need = lo - 2 * gy - ch;
			z = H / (by + H / bz - need + (hi - need) * pad);
		} else {
			double need = hi + 2 * gy + ch;
			z = H / (need + (need - lo) * pad - by);
		}
		z = fmax(fmin(z, bz), 0.02);
	}
	if (z < bz * 0.999) {
		// pinned edge stays, the other axis zooms around the centre
		if (edge == 1)
			x0 = bx + W / bz - W / z;
		if (edge == 1 || edge == 2)
			y0 = by + H / (2 * bz) - H / (2 * z);
		if (edge == 3)
			y0 = by + H / bz - H / z;
		if (edge == 3 || edge == 4)
			x0 = bx + W / (2 * bz) - W / (2 * z);
	}
	reframed_tag = tag;
	reframed_edge = edge;
	wsov_canvas_reframe_cell(cell, true, x0 - ox / z, y0 - oy / z, z);
}

/* Canvas snap preview: shown only where the canvas snap would move it. */
static void wsov_update_canvas_drop_indicator(struct wsov_cell *cell) {
	Client *c = wsov.drag_client;
	double cx, cy;
	if (!wsov_canvas_drop_pos(cell, &cx, &cy)) {
		wsov_hide_drop_indicator();
		return;
	}
	double sx = cx, sy = cy;
	double z = wsov_cell_canvas_scale(cell);
	bool snapped = canvas_snap_on_tags(c, wsov_tag_bit(cell->tag), z, &sx, &sy);
	wsov_drag_reframe(cell->tag);
	if (!snapped) {
		wsov_hide_drop_indicator();
		return;
	}
	// the canvas drop box, as on the canvas, at the preview's scale
	if (wsov.drop_fill)
		wlr_scene_node_set_enabled(&wsov.drop_fill->node, false);
	struct wlr_box b = {
		.x = wsov.mon->m.x + wsov.drag_preview_x +
			 (int32_t)lround((sx - cx) * z),
		.y = wsov.mon->m.y + wsov.drag_preview_y + wsov.drag_bar_height +
			 (int32_t)lround((sy - cy) * z),
		.width = MANGO_MAX(1, (int32_t)lround(c->canvas_geom.width * z)),
		.height = MANGO_MAX(1, (int32_t)lround(c->canvas_geom.height * z)),
	};
	canvas_wsov_drop_show(server.layers[LyrSpecialOverlay], b, z);
	wsov.drop_indicator_visible = true;
}

void wsov_place_floating(Client *c, struct wsov_cell *cell) {
	if (!c || !cell || !wsov.mon)
		return;
	int width = MANGO_MAX(64, c->float_geom.width > 0 ? c->float_geom.width
													  : c->geom.width);
	int height = MANGO_MAX(64, c->float_geom.height > 0 ? c->float_geom.height
														: c->geom.height);
	double lx, ly;
	wsov_drag_local_origin(cell, &lx, &ly);
	int nx = wsov.mon->m.x + (int)lround(lx);
	int ny = wsov.mon->m.y + (int)lround(ly);
	c->float_geom = (struct wlr_box){nx, ny, width, height};
	resize(c, c->float_geom,
		   (ResizeOpts){.interact = 0, .skip_animation = true});
}

void wsov_finish_drop(double x, double y) {
	Client *c = wsov.drag_client;
	struct wsov_cell *target = wsov_drop_cell_at(x, y);
	if (!c || !target || !wsov_client_eligible(c)) {
		uint32_t tags = wsov_client_tags(c) | wsov_tag_bit(wsov.source_tag);
		wsov_cancel_drag();
		wsov_refresh_workspace_or_rebuild(tags);
		return;
	}

	struct wsov_item *over = NULL;
	wsov_get_drop_target(x, y, &target, &over, NULL, NULL);
	bool was_floating = wsov.drag_was_floating;
	double canvas_x = 0.0, canvas_y = 0.0;
	bool canvas_drop = wsov_canvas_drag() &&
					   wsov_canvas_drop_pos(target, &canvas_x, &canvas_y);
	if (canvas_drop)
		canvas_snap_on_tags(c, wsov_tag_bit(target->tag),
							wsov_cell_canvas_scale(target), &canvas_x,
							&canvas_y);

	uint32_t source_bit = wsov_tag_bit(wsov.source_tag);
	uint32_t target_bit = wsov_tag_bit(target->tag);
	uint32_t old_tags = c->isglobal ? 0 : c->tags;
	WorkspaceOverviewDropMode mode = wsov.drop_mode;
	uint32_t mods = keyboard_hard_modifiers();

	if (mode == WORKSPACE_OVERVIEW_DROP_AUTO)
		mode = (mods & WLR_MODIFIER_CTRL)	 ? WORKSPACE_OVERVIEW_DROP_MOVE
			   : (mods & WLR_MODIFIER_SHIFT) ? WORKSPACE_OVERVIEW_DROP_ADD
											 : WORKSPACE_OVERVIEW_DROP_REPLACE;

	if (!c->isglobal && target->tag != wsov.source_tag) {
		if (mode == WORKSPACE_OVERVIEW_DROP_MOVE)
			c->tags = target_bit;
		else if (mode == WORKSPACE_OVERVIEW_DROP_ADD)
			c->tags |= target_bit;
		else
			c->tags = (c->tags & ~source_bit) | target_bit;
		c->tags &= TAGMASK;
		if (!c->tags)
			c->tags = target_bit;
		if (old_tags != c->tags)
			client_reparent_group(c);
	}
	clear_fullscreen_for_incoming_tiled_client(c);

	client_view_on_monitor(&(Arg){.ui = target_bit}, false, wsov.mon, false);
	if (canvas_drop) {
		// the recapture glides the window in from where it was let go, in
		// overview space of the preview as shown now
		double lx, ly, raw_x, raw_y;
		wsov_drag_local_origin(target, &lx, &ly);
		if (target->canvas_zoom > 0.0 &&
			wsov_canvas_drop_pos(target, &raw_x, &raw_y)) {
			c->wsov_ov_x = target->canvas_cam_x + lx / target->canvas_zoom +
						   (canvas_x - raw_x);
			c->wsov_ov_y = target->canvas_cam_y + ly / target->canvas_zoom +
						   (canvas_y - raw_y);
			c->wsov_ov_shown = true;
		}
		reframed_tag = 0; // the recapture frames it for real
		c->canvas_geom.x = canvas_x;
		c->canvas_geom.y = canvas_y;
		c->canvas_placed = true;
	} else if (over) {
		pointer_place_drag_tile_on(c, over->client);
	}
	if (was_floating)
		wsov_place_floating(c, target);

	wsov_restore_drag_floating();
	client_focus(c, 1);
	arrange(wsov.mon, false, false);

	wsov_hide_drop_indicator();
	wsov.pressed = false;
	wsov.moved = false;
	wsov.has_drag_client = false;
	wsov.drag_scroll_direction = 0;
	wsov.drag_scroll_frame_valid = false;
	wsov.drag_client = NULL;
	wsov.source_tag = 0;
	wsov.target_tag = target->tag;
	wsov.scroll_center_tag = target->tag;
	wsov.camera_tag = target->tag;
	wsov_animate_scroll_to_target();
	wsov.drag_return_tag = 0;
	wsov_destroy_drag_buffer();
	if (!server.cursor_hidden)
		wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, "default");
	uint32_t affected_tags = source_bit | target_bit;
	if (c->isglobal)
		affected_tags |= TAGMASK;
	else
		affected_tags |= old_tags | c->tags;
	wsov_refresh_workspace_or_rebuild(affected_tags);
}
