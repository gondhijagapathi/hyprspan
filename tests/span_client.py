#!/usr/bin/env python3
# X11 test client that does what Citrix does: fullscreen across all monitors through GDK, which sends
# _NET_WM_FULLSCREEN_MONITORS when the window manager advertises it. Logs every configure it receives.
import sys

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
import signal

from gi.repository import Gdk, GLib, Gtk

log = open(sys.argv[1], "a", buffering=1)
mode = sys.argv[2] if len(sys.argv) > 2 else "all"

win = Gtk.Window(title=f"span-test-{mode}")
area = Gtk.DrawingArea()
win.add(area)


def draw(widget, cr):
    alloc = widget.get_allocation()
    # 200px stripes so a screenshot shows which part of the window each monitor displays
    for i in range(0, alloc.width, 200):
        shade = (i // 200) % 2
        cr.set_source_rgb(0.1 + 0.5 * shade, 0.3, 0.8 - 0.5 * shade)
        cr.rectangle(i, 0, 200, alloc.height)
        cr.fill()
    cr.set_source_rgb(1, 0, 0)
    cr.set_line_width(16)
    cr.rectangle(0, 0, alloc.width, alloc.height)
    cr.stroke()
    cr.set_source_rgb(1, 1, 1)
    cr.set_font_size(48)
    cr.move_to(40, 90)
    cr.show_text(f"{alloc.width}x{alloc.height}")


def on_map(*_):
    if mode == "window":
        return False
    if mode == "all":
        win.get_window().set_fullscreen_mode(Gdk.FullscreenMode.ALL_MONITORS)
    win.fullscreen()
    return False


def on_configure(_, event):
    log.write(f"configure {event.x} {event.y} {event.width}x{event.height}\n")
    return False


area.connect("draw", draw)
win.connect("map-event", on_map)
win.connect("configure-event", on_configure)
win.connect("destroy", Gtk.main_quit)


def set_mode(fullscreen_mode):
    # GDK sends _NET_WM_FULLSCREEN_MONITORS right away for a mapped window; CURRENT_MONITOR sends the
    # out-of-range "reset" indices.
    win.get_window().set_fullscreen_mode(fullscreen_mode)
    log.write(f"mode {fullscreen_mode.value_nick}\n")
    return True


def toggle_fullscreen():
    state = win.get_window().get_state()
    if state & Gdk.WindowState.FULLSCREEN:
        win.unfullscreen()
    else:
        win.fullscreen()
    log.write("toggle fullscreen\n")
    return True


GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGHUP, toggle_fullscreen)
GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGUSR1, set_mode, Gdk.FullscreenMode.CURRENT_MONITOR)
GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGUSR2, set_mode, Gdk.FullscreenMode.ALL_MONITORS)
win.show_all()
Gtk.main()
