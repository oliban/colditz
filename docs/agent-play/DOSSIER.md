# Colditz Escape — Fair-Play Knowledge Dossier

Research scope: the 1991 Amiga original "Escape From Colditz" (Digital Magic
Software, design by Mike Halsall and John "Jon" Law) [8], and its GPL
reverse-engineered remake "Colditz Escape" (aperture-software) [1][2]. Sources
are the remake's bundled README, the project website, and public web material
(magazine reviews, hint/tip pages, walkthroughs) about the original game.
Nothing under `src/` was consulted except `docs/AGENT-API.md`, which is cited
only for what the remake's own HTTP API exposes to an agent, not for game
logic.

Claims are tagged as either confirmed by the remake's own documentation
(README/API — treated as authoritative for how *this* build behaves) or as
lore/claims from third-party reviews, hint sheets, and player memory (treated
as plausible but unverified until confirmed in play).

## Mechanics

**Doors, keys & lock-pick grades.** Locked doors require a specific tool,
consumed on use: "Low Security" doors take a lock-pick, "Grade One Security"
doors take a security key one, and "Grade Two Security" doors take a security
key two [1]. Period sources describe the same three-tier system (lock-pick /
grade-1 key / grade-2 key) and note each key or lock-pick is single-use, but
that an unlocked door then stays open for reuse [3][4]. To open a door you
select the key/lock-pick and use the action button while at the doorway [4].

**Tunnels & tools.** Tunnel exits require matching the tool to the floor
surface: saw for wood floors, pick-axe for pavement, shovel for grass; the
tool is consumed [1]. From inside a tunnel, a shovel is always required to
open an exit regardless of surface type, or the action key alone if the exit
is already open [1]. Original-game hint sheets agree tools are used by
standing on a "weak spot" in the floor and pressing fire [3][4].

**Candle.** A candle is required to enter an already-open tunnel and is
consumed on entry [1]. Third-party material does not add detail beyond this.

**Appell / roll-call & curfew.** The remake's README and website FAQ do not
document any roll-call, curfew, or scheduled headcount mechanic for the video
game [1][2]; `docs/AGENT-API.md` exposes a `game_time` clock but no
appell/curfew flag. Third-party video-game sources describe restricted areas
("forbidden areas") that trigger solitary confinement and equipment
confiscation if entered [6], guards who "follow a strict timetable" [7], and
brief courtyard "exercise periods" as the legitimate window for outdoor
movement [7] — but none describe an explicit multiple-times-per-day appell
headcount mechanic for the *video game* specifically (that mechanic is
well-documented for the *board game* of the same name and for the real
historical Colditz, which held 3–4 roll calls a day, but this dossier does
not assume the video game implements it — see Unknowns).

**Fatigue / sleep.** Walking, running, and especially crawling increase
fatigue; fatigue also rises with every in-game hour that passes. At max
fatigue, running is disabled (walk-only) [1]. Fatigue is reduced only by
sleeping in the prisoner's own starting-room bed, triggered with the "Sleep"
key while in that room [1]. A prisoner can only sleep in their own bed, not
any bed [1]. Hint sheets corroborate: "if the Fatigue bar is filled, you will
only be able to walk," and recommend resting after capture or heavy running
[3].

**Uniforms, passes & papers.** Wearing a found German uniform lets a
prisoner attempt to pass as a guard; select it in inventory and use the
action key [1]. Guards are "suspicious by nature" and may demand a pass from
an unrecognized face in uniform; a pass is consumed on use, and failing to
produce one when challenged sends the prisoner to prison [1]. In the remake's
"enhanced" (non-original) mode, a guard that has already seen a prisoner's
pass remembers it (shown via a small pass icon over that guard) and won't
re-challenge — this is a remake-only enhancement, off in "Original Mode"
[1]. Escaping the castle itself (final win condition) requires German
papers, distinct from passes; without them the prisoner is returned to the
castle [1]. Period hint material lists "papers (to help in the escape)" as
required at the main gateway, consistent with this [5][3].

**Stooge (lookout).** Setting a prisoner as "stooge" (idle, via the Stooge
key) flags them as an alert lookout: if a guard approaches that stooge's
location, control automatically switches back to the stooging prisoner as a
warning [1]. This is described as a way to post a lookout while another
prisoner works a restricted area [1]. Hint sheets add a specific tactic:
leaving a prisoner in stooge in a room for a couple of minutes can cause a
loitering guard to leave [3].

**Guard behavior & pursuit.** Guards patrol and, per third-party sources,
follow fixed/scheduled routes and are lethal shots that will shoot to kill a
running, spotted prisoner [5][7]. A specific dodge tactic recorded in period
hints: if a pursuing guard is not too close and stops running (about to
raise his rifle), the player should stop too, let the guard close in, then
resume evasion toward a door/obstacle before the guard's aim-and-fire
"animation" completes — timing-dependent [3][4]. Stones can be thrown
(action key, with a selected stone) to slow a *running* pursuit back down to
a walking one, buying an escape window [1]; period hints describe the same
item as a general guard distraction, with throw distance tied to how long
the button is held [4] — the remake's README only documents the
running→walking pursuit-downgrade effect [1], so throw-distance tuning
should be treated as an original-game claim not yet reconfirmed in the
remake.

**Solitary confinement.** Entering forbidden/restricted areas risks capture,
solitary confinement, and confiscation of carried equipment [6]; guards may
also shoot at a fleeing prisoner in such a zone [6][5]. The remake's own
README does not separately describe a "solitary" game-state name; this is a
third-party description of the consequence of getting caught out of bounds.

**Items list.** Confirmed/functional items per the remake README [1]:
lock-pick, security key one, security key two, saw, pick-axe, shovel,
candle, stone, German uniform, pass, (escape) papers. Two items are
documented as non-functional holdovers from the original game: the rifle
("not used for anything in this game... our reverse engineering of the
original game uncovered no use for the rifle whatsoever") [1], and the
stethoscope ("was not used in the original game") [1]; a prisoner's uniform
item also exists (lets you change back into prisoner's clothes) but is
described as "pretty much useless" and normally only reachable via a bug or
cheat [1]. Period hint sheets' item list (keys/lock-picks, uniform, pass,
papers, saw, shovel, pick-axe, candle, stone) matches the remake's
functional set [3][4][5].

**Movement & prisoners (given facts, not re-derived here).** Movement is
8-directional isometric; the remake's HTTP API exposes `/state`, `/room`,
and `/walk` for agent perception/navigation (`docs/AGENT-API.md`); the four
playable prisoners (British, French, American, Polish) each start in their
own national quarters, with the British prisoner starting in room 251
upstairs.

## Strategies & lore

- **Equipment discipline.** Period hint sheets stress not carrying too much
  gear at once, since a capture confiscates everything carried; a
  recommended pattern is stashing collected items in a "safe" area the
  Germans never search (the Chapel is named specifically) and making
  multiple short trips instead of one loaded one [3][6].
- **Guard-evasion timing trick.** The stop-when-they-stop dodge described
  above (stop before the guard raises his rifle, then bolt for cover) is
  the primary recorded evasion technique in period sources [3][4]; it is
  explicitly timing-sensitive ("but only if your timing is right") [4].
- **Stooge-out-a-guard.** Leaving a prisoner in stooge mode in a room for a
  couple of minutes is claimed to make a guard loitering there eventually
  leave [3]. Unverified how "a couple of minutes" maps to in-game time.
- **Door risk on open.** Opening a door can reveal a guard waiting on the
  other side, per hint-sheet warnings — treated as a general caution rather
  than a specific room callout [3].
- **Prisoner-quarters trap.** "Beware of the Prisoner's Quarters because
  many of the doors here lead into small empty rooms" — a period navigation
  warning about wasted detours, not a danger warning per se [3].
- **Scale and route count.** One retrospective review describes the castle
  as roughly 750 rooms across six floors, and repeats a period marketing
  claim of "42 ways to escape" (tunneling, uniform impersonation, or
  slipping past the gate among the named methods) [7]. Treat the specific
  numbers (750 rooms, 42 escape routes) as unverified marketing/reviewer
  claims, not confirmed level data — no primary source (README, API,
  official manual text) in this research repeats them.
- **Tunnel locations.** No period source found in this research names
  specific tunnel room numbers or coordinates; a classicamiga.com PDF titled
  "Escape from Colditz maps" was found in search results but could not be
  fetched (site returned an access-denied/anti-bot page) [see Unknowns].
  Treat all tunnel-siting knowledge as something to discover in play, not
  something available from period sources at research time.
- **Exercise-yard windows.** One review frames the courtyard as accessible
  during scheduled "exercise periods" rather than freely at all times [7],
  consistent with the remake README's general framing of restricted areas
  and guard patrols, though the remake's own docs don't use the phrase
  "exercise period" [1].
- **Historical/board-game roll-call color (context only, not a video-game
  mechanic claim).** The real Colditz held 3–4 roll calls (Appell) per day,
  and the *board game* "Escape from Colditz" (which the 1991 video game is
  explicitly based on, alongside Pat Reid's Colditz books [5]) models
  Appell as a mechanic that excludes prisoners who are tunneling, in safe
  areas, in solitary, or already outside. This is included as background
  only — no source in this research confirms the *video game* implements a
  literal roll-call clock, so it is listed under Unknowns below.

## Unknowns to discover in play

- Whether the video game (remake or original) implements any explicit
  appell/roll-call or curfew timer distinct from ordinary guard patrols and
  restricted-area detection — no primary or secondary source found
  confirms or denies this for the *video game* (only the board game and
  real history clearly have it).
- Actual tunnel entry/exit room locations and which floor surfaces (wood/
  pavement/grass) they sit on — no accessible period source enumerated
  these; the one PDF map guide found in search results
  (classicamiga.com, "Escape from Colditz maps") could not be retrieved
  (anti-bot block).
- Where specific items (keys, uniforms, papers, tools) spawn or are hidden
  per play-through — hint sheets say items are hidden "behind walls,
  tables, bunks, etc." in general but name no specific rooms.
- Exact guard patrol routes/schedules and whether they are fixed per
  room or randomized per game session.
- Precise stone throw-distance-vs-hold-duration tuning in the remake
  specifically (only confirmed effect in the remake's own README is
  downgrading a running pursuit to walking; the "hold longer = throw
  farther" detail is an original-game-era hint-sheet claim not
  reconfirmed against the remake's actual behavior) [1] vs [4].
- Whether "42 ways to escape" and "~750 rooms across six floors" are
  accurate to the shipped game or inflated marketing/reviewer figures.
- Whether stashing items in the Chapel (or any other "safe" room) is
  still guard-search-proof in the remake, versus being an original-game-era
  claim.
- What exactly triggers "solitary confinement" as a distinct state
  (duration, whether it differs from ordinary recapture) — no source gives
  mechanical specifics, only that it can happen in forbidden areas.

## Sources

[1] Colditz Escape bundled README ("Colditz Escape - Readme"), local file
`/Users/fredriksafsten/games/colditz-escape/Colditz Escape/README.html`
(remake's own FAQ/manual; GAMEPLAY, DATA FILES, and OTHER QUESTIONS
sections).

[2] Colditz Escape project website, https://aperture-software.github.io/colditz-escape/
(overview, controls, original-game credits — Mike Halsall and John Law,
music by Bjørn Lynne — and version history; v1.3, Feb 2025).

[3] Escape from Colditz — Amiga Hints, Tips & Tricks, Lemon Amiga,
https://www.lemonamiga.com/games/docs.php?id=570 (door/key grades, guard
evasion timing tactic, stooge-wait tactic, equipment-caution and Chapel
stash tip, fatigue/sleep note).

[4] Escape from Colditz — Amiga Instructions/Docs, Lemon Amiga,
https://www.lemonamiga.com/games/docs.php?id=571 (also mirrored at
https://www.lemonamiga.com/doc/escape-from-colditz/571 — controls, door/key
use, tunnel-tool use, stone-throw distraction, uniform use, stooge/F10).

[5] Escape from Colditz — Amiga Game Review, Lemon Amiga,
https://www.lemonamiga.com/review/escape-from-colditz/36 (board-game and
Pat Reid book origin, four nationalities, security-tool tiers, guard
shoot-to-kill behavior, item list, F1–F4 non-blocking character switch,
no-save-feature criticism).

[6] Escape from Colditz, Skooldays blog,
https://www.skooldays.com/blog/escape-from-colditz/ (forbidden
areas → solitary confinement + confiscation consequence, F1–F4/F5/F10
controls, equipment-caution and Chapel stash tip, stone distraction).

[7] Escape From Colditz, Amiga Reviews (leveluphost),
https://www.amigareviews.leveluphost.com/escapeco.htm (~750 rooms/six
floors claim, guards "follow a strict timetable," courtyard exercise
periods, tunneling requires saw+pick-axe+candle plus a stooge lookout,
"42 ways to escape" marketing claim).

[8] Escape from Colditz review metadata, Amiga Action issue 19 (Apr 1991),
Amiga Magazine Rack, https://amr.abime.net/review_6057 (developer credit
Mike Halsall / Jon Law, publisher Digital Magic, 83% "Super League
Recommended" score — used here only for developer/publisher attribution;
full review body text was not accessible).

[9] `docs/AGENT-API.md` (this repository) — consulted only for what the
remake's HTTP API exposes to an agent (`/state`, `/room`, `/walk`,
`game_time`), not for game logic or source-derived mechanics.
