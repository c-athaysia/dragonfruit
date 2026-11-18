---
title: Installation
description: Install Dragonfruit on Gentoo, Arch, Artix or NixOS, or build it from source anywhere else.
---

Dragonfruit isn't in any distribution's repositories yet, so every route below
builds it from source. The difference is who does the work. Where a real
package format exists, the repo ships the recipe, so your package manager still
owns the files, the dependencies and the uninstall.

| Distribution | Recipe | Dependencies |
|---|---|---|
| [Gentoo](#gentoo) | `packaging/gentoo` overlay | ::gentoo + ::guru |
| [Arch](#arch-linux) | `packaging/arch/dragonfruit{,-git}/PKGBUILD` | extra + AUR |
| [Artix](#artix) | the same PKGBUILD | world/system + AUR |
| [NixOS / Nix](#nixos--nix) | the flake in `packaging/nix` | flake inputs |
| [Guix](#guix) | `packaging/guix/dragonfruit.scm` | Guix channels |
| [Anything else](#building-from-source) | plain meson | yours to find |

> **The two hard dependencies are `wlroots 0.20` and `scenefx 0.5`.** They're
> the only ones your distribution might not already have. Both are
> ABI-versioned, so Dragonfruit won't build against wlroots 0.19 or scenefx 0.4,
> and it won't build against wlroots 0.21 when that shows up. Everything else
> in the list you almost certainly have.

---

## Gentoo

wlroots 0.20 is in ::gentoo and stable on amd64. scenefx is in ::guru. The
Dragonfruit ebuild ships in this repo as a complete overlay, so you point
Portage at your clone and `emerge` from it.

**1. Enable ::guru** (for `gui-libs/scenefx`)

```bash
emerge --ask app-eselect/eselect-repository
eselect repository enable guru
emerge --sync guru
```

**2. Clone Dragonfruit and register its overlay**

```bash
git clone https://codeberg.org/central-lambda-group/dragonfruit.git ~/dragonfruit
```

```bash
# /etc/portage/repos.conf/dragonfruit.conf
[dragonfruit]
location = /home/YOURUSER/dragonfruit/packaging/gentoo
auto-sync = no
```

`auto-sync = no` is on purpose. You update it with `git pull` in the clone, not
with `emerge --sync`.

**3. Accept keywords**

scenefx is testing-only. The overlay has a release ebuild for each tag
(`dragonfruit-0.1.0`, `~amd64`) and a live ebuild (`dragonfruit-9999`) that
builds the newest commit and has no keywords at all, which is what `**` is for.
Pick one:

```bash
# /etc/portage/package.accept_keywords/dragonfruit
gui-libs/scenefx ~amd64
gui-wm/dragonfruit ~amd64     # the newest release
#gui-wm/dragonfruit **        # or the newest commit
```

**4. Set the wlroots USE flags**

wlroots needs `libinput` and `session` (both on by default), and `X` if you
want XWayland. You do, unless you're certain you run no X11 apps:

```bash
# /etc/portage/package.use/dragonfruit
gui-libs/wlroots libinput session X
```

**5. Install**

```bash
emerge --ask gui-wm/dragonfruit
```

To rebuild after a `git pull`, run `emerge --ask --oneshot gui-wm/dragonfruit`.
The live ebuild re-fetches the branch tip every time.

### USE flags

| Flag | Default | Effect |
|---|---|---|
| `X` | off | XWayland support (`-D xwayland=enabled`) |
| `asan` | off | AddressSanitizer build, for debugging Dragonfruit itself |

---

## Arch Linux

`wlroots0.20` is in `extra`. `scenefx0.5` is in the AUR. The PKGBUILD in this
repo builds the branch tip as `dragonfruit-git`.

**1. Install scenefx from the AUR**

```bash
paru -S scenefx0.5        # or: yay -S scenefx0.5
```

`wlroots0.20` comes down with it. Don't install a plain `wlroots`, the
versioned package is the one that provides `libwlroots-0.20.so`.

**2. Build Dragonfruit**

```bash
git clone https://codeberg.org/central-lambda-group/dragonfruit.git
cd dragonfruit/packaging/arch/dragonfruit       # the newest release
# cd dragonfruit/packaging/arch/dragonfruit-git # or the newest commit
makepkg -si
```

`makepkg` clones the source itself (the release tag, or the `dragonfruit`
branch), so `packaging/arch` is only ever the recipe. Each release also attaches
its `PKGBUILD` to the [release page](https://codeberg.org/central-lambda-group/dragonfruit/releases).
To rebuild `dragonfruit-git` on a newer commit just run `makepkg -si` again.
Its `pkgver()` reads the version from `meson.build` and appends the revision
count and short hash, so pacman always sees a newer version.

Everything else the build needs (`pango`, `cjson`, `pcre2`, `libglvnd`,
`xorg-xwayland`, `libxcb`, `xcb-util-wm`, ...) is in `core` or `extra`, and
`makepkg` pulls it in as a dependency.

---

## Artix

Artix is Arch with a different init, and for Dragonfruit that's the only
difference. **Use the Arch instructions above as written.** The packages are in
Artix's own repositories under the same names (`wlroots0.20` and `cjson` in
`world`, `pcre2` in `system`), and the AUR works normally, so `scenefx0.5`
installs the same way.

Artix doesn't set up two things for you, because there's no systemd or logind
by default.

**Install `base-devel` if you haven't:**

```bash
sudo pacman -S base-devel git
```

**Give yourself a seat.** wlroots gets its seat through libseat, which needs
either seatd or elogind running. seatd is the lighter one:

```bash
sudo pacman -S seatd
sudo gpasswd -a "$USER" seat        # then log out and back in
```

Then enable it for your init:

```bash
sudo rc-update add seatd default && sudo rc-service seatd start   # OpenRC
sudo ln -s /etc/runit/sv/seatd /run/runit/service                 # runit
sudo s6-rc-bundle-update add default seatd                        # s6
sudo dinitctl enable seatd                                        # dinit
```

If you'd rather use elogind (some desktop software expects it), install
`elogind` and its init package instead and skip the `seat` group, since
elogind hands out the seat on login.

Without one of the two, Dragonfruit exits at startup with a libseat error and
no display.

---

## NixOS / Nix

The flake lives in `packaging/nix`. It exports the package, a NixOS module and a
Home Manager module, all under the name `dragonfruit`.

**1. Add the input**

```nix
# flake.nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    dragonfruit = {
      # newest commit; use ?ref=0.1.0&dir=packaging/nix for a release
      url = "git+https://codeberg.org/central-lambda-group/dragonfruit.git?ref=dragonfruit&dir=packaging/nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };
}
```

The `?ref=dragonfruit` matters. The default branch is `dragonfruit` today, but
being explicit means a later change of default branch can't quietly switch
what you build.

**2. Import the NixOS module and enable it**

```nix
# configuration.nix
{ inputs, ... }:
{
  imports = [ inputs.dragonfruit.nixosModules.dragonfruit ];

  programs.dragonfruit.enable = true;
}
```

That installs the package, registers the session with your display manager
(`addLoginEntry`, on by default), turns on polkit and XWayland, and wires up
xdg-desktop-portal with `wlr` for screencast and screenshot.

**3. Configure it with Home Manager**

```nix
# home.nix
{ inputs, ... }:
{
  imports = [ inputs.dragonfruit.hmModules.dragonfruit ];

  wayland.windowManager.dragonfruit = {
    enable = true;
    settings = {
      canvas_shader = "grain";
      canvas_placement = "auto";
      bind = [
        "SUPER,Return,spawn,foot"
        "SUPER,Left,canvas_pan,-5,0"
        "SUPER,Right,canvas_pan,5,0"
        "SUPER,Tab,canvas_overview"
      ];
    };
    autostart_sh = ''
      waybar &
    '';
  };
}
```

The module writes `~/.config/dragonfruit/config.conf` and **checks it at build
time** by running `dragonfruit -c ... -p` against the exact package you're
installing. A typo in a key name fails the rebuild instead of silently doing
nothing at runtime, which is what this config format would otherwise do.

**4. Start it on login**

With greetd, no display manager:

```nix
services.greetd = {
  enable = true;
  settings.default_session = {
    command = "${pkgs.greetd.tuigreet}/bin/tuigreet --cmd dragonfruit";
    user = "greeter";
  };
};
```

Or straight from TTY1:

```nix
services.getty.autologinUser = "YOURUSER";
environment.loginShellInit = ''
  [ "$(tty)" = /dev/tty1 ] && exec dragonfruit
'';
```

### Without flakes, or on non-NixOS Nix

```bash
nix build 'git+https://codeberg.org/central-lambda-group/dragonfruit.git?ref=dragonfruit&dir=packaging/nix'
./result/bin/dragonfruit
```

A dev shell with the toolchain and `clang-tools` is there too:

```bash
nix develop 'git+https://codeberg.org/central-lambda-group/dragonfruit.git?ref=dragonfruit&dir=packaging/nix'
```

---

## Guix

`packaging/guix/dragonfruit.scm` builds the checkout it sits in, so check out
the branch for the newest commit or a tag for a release:

```bash
git clone https://codeberg.org/central-lambda-group/dragonfruit.git
cd dragonfruit
git checkout 0.1.0        # leave this out for the newest commit
guix build -f packaging/guix/dragonfruit.scm
guix install -f packaging/guix/dragonfruit.scm
```

---

## Building from source

For any distribution not listed above.

### Dependencies

These are the pkg-config names meson actually asks for. Translate them to your
distribution's package names:

| pkg-config | Minimum | Usually provided by |
|---|---|---|
| `wlroots-0.20` | 0.20.0 | wlroots (an ABI-versioned package) |
| `scenefx-0.5` | 0.5.0 | scenefx (likewise) |
| `wayland-server` | 1.23.1 | wayland |
| `wayland-client` | n/a | wayland |
| `libinput` | 1.27.1 | libinput |
| `xkbcommon` | n/a | libxkbcommon |
| `libdrm` | n/a | libdrm |
| `pixman-1` | n/a | pixman |
| `libcjson` | n/a | cjson |
| `pangocairo` | n/a | pango + cairo + glib |
| `glesv2` | n/a | libglvnd (or mesa) |
| `libpcre2-8` | n/a | pcre2 |
| `xcb`, `xcb-icccm` | n/a | libxcb, xcb-util-wm (XWayland only) |

Build-time only: `meson` >= 0.60, `ninja`, `wayland-scanner` >= 1.23,
`wayland-protocols` >= 1.32, `pkg-config`.

If wlroots 0.20 and scenefx 0.5 aren't packaged for you, build them first.
Both are meson projects and both install cleanly into `/usr`:

```bash
git clone -b 0.20.2 https://gitlab.freedesktop.org/wlroots/wlroots.git
meson setup wlroots/build wlroots --prefix=/usr && sudo ninja -C wlroots/build install

git clone -b 0.5 https://github.com/wlrfx/scenefx.git
meson setup scenefx/build scenefx --prefix=/usr && sudo ninja -C scenefx/build install
```

### Build

```bash
git clone https://codeberg.org/central-lambda-group/dragonfruit.git
cd dragonfruit
meson setup build --prefix=/usr
ninja -C build
sudo ninja -C build install
```

There are two meson options:

| Option | Default | |
|---|---|---|
| `-D xwayland=enabled\|auto\|disabled` | `enabled` | XWayland. `enabled` makes `xcb` and `xcb-icccm` hard requirements and fails configuration without them. Use `auto` to build without XWayland when they're missing |
| `-D asan=true\|false` | `false` | AddressSanitizer, for working on Dragonfruit |

`ninja -C build install` puts `dragonfruit` and `dfmsg` on your `PATH`, the
session entry in `/usr/share/wayland-sessions`, the portal preferences in
`/usr/share/xdg-desktop-portal`, the stock config in `/etc/dragonfruit`, and
the three background shaders in `/usr/share/dragonfruit/shaders`.

### Running from a build tree

You don't have to install. `build/dragonfruit` runs as is, with one catch: a
bare `canvas_shader=grain` resolves against the install directory baked into
the binary, which doesn't exist until you install. Point it at the source tree
instead:

```bash
export DRAGONFRUIT_SHADER_DIR=$PWD/assets/shaders
./build/dragonfruit -c "$PWD/assets/config.conf"
```

Give `-c` an absolute path. A path starting with `./` is resolved the same way
a `source=./other.conf` line inside a config is, relative to the config file's
own directory. So `-c ./assets/config.conf` looks for
`./assets/assets/config.conf` and tells you the original path is missing.

---

## After installing

### Your config

Dragonfruit reads the first of these that exists:

```
~/.config/dragonfruit/config.conf
~/.config/mango/config.conf          ← a stock Mango config, unchanged
/etc/dragonfruit/config.conf
/etc/mango/config.conf
```

Start from the shipped one:

```bash
mkdir -p ~/.config/dragonfruit
cp /etc/dragonfruit/config.conf ~/.config/dragonfruit/config.conf
```

**Check a config before you trust it.** Unknown keys are dropped silently and
unknown binds fall through to the focused window, so a typo looks like a
feature that doesn't work instead of an error:

```bash
dragonfruit -c ~/.config/dragonfruit/config.conf -p
```

It prints the offending line and its line number, and exits non-zero. Two
things to know about that command:

- **`-c` must come before `-p`.** Options act as they're parsed, so
  `dragonfruit -p -c FILE` checks your *default* config, ignores `FILE`
  entirely, and exits 0. That's a pass that means nothing.
- **Give `-c` an absolute path**, for the reason under
  [running from a build tree](#running-from-a-build-tree).

### Starting it

From a TTY:

```bash
dragonfruit
```

Or pick **Dragonfruit** from your display manager's session list. The
`.desktop` entry gets installed for you.

To start something with the session, use `exec_once=` in the config, or
`dragonfruit -s 'command'` for a one-off.

### Checking it came up right

```bash
dfmsg get canvas          # the camera, viewport and placed windows
dragonfruit -v            # version and commit
```

If `dfmsg get canvas` gives you the camera block, the canvas is live. If it
errors, you're talking to a compositor that isn't Dragonfruit.

### Portals and environment

The installed `dragonfruit-portals.conf` asks for the GTK portal in general and
the wlroots portal for screencast and screenshot. Install
`xdg-desktop-portal-wlr` and `xdg-desktop-portal-gtk` if you want screen
sharing to work. On NixOS the module does this for you.

If Qt apps look wrong, `QT_QPA_PLATFORM=wayland` and the KDE platform plugin
are the usual fix. If Electron apps run under XWayland when you didn't want
that, `NIXOS_OZONE_WL=1` (the name is historical, it works everywhere) or
`--ozone-platform-hint=auto` moves them over.

---

## Troubleshooting

**`Could not find dependency wlroots-0.20` / `scenefx-0.5`**: you have the
wrong ABI version installed, not a missing package. Check with
`pkg-config --modversion wlroots-0.20`. A plain `wlroots` package on a
rolling-release distribution is usually the newest one, which isn't
necessarily 0.20.

**It exits right away with a libseat error**: no seat manager. See
[Artix](#artix) above, the same goes for any system without logind.

**`XDG_RUNTIME_DIR must be set`**: you're launching from somewhere that
doesn't set it, usually `su` instead of a real login. Log in as the user
properly.

**A keybinding does nothing and there's no error**: the config parser dropped
the line. Run `dragonfruit -c FILE -p`, with an absolute `FILE` and `-c` before
`-p`.

**`-p` says the config is fine and it plainly isn't**: you wrote
`dragonfruit -p -c FILE`. That checks the default config and never opens
`FILE`. Put `-c` first.

**`canvas_shader=grain` does nothing**: the shaders aren't where the binary
expects them. Installed, they live in `$prefix/share/dragonfruit/shaders`. From
a build tree, set `DRAGONFRUIT_SHADER_DIR`.

**Windows tile instead of living on a canvas**: you're running `mango`, not
`dragonfruit`. An installed Mango treats an unknown layout name as a reason to
fall back to tiling, silently. Check `which dragonfruit` and
`dragonfruit -v`.
