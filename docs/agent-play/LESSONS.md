# Lessons — engine mechanics & campaign strategy (learned by playing, fair-play)

Consolidated from journals, run logs, and live debugging across runs 1-10 and
surveys (2026-07-10 .. 2026-07-14). Everything here was learned through the
agent API or on-screen observation — no source-mined game strategy.

## Engine mechanics (hard-won, non-obvious)

1. **Diagonal moves are atomic**: the engine rejects a diagonal step if
   EITHER axis would collide, even when each axis alone is free. Any
   two-key steering must interleave single-axis retries when pinned.
2. **Door unlock requires motion**: a stationary action tap NEVER reaches
   the door-check code. You must be actively pushing INTO the doorway the
   tick the action fires. (/walk door-use mode implements this.)
3. **Run/walk toggle requires motion**: toggling while idle is a no-op —
   the engine only accepts it during a moving animation. Engage run-mode
   mid-walk.
4. **Guards operate doors**: locked doors get opened by patrolling guards
   and remain briefly passable — tailgating is a viable (unimplemented)
   route past graded doors. Observed live (253 east door, day 1).
5. **Item pickup is a 17x17-pixel window** around the prop's sprite anchor
   — tile-precision is insufficient; approaches can be side-dependent
   (furniture blocks some faces; room 251's lockpick only from the east).
6. **Idling in the wrong place is lethal**: the American prisoner died
   (state 512) waiting near room 144's locked door [0,3]. Carried items
   are lost on death. Never park a prisoner in a restricted zone.
7. **Pause via KEY_PAUSE needs a ~2.2s hold to UNpause** (the pause
   screen's fade must poll the key); a 100ms tap can strand the game
   paused.
8. **macOS accept() inherits O_NONBLOCK** from the listener, silently
   disabling SO_RCVTIMEO — the cause of every "empty API response" ghost.

## Castle knowledge (state of exploration)

- All four national quarters spawn a lockpick (251 east-approach, 140,
  200 south-approach quirk; French one unconfirmed).
- Every locked door reachable from all four wings is grade one/two:
  NO lockpick-grade door found yet. Security keys are the real currency.
- Room 144 (American wing) is the castle nexus: 11x11, 8 exits, 7 locked.
- French descent (219-224-227-230-231) is the deepest free chain;
  compiled leg runs it in ~8s at run-speed.
- No candles, keys, or tools found in any free area — deeper progress is
  gated on guard-operated doors (tailgate), keys, or undiscovered areas.
- Route quirks live in campaign/map.md + routes.json (off-center doorways
  needing staged approaches).

## Speedrun operations

- Compiled routes (campaign/routes/*.route + runner.py) execute with zero
  LLM latency: demo descent 15.2s IGT vs 13min discovery. Route legs must
  be keyed by room id and verified at execution — never blind sequences.
- The in-game display stays untouched during runs (/say deprecated);
  narration goes to the dashboard event feed.
- Timing: run-timer.sh (RTA+IGT), rules in campaign/RULES.md ("AI Any%
  (remake)"), runs logged in campaign/runs.md. PB machinery ready;
  recording gated on macOS Screen Recording permission (REC toggle,
  off by default).
- Guard timing is the only nondeterminism (reset-wait jitter): routes
  through guard corridors need one scripted retry (see 253 [5,3]).

## Process lessons

- Film-strip debugging (film.sh contact sheets) finds movement bugs that
  logs cannot: both the bed-corner bias and the pinned-oscillation were
  diagnosed visually.
- Fair-play boundary that held: geometry/masks = visible = usable;
  lock grades/door states = discoverable only by trying; no source
  strategy mining. It made every discovery feel earned and kept the
  campaign honest.
