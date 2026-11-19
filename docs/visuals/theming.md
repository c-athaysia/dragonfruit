---
title: Theming
description: Customize the visual appearance of borders, colors, and the cursor.
---

## Dimensions

Control the sizing of window borders and gaps.

| Setting | Default | Description |
| :--- | :--- | :--- |
| `border_px` | `4` | Border width in pixels. With a double border, this is the width of the outer ring. |
| `gap_inner_horizontal` | `5` | Horizontal inner gap (between windows). |
| `gap_inner_vertical` | `5` | Vertical inner gap. |
| `gap_outer_horizontal` | `10` | Horizontal outer gap (between windows and screen edges). |
| `gap_outer_vertical` | `10` | Vertical outer gap. |

## Colors

Colors are defined in `0xRRGGBBAA` hex format.

```ini
# Background color of the root window
root_color=0x323232ff

# Inactive window border
border_color=0x444444ff

# Drop shadow when dragging windows
drop_color=0x8FBA7C55


# Active window border
focus_color=0xc66b25ff

# Urgent window border (alerts)
urgent_color=0xad401fff
```

### State-Specific Colors

You can also color-code windows based on their state:

| State | Config Key | Default Color |
| :--- | :--- | :--- |
| Maximized | `maximized_screen_color` | `0x89aa61ff` |
| Scratchpad | `scratchpad_color` | `0x516c93ff` |
| Global | `global_color` | `0xb153a7ff` |
| Overlay | `overlay_color` | `0x14a57cff` |

> **Tip:** For scratchpad window sizing, see [Scratchpad](../window-management/scratchpad.md) configuration.

### Overview Jump Mode
| Setting | Default | Description |
| :--- | :--- | :--- |
| `jump_label_decorate_fg_color` | `0xc4939dff` | text color. |
| `jump_label_decorate_bg_color` | `0x323232ff` | background color.|
| `jump_label_decorate_focus_fg_color` | `0xeda6b4ff` |  text color for focus. |
| `jump_label_decorate_focus_bg_color` | `0x4e453cff` | background color for focus.|
| `jump_label_decorate_border_color` | `0x8BAA9Bff` | border color.|
| `jump_label_decorate_border_width` | `4` | border width.|
| `jump_label_decorate_corner_radius` | `5` | corner radius.|
| `jump_label_decorate_padding_x` | `10` | horizontal padding.|
| `jump_label_decorate_padding_y` | `10` | vertical padding.|
| `jump_label_decorate_font_desc` | `monospace Bold 16` | font set.|

### Title Message Bar

Shown below the switcher panel, using the highlighted window's title and app id
on two lines. The width follows the switcher panel.

| Setting | Default | Description |
| :--- | :--- | :--- |
| `title_message_bar_height` | `52` | Title message bar height. Set to `0` to disable it. |
| `title_message_bar_gap` | `20` | Gap between the bar and the switcher panel. |
| `title_message_bar_decorate_fg_color` | `0xc0caf5ff` | Text color. |
| `title_message_bar_decorate_bg_color` | `0x1d2021ff` | Background color. |
| `title_message_bar_decorate_focus_fg_color` | `0x9ece6aff` | Text color while focused. |
| `title_message_bar_decorate_focus_bg_color` | `0x3c3836ff` | Background color while focused. |
| `title_message_bar_decorate_border_color` | `0x3b4261ff` | Border color. |
| `title_message_bar_decorate_border_width` | `0` | Border width. |
| `title_message_bar_decorate_corner_radius` | `15` | Corner radius. |
| `title_message_bar_decorate_padding_x` | `0` | Horizontal padding. |
| `title_message_bar_decorate_padding_y` | `0` | Vertical padding. |
| `title_message_bar_decorate_font_desc` | `monospace Bold 13` | Font description. |

## Borders

Control the appearance of window borders.

### Double borders

A double border adds a second ring inside the normal border. A transparent gap can separate the two rings. The inner ring and the gap add to `border_px`, so the outer ring keeps its width and the window frame gets larger.

| Setting | Default | Description |
| :--- | :--- | :--- |
| `inner_border_px` | `0` | Width of the inner ring in pixels. `0` gives a single border. The inner ring needs `border_px` above `0`. |
| `inner_border_gap` | `0` | Width of the transparent gap between the two rings, in pixels. |
| `inner_border_color` | outer color | Color of the inner ring on a window that does not have focus. |
| `inner_focus_color` | outer color | Color of the inner ring on the window that has focus. |

If you do not set an inner color, the inner ring uses the same color as the outer ring in that state. Thus urgent, scratchpad and selected windows show their state color on both rings. If you set an inner color, the inner ring shows only the focus state, and the outer ring shows the other states.

```ini
border_px=3
inner_border_gap=2
inner_border_px=2
inner_border_color=0x222222ff
inner_focus_color=0xc66b25ff
```

On the canvas, the rings get thinner when you zoom out, as a single border does. When the border is too thin for two rings, it shows as one ring. Floating windows do not change size when you zoom, so their borders also stay at full width.

## Cursor Theme

Set the size and theme of your mouse cursor.

```ini
cursor_size=24
cursor_theme=Adwaita
```
