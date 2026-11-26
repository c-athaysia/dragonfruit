---
title: Overview
description: The canvas overview, the Mango overview names, hot area, and the window switcher.
---

Dragonfruit has one overview: the canvas overview. It shows the whole canvas,
with distances compressed logarithmically, so every window is on the screen.
See [Canvas](canvas.md) for how it looks and moves.

| Mode | Command |
| :--- | :--- |
| Canvas overview | `canvas_overview` |
| Canvas overview with jump hints | `canvas_jump` |
| Workspace overview | `toggleworkspaceoverview` |
| Switcher | `switcher` |

There is also a separate workspace overview. It renders each tag to an offscreen
workspace texture and lets windows be dragged between the visible workspace
previews.

The keybindings below are examples; replace the shortcuts with ones that are
free in your configuration. For the complete dispatcher reference, see
[Keybindings](../bindings/keys.md).

## Mango overview commands

Mango's grid overview, jump mode and overcircle are removed. Their commands
still work, so a Mango configuration needs no change:

| Command | Does |
| :--- | :--- |
| `toggleoverview` | Same as `canvas_overview`. The argument is ignored. |
| `enteroverview` | Open the canvas overview. Does nothing when it is open. |
| `leaveoverview` | Close the canvas overview. Does nothing when it is closed. |
| `togglejump` | Same as `canvas_jump`. |
| `overcircle` | Open the canvas overview. When it is open, focus the next (`next`, `current_next`) or previous (`prev`, `current_prev`) window. |

The default configuration has four-finger swipe gestures for the overview:

```ini
gesturebind=none,up,4,enteroverview
gesturebind=none,down,4,leaveoverview
```

`overview_gap_inner`, `overview_gap_outer` and `overcircle_center_ratio` are
still accepted, but they have no effect.

### Jump hints

`canvas_jump` puts a one-key hint on each visible window. Press a hint to focus
that window and close the overview. Press `Escape` to close it without a
change. The hint characters come from `jump_labels`:

```ini
jump_labels=asdfghjkl
```

### Hot area

When the pointer goes into the configured corner, the canvas overview opens or
closes.

| Setting | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `enable_hotarea` | integer | `0` | Toggle the canvas overview when the pointer enters the configured corner (`0`: disabled, `1`: enabled). |
| `hotarea_size` | integer | `10` | Width and height of the hot-area trigger in pixels. Range: 1–1000. |
| `hotarea_corner` | integer | `2` | Trigger corner: `0` top-left, `1` top-right, `2` bottom-left, `3` bottom-right. |
| `hotarea_disable_on_fullscreen` | integer | `1` | Do not trigger the hot area while a fullscreen window is focused (`0`: disabled, `1`: enabled). |

```ini
enable_hotarea=1
hotarea_size=10
hotarea_corner=2
hotarea_disable_on_fullscreen=1
```

## Workspace Overview

The workspace overview is independent from the canvas overview. Its scope is
selected when opening it: `all` creates one preview for every tag up to
`tag_num`, including empty workspaces, while `occupied` shows only tags that
contain windows. In `occupied` mode, tags seen at open or selected later with a
`view_*` dispatch remain as previews even after their workspaces become empty.
The default `all` and `occupied` modes use a fitted preview grid. The
`scroll_all` and `scroll_occupied` modes arrange workspaces as a scrolling strip,
similar to the scroller layout with workspaces as its items.
The strip can scroll horizontally or vertically according to
`workspace_overview_scroll_direction`; the selected workspace is centered when
possible. Scrolling and workspace-selection movement use
`animation_duration_workspace_overview_move` and
`animation_curve_workspace_overview_move`. The same move animation is used for
workspace layout changes; there is no separate layout animation.

Wheel scrolling uses the
[workspace overview scroll deceleration options](../configuration/input.md#workspace-overview-scroll-deceleration).

Each tag preview mirrors its workspace, including the background wallpaper and
layer-shell surfaces on the `background` and `bottom` layers, such as status
panels. Layer-shell surfaces on the `top` and `overlay` layers stay at their
normal on-screen position, render directly above the workspace overview, and
continue to receive pointer input. Windows use their workspace geometry, so
partially visible windows in scrolling layouts remain partially visible in the
preview. A window that belongs to multiple tags appears in every matching
preview. Window and layer-surface commits refresh the affected previews, and
mapping or closing a window rebuilds the workspace previews automatically.

```ini
# All workspaces
bind=SUPER,Tab,toggleworkspaceoverview,all
# Optional binding: occupied workspaces only
bind=SUPER+SHIFT,Tab,toggleworkspaceoverview,occupied
# Optional binding: all workspaces in a scrolling strip
bind=SUPER+ALT,Tab,toggleworkspaceoverview,scroll_all
bind=SUPER+ALT,Escape,leaveworkspaceoverview,
```

| Command | Description |
| :--- | :--- |
| `toggleworkspaceoverview` | Toggle the workspace overview. `all` includes empty tags, `occupied` shows occupied tags only, and `scroll_all` / `scroll_occupied` use the corresponding scrolling layout. |
| `enterworkspaceoverview` | Enter the workspace overview with `all`, `occupied`, `scroll_all`, or `scroll_occupied`. |
| `leaveworkspaceoverview` | Leave the workspace overview without selecting a workspace. |

`toggleworkspaceoverview` and `enterworkspaceoverview` can reverse an in-progress
exit directly from its current zoom position. Toggling during the enter
animation similarly reverses back toward the selected workspace.

Pointer interaction:

| Action | Result |
| :--- | :--- |
| Left click a workspace | Switch to that workspace and leave the overview. |
| Left click a window preview | Switch to its workspace, focus the window, and leave the overview. |
| Drag a window within a workspace | Change its tiling order or floating position. |
| Drag a window to another workspace | Move its tag membership to the drop workspace. |
| Right click a window preview | Close the window. |
| Mouse wheel or horizontal scroll | Move to the previous or next workspace and center it in the scrolling strip. |

When a dragged window is over a tiled target, the preview shows the same
mouse-position-based insert zone used by ordinary drag-to-tile. Dropping uses
that zone's layout-specific insertion rule. The borderless indicator uses the
same `drop_color` style as normal layouts.

While dragging a window in scrolling mode, entering the edge in the configured
scroll direction starts a slow automatic scroll. Horizontal mode uses the left
and right edges; vertical mode uses the top and bottom edges.

When a window drag starts, the source workspace immediately lays out again
without that window, so the remaining windows fill the released space before
the drop is committed.

Keyboard interaction:

| Key | Result |
| :--- | :--- |
| Arrow keys or `h`/`j`/`k`/`l` | Select the adjacent workspace preview. |
| Enter | Switch to the selected workspace and leave the overview. |
| Escape | Leave without changing the active workspace. |

While dragging, holding Shift adds the destination tag without removing the
source tag. Holding Ctrl replaces all of the window's tags with the destination
tag. A plain drag removes only the tag represented by the preview that the drag
started from.

### Workspace Overview Bindings

You can change the input of the workspace overview with binds in
`key_mode=workspace_overview`. These binds run before all other binds while
the workspace overview is open. They can be `bind`, `mousebind`, `axisbind` or
`gesturebind` lines.

A bind in this key_mode replaces the built-in input of the same type. One
`bind` turns off the built-in keys. One `mousebind` turns off the built-in
buttons. One `axisbind` turns off the built-in wheel. The built-in input of the
other types stays on. Input that no bind uses does nothing in the workspace
overview.

| Command | Description |
| :--- | :--- |
| `workspace_overview_close` | Switch to the selected workspace and leave the overview. |
| `workspace_overview_select,<left\|right\|up\|down>` | Select the adjacent workspace preview. |
| `workspace_overview_kill` | Close the window whose preview is under the pointer. |
| `workspace_overview_drag[,replace\|move\|add]` | Start a press at the pointer. A release without motion switches to the workspace under the pointer. A drag moves the window preview under the pointer. |

`workspace_overview_drag` sets what a drop to another workspace does to the tags
of the window:

- `replace`: remove the source tag and add the destination tag.
- `move`: set the destination tag as the only tag.
- `add`: add the destination tag and keep all other tags.
- No argument: Ctrl at the drop does `move`, Shift does `add`, and other drops
  do `replace`.

The release of the button that started the press ends it. If a key, a wheel
or a gesture started the press, the next button release ends it.

These binds do the same as the built-in input. The built-in buttons also
accept modifiers, but a `mousebind` needs the exact modifiers. In the scrolling
layout, the built-in wheel scrolls the strip smoothly.

```ini
key_mode=workspace_overview
bind=NONE,Escape,workspace_overview_close
bind=NONE,Return,workspace_overview_close
bind=NONE,KP_Enter,workspace_overview_close
bind=NONE,Left,workspace_overview_select,left
bind=NONE,Right,workspace_overview_select,right
bind=NONE,Up,workspace_overview_select,up
bind=NONE,Down,workspace_overview_select,down
bind=NONE,h,workspace_overview_select,left
bind=NONE,l,workspace_overview_select,right
bind=NONE,k,workspace_overview_select,up
bind=NONE,j,workspace_overview_select,down
mousebind=NONE,btn_left,workspace_overview_drag
mousebind=NONE,btn_right,workspace_overview_kill
key_mode=default
```

This example moves windows only with SUPER and the right button. A plain right
click does not close windows. The wheel selects workspaces and the middle
button leaves the overview:

```ini
key_mode=workspace_overview
mousebind=NONE,btn_left,workspace_overview_drag
mousebind=SUPER,btn_right,workspace_overview_drag,move
mousebind=NONE,btn_middle,workspace_overview_close
axisbind=NONE,UP,workspace_overview_select,up
axisbind=NONE,DOWN,workspace_overview_select,down
key_mode=default
```

With `occupied` scope, kept previews remain empty; otherwise the overview closes
when every workspace becomes empty. `all` scope keeps all previews.

### Configuration

| Setting | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `workspace_overview_border_width` | integer | `2` | Width of each workspace preview border and its inset around the preview texture. Range: 0–100. |
| `workspace_overview_gap` | integer | `5` | Gap between workspace preview cards. Range: 0–1000. |
| `workspace_overview_drag_threshold` | integer | `5` | Pointer distance required before a click becomes a window drag. Range: 0–1000. |
| `workspace_overview_occupied_keep_seen_tags` | integer | `1` | Keep tags seen at open or through `view_*` while the overview is open. Kept previews remain even if empty. Range: 0–1. |
| `workspace_overview_scroll_direction` | string | `vertical` | Scrolling workspace strip direction: `horizontal` or `vertical`. |
| `workspace_overview_scroll_ratio` | float | `0.8` | Maximum workspace preview size relative to the monitor in scrolling mode. Range: 0.1–1.0. |
| `workspace_overview_drag_scroll_speed` | float | `1.5` | Automatic edge-scroll speed while dragging a window, in workspace strides per second. Range: 0.1–10.0. |
| `workspace_overview_focused_dim_opacity` | float | `0.0` | Black overlay opacity on the selected workspace preview. Range: 0–1. |
| `workspace_overview_unfocused_dim_opacity` | float | `0.2` | Black overlay opacity on unselected workspace previews. Range: 0–1. |
| `animation_duration_workspace_overview_open` | integer | `260` | Enter animation duration in milliseconds. Range: 1–50000. |
| `animation_duration_workspace_overview_close` | integer | `260` | Exit animation duration in milliseconds. Range: 1–50000. |
| `animation_duration_workspace_overview_move` | integer | `382` | Workspace move animation duration in milliseconds. Range: 1–50000. |
| `animation_curve_workspace_overview_open` | string | `0.333333,1.0,0.666667,1.0` | Enter animation. Accepts a named easing or cubic-bezier value. |
| `animation_curve_workspace_overview_close` | string | `0.333333,1.0,0.666667,1.0` | Exit animation. Accepts a named easing or cubic-bezier value. |
| `animation_curve_workspace_overview_move` | string | `0.26,0.99,0.26,1.0` | Workspace scrolling and layout-change curve. Accepts a named easing or cubic-bezier value. |

```ini
workspace_overview_border_width=2
workspace_overview_gap=5
workspace_overview_drag_threshold=5
workspace_overview_occupied_keep_seen_tags=1
workspace_overview_scroll_direction=vertical
workspace_overview_scroll_ratio=0.8
workspace_overview_drag_scroll_speed=1.5

# Keep the selected workspace clear and dim the other previews
workspace_overview_focused_dim_opacity=0.0
workspace_overview_unfocused_dim_opacity=0.2
animation_duration_workspace_overview_open=260
animation_duration_workspace_overview_close=260
animation_duration_workspace_overview_move=382
animation_curve_workspace_overview_open=0.333333,1.0,0.666667,1.0
animation_curve_workspace_overview_close=0.333333,1.0,0.666667,1.0
animation_curve_workspace_overview_move=0.26,0.99,0.26,1.0
```
## Switcher

The switcher opens a centered panel of live window thumbnails. Use it to cycle
through the current tag, all tags on the current monitor, or all windows on all
monitors. Releasing any modifier key commits the highlighted window.


### Keybindings

```ini
# Current tag
bind=ALT,grave,switcher,next
bind=ALT+SHIFT,grave,switcher,prev

# All tags on the current monitor
bind=SUPER+ALT,grave,switcher,all_tag_next
bind=SUPER+ALT+SHIFT,grave,switcher,all_tag_prev

# All monitors and tags
bind=SUPER+CTRL+ALT,grave,switcher,all_next
bind=SUPER+CTRL+ALT+SHIFT,grave,switcher,all_prev
```

| Command | Parameter | Description |
| :--- | :--- | :--- |
| `switcher` | `next` / `prev` | Open or cycle through windows on the current tag. |
| `switcher` | `all_tag_next` / `all_tag_prev` | Open or cycle through every window on the current monitor. |
| `switcher` | `all_next` / `all_prev` | Open or cycle through every window on every monitor. |

### Configuration

The switcher has no dedicated size or layout options. Its focused thumbnail
frame uses `focus_color`, other thumbnail frames use `border_color`, and it
shares the title message bar with overcircle.

| Setting | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `title_message_bar_height` | integer | `52` | Height of the highlighted window's title message bar. Set to `0` to disable it. |
| `title_message_bar_gap` | integer | `20` | Gap between the switcher panel and its title message bar. |
| `focus_color` | color | `0xc66b25ff` | Frame color of the highlighted thumbnail. |
| `border_color` | color | `0x444444ff` | Frame color of the other thumbnails. |

```ini
title_message_bar_height=52
title_message_bar_gap=20
focus_color=0xc66b25ff
border_color=0x444444ff
```

## Mouse Interaction

The canvas overview and the switcher accept direct pointer input:

| Mode | Left click | Right click |
| :--- | :--- | :--- |
| Canvas overview | Focus the clicked window and close the overview. | - |
| Switcher | Commit the clicked thumbnail. | Close the clicked window. |
| Switcher panel background | Close the switcher. | Close the switcher. |
