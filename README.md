# hyprspan  (Not officially related to Hyprland, this is a unofficial plugin)

A Hyprland plugin that lets X11 apps go fullscreen across several monitors, the way Citrix Workspace, VMware,
`xfreerdp /multimon` and `remote-viewer` expect to.

These apps ask the window manager for it with the EWMH `_NET_WM_FULLSCREEN_MONITORS` message. Hyprland neither
advertises nor handles that message, so they stay on one monitor. hyprspan:

- advertises `_NET_WM_FULLSCREEN_MONITORS` in `_NET_SUPPORTED` and records each window's requested monitors;
- sizes the window to the union of those monitors while it is fullscreen;
- makes each other covered monitor treat the window as its fullscreen window: the bar and other windows there
  fade out, the pointer goes to the spanning window, and the window is drawn there.

Upstream closed both requests for this ([#1660](https://github.com/hyprwm/Hyprland/issues/1660),
[#3674](https://github.com/hyprwm/Hyprland/issues/3674)) as not planned and suggested a plugin.

## Install

With [hyprpm](https://wiki.hypr.land/Plugins/Using-Plugins/):

```sh
hyprpm update
hyprpm add https://github.com/gondhijagapathi/hyprspan
hyprpm enable hyprspan
```

and load enabled plugins at startup in `hyprland.lua`:

```lua
hl.on("hyprland.start", function() hl.exec_cmd("hyprpm reload") end)
```

Or build it against the Hyprland headers installed by your distribution and load it directly:

```sh
make
hyprctl plugin load "$PWD/hyprspan.so"
```

The plugin refuses to load into a Hyprland build other than the one it was compiled against. Every Hyprland update
needs a rebuild (`hyprpm update` does this), and may need code changes, because the plugin hooks internal
functions. After rebuilding, restart Hyprland rather than unloading and reloading: glibc keeps the old copy of the
library mapped, so a reload of the same path silently runs the old code.

`hyprctl hyprspan` shows the recorded requests and which workspaces are covered.

## Behaviour

- **Monitor layout.** Hyprland shows X11 apps their own monitor layout: one top-aligned row, left to right in
  monitor order. The app draws for that layout, so the span only lines up when the covered monitors really are
  arranged that way. A laptop panel with an external monitor to its right qualifies. For any other arrangement
  hyprspan leaves the window on one monitor and shows a notification, rather than showing the wrong part of the
  window on each monitor.
- **Scale.** `xwayland:force_zero_scaling` must be off, which is the default.
- **Covered monitors behave like a fullscreen workspace.** Hyprland's usual rules apply there: focusing a window
  on a covered monitor, or a new window there asking to be maximised or fullscreen, takes the spanning window out
  of fullscreen (see `misc:on_focus_under_fullscreen`). Switching workspaces on a covered monitor keeps it covered;
  switch the spanning window's own workspace, or leave fullscreen, to see that monitor again.
- **Hotplug.** Requests survive monitors being added, removed or moved, and resume once the layout lines up again.

## Tests

`tests/run_suite.sh` starts a throwaway nested Hyprland (a window inside your session, never the session itself)
with a second, headless monitor, loads `hyprspan.so` into it, and walks through the scenarios above with a GTK test
client that asks for fullscreen on all monitors the same way Citrix does. It prints the plugin state and window
geometry after each step and saves screenshots of both monitors under `tests/run-*/`.

```sh
make && tests/run_suite.sh
tests/stop_nested.sh
```

It needs `grim`, `xdotool`, `kitty`, `quickshell` (for a stand-in bar) and PyGObject with GTK 3.

## License

MIT, see [LICENSE](LICENSE).
