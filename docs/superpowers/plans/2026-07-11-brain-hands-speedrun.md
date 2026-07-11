# Brain/Hands Speedrun Play System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fable-as-brain + Haiku-as-hands system that speedruns Colditz Escape via the agent API, with every run timed (RTA + IGT) and route knowledge persisting across runs.

**Architecture:** No new C code. A researched knowledge dossier feeds the brain; campaign files on disk carry map/journal/run-log; Haiku subagents execute narrow errands from fixed templates; a bash run-timer standardizes timing; the brain (main session) plans, dispatches, and takes direct control at high-stakes moments.

**Tech Stack:** Markdown docs, bash + curl + python3, the existing agent API (/state /room /walk /input /screen /say /control), Agent-tool subagents (sonnet for research, haiku for errands).

## Global Constraints

- Fair play: dossier sources = bundled README/FAQ + project website + web research on the original 1991 game. **Source-code/data-file mining banned.** Claims carry citations.
- Runtime knowledge only via the API; door status discovered by trying.
- Speedrun rules: timing starts at first /state with intro:false after a fresh launch; stops at first prisoner `escaped: true`. RTA = wall clock, IGT = /state game_time (ms). **No /control pause during timed runs.** Fresh game process per run; carried route knowledge allowed.
- Campaign lives at `/Users/fredriksafsten/games/colditz-escape/campaign/` (outside the repo). Play docs live in the repo at `docs/agent-play/`.
- Game runs from `/Users/fredriksafsten/games/colditz-escape/Colditz Escape/` as `./colditz -a 8765`; never `pkill` during a run except to start a fresh run.
- Errand subagents: haiku, ONE objective, ≤30 tool calls, stop on APPELL/arrest/chase/done, append findings to campaign files, ≤10-line report.

---

### Task 1: Knowledge dossier (research)

**Files:**
- Create: `docs/agent-play/DOSSIER.md`

**Interfaces:**
- Produces: DOSSIER.md read by the brain at campaign start; sections exactly: `## Mechanics`, `## Strategies & lore`, `## Unknowns to discover in play`, `## Sources`.

**Owner:** research subagent (sonnet, WebSearch/WebFetch enabled).

- [ ] **Step 1: Dispatch research** with this brief (verbatim):

> Research the video game "Escape from Colditz" (1991, Amiga, Digital Magic Software) and its modern engine remake "Colditz Escape" (aperture-software.github.io/colditz-escape). ALLOWED sources: the remake's bundled README (file `/Users/fredriksafsten/games/colditz-escape/Colditz Escape/README.html` — read it), the project website/FAQ, and any web material about the original game (manual scans, reviews, walkthroughs, forum threads, YouTube descriptions/transcripts). BANNED: the game's source code and data files — do not open anything under `/Users/fredriksafsten/games/colditz-escape/src/` except `docs/AGENT-API.md` for API context.
> Write `/Users/fredriksafsten/games/colditz-escape/src/docs/agent-play/DOSSIER.md` with sections: `## Mechanics` (doors/keys/lockpick grades, tunnels & tools, candle, appell/roll-call and curfew behavior, fatigue/sleep, uniforms/passes/papers, stooge, guard behavior & pursuit, solitary, items list), `## Strategies & lore` (known escape approaches, tunnel locations IF found in period sources — cite them, item location hints from walkthroughs, timing/route advice), `## Unknowns to discover in play` (explicit list of things sources didn't settle), `## Sources` (numbered; every nontrivial claim in the doc carries [N] citations).
> Facts you may take as given (already verified in play): movement is 8-directional isometric; the remake's API exposes /state /room /walk; the four prisoners start in their national quarters (British room 251, upstairs).
> Report back: 5-line summary + the file path.

- [ ] **Step 2: Fair-play audit** — controller checks: `grep -iE "game\.c|main\.c|OBS\.BIN|ROUTES\.BIN|colditz\.h|source code" docs/agent-play/DOSSIER.md` → only permissible mentions (e.g. "the remake's docs"); spot-check 5 claims have [N] citations; `## Unknowns` non-empty.
- [ ] **Step 3: Commit** — `git add docs/agent-play/DOSSIER.md && git commit -m "docs: fair-play game knowledge dossier"`

### Task 2: Campaign scaffolding, errand templates, run timer

**Files:**
- Create: `/Users/fredriksafsten/games/colditz-escape/campaign/map.md`, `campaign/journal.md`, `campaign/runs.md`, `campaign/run-timer.sh` (outside repo)
- Create (in repo): `docs/agent-play/PROTOCOL.md`, `docs/agent-play/ERRANDS.md`

**Interfaces:**
- Produces: `run-timer.sh start|stop <outcome> <route-note>` (start caches epoch+IGT0 in `campaign/.run-active`; stop appends a row to runs.md and prints it); ERRANDS.md templates referenced verbatim by the brain; PROTOCOL.md brain-loop + run procedure.

**Owner:** implementation subagent (sonnet).

- [ ] **Step 1: campaign skeletons.** `map.md`: header + `## Castle graph` (empty) + `## Rooms` (empty) + legend explaining per-room entry format:
  ```
  ### Room <id> (<floor/wing guess>)
  - exits: [tile] -> room <id> | untested | BLOCKED(<date>, tool?)
  - items: <name> @ [tile] (seen|taken <date>)
  - guards: <pattern notes>
  ```
  `journal.md`: header + legend (one `- [run N | <ts>] <event>` line per event). `runs.md`:
  ```
  | # | date | category | RTA | IGT | outcome | route | PB |
  |---|------|----------|-----|-----|---------|-------|----|
  ```
- [ ] **Step 2: run-timer.sh** (exact content):
  ```bash
  #!/bin/bash
  # Times speedrun attempts. start: cache clocks. stop <outcome> <route>: append runs.md row.
  set -u
  DIR="$(cd "$(dirname "$0")" && pwd)"
  API=http://127.0.0.1:8765
  igt() { curl -s -m 2 "$API/state" | python3 -c "import json,sys; print(json.load(sys.stdin)['game_time'])"; }
  case "${1:-}" in
    start)
      echo "$(date +%s) $(igt)" > "$DIR/.run-active"
      echo "run started: RTA clock 0:00, IGT0=$(cut -d' ' -f2 "$DIR/.run-active")ms" ;;
    stop)
      [ -f "$DIR/.run-active" ] || { echo "no active run"; exit 1; }
      read T0 IGT0 < "$DIR/.run-active"
      RTA=$(( $(date +%s) - T0 ))
      IGT_MS=$(( $(igt) - IGT0 ))
      N=$(( $(grep -c '^|' "$DIR/runs.md") - 1 ))  # rows minus header (separator excluded below)
      N=$(( N < 1 ? 1 : N ))                        # next run number
      printf '| %s | %s | any%% | %d:%02d | %d:%02d | %s | %s |  |\n' \
        "$N" "$(date +%F)" $((RTA/60)) $((RTA%60)) $((IGT_MS/60000)) $((IGT_MS%60000/1000)) \
        "${2:-unknown}" "${3:-}" >> "$DIR/runs.md"
      rm "$DIR/.run-active"
      tail -1 "$DIR/runs.md" ;;
    *) echo "usage: run-timer.sh start | stop <outcome> <route-note>"; exit 1 ;;
  esac
  ```
  `chmod +x`. NOTE: run-number arithmetic must count only data rows (exclude header AND `|---|` separator — implementer: verify with 0, 1, 2 rows and fix the count expression accordingly; the shown expression is a starting point and MUST be validated in Step 4).
- [ ] **Step 3: ERRANDS.md** — the fixed API cheat-sheet header (endpoints + walk→poll→nudge loop + direction rule + /say, condensed from docs/AGENT-API.md) followed by five templates (scout/fetch/probe/reposition/emergency), each a fill-in prompt block with: objective slot, hard limits (≤30 tool calls), stop conditions (APPELL/CURFEW/arrest/chase/done), standing orders (appell→courtyard; never kill/restart the game; /say key moments), report contract (append to map.md+journal.md with run number; ≤10-line return: rooms visited, discoveries, prisoner end state, anomalies). PROTOCOL.md — brain loop (read DOSSIER+campaign → plan → one errand at a time → integrate → repeat), direct-control triggers (consumable key use, tunnel entry, final escape run), run procedure (pkill colditz → launch `./colditz -a 8765` from game dir → dismiss intro via /input action → `run-timer.sh start` at first intro:false → play → at escaped:true `run-timer.sh stop escaped "<route>"` → journal), no-pause rule, split announcements via /say.
- [ ] **Step 4: Test run-timer** against the live game: launch game, `run-timer.sh start`, sleep 5, `run-timer.sh stop test "timer validation"`, assert appended row shows RTA 0:05±1 and IGT ≈ RTA (game runs ~1:1 when unpaused... verify actual ratio and note it), run twice more to validate run numbering (rows 2, 3). Remove test rows after validation, or keep flagged as `timer-test`.
- [ ] **Step 5: Commit repo docs** — `git add docs/agent-play && git commit -m "docs: errand templates, play protocol"` (campaign/ is outside the repo, not committed).

### Task 3: Event bus + spectator dashboard

**Files:**
- Create: `campaign/cmd.sh`, `campaign/log.sh`, `campaign/live-log.jsonl` (empty), `campaign/dashboard/server.py`, `campaign/dashboard/index.html`

**Interfaces:**
- Produces: `cmd.sh input '{"key":"right","ms":800}'` / `cmd.sh walk '{"exit":0}'` / `cmd.sh say '{"text":"..."}'` (logs then curls, prints response); `log.sh reason|say|split|run-start|run-stop "text"`; dashboard at http://127.0.0.1:8900.

**Owner:** implementation subagent (sonnet).

- [ ] **Step 1: cmd.sh** — `#!/bin/bash`; args: `<endpoint> <json-body|empty>`; appends `{"ts": <epoch.ms>, "type": "cmd", "endpoint": "...", "body": <json>}` to `campaign/live-log.jsonl` (python3 one-liner for correct JSON escaping), then `curl -s -X POST -d "$2" http://127.0.0.1:8765/$1` (GET when no body), prints response. `log.sh` — args `<type> <text>`; appends `{"ts":..., "type":"<type>", "text":"..."}`. Both `chmod +x`.
- [ ] **Step 2: server.py** — stdlib http.server on 127.0.0.1:8900: `/` serves index.html; `/api/log?since=N` returns JSONL lines N.. as JSON array; `/api/state` and `/api/screen` proxy the game API (urllib to 127.0.0.1:8765); no external deps.
- [ ] **Step 3: index.html** — single file, no CDN: left = game view (`<img src=/api/screen>` refreshed 500ms) + big RTA timer (starts on latest run-start event, ticks client-side) + IGT (from /api/state 1s poll); right = two feeds polling /api/log: REASONING (type reason/say/split, newest top) and COMMANDS (type cmd — render endpoint+key+ms as highlighted chips, e.g. styled span `[→ RIGHT 800ms]`; arrows for direction keys, distinct color for walk/action/say). Dark theme, readable at a glance from across the room.
- [ ] **Step 4: test** — start dashboard, `log.sh reason "test thought"`, `cmd.sh say '{"text":"hello"}'`, `log.sh run-start "run 0"`; verify in browser (controller checks via curl + one screenshot of the dashboard page): log API returns the 3 events, state proxy returns game JSON, screen proxy returns PNG bytes, RTA ticking.
- [ ] **Step 5:** journal the component, no repo commit needed (campaign/ is outside repo) — but COPY `cmd.sh`, `log.sh`, `server.py`, `index.html` into repo `docs/agent-play/dashboard-src/` and commit there so they're versioned: `git add docs/agent-play && git commit -m "feat: spectator dashboard + event bus (canonical copies)"`.

### Task 4: Recorder + category rules (speedrun compliance)

**Files:**
- Create: `campaign/recorder.sh`, `campaign/RULES.md`; repo copies in `docs/agent-play/dashboard-src/`.

**Interfaces:**
- Produces: `recorder.sh start N` (begins screencapture -v of the region covering game+dashboard windows → `campaign/recordings/run-N.mov`; writes PID file), `recorder.sh stop` (SIGINT, waits, prints path).

**Owner:** implementation subagent (sonnet).

- [ ] **Step 1: recorder.sh** — `start N`: `screencapture -v -R<x,y,w,h> "$DIR/recordings/run-$N.mov" & echo $! > .rec-pid` (region from a REGION variable at top of script, default full main display `-v` without -R if region unset; document how to set); `stop`: `kill -INT $(cat .rec-pid)`, wait for file to finalize, print path + duration (`mdls -name kMDItemDurationSeconds` or ffprobe if available). Handle: screencapture needs Screen Recording permission — if output file is missing/0 bytes, print a loud warning naming the permission.
- [ ] **Step 2: RULES.md** — category "AI Any% (remake)" rules verbatim from the spec's compliance section (engine remake disclosure, AI-driven play disclosure, timing start = first /state intro:false, stop = first escaped:true, single segment, no pause, no save/load, full-run continuous video required, route knowledge allowed, fair-play API constraints).
- [ ] **Step 3: test** — `recorder.sh start 0`, wait 10s (with dashboard + game visible), `recorder.sh stop`; verify .mov exists, >0 bytes, plays (open -a QuickTime or ffprobe duration ≈10s). If permission denied, report the exact dialog needed and stop for controller.
- [ ] **Step 4:** update run procedure in PROTOCOL.md: recorder start BEFORE run-timer start, recorder stop AFTER run-timer stop; runs.md gains `video` column (edit skeleton). Copy canonical recorder.sh + RULES.md to `docs/agent-play/dashboard-src/`, commit: `git add docs/agent-play && git commit -m "feat: run recorder + speedrun category rules"`.

### Task 5: Dry-run scout errand (validation gate)

**Owner:** controller (brain) dispatches ONE haiku errand using ERRANDS.md scout template.

- [ ] **Step 1:** Fresh game (pkill colditz; relaunch; dismiss intro). This is an UNTIMED calibration session (journal it as such).
- [ ] **Step 2:** Dispatch scout errand: "map rooms reachable from British quarters without opening any door; max 5 rooms". Haiku appends to map.md/journal.md.
- [ ] **Step 3:** Controller verifies: map.md gained ≥3 room sections with exits in the specified format; entries agree with a spot-check (/room for one of the rooms); report ≤10 lines and actionable. If Haiku deviated (format drift, overlong report, skipped map updates), fix the ERRANDS.md template wording and re-run ONCE.
- [ ] **Step 4:** Commit template fixes if any.

### Task 6: Timed runs (the campaign)

**Owner:** brain (main session, Fable). Not delegable.

- [ ] All brain/errand API calls go through campaign/cmd.sh and narration through log.sh (dashboard + recording depend on it); dashboard and recorder run for every timed run per PROTOCOL.md.
- [ ] **Run 1 (scout category, timed):** fresh launch → timer start → dispatch scout/fetch errands to build the map and find a lockpick/candle → timer stop on outcome (likely `abandoned` or arrest; that's fine — it's a baseline). Journal lessons; curate map.md.
- [ ] **Run 2+ (execution runs):** plan route from map + dossier; brain takes direct control for door/tunnel/escape moments; announce splits via /say; log every run in runs.md; update PB marker.
- [ ] Continue until a prisoner escapes (Any% complete) or the user calls it.

### Task 7: Phase C — freeze into `colditz-play` skill (after first successful campaign)

**Files:**
- Create: `/Users/fredriksafsten/games/.claude/skills/colditz-play/SKILL.md`

- [ ] **Step 1:** Write SKILL.md: name `colditz-play`, description "Start or resume a Colditz Escape speedrun campaign (brain/hands protocol)". Body: paths (game dir, API port, campaign dir, DOSSIER/PROTOCOL/ERRANDS in repo), the run procedure (from PROTOCOL.md, updated with campaign lessons), errand dispatch instructions (haiku, templates from ERRANDS.md), speedrun rules, and the current PB from runs.md.
- [ ] **Step 2:** Validate: fresh session invokes /colditz-play and can start a run without re-deriving anything.
- [ ] **Step 3:** Commit (note: lives in ~/games/.claude, outside this repo — no git there unless user wants).

## Self-review

- **Spec coverage:** dossier ✓(T1), campaign files ✓(T2), errand protocol ✓(T2 ERRANDS.md), brain loop ✓(T2 PROTOCOL.md + T4), speedrun timing ✓(T2 run-timer + T4), validation ✓(T3), skill wrap-up ✓(T5), failure handling ✓(PROTOCOL.md content spec'd in T2 Step 3). Gaps: none.
- **Placeholders:** run-timer row-count arithmetic flagged as must-validate with exact test cases (deliberate — validated in its own Step 4, not left vague).
- **Type consistency:** run-timer interface (`start|stop <outcome> <route-note>`) matches T4 usage; ERRANDS.md template fields match T3's dispatch and the errand constraints in Global Constraints.
