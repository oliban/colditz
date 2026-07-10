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
| `/screen` | GET | — | 200 `image/png`, or 404 if no frame captured yet | PNG of the last rendered frame (captured from the GL front buffer each display call). Works even while paused. |
| `/input` | POST | `{"key":"left","ms":400}` | 202 `{"queued":N}`, 400 on bad/missing key or full queue | Symbolic key + hold duration (ms, clamped 16–10000) resolved through the loaded key bindings, queued and injected via the same `key_down[]`/`key_readonce[]` path the real keyboard callback uses. Valid key names: `up down left right action pickup drop inv_left inv_right walk_run sleep stooge pause escape prisoner_1 prisoner_2 prisoner_3 prisoner_4`. |
| `/control` | POST | `{"pause":true\|false}` | 200 `{"paused":bool}`, 400 if body missing `"pause"` or the input queue is full | Freeze/resume via the game's own `KEY_PAUSE` key path (see caveats below). Idempotent: if the game is already in the requested state, no key is injected and no toggle occurs. `{"speed":N}` slow-motion is designed in but not implemented in v1. |
| `/say` | POST | `{"text":"..."}` | 200 `{"ok":true}`, 400 if `text` missing | Displays the given text on the in-game status bar (visible to spectators watching the game window), at a priority that overrides routine room/props messages. |

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
```

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
  small delay (up to the key's `ms` hold, ~100 ms) between the `/control`
  response and the game actually reaching the requested `paused` state.
  `/control`'s idempotence guard (`want != paused`) compares against the
  *current* `paused` flag at request time, so a burst of repeated identical
  calls issued faster than that delay could each see the old state and
  re-inject the toggle; callers polling `/state` before re-issuing
  `/control` avoid this.
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
