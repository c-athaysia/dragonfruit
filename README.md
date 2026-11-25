<div align="center">
  <img alt="Dragonfruit Logo (ADD!)" width="120"/>

  <h1>Dragonfruit Wayland Compositor</h1>

  <p>An infinite-canvas Wayland compositor forked from <a href="https://github.com/mangowm/mango">Mango</a></p>

<a href="https://codeberg.org/central-lambda-group/dragonfruit/stars"><img src="https://img.shields.io/gitea/stars/central-lambda-group/dragonfruit?gitea_url=https%3A%2F%2Fcodeberg.org&style=flat" alt="Stars"/></a>
<a href="https://codeberg.org/central-lambda-group/dragonfruit/src/branch/dragonfruit/LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0-blue?style=flat" alt="License"/></a>

</div>

---

<video src="https://codeberg.org/central-lambda-group/dragonfruit/raw/branch/dragonfruit/docs/dragonfruit.mp4" controls muted width="100%"></video>

[Watch the demo](docs/dragonfruit.mp4) - rice by [cathaysia](https://codeberg.org/cathaysia)

---

Your windows should not be confined by the edge of your monitor. Your windows should not be confined by the rules of a tiling window manager. But they are.
Dragonfruit fixes such problems beautifully. 

Dragonfruit places your windows on an infinite plane with your screen being a camera looking at part of it.
Put your windows where they belong, pan over to them, and zoom out to see more.

Dragonfruit removes Mango's layouts and replaces them with a canvas.
Everything else is still Mango.

## Why Dragonfruit?

Everything you get from Mango comes along for free:

- **Lightweight & fast** no functionality compromised
- **Excellent xwayland support** run X11 apps without friction (supports scale without blurring)
- **Tags, not workspaces** each tag maintains its own canvas
- **Smooth animations** window open/move/close, transitions layer surfaces
- **Rich window states** swallow, minimize, maximize, global, overlay, fakefullscreen
- **Window effects** blur, shadow, corner radius, opacity (via scenefx)
- **Excellent input method support** text-input v2/v3
- **Powerful scratchpad** sway-style scratchpad, special workspace and named-scratchpad support
- **Label jump overview** see all windows at a glance with easymotion jump labels
- **Draggable workspace overview** inspect tags and drag windows between them
- **IPC** send/receive messages from external programs
- **Hot-reload config** no restart needed for keybinding changes
- **Zero flickering** every frame is correct

And here's what Dragonfruit adds:

- **Canvas layout** the obvious one
- **Logarithmic app switcher** quick switching between apps
- **Background GLSL shaders** so you can tell the camera is moving even with no windows open
- **Double borders** i wanted them

## Vision

**Maintain Stability.** Dragonfruit only pulls from Mango when it gets a new
version number, and every new bug gets fixed before this repo's own version goes up.

**Visual Perfection.** Visual bugs get solved before a new version number too.

**Canvas Focus.** Code for the other layout modes isn't relevant to the canvas,
so it gets removed.

## Configuration

A stock Mango config drops in perfectly functional. The canvas only adds keys of its own.
See [docs/window-management/canvas.md](docs/window-management/canvas.md) for the
canvas options and dispatches, and [docs/ipc.md](docs/ipc.md) for `dfmsg`.

## AI Policy

- **Pull requests:** AI may be used to assist in preparing a pull request (PR), but direct AI-generated PRs are not accepted. Contributors must clearly understand what the code does, whether the implementation is reasonable and rigorous, whether it wastes resources, and whether the implementation is sufficiently convergent rather than scattered.

- **Bug reports:** Using AI to report bugs is acceptable. AI-provided information may serve as reference material, which is preferable to reports that provide no reference information.

## Installation

See [docs/installation.md](docs/installation.md) for dependencies, per-distribution
steps, running from a build tree, and troubleshooting.

## Documentation

- [docs/](docs/index.md): installation, configuration, canvas, bindings, IPC.

## Community

No community yet. (TODO: Discord server.)

## Acknowledgements

- [mango](https://github.com/mangowm/mango) — the upstream Dragonfruit derives from
  - [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) — Wayland protocol implementation
  - [dwl](https://codeberg.org/dwl/dwl) — the foundation Mango builds on
  - [scenefx](https://github.com/wlrfx/scenefx) — window effects library
  - [owl](https://github.com/dqrk0jeste/owl) — animation groundwork
  - [sway](https://github.com/swaywm/sway) — protocol reference
- [vxwm](https://codeberg.org/wh1tepearl/vxwm) — Dragonfruit's inspiration

## LLM Usage Disclosure

This project uses local-run LLMs for planning, commit message writing, code commenting, and feedback when I'm stuck.
