# Prints one line per client: title, XWayland, geometry, fullscreen mode, monitor, workspace. Reads hyprctl clients -j.
import json, sys
for c in json.load(sys.stdin):
    print(c["title"], "| xwayland", c["xwayland"], "| pos", c["at"], "size", c["size"], "| fullscreen", c["fullscreen"], "| mon", c["monitor"], "ws", c["workspace"]["name"])
