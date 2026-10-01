# Prints "x,y WxH fullscreen|windowed" for the client with the given title, or "gone". Reads hyprctl clients -j.
import json, sys
c = next((c for c in json.load(sys.stdin) if c["title"] == sys.argv[1]), None)
print("gone" if c is None else f'{c["at"][0]},{c["at"][1]} {c["size"][0]}x{c["size"][1]} {"fullscreen" if c["fullscreen"] == 2 else "windowed"}')
