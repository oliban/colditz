# Agent API

An HTTP API embedded in the game process, letting an AI agent perceive the
screen, read game state, and send inputs while a human watches the normal
game window. Off by default; enabled with `-a <port>` (the port argument is
required by getopt; an invalid or zero value falls back to 8765). The server
binds to **127.0.0.1 only** — it is not reachable from the network.

Design background: `docs/superpowers/specs/2026-07-10-agent-api-design.md`.

## Endpoint reference

| Endpoint | Method | Body | Response | Behavior |
|---|---|---|---|---|
| `/state` | GET | — | 200 JSON | Game clock (`game_time`), `paused`/`menu`/`intro` flags, current prisoner index, input queue depth, and per-prisoner nation/room/x/y/direction/speed/state flags/fatigue/escaped/dead/inventory/selected item; status-bar `message`. |
| `/screen` | GET | — | 200 `image/png`, or 404 if no frame captured yet | PNG of the last rendered frame (captured from the GL front buffer each display call). Works even while paused. Encoded at stb's compression level 1 (not the default 8) — noticeably faster per request at the cost of a somewhat larger file, see caveat below. |
| `/input` | POST | `{"key":"left","ms":400}` | 202 `{"queued":N}`, 400 on bad/missing key or full queue | Symbolic key + hold duration (ms, clamped 16–10000) resolved through the loaded key bindings, queued and injected via the same `key_down[]`/`key_readonce[]` path the real keyboard callback uses. Valid key names: `up down left right action pickup drop inv_left inv_right walk_run sleep stooge pause escape prisoner_1 prisoner_2 prisoner_3 prisoner_4`. |
| `/control` | POST | `{"pause":true\|false}` | 200 `{"paused":bool}`, 400 if body missing `"pause"` or the input queue is full | Freeze/resume via the game's own `KEY_PAUSE` key path (see caveats below). Idempotent: if the game is already in the requested state, no key is injected and no toggle occurs. `{"speed":N}` slow-motion is designed in but not implemented in v1. |
| `/say` | POST | `{"text":"..."}` | 200 `{"ok":true}`, 400 if `text` missing | Displays the given text on the in-game status bar (visible to spectators watching the game window), at a priority that overrides routine room/props messages. |
| `/room` | GET | — | 200 JSON, or 500 `"room data unavailable"` if the room's data is unreadable | The **current** room's visible floor grid, exit tile coordinates, the prisoner's own tile, and the room's visible item props (name + tile) — for navigation. See fair-play note below; no room parameter is accepted (always serves the room the current prisoner is actually in). |
| `/walk` | POST | `{"tile":[x,y]}` or `{"exit":N}` or `{"cancel":true}` | 202 `{"walking":true,"target":[x,y],"path_len":K}`; 200 `{"walking":false}` on cancel; 400 on bad/missing/out-of-bounds/void target; 409 `{"error":"no path"}` if unreachable, or `{"error":"input busy"}` if the `/input` queue isn't idle | Autonomous in-room pathing: BFS's a route over the same visible-floor-only geometry `/room` exposes, then drives it by holding the real direction keys (`key_down[KEY_DIRECTION_*]`), same as a held keypress. `{"exit":N}` walks to the Nth entry of `/room`'s own `exits` array. For an exit-tile target, reaching the doorway automatically continues through it (the CROSSING phase) — `arrived` means the room actually changed, not just that the threshold was reached. A stalled path gets one automatic sidestep-and-re-path recovery attempt before giving up. Ends `arrived` or `blocked` (no progress — locked door, furniture, guard body-block, or the 30s whole-walk cap). See fair-play note below. |

Errors: malformed/missing JSON fields → 400 with a reason; unknown endpoint
→ 404; unknown key name → 400 listing valid names. Requests are capped at
4 KB and one request is serviced per ~16 ms tick, with a 200 ms recv timeout
and a 400 ms deadline on writing the response, so a misbehaving client can
stall the game loop by at most ~0.4 s per request; a client that repeatedly
connects and stalls can hold the game to a few fps for the duration, but can
never crash or block it permanently.

## curl examples

```bash
./colditz -a 8765 &
curl -s localhost:8765/state | python3 -m json.tool
curl -s localhost:8765/screen -o frame.png
curl -s -X POST -d '{"key":"right","ms":500}' localhost:8765/input
curl -s localhost:8765/room | python3 -m json.tool
curl -s -X POST -d '{"exit":0}' localhost:8765/walk
curl -s localhost:8765/state | python3 -c "import json,sys;print(json.load(sys.stdin)['walk'])"
```

## `/room` — fair-play navigation geometry

```json
{
  "room": 249,
  "outside": false,
  "width": 20, "height": 12,
  "my_tile": [3, 7],
  "grid": [
    "....##E#....",
    "....####....",
    "..E#####...."
  ],
  "exits": [ {"tile": [6, 0]}, {"tile": [2, 2]} ],
  "items": [ {"name": "lockpick", "tile": [4, 3]} ]
}
```

- `grid`: one string per row (`y=0` first), one character per tile:
  `.` = void (tile id 0, no floor), `#` = floor (any nonzero tile id — a
  coarse walkable test; furniture/walls within a nonzero tile can still
  block movement at pixel level, this is a v1 approximation for corridor
  navigation), `E` = exit cell (a doorway/stair — `readexit(x,y) & 0x1F`
  is nonzero).
- `exits`: tile coordinates only, one entry per exit cell — no locked/open
  status, no key grade, no destination room. Duplicated with `E` cells in
  `grid` for convenience.
- `items`: the CURRENT room's pickable props (Task 9) — one entry per prop
  actually present (already-picked-up props are omitted), each `{"name":
  "<prop name>", "tile":[x,y]}`. `name` is the same string `/state`'s
  per-prisoner `inventory` keys use (the shared `prop_name[]` table, e.g.
  `"lockpick"`, `"key_one"`, `"pass"`); `tile` is the prop's position
  converted to tile coordinates the same way `my_tile` is. This is exactly
  the data `set_room_props()`/`set_props_overlays()` draw on screen every
  frame, read the same way (including skipping a prop hidden behind a
  removable outside wall) — nothing about a prop's lock/hidden state
  (there isn't any) or any other room's props is exposed.
- `my_tile`: the current prisoner's own position, as `[tile_x, tile_y]`
  (`px/32`, `p2y/32`), always inside `[0,width) x [0,height)`. During a
  room-transition frame, my_tile may be clamped to the grid edge rather than
  the true position.
- Outside (`current_room_index == ROOM_OUTSIDE`), `width`/`height` are the
  fixed compressed-map dimensions (`CMP_MAP_WIDTH`=84, `CMP_MAP_HEIGHT`=72).
- **Fair-play mandate (non-negotiable): `/room` exposes only what a human
  player can see on screen.** It never reads or emits door locked/open
  flags, key grades, or any other-room's data — an agent learns whether a
  door is locked the same way a human does, by trying it (via `/input`)
  and reading the resulting status message via `/state`. Room props (see
  `items` above) ARE exposed by name and position, because a human sees
  exactly that much just by looking at the screen — this is not a
  cheating vector, it's the same visibility standard as the floor grid.
  There is also no room-index parameter: `/room` always serves whatever
  room the current prisoner is actually standing in, both because the
  engine's `readtile`/`readexit` macros are only self-consistent for the
  current room (they key off `is_outside`, which tests
  `current_room_index`), and because letting an agent peek into rooms it
  hasn't physically entered would itself be a form of cheating.
- Returns 500 `"room data unavailable"` instead of crashing if the room's
  data can't be safely read: a CRM-file "gap" room (offset `0xFFFFFFFF`,
  checked before touching any engine state) or implausible/garbage
  dimensions (e.g. a tunnel room reading beyond `ROOM_MAX_TILES`, sized
  well above the real 84x72 outside-map maximum).
- Reads via a dedicated 32 KB static response buffer (the 8 KB `/state`
  buffer is intentionally not reused — the outside grid alone is several
  KB of characters before JSON overhead).
- Saves and restores the engine's own `room_x`/`room_y`/`offset` globals
  (used by `set_room_xy()`/`readtile`/`readexit` for the engine's own
  mid-frame bookkeeping) around the read, on every return path, so serving
  this request never disturbs other engine code that runs later in the
  same frame or on the next callback.

## `/walk` — autonomous in-room pathing

```bash
curl -s -X POST -d '{"tile":[6,0]}' localhost:8765/walk
# => {"walking":true,"target":[6,0],"path_len":9}
curl -s -X POST -d '{"exit":0}' localhost:8765/walk       # walk to /room's exits[0]
curl -s -X POST -d '{"cancel":true}' localhost:8765/walk  # => {"walking":false}
```

- Takes a target tile (`"tile":[x,y]`, in the current room's own coordinate
  space) or an exit index (`"exit":N`, resolving to the Nth entry of
  `/room`'s own `exits` array — a convenience so a caller doesn't have to
  round-trip through `/room` just to get a coordinate it already has).
  `"cancel":true` stops an in-progress walk immediately (keys released, 200
  `{"walking":false}`); any other `"tile"`/`"exit"` body is ignored once
  `"cancel"` is present.
- Paths with a 4-connected BFS over a **snapshot** of the same visible-floor
  grid `/room` exposes (any nonzero tile id, taken once when the walk is
  accepted), then drives it exactly the way a held keypress would: holding
  `key_down[KEY_DIRECTION_*]` toward each waypoint tile's center in turn
  (two direction keys held together give the engine's own diagonal motion
  for free). No malloc — the grid, BFS working arrays, and path are all
  bounded static buffers sized for the largest room the engine ever serves
  (the 84x72 outside map).
- `path_len` in the 202 response is the number of waypoint tiles the BFS
  found (informational only — no field of `/walk`'s response is meant for
  precise dead-reckoning; poll `/state`'s `walk` field for progress).
- **Fair-play (same mandate as `/room`, non-negotiable): the BFS treats
  every nonzero tile — including exit/doorway cells — as walkable. It never
  reads door locked/open flags, key grades, or anything else `/room`
  doesn't already expose.** This means `/walk` can be accepted toward, and
  legitimately end `blocked` at, a **locked door** — exactly how a human
  player discovers a door is locked: by walking up to it and finding it
  won't open. `/walk` is not a teleport or an oracle; it's a scripted hand
  on the same keys a human has.
- A waypoint (including the final target, for a **non-exit** `"tile"`
  target) is considered "reached" as soon as the prisoner's tile (`px/32`,
  `p2y/32`) matches it — not necessarily centered on it — and immediately
  ends the walk `arrived`.
- **Walk-through exits (Task 9).** If the target is an exit/doorway tile
  (`"exit":N`, or a `"tile":[x,y]` that happens to land on one), reaching
  it does **not** end the walk. Instead it enters a **CROSSING** phase
  that holds a single "outward" direction key — continuing straight
  through the doorway — until `guybrush[nation].room` actually changes
  (`arrived`) or the crossing attempt's own time budget runs out
  (`blocked`, keys released). `/state`'s `walk` field stays `"walking"`
  throughout the CROSSING phase; `arrived` for an exit walk now always
  means the room changed, in a single `/walk` call, matching how a human
  just keeps holding the direction key through a door. The outward
  direction is derived purely from geometry already read for the walk's
  own floor snapshot (never from door lock/grade data): primarily the
  direction the BFS path approached the door from (continuing straight
  through it), falling back to — and, if that direction doesn't qualify,
  filling in from — whichever of the 4 directions leads off the room's
  grid or into a non-walkable neighbor cell. If two directions qualify,
  both are tried, alternating ~0.8s each for up to 2 rounds (~3.2s worst
  case) before giving up; a single qualifying direction gets ~1.6s. (The
  engine's own `exit_dx[]`/`check_footprint()` door-orientation data was
  considered and rejected for this — see the `walk_exit_dir_candidates()`
  comment in `agent_api.c` for why: it's a footprint-quadrant selector
  tied to one specific attempted motion, not a portable door-orientation
  value, and reusing it would mean touching the adjacent lock/grade reads
  fair play forbids.)
- **Stall auto-recovery (Task 9).** If the walk stalls (see below) while
  still en route to a non-final waypoint, and this walk hasn't already
  used its one recovery attempt, `/walk` tries to route around the
  obstruction automatically: sidestep perpendicular to the blocked leg's
  direction of travel (~0.4s each side), then re-run the BFS from wherever
  that leaves the prisoner to the walk's original target on a **fresh**
  floor snapshot, and resume. Only one recovery per walk — a second stall,
  or any stall on the final waypoint itself, is `blocked` immediately, as
  before Task 9.
- Ends `blocked` if the prisoner's pixel position hasn't moved for 40
  consecutive ticks (~0.65s at the ~16ms tick rate) while direction keys
  are held — covers a locked door, blocking furniture, or a guard body
  block, all indistinguishable from each other at this level (same as what
  a human bumping into any of them experiences) — subject to the one
  stall-recovery attempt above.
- Ends `blocked` (not `arrived`) if the current prisoner changes mid-walk
  (`prisoner_N` key, or any other cause) — the walk's whole premise (path
  computed for a specific prisoner's position) no longer holds.
- **Whole-walk 30s hard cap (Task 9).** Path-following, one stall
  recovery, and the CROSSING phase can in the worst case each take several
  seconds; the walk as a whole is capped at 30s wall-clock (`blocked` if
  exceeded), protecting against any pathological loop across all of them.
- `/input` while walking cancels the walk first (keys released), then
  queues the requested key — a manual key always overrides autonomous
  walking. A `/walk` request while the `/input` queue is still busy (e.g.
  a `/control` pause/unpause key still draining) is rejected with 409
  `{"error":"input busy"}` rather than interleaving the two key-holding
  mechanisms; poll `/state`'s `input_queue` field and retry once it's 0.
- `/state` gains a `walk` field: `"idle"` (never walked, or cancelled),
  `"walking"`, `"arrived"`, or `"blocked"` — the last-completed status
  persists until the next `/walk` or a manual `/input` cancels it.

## `/control` caveats

- **Pause goes through the real `KEY_PAUSE` binding (F5 by default), not a
  direct `game_state` write.** This was a deliberate choice so pausing via
  the API stays in sync with everything else that key does — including
  switching the display to the game's own pause/prisoner-overview picture
  screen (`create_pause_screen()` / the `picture_state` fade machine in
  `main.c`). **v1 accepts this as expected behavior**: a human watching the
  game window will see the pause screen, not the last gameplay frame,
  while paused. `/screen` still returns whatever is on screen (the pause
  picture), and `/state`'s `paused` field reflects the true state.
- **`game_time` does freeze while paused**, confirmed by reading
  `main.c`: `KEY_PAUSE` switches the GLUT idle callback from
  `glut_idle_game` (which calls `update_timers()` unconditionally every
  tick) to `glut_idle_static_pic`. That function also calls
  `update_timers()`, but `update_timers()` only advances `game_time` when
  `GAME_STATE_ACTION` is set, and `GAME_STATE_ACTION` is cleared as the very
  first action of the `GAME_FADE_OUT_START` picture-state (entered
  immediately on the pause keypress) and not restored until `PICTURE_EXIT`
  on unpause. So `game_time` is frozen for the entire duration of the pause,
  not just approximately — the pause-freeze test in `tests/agent-api.sh`
  asserts this directly and needed no adaptation.
- Because the toggle is injected as a queued key (`enqueue_key`, same
  mechanism as `/input`) rather than applied synchronously, there is a
  small delay (up to the key's `ms` hold) between the `/control` response
  and the game actually reaching the requested `paused` state.
  `/control`'s idempotence guard (`want != paused`) compares against the
  *current* `paused` flag at request time, so a burst of repeated identical
  calls issued faster than that delay could each see the old state and
  re-inject the toggle; callers polling `/state` before re-issuing
  `/control` avoid this.
- **Unpausing holds the injected `KEY_PAUSE` for 2200 ms (pausing itself
  only needs 100 ms).** These are not symmetric: pausing is consumed
  immediately by `user_input()`'s `read_key_once(KEY_PAUSE)` on the very
  next tick (`main.c:587`), so a short hold is plenty. Unpausing is only
  detected once the pause screen's own fade state machine
  (`glut_idle_static_pic`'s `PICTURE_WAIT` case) cycles back around to
  polling for a key again — which takes ~2×`TRANSITION_DURATION` (2000 ms:
  one fade-out, one fade-in, both defined at 1000 ms in `colditz.h`) to
  reach from the moment pausing started, and it's the *only* place that
  looks at `key_down[KEY_PAUSE]` again while paused (`user_input()`, and
  every other key check, doesn't run at all while paused — see
  `glut_idle_game`'s early return on `GAME_STATE_PAUSED`). A short
  injected press releases well before that ~2 s mark if unpause is
  requested soon after pause, so `PICTURE_WAIT` never catches it held and
  the game is stranded paused forever (`paused` stays `true`, `game_time`
  stays frozen indefinitely — reproduced and confirmed against the
  pre-`/walk` build too, so this is a latent bug in the original
  `/control`, not something the `/walk` feature introduced; fixed here
  because `/walk`'s own test runs immediately after `/control`'s
  pause/unpause test and depends on the game actually being unpaused and
  responsive). One consequence: the `/input` queue can legitimately report
  busy for up to ~2.2 s after an unpause request — callers issuing
  `/input` or `/walk` right after unpausing should poll `/state`'s
  `input_queue` field and wait for it to reach 0 first (as
  `tests/agent-api.sh` now does before its `/walk` tests).
- If the input queue is full, `/control` returns 400 rather than silently
  dropping the pause/unpause request (a deliberate deviation from the
  originally sketched implementation, which returned `void` from the
  key-enqueue helper and always reported 200 — silently swallowing a
  failed pause would leave a caller believing the game was paused when it
  was not).

## `/say` caveats

- `text` is capped at 127 characters (truncated, not rejected) and is
  displayed as-is on the in-game status bar, which renders the game's own
  bitmap font — **plain ASCII only**; non-ASCII bytes are not stripped but
  will not render as intended.
- The status bar shows one message at a time. `/say` uses status message
  priority 3 (the highest priority used by any existing in-game status
  message — see `game.h:89-96` and call sites in `game.c`/`main.c`), so
  agent commentary always pre-empts routine room/props/exit messages
  instead of being silently dropped by the priority gate. It does not,
  however, pre-empt other priority-3 messages (e.g. the in-game debug/exit
  overlay), which is an acceptable v1 trade-off.
- `text` is stored in a static buffer and displayed by pointer (not
  copied by `set_status_message`); `/state`'s `message` field re-serializes
  whatever is currently on the status bar (including agent commentary set
  via `/say`) through the existing JSON string sanitizer, so no additional
  escaping was needed for this endpoint.
- While the game is paused, `game_time` is frozen, so a `/say` message's
  4-second timeout doesn't tick down — the game's own lower-priority status
  messages stay suppressed until about 4 seconds of *game* time after
  unpausing, not 4 seconds of wall-clock time.
