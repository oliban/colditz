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

# Guard against a leftover instance from a prior aborted run (crash, ^C,
# CI timeout): if one is still bound to $PORT, this run's own agent_api
# fails to bind and curl silently talks to the stale process instead.
pkill -f colditz-test 2>/dev/null && sleep 1

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

# 3. /state has real game fields
python3 - <<'EOF' && pass "/state schema" || fail "/state schema"
import json
s = json.load(open('/tmp/state.json'))
assert isinstance(s['game_time'], int)
assert isinstance(s['paused'], bool)
assert s['current_prisoner'] in (0,1,2,3)
assert len(s['prisoners']) == 4
p = s['prisoners'][s['current_prisoner']]
assert all(k in p for k in ('nation','room','x','y','fatigue','inventory','selected'))
assert 'input_queue' in s
EOF

# 4. /screen is a real PNG
curl -s -o /tmp/screen.png http://127.0.0.1:$PORT/screen
python3 - <<'EOF' && pass "/screen PNG" || fail "/screen PNG"
d = open('/tmp/screen.png','rb').read()
assert d[:8] == b'\x89PNG\r\n\x1a\n', "bad magic"
assert len(d) > 1000, "suspiciously small"
EOF

# 5. /input moves the prisoner (game must be unpaused, in-game)
curl -s -X POST -d '{"key":"action","ms":100}' http://127.0.0.1:$PORT/input >/dev/null  # dismiss intro
sleep 1
X0=$(curl -s http://127.0.0.1:$PORT/state | python3 -c "import json,sys;s=json.load(sys.stdin);print(s['prisoners'][s['current_prisoner']]['x'])")
curl -s -X POST -d '{"key":"right","ms":600}' http://127.0.0.1:$PORT/input >/dev/null
sleep 1.5
X1=$(curl -s http://127.0.0.1:$PORT/state | python3 -c "import json,sys;s=json.load(sys.stdin);print(s['prisoners'][s['current_prisoner']]['x'])")
[ "$X0" != "$X1" ] && pass "/input moved prisoner ($X0 -> $X1)" || fail "/input did not move prisoner (x=$X0)"

# 6. bad key name → 400
CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST -d '{"key":"fly"}' http://127.0.0.1:$PORT/input)
[ "$CODE" = "400" ] && pass "bad key 400s" || fail "bad key returned $CODE"

kill $PID 2>/dev/null
# Belt-and-suspenders teardown, matched by name rather than trusting $PID:
# colditz-test is launched inside a subshell ("cd ... && ./colditz-test ..."
# &), and $! there does not reliably end up as the real game PID across
# invocations. A previous run's process surviving `kill $PID` was observed
# to (a) keep the listen port bound, so the next run's own agent_api can't
# bind and silently serves nothing, while curl instead talks to the stale
# still-running instance, and (b) let this run's `cp` truncate the binary
# out from under a still-mapped running process. Poll by process name so a
# back-to-back rerun never races a leftover instance.
pkill -f colditz-test 2>/dev/null
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
    pgrep -f colditz-test >/dev/null 2>&1 || break
    sleep 0.2
done
rm -f "$GAMEDIR/colditz-test"
exit $FAIL
