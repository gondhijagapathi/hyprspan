#!/usr/bin/env bash
# Starts a fresh nested Hyprland (never the live session), loads a uniquely named copy of hyprspan so dlopen
# can't reuse a stale build, and runs the span scenarios. Leaves the instance up; stop it with stop_nested.sh.
set -u
T=$(dirname "$(readlink -f "$0")")
PLUGIN=${PLUGIN:-$T/../hyprspan.so}
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
echo "nested: $info" > "$T/instance.txt"
set -- $info; export HYPRLAND_INSTANCE_SIGNATURE=$1; NWL=$2
d() { hyprctl dispatch "$1" > /dev/null; }
state() { echo "  hyprspan: $(hyprctl hyprspan | tr '\n' ';')"; hyprctl clients -j | python3 "$T/clients.py" | sed 's/^/  /'; }
shot() { WAYLAND_DISPLAY=$NWL grim -o "$1" "$RUN/$2.png"; }

hyprctl output create headless SPAN2 > /dev/null; sleep 1
W1=$(hyprctl monitors -j | python3 -c 'import json,sys; print(next(m["width"] for m in json.load(sys.stdin) if m["name"]=="WAYLAND-1"))')
sed -i "s/position = \"auto-right\"/position = \"${W1}x0\"/" "$T/hyprland.lua"; hyprctl reload > /dev/null; sleep 1
echo "== monitors"; hyprctl monitors -j | python3 "$T/mons.py" | sed 's/^/  /'
echo "== load plugin: $(hyprctl plugin load "$RUN/hyprspan.so")"

d "hl.dsp.exec_cmd(\"qs -p $T/bar\")"; sleep 3
d "hl.dsp.focus({ workspace = 2 })"; d "hl.dsp.exec_cmd(\"kitty --title local-app\")"; sleep 3
d "hl.dsp.focus({ workspace = 1 })"; d "hl.dsp.exec_cmd(\"env GDK_BACKEND=x11 python3 $T/span_client.py $RUN/client.log all\")"; sleep 3
CPID=$(pgrep -f "span_client.py $RUN/client.log")
export DISPLAY=$(tr '\0' '\n' < /proc/$CPID/environ | sed -n 's/^DISPLAY=//p'); echo "$DISPLAY" > "$T/xdisplay.txt"

echo "== 1. span client fullscreen on WAYLAND-1, local-app + bar on SPAN2"; state; shot WAYLAND-1 s1-left; shot SPAN2 s1-right

echo "== 2. pointer onto SPAN2 (covered monitor)"
d "hl.dsp.cursor.move({ x = $((W1 + 300)), y = 300 })"; sleep 1
echo "  active window: $(hyprctl activewindow -j | python3 -c 'import json,sys; print(json.load(sys.stdin).get("title"))')"
eval "$(xdotool getmouselocation --shell)"; echo "  X pointer at $X,$Y over: $(xdotool getwindowname "$WINDOW" 2>/dev/null || echo "root/none")"

echo "== 2b. a floating window opens silently on the covered workspace"
d "hl.dsp.exec_cmd(\"env GDK_BACKEND=x11 python3 $T/span_client.py $RUN/late.log window\")"; sleep 3; state; shot SPAN2 s2b-right
d "hl.dsp.cursor.move({ x = $((W1 + 500)), y = 400 })"; sleep 1
echo "  active window after pointer move over SPAN2: $(hyprctl activewindow -j | python3 -c 'import json,sys; print(json.load(sys.stdin).get("title"))')"

echo "== 3. client asks for its current monitor only (GTK reset message)"; kill -USR1 $CPID; sleep 1.5; state; shot SPAN2 s3-right
echo "== 4. client asks for all monitors again"; kill -USR2 $CPID; sleep 1.5; state
echo "== 5. client leaves fullscreen"; kill -HUP $CPID; sleep 1.5; state; shot SPAN2 s5-right
echo "== 6. client re-enters fullscreen (request should still apply)"; kill -HUP $CPID; sleep 1.5; state
echo "== 7. client exits"; pkill -f "span_client.py $RUN/late.log"; kill $CPID; sleep 1.5; state; shot SPAN2 s7-right
echo "== 8. unload plugin: $(hyprctl plugin unload "$RUN/hyprspan.so")"; sleep 0.5
echo "== nested still alive: $(kill -0 $3 2>/dev/null && echo yes || echo NO)"
echo "RUN=$RUN"
