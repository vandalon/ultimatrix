# Tetris clock migration plan

Source of truth: `~/clockwise` (read-only), clockface `cw-cf-0x08`, build env `cw-cf-0x08`
(`CW_FW_NAME=Tetris`, `CLOCKFACE_UPDATE_MS=10`). Target: a third Ultimatrix build mode, `tetris`.

## What the Clockwise Tetris face does

64×64 panel (the only hardware Ultimatrix supports):

- Time `H:MM` in 24-hour format as four large digits (scale 2) that are built from falling
  Tetris pieces. Pieces fall one after another inside a digit, all four digits in parallel,
  and rotate on the way down. Only digits that changed fall again; the others stay.
  A one-digit hour is a blank plus a digit. The block is shifted by hour (`-5`, `-2`, `2`).
- Date `D:M` as a second line of falling digits at the bottom (y = 58). It falls once at start
  and again when the date changes (midnight). Seven layout cases (1 January, one/two-digit day
  and month) decide x-position and the position of the 4×2 white separator that is drawn
  when the date has landed. While the date falls the screen is cleared, so the time is blank.
- Colon between hours and minutes: white on even seconds, black on odd, drawn only when idle
  (not while digits fall); shifted 4 px when the minute tens digit is 1.
- Animation pace: 50 ms per step, 30 ms at start-up and for the date, 0 ms in the last two seconds
  of a minute so the animation always ends before the minute changes.
- Not on 64×64: the 64×32 variant (smooth colon, football/F1 ticker row, goal animations) belongs to
  the 32-row hardware Ultimatrix does not support. It is **not** migrated.

Clockwise settings for it: none specific (brightness, rotation, blue/green swap, timezone are global).
No network feed: only NTP time. No cache, no TLS.

## Dependencies found in `~/clockwise`

| Item | Where | Licence | Decision |
|---|---|---|---|
| Face logic | `cw-cf-0x08/Clockface.cpp/.h` (jnthas upstream, 64×32/ticker changes by Joris) | MIT (upstream) / PolyForm (Joris's parts) | **Rewrite** as Clockface glue + Screens + Renderer; 64×64 only |
| Tetris drawing library | `TetrisMatrixDraw.cpp/.h` | © 2018 Tobias Blum, **LGPL-2.1-or-later** (original project: https://github.com/toblum/TetrisAnimation) | **Rewrite, derived**: piece shapes, rotation thresholds, colours, layout constants taken over, so the notice and licence stay with the derived files (`lib/tetris/TetrisShapes.*`, `TetrisFall.h`, renderer) |
| Digit fall tables | `TetrisNumbers.h` (no notice in the file; same upstream) | upstream LGPL project | **Reuse data**, converted to `const` packed bytes; treated as LGPL-derived |
| Letter fall tables | `TetrisLetters.h` ("Font created by Mike Swan", https://github.com/n00dles101) | unspecified | **Dropped**: the face never calls `drawText`. 23 KB of source, non-const arrays (RAM), unclear provenance |
| Football ticker, goal animation | `FootballTicker`, `GoalAnimation` | PolyForm | **Dropped** (64×32 only) |
| `CWPreferences`, `CWTelnet`, `BackgroundWait` | cw-commons | MIT/PolyForm | Replaced by Ultimatrix `Settings`, `Console`, nothing |
| Web settings page | `settings.html` | MIT | Not needed; Ultimatrix page is used |

LGPL consequence: the project is source-available, so users can rebuild with a modified
Tetris library. `LICENSES/LGPL-2.1.txt` is copied from Clockwise and listed in
`THIRD_PARTY_NOTICES.md`; the derived files keep the original copyright and LGPL header, URL added.

## Reused vs rewritten

Reused unchanged from the Ultimatrix runtime: `ClockTime`/`TimeService` (local time, NTP), `Settings`
(brightness, rotation, blue/green swap, timezone), `PanelLayout` (64×64, `frameMs` = 50), the Wi-Fi
setup portal, settings web server, and the display setup in `src/main.cpp`. The network gate and the
download budget are **not used**: there are no downloads. Fonts: none (only the Tetris digit bitmaps).

Rewritten:

- `lib/tetris/` (no GFX dependency, plain data and functions):
  - `TetrisDigits.h/.cpp`: the ten digits as `const` arrays of 5-byte pieces (shape, colour, x, stop row, rotations).
  - `TetrisShapes.h/.cpp`: the 8 piece types × 4 rotations as `const` cell tables; `rotationAt(piece, fall)`.
  - `TetrisLayout.h/.cpp`: time and date digits, x offsets and separator position, from hour/minute/day/month.
- `clockfaces/tetris/`:
  - `TetrisScreens.h/.cpp`: state machine (date falls, time falls, idle with colon), step pacing, fills a POD `Frame`.
  - `TetrisRenderer.h/.cpp`: draws a `Frame` (the only input) onto the panel, full redraw, no state.
  - `Clockface.h/.cpp`: glue: reads `ClockTime`, calls `advance()`, redraws when `Frame` differs (memcmp), flips.

Fixed size by construction: no `String`, no `new` after boot, no per-frame heap (the original built
four `String`s per 50 ms pass), no blocking waits (the original already stepped one frame per `update()`).

## Memory

- Static RAM of the face: `Frame` 30 B (static_assert), `TetrisScreens` ≈ 50 B, no heap. Digit/shape tables are `const`
  (flash). Original: the digit and letter arrays were writable `int` structs in RAM (about 2.4 KB for the digits alone).
- Measured (ESP-IDF `size`, whole image): Tetris `.bss` 24 712 B / `.data` 17 736 B; Football 45 056 / 18 520; F1 33 288 / 20 376.
- Panel DMA framebuffer: 57 344 B (same as Football/F1).
- No feed tasks, no TLS, no cache mount → far more free heap than Football (86 KB free after feeds).
  Wi-Fi + web server + NTP only. Clock task stack stays 8192 B.
- Flash: `ultimatrix.bin` 0x11ef90 (40 % of the app partition free) vs Football 0x155890 and F1 0x153db0 (29 % free):
  no feed parsers or TLS client.
- No new limits are needed; the only variable data are four digit slots per line (time, date), `uint8_t` indices.

## Fit in the existing runtime

- Build: `ULTIMATRIX_MODE=tetris`, env/build dir `build/tetris`, defines `UM_TETRIS`, `UM_MODE_NAME="Tetris"`.
  `firmware/CMakeLists.txt` accepts `tetris`; `main/CMakeLists.txt` compiles `lib/tetris` and `clockfaces/tetris`
  instead of a sport (include dirs follow the same `lib/<module>` + `clockfaces/<mode>` pattern).
- `src/main.cpp`: the settings page and save handler already use `#ifdef UM_FOOTBALL_ONLY` / `#else` F1. They become
  three-way: Tetris shows only the global fields (brightness, rotation, timezone, swap blue/green); sport fields
  (seconds per page, keep results, clock animation, leagues, drivers) are hidden and not validated.
- Shared settings storage is unchanged (`ultimatrix` NVS namespace); a Tetris image reads and writes the same keys.
- `tools/idf.sh` accepts `tetris`.
- Previews: `tools/preview/showcase.py --face tetris` compiles the real face with the same shims; the scenario is a boot,
  a minute change, an hour change and a new-year date change; the GIF goes to `docs/previews/tetris.gif`.
  Football and F1 previews must stay byte-identical to their baselines.

## Behaviour kept / deliberate differences

Kept: layout coordinates, colours, fall order, rotation thresholds, date layout table, separator, colon rule, pacing, 24 h.

Differences (small, on purpose):

1. The original update loop ran every 10 ms; Ultimatrix calls `update()` every 50 ms. The pace is kept by stepping
   several animation steps per call when due (30 ms → 1–2 steps, 0 ms → 5 steps) and showing the last one.
2. Time and date have separate digit state (the original shared one array, so a coincidentally equal digit could skip
   falling after midnight). The normal paths (start-up, minute change) are identical.
3. The colon appears on the frame in which the last piece lands (the original: one frame later).
4. Everything is redrawn from the frame each time, so the picture no longer depends on what the previous buffer held
   (the original drew the separator twice to fill both DMA buffers).

## Steps (all done except the on-device check)

1. Plan (this file).
2. `lib/tetris` + `clockfaces/tetris` + preview support; `docs/previews/tetris.gif`; Football/F1 previews byte-identical.
3. Build mode: both CMake files, `main.cpp` guards, `tools/idf.sh`; football, f1 and tetris ESP-IDF builds pass
   (Football and F1 binaries unchanged in size).
4. Licence/notice files, README and ARCHITECTURE entries.
5. Open: flash and look at the panel (Joris).

## Risks

- **Licence of the digit tables**: no explicit notice in the Clockwise copy. They are treated as part of the LGPL
  library they came from; the original upstream URL is recorded; provenance should be confirmed before a release.
- **Visual drift**: no automatic comparison against Clockwise exists. A one-off scratch comparison (not in the repo) of the old
  library against the new renderer and layout gave 0 pixel differences over 432 time/date runs (66 966 frames);
  the step pacing and the colon were not compared that way.
- **Pacing**: 50 ms update granularity means the fast end-of-minute animation shows fewer intermediate frames than the
  original; verify on the panel that it still looks smooth.
- **Double buffering on the panel**: full redraw each changed frame is safe, but flicker/tearing and the colon timing
  need an on-device look.
- **`main.cpp` is shared**: guard edits must leave the Football and F1 binaries unchanged (sizes are compared).
- 64×32 users lose the ticker row: out of scope for the 64×64 baseline.
