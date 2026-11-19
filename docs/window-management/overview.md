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
| Switcher | `switcher` |

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
