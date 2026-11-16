#!/usr/bin/env bash
# Plays 001.mp3 on this Mac in sync with the elf show, driven over MQTT (topic elf/audio).
#
# The ESP32 runs the show's clock and publishes "play" on elf/audio just early enough that the
# sound starts when the legs start kicking, and "stop" if a show is cut short. This script
# pipes those messages into kick-player, a tiny Swift program that keeps the sound preloaded,
# so playback starts within a few milliseconds (launching afplay per sound takes 300-800 ms).
# When the sound plays to its end, the script publishes "end" on elf/intro/end (not when a
# show is cut short and the sound is stopped).
#
# Broker settings are read from secrets.h next to this script (MQTT_HOST, MQTT_PORT,
# MQTT_USER, MQTT_PASSWORD). Any of them can be overridden with an environment variable
# of the same name, as can SOUND (the file to play), TOPIC and END_TOPIC.
#
# Usage:  ./kick-sound.sh        (Ctrl+C to stop)
# Needs:  brew install mosquitto, and swiftc (Xcode Command Line Tools)

set -euo pipefail

dir="$(cd "$(dirname "$0")" && pwd)"
secrets="$dir/secrets.h"
sound="${SOUND:-$dir/001.mp3}"
topic="${TOPIC:-elf/audio}"
end_topic="${END_TOPIC:-elf/intro/end}"
player_src="$dir/kick-player.swift"
player="$dir/build/kick-player"

log() { echo "[$(date '+%H:%M:%S')] $*"; }

# Read a #define from secrets.h: quoted string or plain number.
str_define() { sed -nE "s/^#define[[:space:]]+$1[[:space:]]+\"([^\"]*)\".*/\1/p" "$secrets" | head -n1; }
num_define() { sed -nE "s/^#define[[:space:]]+$1[[:space:]]+([0-9]+).*/\1/p" "$secrets" | head -n1; }

command -v mosquitto_sub >/dev/null || { echo "mosquitto_sub not found: brew install mosquitto" >&2; exit 1; }
command -v mosquitto_pub >/dev/null || { echo "mosquitto_pub not found: brew install mosquitto" >&2; exit 1; }
[[ -f "$sound" ]] || { echo "Sound file not found: $sound" >&2; exit 1; }
[[ -f "$secrets" ]] || { echo "Missing $secrets (copy secrets.example.h and fill it in)" >&2; exit 1; }

# Build the player on first run, and again whenever its source changes.
if [[ ! -x "$player" || "$player_src" -nt "$player" ]]; then
  command -v swiftc >/dev/null || { echo "swiftc not found: xcode-select --install" >&2; exit 1; }
  log "Building kick-player"
  mkdir -p "$(dirname "$player")"
  swiftc -O -swift-version 5 -o "$player" "$player_src"
fi

host="${MQTT_HOST:-$(str_define MQTT_HOST)}"
port="${MQTT_PORT:-$(num_define MQTT_PORT)}"
user="${MQTT_USER:-$(str_define MQTT_USER)}"
pass="${MQTT_PASSWORD:-$(str_define MQTT_PASSWORD)}"
[[ -n "$host" ]] || { echo "MQTT_HOST not set in $secrets" >&2; exit 1; }

broker=(-h "$host" -p "${port:-1883}")
args=("${broker[@]}" -t "$topic" -q 1 -i "elf-kick-sound-$$")

# The login goes in private mosquitto_sub/mosquitto_pub options files instead of on the
# command line, where any process listing (ps) would show the password.
optdir="$(mktemp -d "${TMPDIR:-/tmp}/elf-kick.XXXXXX")"
chmod 700 "$optdir"
for tool in mosquitto_sub mosquitto_pub; do
  : > "$optdir/$tool"
  chmod 600 "$optdir/$tool"
  if [[ -n "$user" ]]; then
    printf -- '-u %s\n-P %s\n' "$user" "$pass" > "$optdir/$tool"
  fi
done

# Show the player's log, and publish on end_topic each time it reports the sound finished.
watch_player() {
  local line
  while IFS= read -r line; do
    echo "$line"
    if [[ "$line" == *"] finished" ]]; then
      if XDG_CONFIG_HOME="$optdir" mosquitto_pub "${broker[@]}" -t "$end_topic" -m end -q 1 \
          -i "elf-kick-sound-pub-$$"; then
        log "published end on $end_topic"
      else
        log "could not publish on $end_topic"
      fi
    fi
  done
}

# Stopping the script (Ctrl+C or kill) also stops the listener and the player.
cleanup() {
  trap - EXIT
  pkill -P $$ 2>/dev/null || true
  rm -rf "$optdir"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

log "Listening on $host:${port:-1883}, topic $topic (sound end -> $end_topic)"
while true; do
  # Run in the background and wait: bash only handles a signal (and its trap) once the foreground
  # command finishes, but `wait` returns as soon as a signal arrives.
  XDG_CONFIG_HOME="$optdir" mosquitto_sub "${args[@]}" | "$player" "$sound" | watch_player &
  wait $! || true
  # mosquitto_sub reconnects on its own after a drop; it only exits if it can't connect at all.
  log "Not connected to the broker, retrying in 5 s"
  sleep 5
done
