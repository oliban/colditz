#!/bin/bash
# log.sh <type> <text>
#
# Appends a free-text event to live-log.jsonl for the spectator dashboard's
# REASONING feed (types: reason, say, split, run-start, run-stop, or any
# other label the caller wants to introduce).
#
# Examples:
#   log.sh reason "heading to french quarters for the spare key"
#   log.sh say "QUARTERS CLEARED 0:42"
#   log.sh split "key acquired"
#   log.sh run-start "run 5"
#   log.sh run-stop "escaped via north tunnel"
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$DIR/live-log.jsonl"

TYPE="${1:-}"
if [ -z "$TYPE" ]; then
  echo "usage: log.sh <type> <text>" >&2
  exit 1
fi
TEXT="${2:-}"

python3 - "$TYPE" "$TEXT" >> "$LOG" <<'PYEOF'
import json, sys, time

typ = sys.argv[1]
text = sys.argv[2]

entry = {
    "ts": round(time.time(), 3),
    "type": typ,
    "text": text,
}
print(json.dumps(entry))
PYEOF
