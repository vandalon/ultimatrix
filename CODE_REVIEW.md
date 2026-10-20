# Ultimatrix code review (20 Oct 2026)

Read-only review of `main` at ba99333, done unattended. Nothing was built, flashed or run; no code was changed.
Method: read the HANDOFF/filemap, then read the code paths below line by line and checked suspicions with grep. Findings were cross-checked against the code before listing; items I could not confirm are marked *unverified*.

## Coverage

| Read in full or near full | Read in part / skimmed | Not reviewed in depth |
|---|---|---|
| src/main.cpp, FirmwareUpdate, Settings, Storage, Mqtt, Notice/NoticeImage/NoticePng, DlnaRenderer, TelnetConsole, JsonScan, CacheBlob, NetworkBudget, DisplayControl, TimeService/ClockTime, SunTimes, PanelTier, HaClient, HaQuery, HaConfig, FootballTicker, EspnParse/EspnFeed, F1Live/F1LiveState, WledRealtime/WledApi/WledWs/WledCanvas, Pac-Man (+Maze), Mario, Tetris screens, Word (Clockface + Dutch/German/Nordic phrase maths), Home/Football/F1 Clockface + Screens, Scanner, Bridge, CI workflow, sdkconfig | HaTemplate (first 330 lines), F1Ticker (first 420 lines), HaMedia (fetch path only), web JS (grepped for XSS/navigation sinks), Birthdays, repo secret scan | FootballRenderer/F1Renderer/HomeRenderer/GoalAnimation drawing code, Luigi (compared to Mario by diff only), MotoGP ticker/scanner, WledEffects/WledState, BootScreen, vendor/hub75 driver, WiFiManager, tools/preview, rest of settings.js/canvas.html |

## What each face is supposed to do (as understood) and the verdict

| Face | Purpose | Verdict |
|---|---|---|
| Home | Home Assistant dashboard: Energy, Home (tiles + icon bar), Now playing (album art), Devices (low battery/offline) pages; alerts for alarm, doorbell, washer/charger done, door/window open too long. HA WebSocket + `render_template`. | Logic sound. Findings H1, M1, L11. |
| Football | Live matches with goal/card timeline, results, upcoming fixtures, league tables; goal, card and substitution animations; ESPN over HTTP; LittleFS cache. | Findings M3, L7. |
| F1 | Live timing (SignalR) for race/quali/practice, results lists, between-sessions countdown, standings; ESPN/Jolpica/OpenF1 fallbacks. MotoGP reuses it. | Parsers are bounded and defensive. No defects found in the parts read. |
| Tetris | Falling-digit clock, date falls first at each new day. | State machine correct. Licence provenance of digit table is open (M9). |
| Words | 16 languages, four transition effects, optional date in words. | Phrase maths checked for Dutch, German, Danish, Swedish, Norwegian: correct, array bounds hold. No defects. |
| Mario / Luigi | Scrolling course, blocks hit at minute/hour turn, nine worlds, noon finale, night sky follows the sun. | Works as designed; over 90 % identical to Luigi (L6). |
| Pac-Man | Self-playing 16x13 maze, clock overlay, generated mazes. | Cosmetic logic slips (L5). |
| Bridge | Stop-over image that shows where to choose a face. | Fine (L14). |
| Scanner | Developer benchmark face, not shipped, not in CI. | Fine; will rot unbuilt. |

## Findings (severity: High > Medium > Low > Info)

### High

**H1. [Optional admin PIN added, see docs/ARCHITECTURE.md] Configuration API is unauthenticated and CSRF-able; it can send stored secrets to an attacker.**
`POST /api/ha` (main.cpp `haSave`) accepts a new `host` and keeps the saved long-lived token; `HaClient` then signs in to the new host with it (TLS is unverified, so even "https" gives no protection). `POST /api/mqtt` (`mqttSave`) does the same with the saved MQTT user/password (plain TCP). Any LAN host, or any web page the user opens (an urlencoded form POST is a "simple" request and needs no CORS), can trigger it.
Fix: require a per-device admin PIN or token (set during welcome; sent as header, not cookie); reject requests whose `Origin`/`Host` do not match the clock; clear the stored token/password whenever the host changes unless the secret is re-entered; make state-changing routes POST-only with `Content-Type: application/json` and a custom header.

**H2. No authentication or CSRF protection on control and destructive routes; CORS is `*` on the WLED API.**
Unprotected: `/api/firmware` (install, switch face), `/api/firmware/server`, `/api/firmware/reboot`, `/settings` (enables telnet), `/api/storage/clear`, `/api/canvas/slot` (delete drawing), `/api/wled/backup` (restore wipes presets), `/api/display`, `/api/wifi/setup`. `GET /win?...` changes state with a GET (an `<img>` tag is enough). OPTIONS preflights are answered with `Allow-Origin: *` and `Allow-Headers: *`, and WLED responses expose name, MAC, IP, BSSID cross-origin. HANDOFF already lists "no auth" as known; the CSRF angle makes it reachable from the internet side of a browser.
Fix: same token/Origin check as H1 in one shared guard in `postRoute`; remove state changes from GET (`/win`) or gate them; drop `Access-Control-Allow-Origin: *` except for the read-only `/json*` endpoints the WLED app needs, and never for POST.

**H3. Release signing key sits in a CI job with unpinned third-party code, and auto-update is on by default.**
`.github/workflows/release.yml` pins actions by tag (`@v7`, `esp-idf-ci-action@v1`) and runs `pip install cryptography` unpinned in the same job that writes `update-signing.pem`. One compromised dependency can sign firmware that every clock installs (auto-update default ON, one key for all faces).
Fix: pin actions to commit SHAs and pip with `--require-hashes`; split signing into its own job with an environment approval and only the artifacts it needs; consider signing offline or with a KMS key; add a compiled-in minimum release id (see M1).

### Medium

**M1. TLS certificates are never verified (feeds, HA, update download); update replay floor is only in NVS.**
`CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y`, `setInsecure()` everywhere. Update images are protected by the ECDSA-signed manifest, which is good, but a man-in-the-middle can (a) freeze a clock on an old release by serving a stale signed manifest, (b) install an older signed release of a face that has no history entry (new face, or after factory reset wipes `um-updates`), (c) read the HA token (see H1). The KLIPY client already uses `esp_crt_bundle_attach` with `VERIFY_REQUIRED`, so the bundle exists.
Fix: use the bundle for `github.com` and `objects.githubusercontent.com`; for HA offer fingerprint pinning (trust on first use) or a custom CA; add `constexpr kMinRelease` to the firmware so replay protection survives a factory reset.

**M2. KLIPY key is shipped in binaries (XOR "ultimatrix"), the search route is unauthenticated, and official release builds have no key.**
`tools/obfuscate-key.py` itself says the XOR is not protection; anyone with a release `.bin` can recover it, and anyone on the LAN can burn the quota through `GET /api/canvas/search`. Separately, `klipy_key.h` is gitignored and CI has no step to create it, so builds from the release workflow answer 501 "This firmware has no KLIPY key" (`WledCanvas.cpp:833`). A clock that OTA-updates from a local build to the GitHub release loses GIF search. README does not mention this.
Fix: either let the user paste their own key in Settings (NVS), or run a small proxy you control; rate-limit the route; document the limitation until fixed.

**M3. Football: one failing board stops the whole refresh and the overview is never rebuilt.**
`FootballTicker::refresh()` ORs every board result into `ok`; `!ok` returns `RETRY_MS` before `buildOverview` (FootballTicker.cpp:614). One league with a retired ESPN slug, or one favourite whose team id 404s, keeps the face on stale data and re-hits ESPN every 30 s. `checkFavourites` has the same shape: a failing id keeps `_nationalCheckedAt` at 0 so it is retried every round.
Fix: track failure per board with exponential backoff; always build the overview from what is available; surface "league X unavailable" in settings.

**M4. Changing any time setting resets the "time is synced" flag.**
`ClockTime::begin` -> `ultime::begin` sets `synced=false` and restarts SNTP. `saveSettings` and the MQTT hook call it when 12/24 h, timezone or NTP server changes (and geo-IP on zone change). For seconds (or until the server answers) `timeStatus()==timeNotSet`: F1 shows its loading flag, the night schedule drops out (brightness jumps), the birthday screen and Football refresh pause.
Fix: only reset the flag when the NTP server changes; for `use24h` just store the bool; for timezone call `setenv("TZ")` + `tzset()` without touching SNTP.

**M5. A picture notification held for the morning parks all feeds for up to an hour.**
`notice::poll` calls `holdFeeds(HOLD_NOTICE, !imageDone || current.picture)`; a normal-priority notification with a picture sent at night stays `on` but not `started` for up to `kHoldMs` (1 h) with `picture` set (Notice.cpp:129), holding Football/F1/Home feeds and the 8 KB picture.
Fix: hold only while loading or while actually showing (`started`); keep the picture but release the feeds while waiting.

**M6. Appending a frame that fails to write deletes the whole saved animation.**
`WledCanvas.cpp:562`: on `!ok` the code calls `LittleFS.remove(file)` also in append mode, destroying earlier frames (a full flash disk is the likely trigger).
Fix: in append mode write to `canvas-save.tmp` (copy + new frame) and rename, or truncate back to the old size; only remove on non-append failure.

**M7. The web server runs inside the render loop, so slow clients and long handlers freeze the clock.**
`server.handleClient()` is called from `background()` on the drawing task. A client that opens a socket and dribbles bytes holds it up to the library's wait (seconds); GIF search (up to ~30 s of TLS), `/api/canvas/pixels` readback, backup download and update checks run synchronously. HANDOFF notes "POST bodies have no total deadline".
Fix: lower `HTTP_MAX_DATA_WAIT`, add an overall per-request deadline, run long handlers on a task and poll for the result, limit to one connection per client IP.

**M8. Unauthenticated realtime and DLNA inputs.**
(When WLED is enabled.) One UDP packet in WLED-realtime format with timeout byte 255 holds the panel for an hour (`WledRealtime.cpp:90`) and parks the feeds; DDP/Art-Net/E1.31 do the same for 3 s per packet. DLNA `SetAVTransportURI` makes the clock fetch any http(s) URL up to 1024 chars, and each M-SEARCH triggers up to six unicast replies (small reflection factor). WebSocket `/ws` lets any host evict the app's connection.
Fix: cap hold at ~30 s unless refreshed, honour the existing `ignoreLive` option for realtime, optional source-IP allowlist, per-source rate limit for SSDP replies.

**M9 (downgraded to Low). Tetris digit table: origin is known (TetrisAnimation, Tobias Blum, LGPL-2.1+, credited in THIRD_PARTY_NOTICES); only the lack of a notice on the digit tables themselves is unconfirmed.**
The repo is public. If the table is derived from third-party work the notice must be present or the table replaced.
Fix: trace the origin, add it to THIRD_PARTY_NOTICES, or regenerate the digits from your own font.

**M10. `kKnownKeys` erases every unknown NVS key on every boot, and crowded-NVS cleanup removes half of a face's records.**
Any new key added to code but not to `kKnownKeys` (main.cpp:1569) is silently erased at the next boot. Verified by grep that the list matches today's keys, but there is no guard against drift. Separately, when NVS has <100 free entries a non-Home build erases `haCfg, haTile4, haWasher, haMedia, haTok, haEn, haBarV, haIcnV, haWeather` but not `haTile5/6` or `haIcn0-5`, so a later switch back to Home finds orphan icon records but no config.
Fix: keep key lists next to the code that owns them (one registry per module, compile-time asserted unique) and erase per module (all or none); log a warning instead of deleting for unknown keys older than N boots, or at least add a build-time check that greps `put*("key"` against the list.

**M11. [PIN prompt added] Telnet console has no authentication.**
Default off, but `/settings` can turn it on without auth (H2), and it offers `factory-reset yes`, `reboot`, `update`. Port 23 is cleartext.
Fix: PIN prompt (same admin token), or drop `factory-reset` from telnet.

### Low

**L1. Time is read field by field.** `ClockTime::getHour()/getMinute()/getSecond()` each call `localtime_r` separately. At a minute rollover a face can read 12:59 hour with 13:00 minute and draw 12:00 for a frame; faces that redraw only on change (Football, Tetris `timeKey`) can then run a false transition. About once per week per clock. Fix: one `struct tm` snapshot per frame (`ClockTime::snapshot()`).

**L2. MQTT command handling.** (a) Retained messages are not ignored for commands, so a retained `restart/set` or `update/set INSTALL` repeats after every reconnect. (b) `E_NTP_SERVER` accepts up to 767 characters but `Settings::save()` rejects >128, so the value lives in RAM, saves fail silently. (c) `atoi` accepts garbage: `"abc"` sets brightness 1. Fix: ignore `event->retain` for button/update/face; check lengths; use `strtol` with end-pointer check.

**L3. WebSocket buffer underflow.** `WledWs.cpp:70` `have += client.read(...)` without checking for -1; a read error right after `available()>0` underflows `have`, then `memmove`/index use it. Rare; fix: `int got = ...; if (got > 0) have += got;`.

**L4. WLED backup restore is destructive before it validates.** `backupRestore` deletes all presets, then parses; a malformed `presets` object leaves none. Fix: parse into a temporary array, validate, then swap.

**L5. Pac-Man.** (a) Ghosts leaving the house and `endScare()` choose scatter/chase from `(modeClock/1000)%2` instead of `scheduleMode(modeClock)` (Clockface.cpp:286, 316). (b) Eyes steer greedily by distance to the door and can orbit a wall forever; a BFS from the door would fix it. (c) `MazeBuilder::build` ends in `do { attempt } while (!valid)` that assumes seed 1 is valid; any generator change turns this into a watchdog reset; bound it. (d) A comment promises a lives HUD that does not exist; `chooseGhostDir` has a dead loop (lines 260-263).

**L6. Mario/Luigi duplication and wrap-prone timers.** `luigi/Clockface.cpp` and `mario/Clockface.cpp` differ in only about 170 diff lines (both sides combined) out of ~2160 after name normalisation. Fix: shared `PlumberFace` with a character config. Also `now * k` in scenery (`drawFlakes`, fish, embers) overflows uint32 after ~5 days and the pattern jumps; `now >= bumpAt + 150` wraps at 41 days. Use signed differences.

**L7. Settings reloaded from flash every 2 s.** `FootballTicker::loadConfig()` builds a `Settings`, runs `load()` (NVS blob read, ~7 String allocations) every `CONFIG_POLL_MS`, on a heap-starved face. Until the first save it also runs `lightSensorReadable()` (16 ADC reads, 48 ms) each time. `restoreBoard/readMatch` reopen the cache file per index every round (O(n²) opens, every 10 s during live play). Fix: a change counter in `Settings` and a snapshot struct; read the cache file sequentially.

**L8. Dead migration code and upgrade path.** `tidyNvs()` runs before `Settings::load()` and erases `settingsV1` and the legacy keys, so the V1/legacy branches in `Settings::load` (about 60 lines) never run, and a clock upgrading from pre-V2 firmware starts from defaults. Intended per HANDOFF; delete the dead code or move tidy after load.

**L9. LittleFS failure mode.** If the mount fails on non-blank flash, `beginDiskCache()` disables the cache forever and rescans all 128 KiB on every attempt (`WledCanvas::mount()` retries per request). There is no way to reformat from the UI (factory-reset via telnet only). Fix: remember the failure per boot, add "Reformat flash disk" in Advanced > Storage with confirmation.

**L10. Serial output in request paths.** `onNotFound` and `wantsLightPage` `Serial.printf` per request; 115200 baud stalls the loop that also draws (the code comments say so elsewhere). Captive-portal probes make this common. Fix: rate-limit or log through `consoleLog` at a debug level.

**L11. HA client details.** (a) Control flow uses `strstr(text, "\"auth_ok\"")` on whole messages; a sensor name containing that token would misfire. Parse `"type"` instead. (b) `xTaskCreatePinnedToCore` is used directly, so `reapFeedTasks()` does not know the HA task and can report "all feeds stopped" while it lingers (≤3 s, up to 60 s after failures). Use `createFeedTask`/`stopFeedTask`. (c) Header lines >95 bytes split and a remainder of `"\r"` ends header parsing early. (d) `haSave` applies the new config in RAM before `ha::save()` succeeds.

**L12. JsonScan escapes.** `\b`, `\f`, `\r` become the letters b, f, r (JsonScan.cpp:57-58); lone surrogates become '?'. Map control escapes to a space.

**L13. Sun position recomputed twice per frame.** `DisplayControl::update` calls `sun::isNight` twice (about 20 trig calls each) at the frame rate. Cache per minute.

**L14. Bridge build bakes `http://192.168.7.77/ultiMatrix` into a public image and saves it permanently.** `FirmwareUpdate::begin` stores it when no server is set, and the installed face keeps it, so migrated clocks on other networks never reach GitHub. Fix: default to GitHub, make the private server an explicit CMake option, clear it after the first successful install.

**L15. Latent: `loadImage` leaks its `busy` flag** if the URL check (line 208) fails after `busy.exchange(true)` succeeded. Unreachable today (callers cap URLs below `kDownloadUrlMax`), but one constant change away from blocking all pictures until reboot. Move the URL check before the `exchange`.

**L16. Secrets in plain flash.** Wi-Fi password, HA token and MQTT password are in NVS without flash encryption; MQTT, HTTP and telnet are cleartext. Acceptable for the class of device; document it, optionally use NVS encryption.

**L17. Housekeeping.** `panelBrightness()` returns the same value on every branch (dead code); `logf("%d", size_t)` at FootballTicker.cpp:818, 904; several hand-built JSON strings do not escape (Storage names, effect names; sources are trusted today); README DLNA paragraph is stale per HANDOFF; the Scanner face is not in CI and will bit-rot; `Mqtt::_error` is written by the MQTT task and read by the main task without a lock (benign); `Notice` draws UTF-8 text with the ASCII-only font, so accented characters probably show as garbage (unverified); the DHCP hostname stays `ultimatrix` even when the mDNS name differs (two clocks share a DHCP name).

## What is solid

Signed manifests with replay history and ELF-hash match; bounded streaming JSON/PNG/JPEG decoders with size caps; CRC'd, versioned NVS blobs; entity-id validation before Jinja interpolation (no template injection found); album-art fetch restricted to HA-served paths (token never goes to other hosts); `postMessage` origin check; no `innerHTML` sinks in the web UI; memory guards before large allocations; rollback after failed boot; no secrets in git (`klipy_key.h`, signing key and publish script are ignored).
