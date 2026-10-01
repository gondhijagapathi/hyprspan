# Contributing to hyprspan

Bug reports, fixes for new Hyprland releases and behaviour improvements are all welcome. This page covers what you
need to build the plugin, try a change and get a pull request merged.

## Building

You need a compiler with C++26 support, `make`, `pkg-config`, the headers of the Hyprland you are running (on Arch
they come with the `hyprland` package) and `xcb-xinerama`.

```sh
make
hyprctl plugin load "$PWD/hyprspan.so"
hyprctl hyprspan
```

The plugin refuses to load into a Hyprland build other than the one it was compiled against, so build against the
headers of the Hyprland that is actually running.

After a rebuild, restart Hyprland instead of unloading and reloading the plugin: glibc keeps the old copy of the
library mapped, so loading the same path again silently runs the old code. The test suite avoids this by loading a
copy under a new path on every run.

## Hyprland versions

`main` follows the current Hyprland release. The plugin hooks internal functions, so a Hyprland update can break
the build or change behaviour without any change here. A pull request that makes hyprspan work on a new release is
one of the most useful contributions; say which version you built and tested against.

## Tests

```sh
make && tests/run_suite.sh
tests/stop_nested.sh
```

`tests/run_suite.sh` starts a nested Hyprland as a window inside your session, loads the plugin into it and walks
through the scenarios in the README. It needs `grim`, `xdotool`, `kitty`, `quickshell` and PyGObject with GTK 3.

Every step prints `PASS` or `FAIL` for each thing it checks, with the expected and actual value on a failure. The
last line counts them and the exit status is non-zero when a check failed. The plugin state and window geometry are
printed along the way and screenshots are saved under `tests/run-*/`, which helps when a check fails.

When a change alters behaviour, add a step with a `check` for it. A check names what should be true, the expected
output and the command that prints it:

```sh
check "the workspace on SPAN2 is covered" "2 on SPAN2" covered
```

## Pull requests

- Branch from `main` and keep each pull request to one change.
- Describe the behaviour a user will see first, then how the code gets there.
- State the Hyprland version you tested on and the last line of `tests/run_suite.sh` (`== N passed, 0 failed`), or
  say why you could not run it.
- The `build` check compiles the plugin against Arch's current `hyprland` package and has to pass.
- Follow the style of `main.cpp`: four-space indent, `S` and `e` prefixes on structs and enums, `g_` on globals, and
  comments that explain why the code is there rather than what it does.
- Update the Behaviour section of the README when behaviour changes.

## Reporting bugs

Open an issue with the bug report form. It asks for the output of `hyprctl version`, `hyprctl monitors` and
`hyprctl hyprspan`, which answers most questions up front. Before reporting a window that stays on one monitor,
check the monitor layout note under Behaviour in the README: the span only works when the covered monitors form one
top-aligned row.

## License

By contributing you agree that your contribution is licensed under the [MIT license](LICENSE).
