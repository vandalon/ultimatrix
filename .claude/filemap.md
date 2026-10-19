# Ultimatrix file map
ESP32 HUB75 LED-matrix clock firmware (ESP-IDF + Arduino component), 8 clock faces plus a bridge image, web settings UI. Use this index to choose targeted reads. Skips dependency internals, build output, generated files and binary assets.

## Root
- AGENTS.md -> CLAUDE.md: shared Codex/Claude project rules (one source)
- .claude/filemap.md: this routing index; update affected entries when files change purpose
- HANDOFF.txt, WORKLOG.txt: current shared session handoff (read first); local work log (both gitignored)
- README.md: user docs for all clock faces. LICENSE*, THIRD_PARTY_NOTICES.md, LICENSE_SCOPE.md: licensing
- .github/workflows/release.yml: release CI
- .gitignore: local tooling, build output and handoff/log exclusions
- docs/: HARDWARE.md (wiring, boards, power, LDR, troubleshooting), ARCHITECTURE.md (runtime/design), UPDATES.md (signed OTA)
- docs/photos/: real-panel Home Assistant photos (connecting, home, energy, now playing; 1000 px) shown in a row at the top of README
- docs/previews/: README GIFs and welcome-preview.html (standalone welcome mockup)
- hardware/enclosure/: parametric OpenSCAD P3/P2.5 casing prototype, optical grid samples, STL exports and fit limitations
- web-flasher/index.html: ESP Web Tools installer; uses generated firmware/<mode>/manifest.json payloads

## firmware/
- CMakeLists.txt, main/idf_component.yml: ESP-IDF project and Arduino dependency
- main/CMakeLists.txt: mode/source selection, generated timezone table, embedded web assets; MotoGP selects the F1 face
- components/clock-libraries/CMakeLists.txt: vendored display/Wi-Fi libraries and shared panel colour-depth setting
- partitions.csv, sdkconfig.defaults, dependencies.lock: partition table, IDF config, deps
- src/main.cpp: app entry (setup/loop), service wiring, boot/geo-IP/update flow and web/API routes, read-only /api/screen BMP preview
- HANDOFF.txt: older firmware-local notes; root HANDOFF.txt holds current state

### clockfaces/<name>/ (each: Clockface.cpp/.h, library.json)
- scanner/: standalone moving white bar, no WLED; drawing benchmark + refill telemetry, normal quality and 8-row ring
- bridge/: migration image showing where to choose a face, rather than a time display
- mario/, luigi/: scrolling worlds, pipe/plant jumps, separate minute/hour hits, exit/swipe/entry; shared sprites/course in lib/faces/
- pacman/: self-playing maze clock; Maze.h generates/stores the maze; palette in lib/core/PacmanColors.h
- word/: word clock; WordFont draws glyphs, WordLanguages defines language layouts
- tetris/: falling-digit clock; TetrisRenderer draws, TetrisScreens controls timing/frame state
- home/: Home Assistant face; Clockface.cpp fills a Frame (pages, alerts, hourly bars) from HaClient, HomeRenderer.cpp draws the configurable tiles (including weather and the EV car with its bolt/plug badge), the dimmed album-art backdrop and the Now playing page, configurable status counts, plain background and breathing colon (shared with the native preview)
- f1/: shared F1/MotoGP face; F1Renderer draws, F1Screens controls pages/timing, LoadingTyre.h holds the loading tyre bitmap
- football/: sports face; FootballRenderer draws, FootballScreens controls pages/timing; releases display snapshot during WLED and restores it on return (feed: lib/football-feed)
- MotoGP has no separate clockface directory: reuses f1/ with UM_MOTOGP and lib/motogp-feed

### lib/core/ (shared services)
- IClockface.h: face interface. Locator.h: service locator
- Settings.cpp/.h: persisted settings (NVS). JsonScan: lightweight JSON scanning
- Birthdays.h: birthday setting parse/clean/lookup. BirthdayCanvas.h: 3 KB band renderer + fonts; BirthdayParty.h: the birthday screen (24 s story loop)
- FirmwareUpdate.cpp/.h, UpdateTrust.h: OTA update + trust/verification
- TimeService, ClockTime, NightClock, SunTimes: time, timezone, night mode, sunrise/sunset
- PanelTier.h: panel colour quality levels (depth, I2S clock, refresh), the NVS state and the retry rule
- DisplayControl, PanelLayout, BootScreen, ScreenTimer, FlipDigits, NumericFont, LapFonts: display/panel helpers, fonts
- TelnetConsole.cpp/.h, Console.h: remote logging console
- ScreenPng.h: bounded 192x192 indexed PNG encoder (kScale 3) from 64x64 display rows; square LED gaps, fixed DEFLATE runs/repeated cells/rows, no large framebuffer or zlib window
- Mqtt.cpp/.h: optional MQTT link to Home Assistant (default off): config in NVS, discovery entities, commands queued by the esp-mqtt task and applied by poll(), state publishing, 10-second CPU task/refill diagnostics, changed-screen MQTT camera snapshots and a dynamic Saved picture dropdown for Canvas slots
- DlnaRenderer.cpp/.h, DlnaDescriptions.h: DLNA media renderer (HA Browse media/Play/Stop of pictures; SSDP + SOAP, polling only) and UPnP service descriptions; wired in main.cpp, untested on device
- Notice.cpp/.h: full-panel text/image notifications from Home Assistant, drawn over any face by main.cpp. NoticeImage.cpp/.h: bounded asynchronous HTTP(S) UMIMG/JPEG/PNG downloads and 64x64 fit/fill; NoticePng.cpp/.h decodes PNG with image-sized history and alpha over black; normal-priority images alternate with optional text
- DiskCache.h: mounts LittleFS; formats only genuinely erased cache flash
- CacheBlob.h: versioned NVS POD blobs with size/CRC validation
- NetworkBudget.h, MemoryLimits.h: shared download/update gates, feed-task coordination and collection limits
- BackgroundWait.h: waits while servicing the background callback registered by main.cpp
- EspnFeed.h: yielding HTTP/JSON stream reader and ESPN timestamp parsing
- FootballCatalog.h, F1Drivers.h, PacmanColors.h: competition/team catalog, driver metadata and Pac-Man palettes
- qrcodegen.c/.h: QR code (Nayuki)

- WledCanvas.cpp and web/canvas.html: canvas editor (/canvas) and dedicated GIF loader (/gifs; same embedded asset, separate views); GIF search is done by the clock: /api/canvas/search, key in gitignored lib/wled/klipy_key.h, with RAM animations and copyable slugs; explicit Save played GIF keeps up to 24 frames / 5 seconds in a vacant flash slot for HA playback; GIF imports use a shared 32-colour RGB888 palette (no dithering), adjustable import contrast and packed/RLE/delta/LZ/XOR-LZ indexes and bounded upload chunks; playback reuses the canvas buffer; import budget follows available data heap with reserved headroom; abandoned imports are reaped independently of the visible effect.

### lib/wled/ (optional WLED overlay, compiled into every face but the bridge; main.cpp UM_WLED_OVERLAY blocks)
- Wled.h: API (state, options, effects, overlay poll/wanted/draw/hidden, realtime, JSON API). WledState.cpp: state, presets/playlists, timers, nightlight, fades (NVS wledS/P/T). WledEffects.cpp: 36 effects + palettes (draw into the back buffer). WledRealtime.cpp: DDP/WLED UDP/Art-Net/E1.31 listeners (non-blocking, drop when not on the panel), panel lock, overlay glue. WledApi.cpp: WLED JSON API, /win, /api/wled options, mDNS _wled._tcp. WledPaint.cpp: 8 KB Paint buffer for the "i" pixel array

### lib/ feeds and face kits
- f1-feed/: F1Live (SignalR/WebSocket live timing), F1LiveState (message/state parser), F1Ticker (calendar/results/snapshots)
- football-feed/: FootballTicker collects ESPN data; EspnParse parses it; FootballSnapshot.h defines display snapshots; staging is allocated on demand and trimmed with disposable caches during WLED; GoalAnimation, TeamColors, TeamKit.h handle goal visuals and kits
- motogp-feed/: MotoGpTicker.cpp collects results/live timing into F1Ticker snapshots; MotoGpScan.h parses streams; MotoGpRiders.h maps riders/bikes
- tetris/: TetrisDigits, TetrisLayout, TetrisShapes (digit animation, layout and block shapes)
- ha-feed/: HaConfig (slot settings in one NVS blob, separate fourth-tile/legacy-weather/washer-alert keys, token write-only), HaDefaults (defaults and entity rules), HaTemplate (builds the Jinja template, parses the result line), HaClient (WebSocket task: auth, render_template subscriptions, reconnect), HaMedia (Now playing: players template/parser, album-art fetch + ROM JPEG decode to 36x36)
- firmware/lib/ha-feed/HaDevices.cpp/.h — Devices page: names of low-battery and offline devices (own HA template + parser)
- faces/: FaceKit.h, MiniFont.h, PlumberSprites.h (drawing helpers, small font and walking poses); PlumberWorld.h defines courses, mushroom drops, items and Piranha pipes

### web/ (settings UI served by device)
- settings.html, settings.js, theme.css, manifest.webmanifest, icons/, faces/ (face thumbnails .png)
- wled.html: effects/presets controls and navigation; canvas.html: canvas and GIF views, visible-only live screen in the drawing editor, LED dot styling and pause-on-edit; static panel-ready UMIMG download from the editor; opening a page does not take over the panel

### tools/
- idf.sh, setup-idf.sh: per-mode IDF command wrapper and SDK setup
- build-bridge.sh: builds/packages the migration bridge image
- build-webflasher.sh: merges full-flash images and ESP Web Tools manifests into web-flasher/firmware/ (generated)
- package-firmware.py: packages application images and signed OTA manifests into dist/ (generated)
- check-static-ram.py: post-build check that fails a face above the 60 KB static RAM budget (hooked in firmware/CMakeLists.txt)

- gen_icons.py: renders BootScreen's U logo into web app icons
- gen_tzdb.py, zones.csv: generate the build-time timezone table from zone rules
- preview/faces.cpp, preview/faces.py, preview/faces-scenarios.h: native pixel-art face runner, GIF generation and scenarios
- preview/boot.cpp, preview/showcase.cpp, preview/showcase.py, preview/showcase-data.json: native boot/sports/Tetris previews and sample snapshots
- preview/birthday.cpp, preview/birthday.py: native birthday-screen render to GIF
- preview/home.cpp, preview/home.py: native Home Assistant face scenes (home, energy, alerts, status) with made-up values, contact sheet via ffmpeg
- preview/refresh.py: exports README GIFs, stills/contact sheets and web face thumbnails from native renders
- preview/motogp-snapshot.py: fetches real MotoGP API data for preview snapshots; preview/motogp-scan-test.cpp checks scanners against saved downloads
- preview/shim/: Arduino/ESP/display/service stubs for native previews

### vendor/
- uzlib/: vendored inflate/checksums for PNG notifications; size-checked output buffer supplies history, no fixed dictionary; local input validation enabled
- gfx/, busio/, hub75/, wifi-manager/: vendored graphics, bus I/O, HUB75 driver and Wi-Fi provisioning dependencies; inspect only when the task needs them
- hub75/src/ESP32-HUB75-MatrixPanel-I2S-DMA.cpp: row streaming from packed six-bit lanes; full-precision color buffers allocated per frame, optimized 7/8-bit refill and aligned pixel writes; dense 6-byte column buffers and allocation-free displayed-row readback; lower panel half switches one sweep after the upper (seam fix, setRingMode) with the draw side waiting once per flip (RingStats flips/waits). Published as vandalon/ESP32-HUB75-DMA-Memory-Optimized (MIT): keep both copies in step
