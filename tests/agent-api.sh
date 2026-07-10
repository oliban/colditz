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

# 7. pause freezes game_time
curl -s -X POST -d '{"pause":true}' http://127.0.0.1:$PORT/control >/dev/null
sleep 0.5
T0=$(curl -s http://127.0.0.1:$PORT/state | python3 -c "import json,sys;print(json.load(sys.stdin)['game_time'])")
sleep 1
T1=$(curl -s http://127.0.0.1:$PORT/state | python3 -c "import json,sys;print(json.load(sys.stdin)['game_time'])")
[ "$T0" = "$T1" ] && pass "pause freezes game_time" || fail "game_time advanced while paused ($T0 -> $T1)"
curl -s -X POST -d '{"pause":false}' http://127.0.0.1:$PORT/control >/dev/null

# 8. /say returns 200
CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST -d '{"text":"hello from the agent"}' http://127.0.0.1:$PORT/say)
[ "$CODE" = "200" ] && pass "/say accepted" || fail "/say returned $CODE"

# 9. /room returns fair-play geometry
curl -s -o /tmp/room.json http://127.0.0.1:$PORT/room
python3 - <<'EOF' && pass "/room geometry" || fail "/room geometry"
import json
r = json.load(open('/tmp/room.json'))
assert isinstance(r['room'], int)
assert isinstance(r['width'], int) and isinstance(r['height'], int)
assert len(r['grid']) == r['height']
assert all(len(row) == r['width'] for row in r['grid'])
assert set(''.join(r['grid'])) <= set('.#E')
tx, ty = r['my_tile']
assert 0 <= tx < r['width'] and 0 <= ty < r['height']
# fair play: no forbidden knowledge anywhere in the payload
raw = open('/tmp/room.json').read()
for word in ('locked','grade','open','prop','key'):
    assert word not in raw, f"cheating field: {word}"
EOF

# 10. /walk to first exit eventually changes room or reports blocked
# Test 7's unpause needs a long injected KEY_PAUSE hold to bridge the pause
# screen's ~2s fade-transition (see /control's handle_control comment) --
# drain that before exercising /walk's own "input busy" precondition, same
# as any other client would poll /state before issuing the next command.
for i in $(seq 1 20); do
  Q=$(curl -s http://127.0.0.1:$PORT/state | python3 -c "import json,sys; print(json.load(sys.stdin)['input_queue'])")
  [ "$Q" = "0" ] && break
  sleep 0.2
done
EXIT_TILE=$(curl -s http://127.0.0.1:$PORT/room | python3 -c "import json,sys; r=json.load(sys.stdin); e=r['exits'][0]['tile']; print(f'{e[0]},{e[1]}')")
ROOM0=$(curl -s http://127.0.0.1:$PORT/state | python3 -c "import json,sys; print(json.load(sys.stdin)['prisoners'][0]['room'])")
CODE=$(curl -s -o /tmp/walk.json -w '%{http_code}' -X POST -d "{\"tile\":[${EXIT_TILE%,*},${EXIT_TILE#*,}]}" http://127.0.0.1:$PORT/walk)
[ "$CODE" = "202" ] && pass "/walk accepted" || fail "/walk returned $CODE"
for i in $(seq 1 20); do
  sleep 0.5
  W=$(curl -s http://127.0.0.1:$PORT/state | python3 -c "import json,sys; print(json.load(sys.stdin)['walk'])")
  [ "$W" != "walking" ] && break
done
ROOM1=$(curl -s http://127.0.0.1:$PORT/state | python3 -c "import json,sys; print(json.load(sys.stdin)['prisoners'][0]['room'])")
if [ "$W" = "arrived" ] || [ "$ROOM1" != "$ROOM0" ]; then pass "/walk completed ($W, room $ROOM0 -> $ROOM1)"; else fail "/walk ended '$W' room unchanged"; fi

# 11. /walk rejects garbage
CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST -d '{"tile":[999,999]}' http://127.0.0.1:$PORT/walk)
[ "$CODE" = "400" ] && pass "/walk 400 on out-of-bounds" || fail "/walk oob returned $CODE"

# 12. /walk cancel: cancelling a walk in progress returns 200 and /state
# settles back to "idle" rather than leaving the pump running or the walk
# stuck in some other terminal state.
ROOMJSON=$(curl -s http://127.0.0.1:$PORT/room)
FAR_TILE=$(echo "$ROOMJSON" | python3 -c "
import json,sys
r = json.load(sys.stdin)
tx, ty = r['my_tile']
best, bestd = None, -1
for y, row in enumerate(r['grid']):
    for x, c in enumerate(row):
        if c in ('#', 'E') and (x, y) != (tx, ty):
            d = abs(x - tx) + abs(y - ty)
            if d > bestd:
                bestd, best = d, (x, y)
print(f'{best[0]},{best[1]}' if best else '')
")
if [ -n "$FAR_TILE" ]; then
  CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST -d "{\"tile\":[${FAR_TILE%,*},${FAR_TILE#*,}]}" http://127.0.0.1:$PORT/walk)
  CODE2=$(curl -s -o /dev/null -w '%{http_code}' -X POST -d '{"cancel":true}' http://127.0.0.1:$PORT/walk)
  [ "$CODE2" = "200" ] && pass "/walk cancel accepted" || fail "/walk cancel returned $CODE2 (walk start was $CODE)"
  W=""
  for i in $(seq 1 20); do
    W=$(curl -s http://127.0.0.1:$PORT/state | python3 -c "import json,sys; print(json.load(sys.stdin)['walk'])")
    [ "$W" = "idle" ] && break
    sleep 0.2
  done
  [ "$W" = "idle" ] && pass "/walk cancel settles to idle" || fail "/walk cancel left walk='$W'"
else
  fail "/walk cancel: no far walkable tile found to test with"
fi

# 13. /walk to (0,0) -- plausibly void/unreachable in the start room -- must
# reject (400 bad target, or 409 no path) rather than 202/500, and the
# server must keep serving requests afterwards.
CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST -d '{"tile":[0,0]}' http://127.0.0.1:$PORT/walk)
if [ "$CODE" = "400" ] || [ "$CODE" = "409" ]; then
  pass "/walk (0,0) rejected ($CODE)"
else
  fail "/walk (0,0) returned $CODE"
fi
CODE=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:$PORT/state)
[ "$CODE" = "200" ] && pass "/state still responds after /walk (0,0)" || fail "/state returned $CODE after /walk (0,0)"

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
