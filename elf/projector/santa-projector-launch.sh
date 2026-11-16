#!/usr/bin/env bash
# Launched from the desktop icon: runs projector-santa.sh once (a second click does nothing while
# it is running) and writes its output to projector.log next to it.
dir="$(cd "$(dirname "$0")" && pwd)"
exec flock -n /tmp/elf-projector.lock "$dir/projector-santa.sh" "$@" >> "$dir/projector.log" 2>&1
