# Prints one line per monitor: name, size, position, active workspace. Reads hyprctl monitors -j.
import json, sys
for m in json.load(sys.stdin):
    print(m["name"], f'{m["width"]}x{m["height"]}', "@", m["x"], m["y"], "ws", m["activeWorkspace"]["name"])
