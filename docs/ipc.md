---
title: IPC
description: Talk to Dragonfruit from scripts with dfmsg.
---

# dfmsg(1) - User Manual

`dfmsg` is the command line client for Dragonfruit's IPC. Use it to ask the compositor about its state, or to subscribe to live events.

## SYNOPSIS
`dfmsg <command> [arguments...]`

## DESCRIPTION
`dfmsg` connects to the compositor over a Unix socket, set by the `DRAGONFRUIT_INSTANCE_SIGNATURE` environment variable. It has two modes:
1. **One-shot (`get`)**: Sends a query, gets one JSON reply, and exits.
2. **Stream (`watch`)**: Subscribes to some state and keeps printing JSON whenever it changes.

## ENVIRONMENT VARIABLES
* **`DRAGONFRUIT_INSTANCE_SIGNATURE`**: Path to the socket the running Dragonfruit created. It's already set if you run `dfmsg` from a terminal the compositor started.

## COMMANDS

### GET (One-Shot Queries)
| Command | Description |
| :--- | :--- |
| `get version` | Returns the current version of the compositor. |
| `get cursorpos` | Returns the global pointer position (`x`, `y`) and the monitor under it. |
| `get keymode` | Returns the current active keyboard mode (e.g., normal, insert). |
| `get keyboardlayout` | Returns the active XKB layout (abbreviated). |
| `get monitor <name>` | Returns full JSON details for a specific monitor. |
| `get focusing-client` | Returns full JSON details for the client currently in focus. |
| `get client <id>` | Returns full JSON details for a client with the given ID. |
| `get tag <mon> <idx>` | Queries status of a specific tag on a monitor. |
| `get tags <mon>` | Returns a JSON object containing the status of all tags on a monitor. |
| `get all-clients` | Returns a JSON array of all active clients. |
| `get all-monitors` | Returns a JSON array of all connected monitors. |
| `get all-devices` | Returns a JSON array of all physical input devices, grouped by libinput device group (`name`, `types`, `identifier`, `vendor`, `product`, `interfaces`, `matched`). |
| `get all-layers` | Returns a JSON array of all open layer surfaces (`monitor`, `layer`, `name`). |
| `get all-tags` | Returns a JSON object containing the status of all tags. |
| `get canvas [<mon>]` | Returns the camera (position and zoom), the visible viewport, the box around every placed window (in canvas units) and the canvas settings. Clients also report `canvas_x`, `canvas_y`, `canvas_width` and `canvas_height`. |
| `get last_open_surface [<mon>]` | Returns the last focused surface name for a monitor,if the mon not set, it will get current monitor. |

*Example:*
```bash
dfmsg get monitor eDP-1
dfmsg get all-clients
dfmsg get all-monitors
dfmsg get all-devices
dfmsg get all-layers
dfmsg get cursorpos
dfmsg get canvas
```

### WATCH (Event Subscription)
Subscribes the client to real-time updates. When the state changes, the server pushes a new JSON object to the output stream.

* `watch monitor <name>`
* `watch focusing-client`
* `watch client <id>`
* `watch tags <mon_name>`
* `watch all-monitors`
* `watch all-tags`
* `watch all-clients`
* `watch all-devices` — streams the last input device (`name`, `type`) that triggered an event
* `watch keymode`
* `watch keyboardlayout`
* `watch last_open_surface [<mon_name>]`

*Example:*
```bash
# watch all monitors
dfmsg watch all-monitors
# watch all tags
dfmsg watch all-tags
```

### DISPATCH
Allows sending commands to the compositor to alter its state.
* `dispatch <func_name>,[args...] [client,<id>]`

*Example:* 
```bash   
# operate specific client by id
dfmsg dispatch exchange_client,left client,375
# operate current client
dfmsg dispatch exchange_client,left
```

## The canvas

```console
$ dfmsg get canvas
{
  "monitor": "DP-1", "active": true,
  "camera":   { "x": -640, "y": -360, "zoom": 0.5, "zoom_percent": 50, "animating": false },
  "viewport": { "x": -640, "y": -360, "width": 2560, "height": 1440 },
  "overview": { "active": false, "blend": 0 },
  "placed_clients": 3,
  "content":  { "x": 320, "y": 180, "width": 1952, "height": 360 },
  "config":   { "placement": "auto", "follow_focus": true, ... }
}
```

The viewport is the rectangle of the plane you can currently see, and `content`
is the bounding box of every window placed on it. Between the two you've got
enough to draw a minimap, find free space, or work out how far off-screen
something is.

Every client also reports `canvas_x`, `canvas_y`, `canvas_width` and
`canvas_height` next to the usual `x`/`y`/`width`/`height`. The usual ones are
where a window sits **on screen**, which depends on where the camera happens to
be. The `canvas_` ones are where it lives **on the plane**, which doesn't. Session
restore, or anything else that wants to remember a position, needs the second.
