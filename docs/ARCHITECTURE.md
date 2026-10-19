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
  face redraws only when the frame changes. No heap use after boot.
- Home is the clock with Home Assistant on the side (`lib/ha-feed`): `HaClient` keeps one WebSocket open to
  `/api/websocket`, authenticates with a long-lived token and subscribes to a single server-rendered template, so
  it only receives a short line of values whenever they change. `HaTemplate` builds that Jinja template from the
  configured entities and parses Home Assistant's responses (including `energy/get_prefs` for Auto mode and
  `recorder/statistics_during_period` for the hourly history). `clockfaces/home/Clockface.cpp` fills a fixed-size
  `Frame` from the latest snapshot, deriving house usage from grid/solar/battery when no direct sensor is set;
  `HomeRenderer` paints it, same change-detection pattern as Football and F1. A tile or icon with an empty entity
  field is an Auto slot: Home Assistant's own device classes pick its entities (the settings page shows what they
  are as grey tags, from `/api/ha/suggest`). The EV tile stores three entities (`charge,charging,plugged`, `-` for
  one left to detection, `off` for one not used) and returns one number, the charge plus 1000 when plugged in and
  2000 when charging, so the settings layout did not change. The album-art backdrop is bit 16 of the saved page
  flags; it is not a page.
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

## Notification images

The existing MQTT Notification entity accepts either plain text or JSON. JSON can include an `image` URL alongside the title, text, duration and priority. `NoticeImage` fetches a baseline JPEG or non-interlaced PNG in a separate task, using the shared network budget and memory gates. Downloads have byte, dimension and time limits. The decoder samples into a 64×64 RGB565 buffer while preserving the image's proportions. The default `scale: fit` retains the whole picture with black bars; `scale: fill` crops a centred square to fill the panel.

The display loop draws only completed images. At normal priority, optional text alternates with the image. High-priority images stay on the full panel at full brightness, including at night, with no border, blinking or text. Loading does not consume the display duration. Normal notifications keep the existing night and priority rules. Failed image loads fall back to text, or leave the face visible for image-only messages. Images stay in RAM and are released when dismissed, replaced or finished. PNG decompression uses uzlib with its dimension-checked filtered output as LZ history, rather than a fixed dictionary. Grayscale/palette at 1/2/4/8 bits and 8-bit RGB/alpha are supported; transparent pixels are composited onto black. CRC and zlib checksums are checked. Interlaced/16-bit/animated PNG, GIF and video notification URLs are not supported. See the README for HA action examples.

## Saved Canvas playback from Home Assistant

The MQTT Saved picture select lists saved flash slots by number and name, with Off to release the WLED overlay. Discovery options refresh after catalog changes, and playback waits for the feed tasks to park before loading a slot. The bridge omits this entity.

The browser converts KLIPY imports as before. An explicit Save played GIF operation converts the loaded frames to the existing RGB565 saved-animation format (using frame differences when memory permits), limits new saves to 24 frames / 5 seconds, checks storage and writes a temporary file before renaming it into a vacant slot. Saved items share eight slots and the 128 KiB filesystem; temporary imports remain RAM-only. Search responses include URL/slug pairs, and the browser displays selectable slugs. No GIF conversion runs in HA and no direct slug notification playback is implemented.

Static picture export: Canvas downloads the current editor pixels as an uncompressed 24-bit 64×64 `.bmp` (12,342 bytes; Home Assistant's media uploader accepts it). NoticeImage streams it row by row (also still reads the older `UMIMG001` RGB565 format). DLNA (`DlnaRenderer`): the clock announces itself over SSDP as a media renderer; HA's DLNA Digital Media Renderer integration lists it as a media player, where Browse media/Play shows a BMP/PNG/JPEG until Stop. Notices and the night clock take priority; it sits above WLED and the face. Event subscriptions answer 412 (HA polls). No audio, video or animation. Untested on a device.
