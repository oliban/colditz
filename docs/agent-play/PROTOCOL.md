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
4. **Check/announce REC state before a PB attempt.** Recording is
   optional and toggled per run (dashboard REC button, default OFF, or
   `/Users/fredriksafsten/games/colditz-escape/campaign/recorder.sh status`
   from the CLI) — see `## Recording` below for full mechanics. Before
   starting the timer on any run intended as a serious/PB attempt: confirm
   REC is ON (toggle it on if it isn't, and confirm via `status` that it
   actually started — `recorder.sh`/the dashboard fail loudly if Screen
   Recording permission isn't granted) and announce the state via
   `/say` or the reasoning log (e.g. "REC ON — PB attempt" or "REC OFF —
   practice run"). Per `campaign/RULES.md`, **a run only counts as an
   official/PB run if REC was on for the run's full duration**, so start
   recording *before* the run timer and stop it *after* the run timer.
   Practice/scout runs may skip recording entirely — journal them as
   non-qualifying if so.
5. Start the clock: `/Users/fredriksafsten/games/colditz-escape/campaign/run-timer.sh start` (call this at the first
   `/state` observed with `intro:false` — do not start it before the
   intro is actually dismissed, and do not delay it after).
6. Play: brain loop (above) drives errands and direct control until a
   prisoner's `/state` shows `escaped:true`.
7. Stop the clock immediately on the first `escaped:true` observed:
   `/Users/fredriksafsten/games/colditz-escape/campaign/run-timer.sh stop escaped "<route summary>"`.
8. If recording was on, stop it: dashboard REC toggle, or
   `/Users/fredriksafsten/games/colditz-escape/campaign/recorder.sh stop`.
9. Journal the run in `/Users/fredriksafsten/games/colditz-escape/campaign/journal.md` (outcome, lessons, and
   whether it was PB-eligible per the REC-coverage rule) and, if recorded,
   fill in `/Users/fredriksafsten/games/colditz-escape/campaign/runs.md`'s `video` column with the recording
   path for that row (the run-timer already appended the row with
   RTA/IGT/outcome/route; video is added after `recorder.sh stop` reports
   its output path). Update the `PB` marker only if this run is both the
   new best `escaped` time in its category AND was recorded start-to-stop
   (see `campaign/RULES.md`).

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

## Recording

Video proof is **optional per run**, toggled on/off — it is NOT wired
into `run-timer.sh` and does not start/stop automatically. Default is
OFF; you must explicitly turn it on.

- **Dashboard toggle (normal path):** the spectator dashboard
  (`http://127.0.0.1:8900/`) has a REC button next to the timers. Click
  once to start (posts `/api/record/start`, which derives the next run
  number from `campaign/runs.md`'s row count), click again to stop
  (`/api/record/stop`). The button shows a red pulsing `● REC` plus the
  output filename while recording, and the dashboard polls
  `/api/record/status` every 2s so it can never drift from the truth
  (e.g. if the recorder process dies mid-run). Any failure (most likely
  missing OS permission — see below) is surfaced immediately in an amber
  warning strip with the exact error text. Every successful start/stop
  also appends a `RECORDING STARTED`/`RECORDING STOPPED` event to
  `campaign/live-log.jsonl`, so it shows up in the reasoning feed and, if
  the dashboard itself is in-frame, in the recording too.
- **CLI equivalent:** `campaign/recorder.sh start <N>` / `stop` / `status`,
  same underlying mechanism the dashboard shells out to (macOS
  `screencapture -v`, region set via the `REGION` variable at the top of
  the script — empty/default records the whole main display; set it to
  `"x,y,w,h"` to cover a specific rect, e.g. game window + dashboard
  window side by side, for a proof-grade compliant recording per
  `campaign/RULES.md`). Output: `campaign/recordings/run-N.mov`. `status`
  prints `state: recording|idle`, `file:`, and `error:` lines.
- **Permission requirement:** recording requires macOS **Screen
  Recording** permission, granted to whichever app/terminal is running
  `recorder.sh` (directly, or indirectly via the dashboard's
  `server.py`). Grant it at **System Settings -> Privacy & Security ->
  Screen & System Audio Recording**. Without it, `screencapture -v` exits
  immediately or produces a 0-byte file; `recorder.sh` detects this
  within ~3 seconds, prints a loud terminal warning naming the exact
  System Settings path, cleans up its PID file (no stale state), and
  reports the failure through `status`'s `error:` field — which the
  dashboard surfaces in its amber warning strip. A permission failure
  does not block play; it just means the run cannot be PB-eligible (see
  `campaign/RULES.md`) until permission is granted.
- **PB eligibility:** per `campaign/RULES.md`, a run only counts as an
  official/PB run if REC was ON for the run's *entire* duration (timer
  start to timer stop, no gaps) — see Step 4/8 of the run procedure
  above.

## Spectator dashboard

A live scoreboard for a human watching from across the room, and for
side-by-side speedrun recordings next to the game window. It shows the
game screen (refreshed ~500ms), big RTA/IGT timers, a REASONING feed
("Fable's brain" — `reason`/`say`/`split`/run-start/run-stop events from
`log.sh`), and a COMMANDS feed ("Haiku's hands" — every `/input`, `/walk`,
`/say`, `/control` call made through `cmd.sh`, rendered as colored chips).

**Start it:**

```
python3 /Users/fredriksafsten/games/colditz-escape/campaign/dashboard/server.py &
```

**URL:** http://127.0.0.1:8900/ (binds to localhost only; open in a
browser window placed next to the game window for recording).

Canonical source for these files is versioned in the repo at
`/Users/fredriksafsten/games/colditz-escape/src/docs/agent-play/dashboard-src/`
(`cmd.sh`, `log.sh`, `server.py`, `index.html`, `recorder.sh`, `RULES.md`)
— the live copies that actually run are under `campaign/` (outside the
repo) and `campaign/dashboard/`; keep both in sync if either is edited.

**`cmd.sh`/`log.sh` are MANDATORY, not optional, for every brain/errand
action during a run.** The dashboard and any recording only show what
flows through `campaign/cmd.sh` (game API calls) and `campaign/log.sh`
(reasoning/say/split/run-start/run-stop events) — a raw `curl` to
`127.0.0.1:8765` or a thought never written via `log.sh` is invisible to
a spectator and to the recording, even though it still affects the game.
Direct-control moments (see above) go through `cmd.sh` exactly like
errand-dispatched actions; there is no exemption for the brain acting
directly.
