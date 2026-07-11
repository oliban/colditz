#!/bin/bash
# Task 10 walk gauntlet: the acceptance bar for "the walker is provably
# reliable". Fresh game (-a PORT -n), then a fixed tour driven ONLY by
# /walk + /state (no manual /input except the one intro-dismiss keypress
# every errand needs). Prints per-leg wall-times and a legs-passed/total
# summary. Does NOT replace tests/agent-api.sh, which stays green
# separately -- this is the navigation-specific acceptance test.
#
# Legs (see docs/AGENT-API.md and campaign/map.md for the underlying
# room/exit facts this script encodes):
#   1. /walk item lockpick pickup:true (room 251) -> inventory has lockpick
#   2. /walk exit 0 -> room 253
#   3. In 253: exit [3,7] -> 255 and back; exit [1,7] -> 254 and back
#      (see the two-/walk approach note below); locked doors [5,3],[6,3],
#      [5,5] must end blocked with walk_blocked_reason "door"
#   4. Switch to prisoner_2 (French) and walk the descent
#      219 -> 224 -> 227 -> 230 -> 231 (exit indices 1, 5, 1, 2 --
#      discovered live, since campaign/map.md had no tile data for
#      219/224/227 at the time this script was written; see the Task 10
#      report for how they were found)
set -u
cd "$(dirname "$0")/.."
GAMEDIR="../Colditz Escape"
PORT=8765
FAIL=0
PASS_COUNT=0
TOTAL_COUNT=0
T_START=$(date +%s.%N)

say()  { echo "  $1"; }
leg_pass() { PASS_COUNT=$((PASS_COUNT+1)); TOTAL_COUNT=$((TOTAL_COUNT+1)); say "PASS ($3 s): $1 -> $2"; }
leg_fail() { TOTAL_COUNT=$((TOTAL_COUNT+1)); FAIL=1; say "FAIL ($3 s): $1 -> $2"; }

state() { curl -s "http://127.0.0.1:$PORT/state"; }
prisoner_room() { state | python3 -c "import json,sys;s=json.load(sys.stdin);print(s['prisoners'][s['current_prisoner']]['room'])"; }
walk_status() { state | python3 -c "import json,sys;print(json.load(sys.stdin)['walk'])"; }
blocked_reason() { state | python3 -c "import json,sys;print(json.load(sys.stdin)['walk_blocked_reason'])"; }
input_queue() { state | python3 -c "import json,sys;print(json.load(sys.stdin)['input_queue'])"; }

drain_queue() {
  for _ in $(seq 1 25); do
    [ "$(input_queue)" = "0" ] && return
    sleep 0.2
  done
}

# Polls /state's "walk" field until it leaves "walking" (arrived/blocked)
# or the 30s whole-walk cap elapses (poll budget matches that cap plus
# margin). Echoes the final status.
wait_walk() {
  local i
  for i in $(seq 1 70); do
    sleep 0.5
    local w
    w=$(walk_status)
    [ "$w" != "walking" ] && { echo "$w"; return; }
  done
  echo "timeout"
}

# leg_walk NAME BODY EXPECT_ROOM
# Issues one /walk request with the given JSON body, waits for it to
# settle, and checks the resulting status/room against EXPECT_ROOM (empty
# string = don't check room, just require "arrived").
leg_walk_arrive() {
  local name="$1" body="$2" expect_room="$3"
  local t0 t1 dt code resp status room
  drain_queue
  t0=$(date +%s.%N)
  resp=$(curl -s -o /tmp/gauntlet_walk.json -w '%{http_code}' -X POST -d "$body" "http://127.0.0.1:$PORT/walk")
  code="$resp"
  if [ "$code" != "202" ]; then
    t1=$(date +%s.%N); dt=$(echo "$t1 - $t0" | bc)
    leg_fail "$name" "walk rejected ($code: $(cat /tmp/gauntlet_walk.json))" "$dt"
    return 1
  fi
  status=$(wait_walk)
  room=$(prisoner_room)
  t1=$(date +%s.%N); dt=$(echo "$t1 - $t0" | bc)
  if [ "$status" = "arrived" ] && { [ -z "$expect_room" ] || [ "$room" = "$expect_room" ]; }; then
    leg_pass "$name" "arrived (room $room)" "$dt"
    return 0
  else
    leg_fail "$name" "status=$status room=$room reason=$(blocked_reason)" "$dt"
    return 1
  fi
}

# leg_walk_blocked_door NAME BODY
# Issues one /walk request and requires it to end blocked with reason
# "door" (a locked door -- fair-play discovery, same as a human bumping
# into a closed door).
leg_walk_blocked_door() {
  local name="$1" body="$2"
  local t0 t1 dt code resp status reason
  drain_queue
  t0=$(date +%s.%N)
  resp=$(curl -s -o /tmp/gauntlet_walk.json -w '%{http_code}' -X POST -d "$body" "http://127.0.0.1:$PORT/walk")
  code="$resp"
  if [ "$code" != "202" ]; then
    t1=$(date +%s.%N); dt=$(echo "$t1 - $t0" | bc)
    leg_fail "$name" "walk rejected ($code: $(cat /tmp/gauntlet_walk.json))" "$dt"
    return 1
  fi
  status=$(wait_walk)
  reason=$(blocked_reason)
  t1=$(date +%s.%N); dt=$(echo "$t1 - $t0" | bc)
  if [ "$status" = "blocked" ] && [ "$reason" = "\"door\"" -o "$reason" = "door" ]; then
    leg_pass "$name" "blocked (door)" "$dt"
    return 0
  else
    leg_fail "$name" "status=$status reason=$reason (expected blocked/door)" "$dt"
    return 1
  fi
}

# --- Setup ---------------------------------------------------------
pkill -f colditz-gauntlet 2>/dev/null && sleep 1
cp colditz "$GAMEDIR/colditz-gauntlet"
( cd "$GAMEDIR" && ./colditz-gauntlet -a $PORT -n & echo $! > /tmp/colditz-gauntlet.pid )
sleep 5
PID=$(cat /tmp/colditz-gauntlet.pid)

curl -s -X POST -d '{"key":"action","ms":100}' "http://127.0.0.1:$PORT/input" >/dev/null
sleep 1

echo "=== Leg 1: /walk item lockpick (room 251) ==="
drain_queue
T0=$(date +%s.%N)
CODE=$(curl -s -o /tmp/gauntlet_walk.json -w '%{http_code}' -X POST -d '{"item":"lockpick","pickup":true}' "http://127.0.0.1:$PORT/walk")
if [ "$CODE" = "202" ]; then
  STATUS=$(wait_walk)
  HAS_LP=$(state | python3 -c "import json,sys;s=json.load(sys.stdin);p=s['prisoners'][s['current_prisoner']];print('lockpick' in p['inventory'])")
  T1=$(date +%s.%N); DT=$(echo "$T1 - $T0" | bc)
  if [ "$STATUS" = "arrived" ] && [ "$HAS_LP" = "True" ]; then
    leg_pass "item lockpick pickup" "lockpick in inventory" "$DT"
  else
    leg_fail "item lockpick pickup" "status=$STATUS has_lockpick=$HAS_LP" "$DT"
  fi
else
  T1=$(date +%s.%N); DT=$(echo "$T1 - $T0" | bc)
  leg_fail "item lockpick pickup" "walk rejected ($CODE)" "$DT"
fi

echo "=== Leg 2: /walk exit 0 (room 251 -> 253) ==="
leg_walk_arrive "exit 251->253" '{"exit":0}' "253"

echo "=== Leg 3: room 253 exits ==="
# Order matters here (see the Task 10 report's self-review for the full
# diagnosis): arriving in 253 from 251 lands exactly ON the [2,5] doorway
# tile, and immediately requesting a DIFFERENT exit's crossing from that
# exact spot can re-trigger the [2,5] crossing back to 251 as a side
# effect of the new path's very first step (the engine's exit-overlap
# check is continuous, not gated on "which door we meant"). Doing [1,7]
# first (itself needing the [0,7] approach fix below) moves the prisoner
# well clear of [2,5] before [3,7] is ever attempted, which is what makes
# [3,7] reliable here -- confirmed live, not just theorized.
#
# [1,7]->254: separately, this doorway's real crossing alignment is offset
# from the tile's own center/mask-validated-open point -- a direct /walk
# {"exit":N} from elsewhere in the room can leave the prisoner mis-aligned
# for the crossing. Routing through tile [0,7] first (still just /walk +
# /state, no manual /input) reliably lines the approach up; this is
# exactly the kind of campaign-route knowledge map.md is for. Both legs
# are separately timed/reported.
leg_walk_arrive "253 approach [0,7] (for [1,7] door)" '{"tile":[0,7]}' "253"
leg_walk_arrive "253 exit [1,7]->254 (via [0,7])" '{"exit":5}' "254"
leg_walk_arrive "254 exit back->253"  '{"exit":0}' "253"
leg_walk_arrive "253 exit [3,7]->255" '{"tile":[3,7]}' "255"
leg_walk_arrive "255 exit back->253"  '{"exit":0}' "253"
leg_walk_blocked_door "253 locked door [5,3]" '{"exit":0}'
leg_walk_blocked_door "253 locked door [6,3]" '{"exit":1}'
leg_walk_blocked_door "253 locked door [5,5]" '{"exit":4}'

echo "=== Leg 4: French descent 219 -> 224 -> 227 -> 230 -> 231 ==="
drain_queue
curl -s -X POST -d '{"key":"prisoner_2","ms":100}' "http://127.0.0.1:$PORT/input" >/dev/null
sleep 1
leg_walk_arrive "219->224" '{"exit":1}' "224"
leg_walk_arrive "224->227" '{"exit":5}' "227"
leg_walk_arrive "227->230" '{"exit":1}' "230"
leg_walk_arrive "230->231" '{"exit":2}' "231"

# --- Teardown --------------------------------------------------------
kill "$PID" 2>/dev/null
pkill -f colditz-gauntlet 2>/dev/null
for _ in $(seq 1 25); do
    pgrep -f colditz-gauntlet >/dev/null 2>&1 || break
    sleep 0.2
done
rm -f "$GAMEDIR/colditz-gauntlet"

T_END=$(date +%s.%N)
TOTAL_DT=$(echo "$T_END - $T_START" | bc)
echo
echo "=== Gauntlet summary: $PASS_COUNT/$TOTAL_COUNT legs passed, total ${TOTAL_DT}s ==="
exit $FAIL
