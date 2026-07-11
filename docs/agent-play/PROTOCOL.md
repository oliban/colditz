# Play Protocol — Brain/Hands Speedrun

How the main session (the "brain") runs a Colditz Escape speedrun campaign:
reads accumulated knowledge, dispatches narrow Haiku "hands" errands
(`/Users/fredriksafsten/games/colditz-escape/src/docs/agent-play/ERRANDS.md`), takes direct control at high-stakes moments,
and times every run. See `/Users/fredriksafsten/games/colditz-escape/src/docs/agent-play/DOSSIER.md` for game knowledge and
`/Users/fredriksafsten/games/colditz-escape/src/docs/AGENT-API.md` for the full API reference.

## Brain loop

1. Read `/Users/fredriksafsten/games/colditz-escape/src/docs/agent-play/DOSSIER.md` (mechanics/strategy knowledge) and the
   campaign files (`/Users/fredriksafsten/games/colditz-escape/campaign/map.md`, `/Users/fredriksafsten/games/colditz-escape/campaign/journal.md`,
   `/Users/fredriksafsten/games/colditz-escape/campaign/runs.md`) to establish current state and an escape plan
   (target prisoner, route, required items).
2. Dispatch **one errand at a time** (never multiple concurrent errands —
   the game is single-focus) using a template from `/Users/fredriksafsten/games/colditz-escape/src/docs/agent-play/ERRANDS.md`, filled in
   for the immediate next sub-goal.
3. Integrate the errand's ≤10-line report: update the plan, note any
   surprises. The campaign files (map.md/journal.md) are already updated
   by the errand itself per its report contract. Report is appended to `/Users/fredriksafsten/games/colditz-escape/campaign/map.md` and `/Users/fredriksafsten/games/colditz-escape/campaign/journal.md`.
4. Repeat from step 2 until the plan calls for a direct-control moment (see
   below) or a run milestone (start/stop).

Early runs are expected to be slow scout runs that build `map.md`; later
runs are execution runs racing the route already mapped. Route knowledge
persists across runs — the game's maps/item positions/patrols are static,
so this is fair (a replaying human would accumulate the same knowledge).

## Direct-control triggers

The brain takes over directly (curling the API from the main session,
not delegating to a Haiku errand) for high-stakes sequences where a
mistake is costly and hands-off delegation isn't worth the risk:

- **Consuming a keyed item** — lock-picks, security keys, tunnel tools,
  the candle: anything single-use where a wasted attempt costs a
  resource. The brain selects the item and presses `action` itself.
- **Tunnel entry** — entering an opened tunnel (candle-gated, one-way
  commitment into unmapped territory).
- **The final escape run** — the sequence from "route is known and ready"
  through crossing whatever counts as the escape boundary and `/state`
  reporting that prisoner's `escaped: true`.

Everything else (scouting, fetching, probing doors, repositioning) is
errand work.

## Run procedure

A "run" is one fresh game launch, timed start to finish. Order matters:

1. `pkill colditz` — kill any existing game process. Never do this
   *during* an in-progress timed run; only when starting a fresh one.
2. Launch a fresh process from the game directory:
   ```
   cd "/Users/fredriksafsten/games/colditz-escape/Colditz Escape" && ./colditz -a 8765 &
   ```
3. Dismiss the intro: `POST /input {"key":"action","ms":200}`, then poll
   `GET /state` until `intro:false`. Repeat the tap if it's still `true`.
4. Start recording: `/Users/fredriksafsten/games/colditz-escape/campaign/recorder.sh start <N>` (built in a later
   task — once it exists, this must run *before* the timer starts, so no
   run footage is missing).
5. Start the clock: `/Users/fredriksafsten/games/colditz-escape/campaign/run-timer.sh start` (call this at the first
   `/state` observed with `intro:false` — do not start it before the
   intro is actually dismissed, and do not delay it after).
6. Play: brain loop (above) drives errands and direct control until a
   prisoner's `/state` shows `escaped:true`.
7. Stop the clock immediately on the first `escaped:true` observed:
   `/Users/fredriksafsten/games/colditz-escape/campaign/run-timer.sh stop escaped "<route summary>"`.
8. Stop recording: `/Users/fredriksafsten/games/colditz-escape/campaign/recorder.sh stop`.
9. Journal the run in `/Users/fredriksafsten/games/colditz-escape/campaign/journal.md` (outcome, lessons) and fill in
   `/Users/fredriksafsten/games/colditz-escape/campaign/runs.md`'s `video` column with the recording path for that
   row (the run-timer already appended the row with RTA/IGT/outcome/route;
   video is added after `recorder.sh stop` reports its output path).
   Update the `PB` marker if this is the new best `escaped` run in its
   category.

If the run ends some other way (arrest, abandoned, game crash) before
`escaped:true`, still run `/Users/fredriksafsten/games/colditz-escape/campaign/run-timer.sh stop <outcome> "<note>"` and
recorder stop/journal it — every timed run gets a row, not just wins.

**Errand dispatch note:** once `/Users/fredriksafsten/games/colditz-escape/campaign/cmd.sh` exists (a later task),
every API call the brain or an errand makes — direct-control curls
included — goes through it instead of raw curl, so the dashboard/event
log captures every action. Until then, call the API directly with curl as
shown in `/Users/fredriksafsten/games/colditz-escape/src/docs/agent-play/ERRANDS.md`'s cheat-sheet.

## No-pause rule

**Never pause during a timed run.** Pausing by ANY means is forbidden:
do not call `/control {"pause":...}` or `/input {"key":"pause"}`. Pausing
freezes `game_time` (confirmed in `/Users/fredriksafsten/games/colditz-escape/src/docs/AGENT-API.md`), which would corrupt IGT
and isn't something a real speedrun attempt could do either. Pausing
is permitted only in explicitly untimed scout/calibration sessions (journal
them as such), never between a `/Users/fredriksafsten/games/colditz-escape/campaign/run-timer.sh start` and its
matching `stop`.

## Split announcements

Announce meaningful milestones via `/say` as they happen during a timed
run (spectator-visible on the game's status bar) — e.g. `"QUARTERS
CLEARED 0:42"`, `"KEY ACQUIRED 1:15"`, `"TUNNEL ENTRY 2:03"`. Keep them
short (127-char cap, plain ASCII) and reserve them for real milestones,
not routine navigation.

## IGT:RTA ratio (measured, Step 4 validation)

Measured against the live game process on port 8765 by sampling
`/state`'s `game_time` twice, 10 real seconds apart, while unpaused:

```
RTA elapsed: 10s (wall clock)
IGT elapsed: 10051 (game_time units, unpaused)
ratio: ~1.005 IGT-units per RTA-ms  →  IGT runs ≈ 1x RTA (essentially 1:1)
```

`game_time` is in milliseconds and advances at real-clock speed while the
game is unpaused (it only stops when paused — see `/Users/fredriksafsten/games/colditz-escape/src/docs/AGENT-API.md`'s
`/control` caveats). So RTA and IGT should track each other closely for
any run with no pauses; a run-timer test with a 5s sleep confirmed this
directly (RTA 0:05, IGT 0:05 — see `/Users/fredriksafsten/games/colditz-escape/campaign/runs.md` rows flagged
`timer-test`). Do not assume this ratio holds if `/control` is ever used
mid-run (it isn't, per the no-pause rule above) — it was measured
specifically to confirm IGT is a trustworthy secondary clock, not just a
duplicate of RTA by coincidence of this one sample.
