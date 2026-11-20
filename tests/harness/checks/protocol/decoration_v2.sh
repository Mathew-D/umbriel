#!/usr/bin/env bash
# zxdg_decoration_manager_v1 version 2: decoration objects may be created after
# the initial commit, recreation works, and the protocol error paths hold.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
title_bar_config

wait_exit() {
  local pid=$1 title=$2
  for _ in $(seq 120); do
    kill -0 "$pid" 2>/dev/null || return 0
    sleep 0.025
  done
  echo "$title: client survived a protocol error"
  return 1
}

# A version 2 client may create its decoration after the first commit, as Wine does.
env XDG_DECORATION=server XDG_DECORATION_VERSION=2 XDG_DECORATION_LATE=1 LOG_CONFIGURES=1 \
  "$UMBRIEL_FRACTIONAL_CLIENT" ssd-v2-late 480 300 > "$UMBRIEL_RUNTIME_DIR/ssd-v2-late.log" 2>&1 &
title_bar_wait_field ssd-v2-late title_bar true
rg -q '^decoration 2' "$UMBRIEL_RUNTIME_DIR/ssd-v2-late.log"

# The same late creation is unconfigured_buffer for a version 1 binding.
env XDG_DECORATION=server XDG_DECORATION_VERSION=1 XDG_DECORATION_LATE=1 \
  "$UMBRIEL_FRACTIONAL_CLIENT" ssd-v1-late 480 300 > "$UMBRIEL_RUNTIME_DIR/ssd-v1-late.log" 2>&1 &
v1_late_pid=$!
wait_exit "$v1_late_pid" ssd-v1-late

declare -A controls pids
start_controlled() {
  local title=$1 fd
  shift
  mkfifo "$UMBRIEL_RUNTIME_DIR/$title.fifo"
  exec {fd}<>"$UMBRIEL_RUNTIME_DIR/$title.fifo"
  controls[$title]=$fd
  env DECORATION_CONTROL=1 LOG_CONFIGURES=1 "$@" "$UMBRIEL_FRACTIONAL_CLIENT" "$title" 480 300 \
    <&"$fd" > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  pids[$title]=$!
}
command_to() {
  printf '%s\n' "$2" >&"${controls[$1]}"
}

# A version 2 client can destroy and recreate its decoration object.
start_controlled ssd-v2-recreate XDG_DECORATION=server XDG_DECORATION_VERSION=2
title_bar_wait_field ssd-v2-recreate title_bar true
command_to ssd-v2-recreate 'xdg client'
title_bar_wait_field ssd-v2-recreate title_bar false
command_to ssd-v2-recreate 'xdg destroy'
title_bar_wait_field ssd-v2-recreate title_bar true
command_to ssd-v2-recreate 'xdg create'
title_bar_wait_field ssd-v2-recreate title_bar true
command_to ssd-v2-recreate 'xdg client'
title_bar_wait_field ssd-v2-recreate title_bar false
command_to ssd-v2-recreate 'xdg destroy'
title_bar_wait_field ssd-v2-recreate title_bar true

# An out-of-range mode is invalid_mode and a second object is already_constructed.
start_controlled ssd-v2-bad XDG_DECORATION=server XDG_DECORATION_VERSION=2
title_bar_wait_field ssd-v2-bad title_bar true
command_to ssd-v2-bad 'xdg bad'
wait_exit "${pids[ssd-v2-bad]}" ssd-v2-bad

start_controlled ssd-v2-dup XDG_DECORATION=server XDG_DECORATION_VERSION=2
title_bar_wait_field ssd-v2-dup title_bar true
command_to ssd-v2-dup 'xdg dup'
wait_exit "${pids[ssd-v2-dup]}" ssd-v2-dup

echo 'xdg decoration v2 late creation, recreation, and error paths passed'
