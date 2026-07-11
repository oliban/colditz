# Colditz Escape — Speedrun Category Rules

Community standards referenced: speedrun.com general practice (single-
segment RTA, video proof with no missing footage start-to-finish, rules
documented per category, RTA primary with IGT alongside). There is no
existing public leaderboard for this game; this document defines and
documents our own category.

## Category: "AI Any% (remake)"

- **Engine disclosure:** runs are played on the open-source engine remake
  of the original 1991 Colditz Escape, not original Amiga hardware. This
  is disclosed on every submission.
- **AI-driven play disclosure:** play is AI-driven — a "brain" agent
  (Fable, strategy/direct control) dispatching narrow "hands" agents
  (Haiku errand runners) — operating the game exclusively through the
  fair-play HTTP API described below. This is disclosed on every
  submission; the category is not a human-execution category.
- **Timing:**
  - **Start:** the first `/state` response observed with `intro:false`
    (i.e. the moment the intro is dismissed and prisoner control begins).
  - **Stop:** the first `/state` response observed with any prisoner's
    `escaped:true`.
  - Both RTA (wall clock) and IGT (`game_time` delta from the API) are
    recorded; RTA is primary, IGT is the secondary/corroborating clock.
- **Single segment:** one run = one fresh game process launch, timed
  continuously start to finish. No segment splicing, no re-attempts
  spliced together.
- **No pause, by any means:** pausing is forbidden for the entire timed
  segment, whether via `/control {"pause":true}` or `/input
  {"key":"pause"}`. Pausing freezes `game_time` and would corrupt IGT; it
  also isn't something a real speedrun attempt could do. (Pausing is
  permitted only in explicitly untimed scout/calibration sessions, never
  between a run's timer start and stop.)
- **No save/load:** in-game save/load is not used during a timed run.
- **Route knowledge allowed:** knowledge accumulated across previous runs
  (map layout, item positions, patrol patterns) may be used freely. The
  game's maps, item positions (`OBS.BIN`), and patrol routes
  (`ROUTES.BIN`) are static — only guard reset timing is randomized — so
  this mirrors what a replaying human would accumulate. Source-code and
  data-file mining to obtain this knowledge is banned; knowledge must come
  from the bundled README/FAQ, the project website, and public research
  on the original game (see `docs/agent-play/DOSSIER.md` for citations).
- **Fair-play API constraints:** all game interaction goes through the
  documented HTTP API only (`/state`, `/room`, `/walk`, `/input`,
  `/screen`, `/say`, `/control` for non-pause uses). No reading memory,
  save files, or other out-of-band state; no door/item status obtained
  except via `/room` geometry and `/screen` observation.

## Recording requirement (video proof)

Recording is **optional and toggled per run** from the spectator dashboard
(REC button, default OFF) or equivalently via `campaign/recorder.sh`.
Practice and scout runs may be played with recording off.

- **A run only counts as an official/PB run if the REC toggle was ON for
  the full run** — video must cover the timer start (first `intro:false`)
  through the timer stop (first `escaped:true`) with no gaps. A run timed
  in `runs.md` without continuous video for its full duration cannot be
  submitted as, or recognized as, a category PB, regardless of its time.
- Practice/scout runs recorded without video, or with partial/interrupted
  video, are still logged in `runs.md` (every timed run gets a row) but
  are marked as non-qualifying for PB purposes — do not set the `PB`
  marker on such a row even if it would otherwise be the fastest `escaped`
  time.
- Recommended practice: before a serious/PB attempt, turn REC on from the
  dashboard (or `recorder.sh start <N>`) *before* starting the run timer,
  and leave it on until *after* the run timer stops — see
  `docs/agent-play/PROTOCOL.md`'s run procedure and `## Recording`
  section for the exact sequencing.
- Output: `campaign/recordings/run-N.mov`, linked from the `video` column
  in `runs.md`.
- **Permission dependency:** recording requires macOS Screen Recording
  permission (System Settings -> Privacy & Security -> Screen & System
  Audio Recording) granted to the terminal/app running `recorder.sh`. If
  not granted, `recorder.sh`/the dashboard REC toggle fail loudly and
  cleanly (see `## Recording` in PROTOCOL.md) rather than silently
  producing an invalid or missing video — such a failure simply means the
  run cannot qualify as PB-eligible, it does not block the run itself.
- Fallback (documented, non-proof-grade, not eligible for PB without
  additional corroboration): `/screen`-sampled frames assembled into video
  via ffmpeg.
