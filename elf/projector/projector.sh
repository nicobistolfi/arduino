#!/usr/bin/env bash
# Keeps the projector black and plays a random santa video on it each time the elf's audio
# ends (a message on the elf/intro/end MQTT topic, published by kick-sound.sh).
#
# Plays one of the santa-*.mp4 files next to this script, never the same one twice in a row.
# Broker settings are read from ../secrets.h (MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASSWORD).
# Any of them can be overridden with an environment variable of the same name, as can TOPIC.
#
# Usage:  ./projector.sh        (Esc on the projector, or Ctrl+C here, to stop)
#         ./projector.sh --now  (also play a video right away, to test)
# Needs:  brew install mosquitto, and swiftc (Xcode Command Line Tools)

set -euo pipefail

dir="$(cd "$(dirname "$0")" && pwd)"
secrets="$dir/../secrets.h"
topic="${TOPIC:-elf/intro/end}"
player_src="$dir/projector-player.swift"
player="$dir/build/projector-player"

log() { echo "[$(date '+%H:%M:%S')] $*"; }

str_define() { sed -nE "s/^#define[[:space:]]+$1[[:space:]]+\"([^\"]*)\".*/\1/p" "$secrets" | head -n1; }
num_define() { sed -nE "s/^#define[[:space:]]+$1[[:space:]]+([0-9]+).*/\1/p" "$secrets" | head -n1; }

command -v mosquitto_sub >/dev/null || { echo "mosquitto_sub not found: brew install mosquitto" >&2; exit 1; }
[[ -f "$secrets" ]] || { echo "Missing $secrets (copy secrets.example.h and fill it in)" >&2; exit 1; }

shopt -s nullglob
videos=("$dir"/santa-*.mp4)
(( ${#videos[@]} )) || { echo "No santa-*.mp4 files in $dir" >&2; exit 1; }

if [[ ! -x "$player" || "$player_src" -nt "$player" ]]; then
  command -v swiftc >/dev/null || { echo "swiftc not found: xcode-select --install" >&2; exit 1; }
  log "Building projector-player"
  mkdir -p "$(dirname "$player")"
  swiftc -O -swift-version 5 -o "$player" "$player_src"
fi

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

cleanup() { trap - EXIT; pkill -P $$ 2>/dev/null || true; rm -rf "$optdir"; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

log "Listening on $host:${port:-1883}, topic $topic, ${#videos[@]} video(s)"
# The player exits 0 on Esc (stop for good) and 2 if the MQTT listener died (retry).
while true; do
  XDG_CONFIG_HOME="$optdir" mosquitto_sub -h "$host" -p "${port:-1883}" -t "$topic" -q 1 \
    -i "elf-projector-$$" | "$player" "$@" "${videos[@]}" &
  wait $! && break || true
  log "Not connected to the broker, retrying in 5 s"
  sleep 5
done
