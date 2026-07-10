#!/bin/bash
# Integration test for the agent API. Launches the real game with -a.
set -u
cd "$(dirname "$0")/.."
GAMEDIR="../Colditz Escape"
PORT=8765
FAIL=0
say()  { echo "  $1"; }
pass() { say "PASS: $1"; }
fail() { say "FAIL: $1"; FAIL=1; }

cp colditz "$GAMEDIR/colditz-test"
( cd "$GAMEDIR" && ./colditz-test -a $PORT -n & echo $! > /tmp/colditz-test.pid )
sleep 3
PID=$(cat /tmp/colditz-test.pid)

# 1. /state returns HTTP 200 and JSON
CODE=$(curl -s -o /tmp/state.json -w '%{http_code}' http://127.0.0.1:$PORT/state)
[ "$CODE" = "200" ] && pass "/state returns 200" || fail "/state returned $CODE"
python3 -c "import json;json.load(open('/tmp/state.json'))" 2>/dev/null \
  && pass "/state is valid JSON" || fail "/state is not valid JSON"

# 2. unknown endpoint returns 404
CODE=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:$PORT/nope)
[ "$CODE" = "404" ] && pass "unknown path 404s" || fail "unknown path returned $CODE"

kill $PID 2>/dev/null
rm -f "$GAMEDIR/colditz-test"
exit $FAIL
