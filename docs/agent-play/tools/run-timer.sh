#!/bin/bash
# Times speedrun attempts. start: cache clocks. stop <outcome> <route>: append runs.md row.
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
API=http://127.0.0.1:8765
igt() {
  local output
  output=$(curl -s -m 2 "$API/state" | python3 -c "import json,sys; print(json.load(sys.stdin)['game_time'])" 2>&1)
  if [ -z "$output" ] || ! echo "$output" | grep -qE '^[0-9]+$'; then
    echo "ERROR: cannot read game_time (game down?)" >&2
    return 1
  fi
  echo "$output"
  return 0
}
LOG="$DIR/live-log.jsonl"
case "${1:-}" in
  start)
    IGT0=$(igt) || { echo "ERROR: cannot start timing until game is reachable"; exit 1; }
    echo "$(date +%s) $IGT0" > "$DIR/.run-active"
    # Log a run-start event carrying the IGT baseline (igt0) so the
    # spectator dashboard can display run-relative IGT instead of raw
    # game uptime. This is the single source of truth shared with
    # dashboard/index.html (which reads live-log.jsonl via the server's
    # /api/log endpoint).
    python3 - "$IGT0" >> "$LOG" <<'PYEOF'
import json, sys, time
igt0 = int(sys.argv[1])
entry = {"ts": round(time.time(), 3), "type": "run-start", "text": "run start", "igt0": igt0}
print(json.dumps(entry))
PYEOF
    echo "run started: RTA clock 0:00, IGT0=${IGT0}ms" ;;
  stop)
    [ -f "$DIR/.run-active" ] || { echo "no active run"; exit 1; }
    read T0 IGT0 < "$DIR/.run-active"
    RTA=$(( $(date +%s) - T0 ))
    IGT_END=$(igt) || { echo "ERROR: cannot stop timing (game down?); stop this run again once the API is reachable"; exit 1; }
    IGT_MS=$(( IGT_END - IGT0 ))
    # Next run number = count of existing DATA rows + 1. runs.md has a
    # header line and a `|---|` separator line, both of which also start
    # with `|`, so `grep -c '^|'` over the whole file counts data rows +
    # 2. Subtract those 2 explicitly (not 1) to get the data-row count,
    # then add 1 for the row about to be appended.
    TOTAL_PIPE_LINES=$(grep -c '^|' "$DIR/runs.md")
    DATA_ROWS=$(( TOTAL_PIPE_LINES - 2 ))
    DATA_ROWS=$(( DATA_ROWS < 0 ? 0 : DATA_ROWS ))
    N=$(( DATA_ROWS + 1 ))
    # Sanitize pipe characters in outcome and route-note
    OUTCOME="${2:-unknown}"
    OUTCOME="${OUTCOME//|/\/}"
    ROUTE_NOTE="${3:-}"
    ROUTE_NOTE="${ROUTE_NOTE//|/\/}"
    printf '| %s | %s | any%% | %d:%02d | %d:%02d | %s | %s |  |  |\n' \
      "$N" "$(date +%F)" $((RTA/60)) $((RTA%60)) $((IGT_MS/60000)) $((IGT_MS%60000/1000)) \
      "$OUTCOME" "$ROUTE_NOTE" >> "$DIR/runs.md"
    # Log a run-stop event with the final RTA/IGT so the dashboard can
    # freeze both timers at their authoritative final values (rather than
    # re-deriving them from event timestamps / last-seen game_time).
    python3 - "$RTA" "$IGT_MS" "$OUTCOME" >> "$LOG" <<'PYEOF'
import json, sys, time
rta_s = int(sys.argv[1])
igt_ms = int(sys.argv[2])
outcome = sys.argv[3]
entry = {
    "ts": round(time.time(), 3),
    "type": "run-stop",
    "text": outcome,
    "rta_ms": rta_s * 1000,
    "igt_ms": igt_ms,
}
print(json.dumps(entry))
PYEOF
    rm "$DIR/.run-active"
    tail -1 "$DIR/runs.md" ;;
  *) echo "usage: run-timer.sh start | stop <outcome> <route-note>"; exit 1 ;;
esac
