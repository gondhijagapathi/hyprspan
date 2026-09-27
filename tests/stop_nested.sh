#!/usr/bin/env bash
# Stops the nested test Hyprland recorded in instance.txt, after checking it really is the test instance.
T=$(dirname "$(readlink -f "$0")")
set -- $(sed 's/^nested: //' "$T/instance.txt")
ARGS=$(ps -o args= -p "$3" 2>/dev/null)
case "$ARGS" in *"--config $T/hyprland.lua"*) kill "$3" && echo "stopped nested $3" ;; *) echo "pid $3 is not the nested test instance ($ARGS); not killing" ;; esac
