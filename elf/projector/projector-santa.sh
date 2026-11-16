#!/usr/bin/env bash
# Raspberry Pi ("santa") version of projector.sh. Runs ON the Pi, which is wired straight to the
# projector over HDMI and plays both the video and its audio. Plays a random santa-*.mp4 each
# time a message arrives on the elf/intro/end MQTT topic (published by kick-sound.sh), never the
# same one twice in a row, and ignores triggers that arrive while a video is playing.
#
# The rest of the time it plays loop-30s.mp4 on repeat. A trigger fades the loop out (150 ms)
# and fades the santa video in (150 ms); when the santa video ends it fades out and the loop
# fades in from black. Click the screen while the loop plays (or press Esc) to quit.
# The playing and fading is done by projector-santa.py, which drives mpv.
#
# Broker settings come from ../secrets.h (MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASSWORD), or
# from environment variables of the same name; TOPIC overrides the topic.
#
# Usage:  ./projector-santa.sh        (Ctrl+C to stop)
#         ./projector-santa.sh --now  (also play a video right away, to test)
# Needs:  sudo apt install mosquitto-clients mpv python3
# Over SSH it attaches itself to the desktop session on the Pi's screen (labwc/Wayland).
# Videos are scaled to cover the whole screen, centered (cropped if the aspect ratio differs).

set -euo pipefail

dir="$(cd "$(dirname "$0")" && pwd)"
secrets="$dir/../secrets.h"
topic="${TOPIC:-elf/intro/end}"

log() { echo "[$(date '+%H:%M:%S')] $*"; }

str_define() { sed -nE "s/^#define[[:space:]]+$1[[:space:]]+\"([^\"]*)\".*/\1/p" "$secrets" | head -n1; }
num_define() { sed -nE "s/^#define[[:space:]]+$1[[:space:]]+([0-9]+).*/\1/p" "$secrets" | head -n1; }

command -v mosquitto_sub >/dev/null || { echo "mosquitto_sub not found: sudo apt install mosquitto-clients" >&2; exit 1; }
command -v mpv >/dev/null || { echo "mpv not found: sudo apt install mpv" >&2; exit 1; }
[[ -f "$secrets" ]] || { echo "Missing $secrets (copy secrets.example.h and fill it in)" >&2; exit 1; }

shopt -s nullglob
videos=("$dir"/santa-*.mp4)
(( ${#videos[@]} )) || { echo "No santa-*.mp4 files in $dir" >&2; exit 1; }
[[ -f "$dir/loop-30s.mp4" ]] || { echo "Missing $dir/loop-30s.mp4" >&2; exit 1; }

# Use the Pi's desktop session even when started from SSH or a service.
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
[[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]] || { echo "No Wayland session at $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY (is the desktop logged in?)" >&2; exit 1; }

host="${MQTT_HOST:-$(str_define MQTT_HOST)}"
port="${MQTT_PORT:-$(num_define MQTT_PORT)}"
user="${MQTT_USER:-$(str_define MQTT_USER)}"
pass="${MQTT_PASSWORD:-$(str_define MQTT_PASSWORD)}"
[[ -n "$host" ]] || { echo "MQTT_HOST not set in $secrets" >&2; exit 1; }

# Keep the login out of the process list: pass it through a private options file.
optdir="$(mktemp -d "${TMPDIR:-/tmp}/elf-projector.XXXXXX")"
chmod 700 "$optdir"
: > "$optdir/mosquitto_sub"; chmod 600 "$optdir/mosquitto_sub"
[[ -z "$user" ]] || printf -- '-u %s\n-P %s\n' "$user" "$pass" > "$optdir/mosquitto_sub"

sub_pid=""
cleanup() {
  trap - EXIT
  [[ -z "$sub_pid" ]] || kill "$sub_pid" 2>/dev/null || true
  pkill -P $$ 2>/dev/null || true; rm -rf "$optdir"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# At boot Wi-Fi may come up after the desktop does: wait (up to 2 min) for the broker.
for _ in $(seq 60); do
  timeout 2 bash -c "exec 3<>/dev/tcp/$host/${port:-1883}" 2>/dev/null && break
  sleep 2
done

log "Listening on $host:${port:-1883}, topic $topic, ${#videos[@]} video(s)"
# The player exits 0 when the screen is clicked (stop for good) and 2 if the MQTT listener
# died (retry); anything else is an error.
while true; do
  # The listener writes into a FIFO the player reads, so we know its pid and can always kill it
  # (a leftover listener would keep the launcher's lock held).
  rm -f "$optdir/trigger"; mkfifo "$optdir/trigger"
  XDG_CONFIG_HOME="$optdir" mosquitto_sub -h "$host" -p "${port:-1883}" -t "$topic" -q 1 \
    -i "elf-projector-santa-$$" > "$optdir/trigger" &
  sub_pid=$!
  set +e
  python3 "$dir/projector-santa.py" "$@" "${videos[@]}" < "$optdir/trigger"
  rc=$?; set -e
  kill "$sub_pid" 2>/dev/null || true; wait "$sub_pid" 2>/dev/null || true; sub_pid=""
  [[ $rc -eq 2 ]] || exit "$rc"
  log "Not connected to the broker, retrying in 5 s"
  sleep 5
done
