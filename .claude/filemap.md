# Ultimatrix file map
ESP32 HUB75 LED-matrix clock firmware (ESP-IDF + Arduino libs), 8 clock faces, web settings UI. Purposes inferred from names; verify before relying on them. Skips vendor/, images, LICENSES/.

## Root
- CLAUDE.md, HANDOFF.txt, WORKLOG.txt: project rules; shared session handoff (read first); work log
- README.md: user docs for all clock faces. LICENSE*, THIRD_PARTY_NOTICES.md, LICENSE_SCOPE.md: licensing
- .github/workflows/release.yml: release CI
- docs/: ARCHITECTURE.md, UPDATES.md (OTA/updates), FOOTBALL_SNAPSHOT_PLAN.md, TETRIS_MIGRATION_PLAN.md, previews/ (README gifs, welcome-preview.html)
- web-flasher/index.html: browser flasher page

## firmware/
- CMakeLists.txt, main/ (CMakeLists, idf_component.yml), components/clock-libraries/CMakeLists.txt: ESP-IDF build wiring
- partitions.csv, sdkconfig.defaults, dependencies.lock: partition table, IDF config, deps
- src/main.cpp (~1080 lines): app entry, setup/loop, face switching, web server/routes

### clockfaces/<name>/ (each: Clockface.cpp/.h, library.json)
- bridge, luigi, mario: simple faces (Clockface only)
- pacman: + Maze.h (maze data); colours in lib/core/PacmanColors.h
- word: + WordFont, WordLanguages (word clock)
- tetris: + TetrisRenderer, TetrisScreens
- f1: + F1Renderer, F1Screens, LoadingTyre.h (feed: lib/f1-feed)
- football: + FootballRenderer, FootballScreens (feed: lib/football-feed)
- MotoGP: no clockface dir; feed in lib/motogp-feed

### lib/core/ (shared services)
- IClockface.h: face interface. Locator.h: service locator
- Settings.cpp/.h: persisted settings (NVS). JsonScan: lightweight JSON scanning
- FirmwareUpdate.cpp/.h, UpdateTrust.h: OTA update + trust/verification
- TimeService, ClockTime, NightClock, SunTimes: time, timezone, night mode, sunrise/sunset
- DisplayControl, PanelLayout, BootScreen, ScreenTimer, FlipDigits, NumericFont, LapFonts: display/panel helpers, fonts
- TelnetConsole.cpp/.h, Console.h: remote logging console
- DiskCache.h, CacheBlob.h (untracked, new): caching of feed data
- NetworkBudget.h, MemoryLimits.h, BackgroundWait.h: resource limits/scheduling
- EspnFeed.h, FootballCatalog.h, F1Drivers.h, PacmanColors.h: shared feed/data tables
- qrcodegen.c/.h: QR code (Nayuki)

### lib/ feeds and face kits
- f1-feed/: F1Live, F1LiveState, F1Ticker
- football-feed/: EspnParse, FootballSnapshot.h, FootballTicker, GoalAnimation, TeamColors, TeamKit.h
- motogp-feed/: MotoGpTicker.cpp, MotoGpRiders.h, MotoGpScan.h
- tetris/: TetrisDigits, TetrisLayout, TetrisShapes
- faces/: FaceKit.h, MiniFont.h, PlumberSprites.h (shared drawing kits)

### web/ (settings UI served by device)
- settings.html, settings.js, theme.css, manifest.webmanifest, icons/, faces/ (face thumbnails .png)

### tools/
- idf.sh, setup-idf.sh: IDF env/build wrappers. build-bridge.sh, build-webflasher.sh, package-firmware.py: build/release packaging
- gen_icons.py, gen_tzdb.py, zones.csv: asset/timezone generators
- preview/: host-side face renderer for previews/gifs. boot.cpp, faces.cpp/.py, showcase.cpp/.py, refresh.py, faces-scenarios.h, showcase-data.json, motogp-*.cpp/.py, shim/ (Arduino/ESP stubs)

### vendor/
- adafruit gfx, busio, hub75 (matrix driver), wifi-manager (patched WiFiManager.cpp). Avoid reading.
