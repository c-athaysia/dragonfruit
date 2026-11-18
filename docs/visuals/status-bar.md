---
title: Status Bar
description: Configure Waybar for Dragonfruit.
---

## Waybar

Dragonfruit implements the standard `ext-workspace` and `wlr-foreign-toplevel` protocols, so Waybar's generic modules work. Mango's own bar integrations (`mangobar` and Waybar's `mango/*` modules) talk to Mango's IPC socket, which Dragonfruit renames, so they do not work here.

### `config.jsonc`

```jsonc
{
  "modules-left": ["ext/workspaces", "wlr/taskbar"],
  "ext/workspaces": {
    "format": "{icon}",
    "on-click": "activate"
  },
  "wlr/taskbar": {
    "format": "{icon}",
    "on-click": "activate"
  }
}
```

For anything else, poll `dfmsg` from a `custom` module, for example `dfmsg get canvas` for the camera and zoom. See [IPC](../ipc.md).

## Styling Example

You can style the tags using standard CSS in `style.css`.

### `style.css`

```css
#workspaces {
  border-color: #c9b890;
  background: rgba(40, 40, 40, 0.76);
}

#workspaces button {
  background: none;
  color: #ddca9e;
}

#workspaces button.hidden {
  color: #9e906f;
  background-color: transparent;
}

#workspaces button.visible {
  color: #ddca9e;
}

#workspaces button:hover {
  color: #d79921;
}

#workspaces button.active {
  background-color: #ddca9e;
  color: #282828;
}

#workspaces button.urgent {
  background-color: #ef5e5e;
  color: #282828;
}

#workspaces button.overview {
  background-color: #ef5e5e;
  color: #282828;
}

#window {
  background-color: #CA9297;
  color: #282828;
}

window#waybar.empty #window {
    background: none;
    margin: 0px;
    padding: 0px;
}

#language {
  background-color: #CA9297;
  color: #282828;
}

```
