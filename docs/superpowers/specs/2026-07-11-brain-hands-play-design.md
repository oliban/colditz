# Brain/Hands Play Architecture — Fable Strategist + Haiku Executors

Date: 2026-07-11
Status: Approved
Depends on: agent-api branch (endpoints /state /room /walk /input /screen /say /control)

## Purpose

Play Colditz Escape with a two-tier agent system: the main session (Fable)
acts as the strategic brain with deep game knowledge; cheap Haiku subagents
execute narrow errands (scouting, fetching, probing). The human watches the
live game window; the brain narrates strategy via /say.

## Fair play

- Knowledge sources: the game's bundled README/FAQ, the project website, and
  web research on the original 1991 game (manuals, walkthroughs, forum lore).
  **Source-code and data-file mining is banned.** Every dossier claim cites
  its source.
- Runtime knowledge only via the fair-play API (no door status before
  trying; /room geometry and /screen only).
- Campaign memory across sessions is fair: the game is static (fixed maps,
  fixed item positions in OBS.BIN, scripted patrols in ROUTES.BIN; only
  guard reset timing is randomized — verified game.c:1384), so a replaying
  human accumulates the same knowledge.

## Components

### 1. Knowledge dossier — `docs/agent-play/DOSSIER.md` (in repo)

Built once by a research subagent (sonnet; WebSearch/WebFetch + the bundled
README.html only). Sections: mechanics reference (doors/tools/tunnels,
appell/curfew, fatigue/sleep, uniforms/passes/papers, stooge, guards);
strategies and escape lore from period sources; explicit "unknown — discover
in play" list; source citations per claim.

### 2. Campaign memory — `/Users/fredriksafsten/games/colditz-escape/campaign/`

- `map.md`: per-room sections (room ID: exits and destinations, door trials
  with dates/results, items seen/taken, guard sightings, stairs) plus a
  castle-graph summary at top.
- `journal.md`: append-only log (errands, outcomes, arrests, lessons).
- Haiku errands append raw entries; the brain curates/compacts periodically.
- New sessions start by reading both files.

### 3. Errand protocol (Haiku subagents)

Every errand dispatch contains:
1. Fixed API cheat-sheet header (endpoints, walk→poll→nudge loop, direction
   semantics, /say usage).
2. ONE narrow objective. Types: **scout** (explore N rooms, map them),
   **fetch** (go to room X, pick up item Y), **probe** (try door at tile T,
   optionally with tool selected), **reposition** (move prisoner P to room
   N), **emergency** (all prisoners to courtyard).
3. Hard limits: ~30 tool calls; stop conditions (APPELL/CURFEW in message,
   arrest, active chase, objective done).
4. Standing orders: appell response (head to courtyard, report); never
   restart/kill the game; narrate key moments via /say.
5. Report contract: append findings to map.md/journal.md; return ≤10-line
   summary (rooms visited, discoveries, prisoner end state, anomalies).

Screenshots are read by Haiku only — never returned to the brain.

### 4. Brain loop (main session, Fable)

Read dossier + campaign → maintain escape plan (target prisoner, route,
required items) → dispatch one errand at a time → integrate report → update
plan → next errand. The brain takes direct control (curl from main session)
only for high-stakes sequences: consuming keys on graded doors, tunnel
entry, final escape run. The brain narrates strategy via /say.

## Speedrun framing (the goal)

The objective is to **speedrun the escape**. Every run is timed.

- **Run definition:** one fresh game launch. Timing starts when prisoner
  control is gained (intro dismissed, first /state with intro:false) and
  stops at the first prisoner with `escaped: true` in /state (category:
  Any% / first-out). A "full house" category (all four out) can be added
  later.
- **Clocks:** RTA (wall clock — brain timestamps run start/end via `date`)
  and IGT (in-game time — `game_time` from /state, ms). Both recorded.
- **Rules:** no /control pause during timed runs (it freezes IGT — allowed
  only in untimed scout runs, and noted as such); fresh game process per
  run; route knowledge from previous runs is allowed (standard speedrun
  practice); fair-play API constraints always apply.
- **Run log:** `campaign/runs.md` — one row per run: #, date, category,
  RTA, IGT, outcome (escaped prisoner / arrest / abandoned), route summary,
  personal best marker.
- **Strategy consequence:** early runs are slow scout runs (build map.md);
  later runs are execution runs racing the optimized route. The brain
  announces splits via /say ("QUARTERS CLEARED 0:42") for the spectator.

## Failure handling

- Errand ends in arrest/blocked: journal it, switch prisoner or re-plan.
- Errand overruns: tool budget caps damage; re-dispatch narrower.
- Game process dies: relaunch `./colditz -a 8765` from the game dir;
  campaign files retain knowledge; game state restarts (v1 limitation —
  in-game save files not yet wired into the API).
- APPELL mid-errand: standing order (above).

## Validation

1. Dry-run: one scout errand; verify map.md entries match reality and the
   summary is usable.
2. Full campaign session with the user watching.

## Wrap-up (phase C, after a successful campaign)

Freeze the proven templates into a `colditz-play` skill: dossier/campaign
paths, errand templates, brain-loop instructions, relaunch procedure. Until
then the protocol lives in this spec and the plan.

## Out of scope (v1)

- Source-code-derived knowledge (banned, fair play).
- In-game save/load integration.
- Multiple concurrent errands (one at a time; the game is single-focus).
- New C endpoints — this design uses the existing API as-is.
