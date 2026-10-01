#!/usr/bin/env bash
# Runs the suite on a machine with no desktop session and no GPU, the way the test workflow does: in a privileged
# Arch container on a host that has the vkms module loaded. The nested Hyprland takes its render device from the
# session it starts in and cannot drive a card without a hardware renderer itself, so labwc on the vkms card is
# that session, and Mesa renders in software.
set -eu
T=$(dirname "$(readlink -f "$0")")
cd "$T/.."

if [ "$(id -u)" = 0 ]; then
	pacman -Syu --noconfirm --needed base-devel hyprland labwc kitty quickshell grim xdotool python-gobject python-cairo gtk3 ttf-dejavu
	chmod a+rw /dev/dri/card*
	mkdir -p /tmp/.X11-unix; chmod 1777 /tmp/.X11-unix
	# the checkout belongs to whoever started the container; build and test as that user
	useradd -m -u "$(stat -c %u .)" ci
	# labwc gets the card through seatd; there is no virtual terminal to bind the seat to in a container
	SEATD_VTBOUND=0 seatd -u ci > /tmp/seatd.log 2>&1 &
	exec runuser -u ci -- env HOME=/home/ci "$0"
fi

CARD=
for c in /sys/class/drm/card[0-9]; do
	case $(readlink -f "$c") in */vkms/*) CARD=/dev/dri/${c##*/} ;; esac
done
[ -n "$CARD" ] || { echo "no vkms card: load the vkms module on the host and start the container with --privileged"; exit 2; }

make

export XDG_RUNTIME_DIR=$(mktemp -d)
LIBSEAT_BACKEND=seatd WLR_BACKENDS=drm WLR_DRM_DEVICES=$CARD WLR_RENDERER=gles2 WLR_RENDERER_ALLOW_SOFTWARE=1 labwc > "$XDG_RUNTIME_DIR/session.log" 2>&1 &
for _ in $(seq 1 80); do
	sock=$(find "$XDG_RUNTIME_DIR" -maxdepth 1 -name 'wayland-*' ! -name '*.lock' -printf '%f\n' -quit)
	[ -n "$sock" ] && break; sleep 0.25
done
[ -n "$sock" ] || { cat /tmp/seatd.log "$XDG_RUNTIME_DIR/session.log"; echo "labwc did not start"; exit 1; }
export WAYLAND_DISPLAY=$sock

status=0; tests/run_suite.sh || status=$?
# keep the logs next to the screenshots
run=$(ls -d tests/run-*/ | tail -n 1)
cp "$XDG_RUNTIME_DIR/session.log" "$run"
for log in "$XDG_RUNTIME_DIR"/hypr/*/hyprland.log; do cp "$log" "$run$(basename "$(dirname "$log")").log"; done
[ "$status" -eq 0 ] || tail -n 60 "$run"*.log
exit "$status"
