# Errand Templates — Haiku Hands

Fixed prompt blocks the brain (main session) fills in and dispatches to a
cheap Haiku subagent for one narrow objective at a time. Copy the relevant
template verbatim, fill the `{PLACEHOLDER}` slots, and dispatch. Do not
paraphrase the standing orders or report contract — they are load-bearing
for keeping errands cheap, safe, and legible to the brain's campaign files.

See `/Users/fredriksafsten/games/colditz-escape/src/docs/agent-play/PROTOCOL.md` for the brain loop that dispatches these,
and `/Users/fredriksafsten/games/colditz-escape/src/docs/AGENT-API.md` for full endpoint semantics.

## API cheat-sheet (condensed from docs/AGENT-API.md)

Base URL: `http://127.0.0.1:8765`. All calls go through `/Users/fredriksafsten/games/colditz-escape/campaign/cmd.sh`
once it exists (Task 3) — that wraps the curl and logs it for the
dashboard; until then, call the endpoints directly with curl exactly as
shown.

| Endpoint | Method | Body | Use |
|---|---|---|---|
| `/state` | GET | — | Poll game clock, `intro`/`paused`/`menu` flags, all 4 prisoners' room/x/y/direction/state/inventory/escaped/dead, status-bar `message`, and `walk` progress (`idle/walking/arrived/blocked`). Your primary sensor. |
| `/room` | GET | — | Current prisoner's room: floor grid (`.`=void, `#`=floor, `E`=exit), `exits` tile list, `my_tile`. Only the current room — no peeking elsewhere. |
| `/walk` | POST | `{"tile":[x,y]}` or `{"exit":N}` or `{"cancel":true}` | Autonomous pathing within the current room. Prefer this over manual `/input` direction spam for any multi-tile move. |
| `/input` | POST | `{"key":"...","ms":N}` | One symbolic keypress held for `ms` (16-10000). Keys: `up down left right action pickup drop inv_left inv_right walk_run sleep stooge pause escape prisoner_1..4`. Use for the final "nudge" through a doorway, and for `action`/`pickup`/`drop`/inventory keys `/walk` can't do. |
| `/screen` | GET | — | PNG of the last rendered frame. Use sparingly (it's for you, not the brain — never return image bytes in your report). |
| `/say` | POST | `{"text":"..."}` | Puts text on the in-game status bar (spectator-visible). Use for key moments only — it overrides routine messages but is capped at 127 ASCII chars and one message at a time. |

**Walk → poll → nudge loop** (the standard movement pattern):
1. `GET /room` to see the grid and your `my_tile`, or `GET /state` if you
   already know where you're going.
2. `POST /walk` with `{"tile":[x,y]}` or `{"exit":N}`.
3. Poll `GET /state`'s `walk` field until it's `arrived` or `blocked`
   (not `walking`) — don't fire more commands while a walk is in flight.
4. `arrived` at an exit tile often means you're at the threshold, not
   through it yet — send one short follow-up `/input` in the same
   direction (e.g. `{"key":"right","ms":300}`) to actually cross into the
   next room.
5. `blocked` means a locked door, furniture, or a guard body-block —
   indistinguishable at this level. Check `/state`'s `message` for a hint,
   then decide: try a tool, try another route, or give up on this path.

**Direction rule:** `right` = +x, `left` = -x, `down` = +y, `up` = -y
(origin top-left, same as the `/room` grid's row order).

**Narration:** use `/say` to announce what you're doing at a moment a
spectator would want to see it (arriving somewhere new, trying a locked
door, an item pickup, a guard sighting) — not every single step.

---

## Standing orders (apply to every errand below)

- **Hard limit:** ≤30 tool calls total. Stop and report before you'd
  exceed it, even mid-objective.
- **Stop conditions:** APPELL or CURFEW appears in `/state`'s `message`;
  the prisoner is arrested (`dead`, or a state-flag/message indicating
  custody); a guard chase is active; or the objective is done. Stop
  immediately on any of these and report — do not try to push through.
- **APPELL/CURFEW response:** head the prisoner toward the courtyard
  (best-effort via `/walk`) and report; do not fight it.
- **Never** kill or restart the game process, and never pause by ANY means
  (`/control {"pause":...}` or `/input {"key":"pause"}`) during a timed run
  — pause is the brain's call only, and never during a timed run.
- **Narrate** key moments via `/say` as you go (see cheat-sheet above).

## Report contract (apply to every errand below)

Before returning, append your findings to the campaign files:
- `/Users/fredriksafsten/games/colditz-escape/campaign/map.md` — new/updated `### Room <id>` sections in the format
  from that file's legend (exits/items/guards).
- `/Users/fredriksafsten/games/colditz-escape/campaign/journal.md` — one `- [run N | <ts>] <event>` line per notable
  event (use `errand` for `run N` if this wasn't inside a timed run; get
  the timestamp from `date`).

Then return a report of **10 lines or fewer** covering exactly: rooms
visited, discoveries (items/exits/guards), the prisoner's end state
(room, escaped/dead/arrested/free), and any anomalies (blocked doors,
unexpected messages, tool-call budget hit). No screenshots, no verbose
step-by-step narration — the brain only needs the summary and the files.

---

## Template: scout

Explore unmapped territory and record what's there. No item pickup, no
door-forcing beyond a single "does it open" try.

```
You are a Haiku hands-agent for a Colditz Escape speedrun. Read the API
cheat-sheet, standing orders, and report contract in
/Users/fredriksafsten/games/colditz-escape/src/docs/agent-play/ERRANDS.md before doing anything else.

OBJECTIVE: Scout from {START_ROOM_OR_CURRENT} outward, mapping up to
{ROOM_LIMIT} new rooms. For each room: record the floor grid's exits,
any items visible, and any guards seen. {OPTIONAL_CONSTRAINTS, e.g. "do
not open any door" or "stay on the upper floor"}.

PRISONER: {PRISONER} (use prisoner_N to switch if not already selected).

HARD LIMITS: {LIMITS, default "≤30 tool calls"}.

Follow the standing orders and report contract in /Users/fredriksafsten/games/colditz-escape/src/docs/agent-play/ERRANDS.md exactly.
```

## Template: fetch

Go get one specific item and bring it back (or confirm it's carried).

```
You are a Haiku hands-agent for a Colditz Escape speedrun. Read the API
cheat-sheet, standing orders, and report contract in
/Users/fredriksafsten/games/colditz-escape/src/docs/agent-play/ERRANDS.md before doing anything else.

OBJECTIVE: Navigate to room {ROOM_ID}, tile {TILE_XY} (per /Users/fredriksafsten/games/colditz-escape/campaign/map.md), and
pick up {ITEM_NAME}. {OPTIONAL: "Then return to room {RETURN_ROOM}."}
Confirm the pickup via /state's inventory field before reporting done.

PRISONER: {PRISONER}.

HARD LIMITS: {LIMITS, default "≤30 tool calls"}.

Follow the standing orders and report contract in /Users/fredriksafsten/games/colditz-escape/src/docs/agent-play/ERRANDS.md exactly.
```

## Template: probe

Test one specific door/exit to learn whether it's locked and what it
takes to open, without committing to going further.

```
You are a Haiku hands-agent for a Colditz Escape speedrun. Read the API
cheat-sheet, standing orders, and report contract in
/Users/fredriksafsten/games/colditz-escape/src/docs/agent-play/ERRANDS.md before doing anything else.

OBJECTIVE: In room {ROOM_ID}, walk to the exit at tile {TILE_XY} and
attempt to open it. {OPTIONAL: "First select {TOOL_NAME} via inv_left/
inv_right, confirmed by /state's selected field."} Try the action key
once; record the resulting /state message verbatim. Do NOT consume a
second tool or force it repeatedly — one clean attempt only, then
report what happened (opened / locked-message / tool required / no
change).

PRISONER: {PRISONER}.

HARD LIMITS: {LIMITS, default "≤15 tool calls"}.

Follow the standing orders and report contract in /Users/fredriksafsten/games/colditz-escape/src/docs/agent-play/ERRANDS.md exactly.
```

## Template: reposition

Move a specific prisoner from wherever they are to a target room, via
known map.md routes only (no new exploration).

```
You are a Haiku hands-agent for a Colditz Escape speedrun. Read the API
cheat-sheet, standing orders, and report contract in
/Users/fredriksafsten/games/colditz-escape/src/docs/agent-play/ERRANDS.md before doing anything else.

OBJECTIVE: Move prisoner {PRISONER} from their current room to room
{TARGET_ROOM}, using the route recorded in /Users/fredriksafsten/games/colditz-escape/campaign/map.md (do not
explore new territory; if the known route is blocked, stop and report
rather than improvising a detour).

PRISONER: {PRISONER}.

HARD LIMITS: {LIMITS, default "≤20 tool calls"}.

Follow the standing orders and report contract in /Users/fredriksafsten/games/colditz-escape/src/docs/agent-play/ERRANDS.md exactly.
```

## Template: emergency

All-prisoners-to-safety response, used when the brain calls for an
immediate stand-down (e.g. before switching strategies, or ending a
session cleanly) rather than reacting to an in-game APPELL.

```
You are a Haiku hands-agent for a Colditz Escape speedrun. Read the API
cheat-sheet, standing orders, and report contract in
/Users/fredriksafsten/games/colditz-escape/src/docs/agent-play/ERRANDS.md before doing anything else.

OBJECTIVE: For each of the 4 prisoners (prisoner_1..prisoner_4), check
/state; if not already in the courtyard/a safe common area, walk them
there by the shortest known route from /Users/fredriksafsten/games/colditz-escape/campaign/map.md. Do not pick up items or
try doors along the way — this is a stand-down, not an errand. Report
each prisoner's final room and state.

HARD LIMITS: {LIMITS, default "≤30 tool calls total across all 4"}.

Follow the standing orders and report contract in /Users/fredriksafsten/games/colditz-escape/src/docs/agent-play/ERRANDS.md exactly.
```
