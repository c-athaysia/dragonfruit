---
title: Introduction
description: An infinite-canvas Wayland compositor forked from Mango.
---


**Dragonfruit** is a Wayland compositor forked from [Mango](https://github.com/mangowm/mango), which is based on [dwl](https://codeberg.org/dwl/dwl/). It replaces Mango's tiling layouts with an infinite canvas: windows live on a 2D plane and the screen is a camera you pan and zoom. Everything else is still Mango, and a stock Mango config drops in unchanged.

## Feature Highlights

Beyond basic window management, Dragonfruit provides a rich set of features designed for a modern Wayland experience.

- **[Animations](visuals/animations.md)** — Smooth, customizable animations for opening, moving, closing windows and tag switching.
- **[Canvas](window-management/canvas.md)** — An infinite plane with a camera, snapping, clusters, a logarithmic overview and GLSL backgrounds.
- **[Visual Effects](visuals/effects.md)** — Built-in blur, shadows, corner radius, and opacity effects powered by scenefx.
- **[IPC & Scripting](ipc.md)** — Control the compositor externally with robust IPC support for custom scripts and widgets.

## Additional Features

- **XWayland Support** — Excellent compatibility for legacy X11 applications.
- **Tag System** — Uses tags instead of workspaces, each a filter on the same canvas.
- **Input Methods** — Great support for text input v2/v3 (Fcitx5, IBus).
- **Window States** — Rich states including swallow, minimize, maximize, fullscreen, and overlay.
- **Hot-Reload Config** — Simple external configuration that supports hot-reloading without restarting.
- **Scratchpads** — Support for both Sway-like and named scratchpads.

## Community

Report bugs and ideas on [Codeberg](https://codeberg.org/central-lambda-group/dragonfruit/issues).

## Acknowledgements

This project is built upon the hard work of several open-source projects:

- **[Mango](https://github.com/mangowm/mango)** — The compositor Dragonfruit is forked from.
- **[wlroots](https://gitlab.freedesktop.org/wlroots/wlroots)** — Implementation of the Wayland protocol.
- **[mwc](https://github.com/nikoloc/mwc)** — Basal window animation reference.
- **[dwl](https://codeberg.org/dwl/dwl)** — Basal dwl features.
- **[sway](https://github.com/swaywm/sway)** — Sample implementation of the Wayland protocol.
- **[scenefx](https://github.com/wlrfx/scenefx)** — Library to simplify adding window effects.
