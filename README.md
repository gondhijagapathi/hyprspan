# hyprspan

[![build](https://github.com/gondhijagapathi/hyprspan/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/gondhijagapathi/hyprspan/actions/workflows/build.yml)
[![test](https://github.com/gondhijagapathi/hyprspan/actions/workflows/test.yml/badge.svg?branch=main)](https://github.com/gondhijagapathi/hyprspan/actions/workflows/test.yml)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A [Hyprland](https://hypr.land) plugin that lets X11 apps go fullscreen across several monitors, the way Citrix
Workspace, VMware, `xfreerdp /multimon` and `remote-viewer` expect to.

> [!NOTE]
> hyprspan is an unofficial plugin. It is not affiliated with or endorsed by the Hyprland project.

## Why

Remote desktop clients span a session over several monitors by asking the window manager for it, with the EWMH
`_NET_WM_FULLSCREEN_MONITORS` message. Hyprland neither advertises nor handles that message, so these apps stay on
one monitor. Upstream closed both requests for it ([#1660](https://github.com/hyprwm/Hyprland/issues/1660),
[#3674](https://github.com/hyprwm/Hyprland/issues/3674)) as not planned and suggested a plugin. This is that plugin.

## What it does

- Advertises `_NET_WM_FULLSCREEN_MONITORS` in `_NET_SUPPORTED` and records each window's requested monitors.
- Sizes the window to the union of those monitors while it is fullscreen.
- Makes each other covered monitor treat the window as its fullscreen window: the bar and other windows there
  fade out, the pointer goes to the spanning window, and the window is drawn there.
- Gives a covered monitor back when you go to something else on it, without taking the window out of fullscreen,
  so a remote session is never resized.

There is nothing to configure. It is developed and used daily with Citrix Workspace; reports about other apps
that send the same message are welcome.

## Requirements

- Hyprland with XWayland enabled. `main` follows the current Hyprland release (0.56 at the time of writing).
- Monitors arranged in one top-aligned row, see [Behaviour](#behaviour).
- `xwayland:force_zero_scaling` off, which is the default.

## Install

### With hyprpm

```sh
hyprpm update
hyprpm add https://github.com/gondhijagapathi/hyprspan
hyprpm enable hyprspan
```

Load enabled plugins at startup in `hyprland.lua`:

```lua
hl.on("hyprland.start", function() hl.exec_cmd("hyprpm reload") end)
```

`hyprpm disable hyprspan` turns it off again. See the
[hyprpm documentation](https://wiki.hypr.land/Plugins/Using-Plugins/) for more.

### From source

You need a compiler with C++26 support, `make`, `pkg-config` and the headers of the Hyprland you are running. On
Arch, `base-devel` and `hyprland` provide all of it.

```sh
git clone https://github.com/gondhijagapathi/hyprspan
cd hyprspan
make
hyprctl plugin load "$PWD/hyprspan.so"
```

### Updating Hyprland

The plugin hooks internal functions, so it refuses to load into a Hyprland build other than the one it was
compiled against. Every Hyprland update needs a rebuild (`hyprpm update` does this) and may need code changes.
After rebuilding, restart Hyprland rather than unloading and reloading: glibc keeps the old copy of the library
mapped, so a reload of the same path silently runs the old code.

## Usage

Start the app and use its own multi-monitor fullscreen option, for example `xfreerdp /multimon`. The window
covers the monitors the app asked for.

`hyprctl hyprspan` shows what the plugin has recorded:

```
$ hyprctl hyprspan
atom _NET_WM_FULLSCREEN_MONITORS = 438
window 0x600003 (Remote Desktop) edges t0 b0 l0 r1 -> 2 monitor(s) box 0,0 3840x1080 active
covering workspace 2 on DP-1
```

| Part | Meaning |
|---|---|
| `window ... (title)` | An X11 window that has asked for a span. |
| `edges t b l r` | The monitors the app named for the top, bottom, left and right edge, as X11 numbers them. |
| `-> N monitor(s) box x,y WxH` | How many monitors the span covers, and its position and size. |
| `active` / `inactive` | Whether the window is fullscreen across more than one monitor right now. |
| `released <monitor>` | Listed under a window while that monitor has been given back, see below. |
| `covering workspace N on <monitor>` | A workspace currently hidden under a span. |

## Behaviour

- **Monitor layout.** Hyprland shows X11 apps their own monitor layout: one top-aligned row, left to right in
  monitor order. The app draws for that layout, so the span only lines up when the covered monitors really are
  arranged that way. A laptop panel with an external monitor to its right qualifies. For any other arrangement
  hyprspan leaves the window on one monitor and shows a notification, rather than showing the wrong part of the
  window on each monitor.
- **Scale.** `xwayland:force_zero_scaling` must be off, which is the default.
- **Leaving and coming back.** Go to a workspace on a covered monitor, focus a tiled window there or open one
  there, and that monitor shows its own workspace again. The spanning window stays fullscreen at its full size
  meanwhile, so a remote session is not resized, and it covers the monitor again as soon as it is focused. On
  the spanning window's own workspace Hyprland's usual fullscreen rules apply (see
  `misc:on_focus_under_fullscreen`).
- **Hotplug and reload.** Requests survive monitors being added, removed or moved, and resume once the layout
  lines up again. They also survive the plugin being reloaded.

## Troubleshooting

| Symptom | What to check |
|---|---|
| The window stays on one monitor and a notification says it can't span | The monitors are not in one top-aligned, left-to-right row. Rearrange them, see [Behaviour](#behaviour). |
| The window stays on one monitor and `hyprctl hyprspan` lists no `window` line | The app never asked for a span. Check that it runs through XWayland and that its multi-monitor option is on. |
| A notification says the plugin was built for a different Hyprland version | Hyprland was updated. Run `hyprpm update`, or `make` again, then restart Hyprland. |
| A rebuilt plugin behaves like the old one | It was reloaded into a running Hyprland. Restart Hyprland. |

For anything else, open a [bug report](https://github.com/gondhijagapathi/hyprspan/issues/new/choose). The form
asks for the output of `hyprctl version`, `hyprctl monitors` and `hyprctl hyprspan`.

## Tests

`tests/run_suite.sh` starts a throwaway nested Hyprland (a separate instance, never your session, and without a
window in it) with two headless monitors, loads `hyprspan.so` into it, and walks through the scenarios above with a
GTK test client that asks for fullscreen on all monitors the same way Citrix does. After each step it checks the
plugin state, the window geometry and what the second monitor shows, and prints `PASS` or `FAIL` for every check.
The last line counts them, and the exit status is non-zero when a check failed. Screenshots of both monitors are
saved under `tests/run-*/`.

```sh
make && tests/run_suite.sh
tests/stop_nested.sh
```

```
== 4. client asks for all monitors again
  PASS the request is active over both monitors
  PASS the client is fullscreen at the size of both monitors
  PASS the workspace on SPAN2 is covered
  PASS no monitor is released
  PASS SPAN2 shows the client instead of its bar
...
== 94 passed, 0 failed
```

Run it from inside a Hyprland session. It needs `grim`, `xdotool`, `kitty`, `quickshell` (for a stand-in bar) and
PyGObject with GTK 3 and pycairo.

The `test` workflow runs the same suite on every pull request. `tests/ci.sh` does that on a machine with no
session and no GPU: in an Arch container, with labwc on a virtual (`vkms`) graphics card as the session and
software rendering. The screenshots and logs of each run are attached to it as the `test-run` artifact.

## Contributing

Bug reports, fixes for new Hyprland releases and behaviour improvements are welcome.
[CONTRIBUTING.md](CONTRIBUTING.md) covers building, running the tests and what a pull request should include.

## License

[MIT](LICENSE)
