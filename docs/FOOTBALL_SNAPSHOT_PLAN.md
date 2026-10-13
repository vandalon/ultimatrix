# Football snapshot and renderer migration plan

Implementation status: steps 1–7 were completed by Claude on 12 October 2026.
This document preserves the original plan and audit; its line numbers and open
decisions are historical. The approved live cap is eight. Standings use bounded
in-memory rows rather than the proposed two-page request protocol. See
[the current architecture](ARCHITECTURE.md) and `HANDOFF.txt` for remaining work.

Proposal for architecture targets 1, 2, 4 and 5 (see `HANDOFF.txt`), scoped to
the Football build. Written by Claude on 12 October 2026 from a read-only review;
no source files were changed. File and line references are to the tree as it was
on that date.

## What the review found

The Football face is one 1,817-line file (`clockfaces/football/Clockface.cpp`)
that mixes three jobs: deciding what to show, timing transitions, and drawing.
`FootballTicker` hands it heap-backed data and, in the Football build, also does
flash reads on the display thread.

**Flash and lock work on the display thread**

- `readResult()` / `readUpcoming()` fall through to `FootballTicker::finished()` /
  `upcoming()`, which open LittleFS and walk the cache file record by record
  (`FootballTicker.cpp:315`, `:398`, `:406`). `drawRow()` (`Clockface.cpp:526`)
  calls this for every row on every redrawn frame, so a sliding results list
  reads flash at ~30 fps. `comingToday()` does the same per call.
- `upcoming()` and `toEntry()` call `tablePositions()`, which scans a whole
  standings file from flash (`FootballTicker.cpp:1217`) for each match.
- `standingsPage()` opens every league's table file twice per page change
  while holding both `_lock` and `cacheLock`.
- The display thread takes `_lock` then `cacheLock`. The data task holds
  `cacheLock` for the full duration of flash writes in `saveMatches()`, so any
  frame that needs a row can stall behind a write. The lock order is consistent
  today, but only by convention.

**Unbounded or heap-heavy hand-over**

- `Overview` is three `std::vector`s of `Entry`, each with several `String`s and
  a `std::vector<Event>`. `overview()` copies all of it on every version change,
  and `update()` catches `std::bad_alloc` to survive a failed copy.
- `o.live` has no cap (`buildOverview`, `FootballTicker.cpp:1651`); it is bounded
  only by 32 matches per scoreboard times the number of competitions followed.
- `liveShown` / `liveOld` / `slotShown` / `slotOld` / `RankingPage` are further
  `String`/`vector` copies kept as statics.
- The renderer only uses three letters of each team name, and only the live main
  match uses `events` (minute, kind, side) for its timeline.

**Screen state and blocking animations**

- `update()` (`Clockface.cpp:1542`) is ~270 lines that select the view, advance
  six independent animation timers held in function statics, build a 27-field
  `snprintf` change key, and draw.
- Goal and incident animations block `update()` for 2.5 to 6 s through
  `serviceWait()`; settings stay responsive only because of the background-work
  callback.
- The preview harness (`tools/preview/showcase.cpp`) stubs nine `FootballTicker`
  methods, so it has to track the ticker's interface by hand.

## Target shape

```
data task (FootballTicker / FootballService)
    parse -> Match records -> FootballSnapshot (fixed size, POD)
                                   | publish: memcpy under one short lock, version++
display task
    FootballScreens (state + timing, no drawing)  <- reads its own snapshot copy
    FootballRenderer (pure draw functions of snapshot + screen state + now)
```

Previews feed a recorded `FootballSnapshot` straight into the same screens and
renderer, so they no longer need to stub the ticker.

### `FootballSnapshot` (new `lib/football-feed/FootballSnapshot.h`)

Plain structs with fixed arrays and fixed-size text; no `String`, no `vector`.

```cpp
namespace football {
constexpr uint8_t kLive = 8, kResults = 12, kFixtures = 16, kMarks = 24;

struct Mark { uint8_t minute; char kind; bool home; };       // timeline only
struct Side { char code[4]; TeamKit kit; uint8_t position; }; // 3 letters, table place or 0

struct LiveRow {
  Side home, away;
  int8_t homeScore, awayScore;
  char clock[8];        // "67'", "45+2", "HT"
  char competition[6];  // "ERE", "KNVB", ...
  uint16_t color;
  uint8_t played;       // 0..255 of 90 minutes
  bool favourite;
  uint8_t markCount; Mark marks[kMarks];
};
struct ResultRow  { Side home, away; int8_t homeScore, awayScore; uint16_t color; bool favourite; };
struct FixtureRow { Side home, away; time_t kickoff; uint16_t color; bool today; };

struct StandingsPage {
  char competition[24]; uint8_t first, rows, pageCount;
  char team[6][5]; int16_t points[6];
};

struct Snapshot {
  uint32_t version;
  uint8_t status;                         // LOADING / OK / FAILED
  uint8_t liveCount, resultCount, fixtureCount;
  LiveRow live[kLive];                    // favourites first
  ResultRow results[kResults];            // most recent first
  FixtureRow fixtures[kFixtures];         // soonest first
  StandingsPage standings[2];             // current and next page
};
}
```

Estimated size is about 2 KiB; pin it with `static_assert(sizeof(Snapshot) <= ...)`
so a change shows up at compile time. Three static instances (service staging,
published, display copy) cost ~6 KiB of BSS, which the build-size report then
counts, instead of today's unaccounted heap copies. `kLive = 8` drops the 9th and
later live matches after favourites; that is a visible behaviour change to confirm
(see "Decisions").

Goals and incidents stay as small queued events but become fixed-size too
(`char home[4]`, `char away[4]`, existing `Incident` fields), in a ring of 4.

### Service side (`FootballTicker`)

1. Add `buildSnapshot(Snapshot &out)` next to `buildOverview()`. It fills the
   staging snapshot from the in-memory `Match` list, resolves `tablePositions()`
   once per match per refresh, and fills both standings pages.
2. `publish()`: lock `_lock`, `memcpy` staging into published, bump version.
   Only `_lock` is held, never `cacheLock`, and never during flash I/O.
3. `snapshot(Snapshot &out)` for the display: lock, `memcpy`, unlock.
4. Standings paging: the display writes the wanted page number to an atomic;
   the service prepares `standings[1]` ahead and swaps it in on its next pass.
   The 4 s page timing hides the latency.
5. The disk cache (`saveMatches`, `readMatch`, slot files) stays as persistence
   for restart recovery only; `restoreOverview()` rebuilds the snapshot at boot.
   Nothing on the display path opens a file.

### Display side

Split `Clockface.cpp` into three files in `clockfaces/football/`, one move at a
time, keeping the drawing code byte-for-byte where possible:

- `FootballRenderer.cpp/.h`: the stateless drawing (`drawStrip`, `drawMatch`,
  `drawResult`, `drawComing`, stadium, flip tiles, rankings). Inputs are rows
  from the snapshot plus an explicit `Frame` struct (offsets, progress values).
- `FootballScreens.cpp/.h`: the screen state machine. One `enum class Screen
  { Live, Fixtures, Standings, Clock, Goal, Incident }` and one struct per
  animation (flip clock, score flip, live swap, slot slide, ranking slide, ball
  roll) with `begin(now)`, `progress(now)`, `done(now)`. Replaces the function
  statics and the long `update()`.
- `Clockface.cpp`: glue only: copy snapshot on version change, step the
  screens, ask the renderer to draw, flip.

The change key becomes a `memcmp` of the `Frame` struct against the previous
one, instead of the 27-field `snprintf`.

Goal and incident animations become screens: `playGoalAnimation()` and
`playCard()` already compute everything from `t`, so the loop body moves into a
`drawGoalFrame(t)` / `drawIncidentFrame(t)` and the screen advances `t` each
`update()`. `serviceWait()` is then no longer needed by Football.

## Order of work

Each step builds and renders previews identically before the next one starts.

1. Add `FootballSnapshot.h` and `buildSnapshot()`, published alongside the
   existing `Overview`. No display change. Log `sizeof` and compare snapshot rows
   with `Overview` in the preview harness.
2. Switch the preview harness to feed a `Snapshot` from `showcase-data.json`.
   Previews must match the current GIFs frame for frame.
3. Move the display to read rows from the snapshot (`readResult`,
   `readUpcoming`, `comingToday`, standings). Remove `finished()`, `upcoming()`,
   `standingsPage()` from the display path. This removes every flash read and
   `cacheLock` acquisition from the display thread.
4. Extract `FootballRenderer` (pure move). Previews unchanged.
5. Extract `FootballScreens` and replace the change key. Previews unchanged.
6. Convert goal and incident animations to frame states. Previews unchanged in
   timing; settings and telnet now stay live during animations without
   `serviceWait()`.
7. Delete `Overview`, `Entry`, `finished()`, `upcoming()`, `standingsPage()` and
   the `bad_alloc` catches once nothing uses them.

Steps 1 to 3 deliver the memory and responsiveness win; 4 to 7 are structure.

## Files touched, and overlap with Codex

All steps touch `lib/football-feed/FootballTicker.*`, `clockfaces/football/*` and
`tools/preview/showcase.cpp`. None of those are in Codex's listed scope, but
`main/CMakeLists.txt` (Codex-owned) will need the new source files added in
steps 1, 4 and 5. Coordinate that one line before starting, or wait for the
ESP-IDF build to be verified first so the previews and builds have a stable base.

## Decisions for Joris

- **Live match cap:** keep at most 8 live matches (favourites first), or pick
  another number. Today it is unbounded.
- **Start now or after the ESP-IDF build is verified:** steps 1 to 3 can start
  in parallel with Codex if the CMake source list change is agreed; otherwise
  wait.

## Not covered

F1 gets the same treatment afterwards; its `F1Ticker::Snapshot` is already
fixed-size, so the remaining work there is mostly the screen and renderer split. Cache versioning (target 4 in ARCHITECTURE.md)
and device checks are separate tasks.
