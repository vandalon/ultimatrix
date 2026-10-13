# Ultimatrix rebuild

This is a gradual rebuild with the existing Football and F1 displays as the
visual baseline. Ported rendering and sport rules remain until their replacements
are ready. A successful build does not establish hardware or live-feed reliability.

## Implemented foundation

- A new runtime owns display setup, Wi-Fi provisioning, time synchronisation,
  local settings, and frame scheduling.
- Football and F1 have separate build environments and data modules. Only the
  selected sport is linked into each image.
- Feed requests and parsing run in the existing background sport tasks. A shared
  network gate serialises short-lived downloads and F1 TLS handshakes. F1 retains
  its existing policy of parking secure downloads during a live connection.
- MotoGP is the F1 face with another data service: `lib/motogp-feed/MotoGpTicker.cpp` implements `F1Ticker` (the same
  snapshot) from the MotoGP results API and live timing, instead of ESPN, Jolpica and OpenF1. `main/CMakeLists.txt` builds
  the F1 face, renderer and screens unchanged with it (`UM_MOTOGP`); riders, codes and bike colours are in
  `MotoGpRiders.h`, the standings carry the rider's name (`F1Ticker::Standing::name`).
- Words, Mario, Luigi and Pac-Man are feedless faces (`UM_SIMPLE_FACE`): each is one `Clockface.cpp` that draws the whole
  panel every update from the wall clock and its own small state, with shared helpers in `lib/faces` (colours, sprites from
  text rows, a 3x5 font). Words keeps its language in the settings' `numberFont` byte (the Football "alternative digits"
  setting), so the stored settings record did not change. Mario takes the night over from the red night clock
  (`IClockface::handlesNight/setNight`) and shows World 1-2 underground.
- Tetris is a build with no feeds: the falling-piece clock (time and date) with the same runtime, settings page
  and previews. It is built the way Football and F1 were reshaped: `lib/tetris` holds const data and layout functions,
  `TetrisScreens` decides what falls when and fills a fixed-size `Frame`, `TetrisRenderer` paints a `Frame`, and the
  face redraws only when the frame changes. No heap use after boot. See [the migration plan](TETRIS_MIGRATION_PLAN.md).
- Time, feed reading, JSON parsing, layout, digit assets, and cache mount policy
  live in shared core modules.
- Download readers stop at 512 KiB. Football scoreboards retain at most 32 games
  per response, two competitors per game, and the first 64 match detail records.
  Standings are capped at 32 groups of 32 teams.
  These limits bound those collections; they do not yet bound every allocation.
- F1 and Football select explicit screen types. F1 results and between-session
  page rotation starts on screen entry instead of using boot uptime.
- Native previews compile the real Clockface.cpp files and goal animations,
  with fake hardware and recorded feed snapshots. They do not fetch live data.
- Preferences use an independent `ultimatrix` namespace. Sport caches also use
  separate names, preserving previous firmware caches. Cache mount failures
  preserve existing data; only an entirely erased cache partition is initialised.

## Completed presentation migrations

Football publishes a bounded `football::Snapshot`: up to eight live matches
(favourites first), twelve results, sixteen fixtures, and bounded standings.
The snapshot is copied under a short service lock; table files and other cache
I/O stay on the data task. The display no longer reads disk pages. Screen timing
and drawing live in `FootballScreens` and `FootballRenderer`. Goal and incident
animations advance one frame per update instead of blocking the display loop.

F1 uses its existing fixed-size snapshot, with drawing extracted into
`F1Renderer` and presentation timing into `F1Screens`. Boot cache reads no longer
hold the snapshot lock during flash I/O. Its existing animations already advance
per frame. Football and F1 compare fixed-size frame values to decide when to redraw.

Tetris renders directly from its screen controller's bounded frame. Football
and F1 still share some presentation state through extern globals: their frame
objects are change detectors, not yet complete renderer inputs. Football drawing
also still creates some short-lived `String` values.

## Remaining work

1. Make Football/F1 renderer inputs self-contained and remove shared mutable
   presentation globals. Keep output identical through recorded-feed previews.
2. Remove remaining short-lived allocations from Football drawing, then measure
   heap fragmentation during live matches and animations on the device.
3. Unify cache versioning and recovery. Sport-specific persistence remains;
   Football's legacy cache metadata still contains unused fields.
4. Extend recorded previews around screen boundaries and cache recovery.
5. Finish device checks: panel/QR appearance, AP setup, live match cap, F1/Tetris,
   reconnects, and longer peak-heap/stack measurement under active feeds.
6. Restore automatic brightness, night scheduling, notifications, and update
   delivery as deliberate new modules. Tetris 12-hour/date options and the
   64×32 sports ticker remain unimplemented.

## Hardware baseline

ESP32 Dev Module, 4 MiB flash, one 64×64 HUB75 panel, E address pin 18,
7-bit colour depth, double buffering, and a 50 ms target frame interval.
Other HUB75 pins use the pinned driver's ESP32 defaults. The settings page
supports blue/green swapping and rotation. Static build RAM is not peak runtime
RAM: panel buffers, TLS, HTTP, task stacks, and snapshots allocate from the heap.

## Memory configuration

ESP-IDF builds the SDK components from source using the selected mode's
`build/<mode>/sdkconfig`. TLS input buffers allow 16 KiB records, output buffers
are 4 KiB, dynamic buffers/config cleanup are enabled, and peer certificates are
not retained after the handshake. CA-certificate cleanup is explicitly enabled.
Wi-Fi uses five static RX buffers and the TCP/IP task has a 3072-byte stack.
Runtime logs report free heap, minimum free heap, largest free block, DMA-capable
heap, task stack headroom, and failed allocations. These values need measuring
under actual live feeds; static image size alone does not prove a memory gain.

## Identity

Product UI, access point, hostname, settings, source names, and build environments
use Ultimatrix. Required copyright notices preserve their original URLs.

## Boot screens, QR, mDNS and telnet

`lib/core/BootScreen` draws the shared boot states from a POD `Frame` (kind, brightness step, page, text) and
redraws only when it changes. Pulses use a 64-step raised-cosine table; no `String`, no waiting. `src/main.cpp`
picks the state (`bootTick`) and keeps it going during the blocking Wi-Fi connect through the WiFiManager wait
callback (a small vendored hook). The access-point QR code is made by the unmodified Nayuki generator
(`lib/core/qrcodegen.*`, MIT). mDNS is set up again after every `GOT_IP`. `TelnetConsole` is polled from
`background()` with a fixed 48-byte line buffer; it is unauthenticated and off unless enabled in the settings.
