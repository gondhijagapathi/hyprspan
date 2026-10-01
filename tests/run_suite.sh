#!/usr/bin/env bash
# Starts a fresh nested Hyprland (never the live session), loads a uniquely named copy of hyprspan so dlopen
# can't reuse a stale build, and runs the span scenarios, checking the result of each. Exits 0 when every check
# passes. Leaves the instance up; stop it with stop_nested.sh.
set -u
T=$(dirname "$(readlink -f "$0")")
PLUGIN=${PLUGIN:-$T/../hyprspan.so}

for tool in Hyprland hyprctl grim xdotool kitty qs python3; do
	command -v "$tool" > /dev/null || { echo "missing: $tool"; exit 2; }
done
python3 -c 'import gi; gi.require_version("Gtk", "3.0")' 2> /dev/null || { echo "missing: PyGObject with GTK 3"; exit 2; }
[ -f "$PLUGIN" ] || { echo "missing: $PLUGIN (run make first)"; exit 2; }

RUN=$T/run-$(date +%H%M%S); mkdir -p "$RUN"
cp "$PLUGIN" "$RUN/hyprspan.so"

cat > "$T/hyprland.lua" <<'LUA'
-- Throwaway nested test instance for hyprspan.
hl.monitor({ output = "", mode = "preferred", position = "0x0", scale = "1" })
hl.monitor({ output = "SPAN2", mode = "1280x720@60", position = "auto-right", scale = "1" })
hl.config({
	animations = { enabled = false },
	misc = { disable_hyprland_logo = true, force_default_wallpaper = 0 },
})
-- step 2b opens this floating window onto the covered workspace without focusing it
hl.window_rule({ name = "late-window-silent", match = { title = "^span-test-window$" }, workspace = "2 silent", float = true })
LUA

before=$(hyprctl instances -j | python3 -c 'import json,sys; print(" ".join(i["instance"] for i in json.load(sys.stdin)))')
env -u HYPRLAND_INSTANCE_SIGNATURE -u DISPLAY HYPRLAND_NO_CRASHREPORTER=1 setsid -f Hyprland --config "$T/hyprland.lua" > "$RUN/hypr.log" 2>&1
for _ in $(seq 1 40); do
	info=$(hyprctl instances -j | python3 -c "import json,sys; b='$before'.split(); print(next((i['instance']+' '+i['wl_socket']+' '+str(i['pid']) for i in json.load(sys.stdin) if i['instance'] not in b), ''))")
	[ -n "$info" ] && break; sleep 0.25
done
[ -n "$info" ] || { echo "FAIL the nested Hyprland did not start, see $RUN/hypr.log"; exit 1; }
echo "nested: $info" > "$T/instance.txt"
set -- $info; export HYPRLAND_INSTANCE_SIGNATURE=$1; NWL=$2
d() { hyprctl dispatch "$1" > /dev/null; }
state() { echo "  hyprspan: $(hyprctl hyprspan | tr '\n' ';')"; hyprctl clients -j | python3 "$T/clients.py" | sed 's/^/  /'; }
# the nested window only renders while it is visible in the outer session, so don't wait for it forever
shot() { WAYLAND_DISPLAY=$NWL timeout 5 grim -o "$1" "$RUN/$2.png" || echo "  (no screenshot of $1)"; }

# What the checks look at. Each prints one line.
active() { hyprctl activewindow -j | python3 -c 'import json,sys; print(json.load(sys.stdin).get("title"))'; }
win() { hyprctl clients -j | python3 "$T/win.py" "$1"; }
mode() { win "$1" | cut -d' ' -f3; }
# the test client's request as hyprctl hyprspan reports it: "x,y WxH active|inactive"
request() { hyprctl hyprspan | sed -n 's/^window .*(span-test-all).* box //p' | grep . || echo none; }
covered() { hyprctl hyprspan | sed -n 's/^covering workspace //p' | paste -sd, | grep . || echo none; }
xpointer() { eval "$(xdotool getmouselocation --shell)"; xdotool getwindowname "$WINDOW" 2> /dev/null || echo none; }
# the colour of one pixel in the top left corner of SPAN2, as rrggbb: the bar's when the monitor shows its own
# workspace, the test client's red border when the span is drawn there
corner() { WAYLAND_DISPLAY=$NWL timeout 5 grim -t ppm -g "$((W1 + 100)),4 1x1" - | tail -c 3 | od -An -tx1 | tr -d ' \n'; }
BAR=00e676; CLIENT=ff0000

# check WHAT EXPECTED COMMAND...: passes once COMMAND prints EXPECTED, retrying for 3s while Hyprland settles
PASS=0; FAIL=0
check() {
	local what=$1 want=$2 got; shift 2
	for _ in $(seq 1 12); do got=$("$@"); [ "$got" = "$want" ] && break; sleep 0.25; done
	if [ "$got" = "$want" ]; then PASS=$((PASS + 1)); echo "  PASS $what"
	else FAIL=$((FAIL + 1)); echo "  FAIL $what: expected '$want', got '$got'"; fi
}
spanning() {
	check "the request is active over both monitors" "$SPAN active" request
	check "the client is fullscreen at the size of both monitors" "$SPAN fullscreen" win span-test-all
	check "the workspace on SPAN2 is covered" "2 on SPAN2" covered
	check "SPAN2 shows the client instead of its bar" $CLIENT corner
}
not_spanning() {
	check "no workspace is covered" none covered
	check "SPAN2 shows its bar" $BAR corner
}

hyprctl output create headless SPAN2 > /dev/null; sleep 1
read -r W1 H1 < <(hyprctl monitors -j | python3 -c 'import json,sys; m = next(m for m in json.load(sys.stdin) if m["name"]=="WAYLAND-1"); print(m["width"], m["height"])')
SPAN="0,0 $((W1 + 1280))x$((H1 > 720 ? H1 : 720))"
sed -i "s/position = \"auto-right\"/position = \"${W1}x0\"/" "$T/hyprland.lua"; hyprctl reload > /dev/null; sleep 1
echo "== monitors"; hyprctl monitors -j | python3 "$T/mons.py" | sed 's/^/  /'
echo "== load plugin"; out=$(hyprctl plugin load "$RUN/hyprspan.so"); check "the plugin loads" ok echo "$out"

d "hl.dsp.exec_cmd(\"qs -p $T/bar\")"; sleep 3
d "hl.dsp.focus({ workspace = 2 })"; d "hl.dsp.exec_cmd(\"kitty --title local-app\")"; sleep 3
d "hl.dsp.focus({ workspace = 1 })"; d "hl.dsp.exec_cmd(\"env GDK_BACKEND=x11 python3 $T/span_client.py $RUN/client.log all\")"; sleep 3
CPID=$(pgrep -f "span_client.py $RUN/client.log")
[ -n "$CPID" ] || { echo "FAIL the test client did not start"; exit 1; }
export DISPLAY=$(tr '\0' '\n' < /proc/$CPID/environ | sed -n 's/^DISPLAY=//p'); echo "$DISPLAY" > "$T/xdisplay.txt"

echo "== 1. span client fullscreen on WAYLAND-1, local-app + bar on SPAN2"; state; shot WAYLAND-1 s1-left; shot SPAN2 s1-right
spanning

echo "== 2. pointer onto SPAN2 (covered monitor)"
d "hl.dsp.cursor.move({ x = $((W1 + 300)), y = 300 })"; sleep 1
check "the client keeps the focus" span-test-all active
check "the X pointer is over the client" span-test-all xpointer

echo "== 2b. a floating window opens silently on the covered workspace"
d "hl.dsp.exec_cmd(\"env GDK_BACKEND=x11 python3 $T/span_client.py $RUN/late.log window\")"; sleep 3; state; shot SPAN2 s2b-right
d "hl.dsp.cursor.move({ x = $((W1 + 500)), y = 400 })"; sleep 1
check "the client keeps the focus with the pointer over SPAN2" span-test-all active
spanning

echo "== 3. client asks for its current monitor only (GTK reset message)"; kill -USR1 $CPID; sleep 1.5; state; shot SPAN2 s3-right
check "the request is gone" none request
check "the client is fullscreen on WAYLAND-1 only" "0,0 ${W1}x${H1} fullscreen" win span-test-all
not_spanning
echo "== 4. client asks for all monitors again"; kill -USR2 $CPID; sleep 1.5; state
spanning
echo "== 5. client leaves fullscreen"; kill -HUP $CPID; sleep 1.5; state; shot SPAN2 s5-right
check "the client is not fullscreen" windowed mode span-test-all
check "the request is kept but inactive" "$SPAN inactive" request
not_spanning
echo "== 6. client re-enters fullscreen (request should still apply)"; kill -HUP $CPID; sleep 1.5; state
spanning
echo "== 7. client exits"; pkill -f "span_client.py $RUN/late.log"; kill $CPID; sleep 1.5; state; shot SPAN2 s7-right
check "the client's window is gone" gone win span-test-all
check "the request is gone" none request
not_spanning
echo "== 8. unload plugin"; out=$(hyprctl plugin unload "$RUN/hyprspan.so"); check "the plugin unloads" ok echo "$out"; sleep 0.5
check "the nested Hyprland is still running" yes sh -c "kill -0 $3 2> /dev/null && echo yes || echo no"

echo "RUN=$RUN"
echo "== $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
