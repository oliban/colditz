#!/bin/bash
# cmd.sh <endpoint> [json-body]
#
# Sends one command to the game API (127.0.0.1:8765), logging it to
# live-log.jsonl first so the spectator dashboard/event log captures every
# action. Endpoint is given without a leading slash (e.g. "input", "walk",
# "say", "state", "screen", "room"). If a JSON body is given, POSTs it;
# otherwise GETs the endpoint. Prints the raw API response.
#
# Examples:
#   cmd.sh input '{"key":"right","ms":800}'
#   cmd.sh walk '{"exit":0}'
#   cmd.sh say '{"text":"hello"}'
#   cmd.sh state
#   cmd.sh screen > frame.png
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
API="http://127.0.0.1:8765"
LOG="$DIR/live-log.jsonl"

ENDPOINT="${1:-}"
if [ -z "$ENDPOINT" ]; then
  echo "usage: cmd.sh <endpoint> [json-body]" >&2
  exit 1
fi
BODY="${2:-}"

# Build and append the log entry via a python3 one-liner so the JSON body
# (which may itself contain quotes/braces) is embedded correctly rather than
# hand-escaped in bash.
python3 - "$ENDPOINT" "$BODY" >> "$LOG" <<'PYEOF'
import json, sys, time

endpoint = sys.argv[1]
body_raw = sys.argv[2]

body = None
if body_raw != "":
    try:
        body = json.loads(body_raw)
    except (json.JSONDecodeError, ValueError):
        body = body_raw

entry = {
    "ts": round(time.time(), 3),
    "type": "cmd",
    "endpoint": endpoint,
    "body": body,
}
print(json.dumps(entry))
PYEOF

if [ -z "$BODY" ]; then
  curl -s "$API/$ENDPOINT"
else
  curl -s -X POST -d "$BODY" "$API/$ENDPOINT"
fi
