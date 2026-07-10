# Agent API — Making Colditz Escape Playable by AI Agents

Date: 2026-07-10
Status: Approved
Branch: `agent-api` (off `macos-native-build`)

## Purpose

Let an AI agent (Claude) play the game — perceiving the screen, reading game
state, and sending inputs — while a human watches the normal game window.
Primary use is entertainment ("can Claude escape from Colditz?"), not
benchmarking. The game runs in **real time** by default; pause and slow-motion
are fallback options if real-time play proves hopeless.

## Approach

An HTTP server embedded in the game process (chosen over external
screen-capture/keystroke automation, which needs fragile macOS permissions and
offers no structured state; and over a hybrid split, which shares those
drawbacks for no meaningful savings).

## Architecture

- New files: `agent_api.c`, `agent_api.h`, vendored `stb_image_write.h`
  (public domain single-header PNG encoder).
- POSIX sockets; compiled on macOS/Linux, `#ifdef`'d out elsewhere.
- Off by default; enabled with `-a [port]` (default 8765), bound to
  **127.0.0.1 only**.
- **No threads.** The listening socket is non-blocking and serviced by a
  recurring `glutTimerFunc` tick (~16 ms). A timer (rather than an idle-func
  hook) keeps the API alive across the game's idle-callback swaps: menus,
  static pictures, pause screens. At most one request is serviced per tick.
- **Frame capture:** at the end of `glut_display`, before `glutSwapBuffers`,
  `glReadPixels` copies the frame into a persistent buffer (only when the API
  is enabled). `/screen` encodes that cache to PNG — works even while paused.
- **Input injection:** `/input` resolves symbolic key names ("left",
  "action") through the user's configured bindings and writes into the same
  `key_down[]` / `key_readonce[]` arrays the keyboard callback uses, held for
  a requested duration, managed by a queue in the timer tick.

## API contract

| Endpoint | Behavior |
|---|---|
| `GET /screen` | PNG of the last rendered frame. |
| `GET /state` | JSON: game clock + pause state; active prisoner (nation, room id, x/y, decoded state flags, fatigue, walk/run); inventory with selected item; other prisoners' room/position/state; status-bar message; screen mode (game / menu / static pic); input queue depth. |
| `POST /input` | `{"key":"left","ms":400}` — symbolic key + hold duration; queued; returns immediately with queue depth. |
| `POST /control` | `{"pause":true|false}` — freeze/resume via the game's existing pause path. `{"speed":N}` slow-motion is designed in but deferred until real-time play is proven unplayable. |
| `POST /say` | `{"text":"..."}` — shows agent commentary in the game status bar, for spectators. |

Symbolic key names: `up down left right action pickup drop inv_left inv_right
walk_run sleep stooge pause escape prisoner_1 prisoner_2 prisoner_3 prisoner_4`.

## Error handling

- Malformed JSON → 400 with reason; unknown endpoint → 404; unknown key name
  → 400 listing valid names.
- Request size cap: 4 KB. One request per tick; a misbehaving client can only
  slow itself, never the game loop.
- Port already in use → warning on stdout, game runs normally without API.

## Testing

`tests/agent-api.sh`: launches the game with `-a`, asserts `/screen` returns
a valid PNG of the window's dimensions, `/state` returns parseable JSON with
required fields, an `/input` right-hold changes prisoner x-position,
`/control` pause freezes `game_time`. Final acceptance: Claude plays several
minutes of the game unassisted while the user watches.

## Success criterion

Claude can, unassisted: leave the starting area, pick up an item, and attempt
an escape — in real time, observable in the live game window.

## Out of scope (v1)

- Semantic/pathfinding commands ("walk to chapel") — key-level input only.
- Guard positions in `/state` (add later if useful).
- Speed factor implementation (schema reserved).
- Windows/PSP support.
- MCP server wrapper (curl is the interface; MCP can wrap the HTTP API later
  without game changes).
