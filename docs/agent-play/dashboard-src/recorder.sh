#!/bin/bash
# recorder.sh — proof-grade run recording via macOS screencapture -v.
#
# Usage:
#   recorder.sh start <N>   start recording to recordings/run-N.mov
#   recorder.sh stop        stop the active recording, print path + size
#   recorder.sh status      print structured status (parsed by server.py)
#
# REGION: set to "x,y,w,h" (screen pixel coordinates) to record only that
# rect (e.g. a rect covering both the game window and the dashboard browser
# window side by side). Leave empty (default) to record the whole main
# display. To find coordinates: open System Settings -> Displays to check
# resolution, or use a screenshot tool (Cmd+Shift+4 shows a live x,y,w,h
# readout in the bottom-left as you drag a selection) without releasing/
# saving, just to read the numbers.
REGION=""

set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
RECDIR="$DIR/recordings"
PIDFILE="$DIR/.rec-pid"
ERRFILE="$DIR/.rec-error"

PERM_MSG="System Settings -> Privacy & Security -> Screen & System Audio Recording: enable it for Terminal (or whichever app is running this shell), then retry."

print_permission_error() {
  local extra="$1"
  {
    echo ""
    echo "############################################################"
    echo "# SCREEN RECORDING PERMISSION REQUIRED"
    echo "# $PERM_MSG"
    [ -n "$extra" ] && echo "# screencapture said: $extra"
    echo "############################################################"
    echo ""
  } >&2
  printf 'screencapture cannot record the screen. %s%s\n' "$PERM_MSG" \
    "${extra:+ (screencapture said: $extra)}" > "$ERRFILE"
}

cmd_start() {
  local n="${1:-}"
  if [ -z "$n" ]; then
    echo "usage: recorder.sh start <N>" >&2
    exit 1
  fi

  if [ -f "$PIDFILE" ]; then
    local old_pid
    old_pid=$(sed -n '1p' "$PIDFILE")
    if [ -n "$old_pid" ] && kill -0 "$old_pid" 2>/dev/null; then
      echo "ERROR: recording already in progress (pid $old_pid)" >&2
      exit 1
    fi
    rm -f "$PIDFILE"
  fi

  mkdir -p "$RECDIR"
  local out="$RECDIR/run-$n.mov"
  rm -f "$out"

  local args=(-v -x)
  if [ -n "$REGION" ]; then
    args+=(-R"$REGION")
  else
    args+=(-m)
  fi
  args+=("$out")

  local errlog
  errlog=$(mktemp)
  screencapture "${args[@]}" >"$errlog" 2>&1 &
  local pid=$!
  {
    echo "$pid"
    echo "$out"
  } > "$PIDFILE"

  # Give screencapture a moment to either fail fast (no permission -> exits
  # immediately) or start writing frames.
  sleep 1
  if ! kill -0 "$pid" 2>/dev/null; then
    rm -f "$PIDFILE"
    print_permission_error "$(cat "$errlog" 2>/dev/null)"
    rm -f "$errlog" "$out" 2>/dev/null
    exit 1
  fi

  # Still alive: confirm it's actually producing output, not stuck behind a
  # permission prompt that will never resolve headlessly.
  sleep 2
  if [ ! -s "$out" ]; then
    kill -INT "$pid" 2>/dev/null
    sleep 0.5
    kill -9 "$pid" 2>/dev/null
    rm -f "$PIDFILE"
    print_permission_error "$(cat "$errlog" 2>/dev/null)"
    rm -f "$errlog" "$out" 2>/dev/null
    exit 1
  fi

  rm -f "$errlog" "$ERRFILE"
  echo "recording started: run $n -> $out (pid $pid)"
}

cmd_stop() {
  if [ ! -f "$PIDFILE" ]; then
    echo "no active recording" >&2
    exit 1
  fi
  local pid out
  pid=$(sed -n '1p' "$PIDFILE")
  out=$(sed -n '2p' "$PIDFILE")

  if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
    echo "recording process not running (stale pid file) - cleaned up" >&2
    rm -f "$PIDFILE"
    exit 1
  fi

  kill -INT "$pid"
  local i
  for i in $(seq 1 20); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.5
  done
  if kill -0 "$pid" 2>/dev/null; then
    kill -9 "$pid" 2>/dev/null
  fi
  rm -f "$PIDFILE"

  if [ -s "$out" ]; then
    local size
    size=$(stat -f%z "$out" 2>/dev/null || wc -c < "$out")
    echo "recording stopped: $out ($size bytes)"
  else
    echo "WARNING: recording stopped but output file missing/empty: $out" >&2
    printf 'recording stopped but output file was missing/empty: %s\n' "$out" > "$ERRFILE"
    exit 1
  fi
}

cmd_status() {
  if [ -f "$PIDFILE" ]; then
    local pid out
    pid=$(sed -n '1p' "$PIDFILE")
    out=$(sed -n '2p' "$PIDFILE")
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      echo "state: recording"
      echo "file: $out"
      echo "error:"
      return 0
    fi
    rm -f "$PIDFILE"
  fi
  echo "state: idle"
  echo "file:"
  if [ -f "$ERRFILE" ]; then
    echo "error: $(tr '\n' ' ' < "$ERRFILE")"
  else
    echo "error:"
  fi
}

case "${1:-}" in
  start) shift; cmd_start "$@" ;;
  stop) cmd_stop ;;
  status) cmd_status ;;
  *) echo "usage: recorder.sh start <N> | stop | status" >&2; exit 1 ;;
esac
