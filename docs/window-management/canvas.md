---
title: Canvas
description: The infinite canvas, its camera, options and dispatches.
---

## Config file

Dragonfruit looks for its config in this order:

```
~/.config/dragonfruit/config.conf
~/.config/mango/config.conf
/etc/dragonfruit/config.conf
/etc/mango/config.conf
```

## Mango compatibility

**A stock Mango config drops in unchanged.** Every existing key means what it
always did, and the canvas only adds keys of its own. If your Mango config
doesn't work here as it stands, that's a bug, so please report it.

## Canvas options

| Key | Default | What it does |
|---|---|---|
| `canvas_shader` | *(none)* | `grain`, `dots`, `plus`, or a path to your own `.glsl` |
| `canvas_placement` | `center` | Where a new window lands. `center`, or one of the `auto` modes, which grow the window cluster's perimeter as little as possible: `auto-focused` (beside the focused window; `auto` is the same), `auto-recent` (beside the window you were working in, so a burst of new windows gathers around it), `auto-compact` (anywhere on the cluster, looking one window ahead) |
| `canvas_follow_focus` | `0` | Pan the camera to whatever takes focus |
| `canvas_follow_focus_delay` | `0` | Debounce for the above, in ms |
| `canvas_zoom_step` | `112` | Percent per zoom notch |
| `canvas_snap_no_overlap` | `0` | Refuse a snap that would land on top of another window |
| `canvas_resize_snap` | `move` | What releasing a resize near a neighbour does: `move` the window to rest beside it, snap only the dragged `edge`s, or `off`. The highlight box previews it |
| `canvas_precompute` | `1` | Keep the cascade arrangement and the auto-placement spots computed in the background, so `canvas_cascade` and new windows apply them instantly |
| `canvas_cluster_max_distance` | `128` | Largest empty gap (canvas units) that still connects two windows into one cluster. `snap` means only windows resting at the tiling gap |
| `canvas_cluster_min_size` | `3` | Fewest windows that make a cluster (minimum 1). Smaller groups are loose |
| `canvas_cascade_scope` | `cluster` | `canvas_cascade` packs the focused window's cluster (`cluster`) or everything (`all`). It packs everything anyway with one cluster or none, when every window is its own cluster, or when the focused window is loose. A repacked cluster is shifted just enough to stay clear of the others |
| `canvas_cascade_anchor` | `cluster` | Keep the packed cluster where it was (`cluster`) or centre it on screen (`viewport`) |
| `canvas_overview_mode` | `clusters` | `clusters`: each cluster keeps its layout and the space between clusters is compressed. `windows`: every window is compressed on its own |
| `canvas_overview_compression` | `1200` | Canvas distance below which the overview's log compression is ~1:1 (bigger = gentler) |
| `canvas_overview_spacing` | `24` | Minimum space between clusters (or windows) in the overview |
| `canvas_overview_margin` | `12` | Overview fit margin, percent |
| `canvas_select_enabled` | `1` | The move bind on empty canvas draws a multi-select rubber band |
| `canvas_select_mode` | `intersect` | Select windows the band touches (`intersect`) or wholly contains (`contain`) |
| `canvas_select_clear_on_click` | `1` | A plain click (no drag) on empty canvas clears the selection |
| `canvas_select_window_color` | band border, else `urgent_color` | Border colour of selected windows |
| `canvas_select_color` | `drop_color` | Fill of the rubber band |
| `canvas_select_border_width` / `_border_color` / `_border_radius` / `_shadow` / `_shadow_size` / `_shadow_blur` / `_shadow_color` / `_shadow_position_x` / `_shadow_position_y` / `_blur` | as `canvas_drop_*` | Rubber band styling, the same options as the drop box |
| `canvas_drop_border_width` | `0` | Border around the snap preview box (follows zoom) |
| `canvas_drop_border_color` | `focus_color` | Its colour |
| `canvas_drop_border_radius` | `border_radius` | Corner radius of the box (follows zoom) |
| `canvas_drop_shadow` | `0` | Shadow under the box |
| `canvas_drop_shadow_size` / `_blur` / `_color` / `_position_x` / `_position_y` | the `shadows_*` values | Shadow details, same as for windows |
| `canvas_drop_blur` | `0` | Blur what's behind the box (global `blur_params_*`) |

## Snapping

Snapping uses Mango's own `enable_floating_snap` and `snap_distance`, and it
feels like Mango's too. Drag freely, release, and the window glides into place.
Snapped windows rest the tiling gap apart (`gap_inner_horizontal` side by side, `gap_inner_vertical`
stacked) instead of flush, and `canvas_placement=auto` places new windows at that
same gap. While you drag, a box in `drop_color` shows where the window will snap.
A window dropped slightly into a neighbour gets pushed back out to the gap.

## Actions

Bind these like any other dispatch.

| Dispatch | Arguments | |
|---|---|---|
| `canvas_pan` | `<dx>,<dy>` | Pan while held. **The magnitude is the speed**, so `-9,0` is faster than `-5,0`, and 5 is normal. Only the ratio sets the direction and it's normalised, so a diagonal never outruns an axis. From anything but a held key (`dfmsg`, a mouse, axis or gesture bind) it's one push that coasts to rest |
| `canvas_drag` | — | Grab the plane with the pointer, release to fling |
| `canvas_zoom` | `<relative>,<percent>` | `0,50` for an absolute 50%, `1,80` for 80% of current |
| `canvas_zoom_in` / `canvas_zoom_out` | — | One notch, for an `axisbind` on the scroll wheel |
| `canvas_center_window` | — | Bring the focused window to the middle |
| `canvas_reset_camera` | — | Glide back to the canvas origin |
| `canvas_focus_dir` | `<dx>,<dy>` | Focus the nearest window in that direction, flying to it if it's off-screen |
| `canvas_overview` | — | Toggle the compressed overview, then click a window to go to it |
| `canvas_jump` | — | The same overview with a letter on each window. Press a letter or click a window to go to it, `Escape` to leave. `togglejump` does the same on the canvas |
| `canvas_cascade` | — | Tidy everything on the current tag into a grid |

Layout switching is disabled. The canvas is the only layout and every tag starts
on it. `setlayout`, `switch_layout` and a `tag_rule`'s `layout_name` still parse so
a Mango config keeps loading, but they do nothing.
