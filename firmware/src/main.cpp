#include <Arduino.h>
#ifdef UM_GAMEPAD
#include "Gamepad.h"
#endif
#include <atomic>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Clockface.h>
#include <PanelLayout.h>
#include <ClockTime.h>
#include <Settings.h>
#include <Tlds.h>
#include <DisplayControl.h>
#include <SunTimes.h>
#include <NightClock.h>
#include <Birthdays.h>
#include <BirthdayParty.h>
#include <FirmwareUpdate.h>
#include <NetworkBudget.h>
#include <PanelTier.h>
#ifdef UM_FOOTBALL_ONLY
#include <FootballCatalog.h>
#elif defined(UM_MOTOGP)
#include <MotoGpRiders.h>
#elif defined(UM_F1)
#include <F1Drivers.h>
#elif defined(UM_WORD)
#include <WordLanguages.h>
#elif defined(UM_HOME)
#include <HaClient.h>
#include <HaConfig.h>
#include <HaMedia.h>
#include <HaQuery.h>
#endif
#include <BackgroundWait.h>
#include <BootScreen.h>
#include <Console.h>
#include <TelnetConsole.h>
#if defined(UM_PONG) || defined(UM_INVADERS)
#include <PlaySocket.h>
#endif
#include <Mqtt.h>
#include <Notice.h>
#include <Storage.h>
#include <AdminPin.h>
#include <DlnaRenderer.h>
#include <GzipAsset.h>
#ifdef UM_SPORTS_SWITCH
#include <Preferences.h>
#include <SportsWatch.h>
#endif
#include <nvs.h>
#ifdef UM_WLED_OVERLAY
#include <Wled.h>
#endif
#include <memory>
#include <new>
#include <strings.h>

// Temporary service flags used by the existing sport data modules during migration.
std::atomic<uint32_t> memoryAllocationFailures{0};
static bool displayReady = false;

static void allocationFailed(size_t, uint32_t, const char *) {
  memoryAllocationFailures.fetch_add(1, std::memory_order_relaxed);
}

std::atomic<bool> firmwareUpdating{false};
std::atomic<int> firmwareInstallPercent{-1};
std::atomic<bool> liveEventOn{false};
std::atomic<bool> liveFavouriteOn{false};
volatile bool faceBootDone = false;

static MatrixPanel_I2S_DMA *display = nullptr;
static Clockface *face = nullptr;
static ClockTime clockTime;
static WebServer server(80);
static WiFiManager wifi(SerialConsole);  // its debug output also goes to the telnet log
static uint32_t nextFrame = 0;
static uint32_t restartAt = 0;
static bool updateOnly = false;  // a restart made to install an update: no face, no feeds, just the update screen, so the whole heap is free for the download
static bool bootWasScheduled = false;  // this boot exists to install an update: automatic updates do not restart again
static char scheduledMode[16] = "";  // update chosen before a low-memory reboot; started once Wi-Fi is up
static uint32_t scheduledSince = 0;
static DisplayControl displayControl;
static NightClock nightClock;
static bool nightShown = false;
static bool birthdayShown = false;  // the birthday screen is on the panel instead of the face
static uint32_t nvsFreeEntries = 1000;  // free entries in the settings storage, refreshed once a minute
static void watchNvs();
static bool dlnaShown = false;      // a picture sent to the DLNA renderer is on the panel instead of the face
static bool noticeShown = false;    // a message from Home Assistant is on the panel instead of the face
#ifdef UM_WLED_OVERLAY
static bool wledShown = false;      // WLED content (an effect, a drawing or a pixel stream) is on the panel instead of the face
#endif
static uint32_t birthdayPreviewUntil = 0;  // telnet "birthday": show the screen until then, whatever the date
constexpr uint32_t kBirthdayFrameMs = 60;
static bool birthdayPreviewing() { return birthdayPreviewUntil && (int32_t)(birthdayPreviewUntil - millis()) > 0; }

// Today's birthday, or nullptr. Never while a favourite team's match or a race session is live (liveFavouriteOn: that is
// what the face is for, the preview included; other football matches do not count), and not for a minute after, so a feed that blinks does not flash the party over the live screens.
// Nor before the time is known: until the first sync the date reads 1 January 1970.
static const Birthdays::Entry *birthdayToday() {
  static const Birthdays::Entry preview = {0, 0, "TEST", 2013};
  static uint32_t liveSeenAt = 0;
  uint32_t now = millis();
  if (liveFavouriteOn.load()) { liveSeenAt = now ? now : 1; return nullptr; }
  if (liveSeenAt && now - liveSeenAt < 60000) return nullptr;
  if (birthdayPreviewing()) return &preview;
  if (ultime::timeStatus() != timeSet) return nullptr;
  return Birthdays::on(Settings::getInstance()->birthdays, clockTime.getYear(), clockTime.getMonth(), clockTime.getDay());
}
static FirmwareUpdate firmwareUpdate;
static uint8_t appliedBrightness = 0;
static bool appliedDither = false;
static bool appliedDeepDim = false;

// The Wi-Fi manager's access point: open, so the QR code on the panel is the open-network form
static const char *kApName = "UM-SETUP";
static boot::Screen bootScreen;
static boot::Kind bootKind = boot::Kind::Logo;
static uint32_t bootSince = 0, bootAt = 0, connectedAt = 0, syncedAt = 0;
static bool portalActive = false;
static uint32_t portalCloseAt = 0;
static bool setupRefreshRestart = false;  // this start is the restart into the fast display: no point trying the saved network again
static bool setupRefresh = false;  // the panel runs fast (20 MHz, high minimum refresh) for the setup QR code only: a phone camera sees no stripes, but it costs colour depth
static bool portalIpServed = false;
static bool bootDone = false;  // the face has taken over from the boot screens
static char ipText[16] = "";
static volatile bool mdnsDue = false;
static bool mdnsStarted = false;
static uint32_t reconnectAt = 0, reconnectDelay = 5000, mdnsRetryAt = 0;
static TelnetConsole telnet;

static const uint32_t kConnectedMs = 2500;  // how long the address stays up before the clock starts
static const uint32_t kAddressMinMs = 3000;  // the address is in view at least this long from the moment the link is up
static const uint32_t kLogoFadeMs = 1400;  // matches boot's kFadeMs
static const uint32_t kLogoMs = 1800;  // the U logo on its own before the Wi-Fi symbol comes in
static const uint32_t kSyncGiveUpMs = 45000;
static const uint32_t kUpdateCheckWaitMs = 6000;  // longest the boot waits for its own update check
static const uint32_t kUpdateNoticeMs = 10000;  // extra time on the boot screen to announce an available update

static bool welcomeMode = false;  // first run: after the Wi-Fi setup the clock stays on the welcome screens (2-bit display, no face) until "Start the clock" is pressed on the settings page
static bool welcomeVisited = false;  // the settings page has been opened since this start
static uint32_t welcomeAt = 0;

static uint32_t noticeAt = 0;  // when the update notice was first drawn; it runs on its own clock because the face's setup can stall the loop

// The Connected screen stays kConnectedMs; an update check still running holds it a little longer, and an
// update that is available adds the notice
static bool bootHoldOver(uint32_t now) {
  FirmwareUpdate::Latest latest; firmwareUpdate.latest(latest);
  uint32_t since = now - syncedAt;
  if (latest.checking && since < kConnectedMs + kUpdateCheckWaitMs) return false;
  if (since < kConnectedMs || now - connectedAt < kAddressMinMs) return false;
  return !latest.available || (noticeAt && now - noticeAt >= kUpdateNoticeMs);
}

static bool updateAvailable() {
  FirmwareUpdate::Latest latest; firmwareUpdate.latest(latest);
  return latest.available;
}

// The boot screens run the panel at no less than kBootPanelBrightness: the driver sets brightness as whole pixel
// clocks per bit plane, so at a low setting the dim end of the logo's gradient has only a few levels. Colours are
// scaled back down in linear light (through the brightness curve) so the screen looks as it would at the setting.
static const uint8_t kBootPanelBrightness = 255;
static unsigned bootScale = 0;  // the user's brightness over the panel's, 8.8 fixed point
static const uint8_t *panelLut = lumConvTab_7bit;  // the brightness table of the colour depth in use (set before the panel starts)
static paneltier::State panelState;  // the colour quality level: see PanelTier.h
static int panelLevel = 0;
static uint8_t bootMap[256];  // a level at full scale -> the input that looks like it at bootScale
static uint8_t bootInverse[256];  // output level -> smallest input that gives at least it

static void setBootScale(unsigned scale) {
  if (scale == bootScale) return;
  if (!bootScale) {
    for (int o = 0; o < 256; o++) {
      int v = 255;
      for (int i = 0; i < 256; i++) if (panelLut[i] >= o) { v = i; break; }
      bootInverse[o] = (uint8_t)v;
    }
  }
  bootScale = scale;
  for (int v = 0; v < 256; v++) bootMap[v] = bootInverse[(panelLut[v] * scale + 128) >> 8];
  bootScreen.invalidate();
}

// The panel brightness for a user setting, rescaling the boot screens to match
static uint8_t panelBrightness(uint8_t userBrightness) {
  // The boot screens, the welcome and the setup QR codes are at full brightness whatever the setting (phones then scan the QR reliably), until the clock is synced and the face takes over
  setBootScale(256);
  return bootDone ? userBrightness : kBootPanelBrightness;
}

// The logo's gradient: the wanted output level is kept fractional and ordered-dithered across the lattice (8x8 Bayer),
// then mapped back to the input that gives it, for gradient steps finer than the panel's whole levels.
static void plotDithered(Adafruit_GFX &gfx, int x, int y, uint16_t r, uint16_t g, uint16_t b, bool fine) {
  unsigned lx = fine ? x : x >> 1, ly = fine ? y : y >> 1, lxy = lx ^ ly;  // lattice coordinates; 8x8 Bayer index 0..63 by bit interleave
  unsigned index = ((ly & 1) << 5) | ((lxy & 1) << 4) | (((ly >> 1) & 1) << 3) | (((lxy >> 1) & 1) << 2) | (((ly >> 2) & 1) << 1) | ((lxy >> 2) & 1);
  unsigned threshold = index * 4;  // 0..252 of a level's 256
  auto channel = [&](uint16_t in) -> uint8_t {
    unsigned i = in >> 8, f = in & 255;
    if (i > 255) i = 255;
    unsigned next = i < 255 ? panelLut[i + 1] : panelLut[i];
    unsigned out = (((panelLut[i] * 256 + (next - panelLut[i]) * f) * bootScale >> 8) + threshold) >> 8;
    return bootInverse[out > 255 ? 255 : out];
  };
  ((MatrixPanel_I2S_DMA &)gfx).drawPixelRGB888(x, y, channel(r), channel(g), channel(b));
}

static void drawBoot(boot::Kind kind, const char *text = "") {
  uint32_t now = millis();
  if (kind != bootKind) { bootKind = kind; bootSince = now; consoleLog("[Boot] screen %u at %u ms\n", (unsigned)kind, (unsigned)now); }
  if (bootScreen.update(*display, kind, now - bootSince, text)) display->flipDMABuffer();
}

// Picks the screen for the state the boot is in; cheap enough to call every 20 to 50 ms
static void bootTick() {
  if (!displayReady || bootDone) return;
  uint32_t now = millis();
  if (portalActive) { drawBoot(WiFi.softAPgetStationNum() > 0 ? boot::Kind::Joined : boot::Kind::Setup); return; }
  if (!setupRefresh && now - bootAt < kLogoMs) { drawBoot(boot::Kind::Logo); return; }  // the setup display has no logo
  static bool wasConnected = false;
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) {
    IPAddress ip = WiFi.localIP();
    snprintf(ipText, sizeof(ipText), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    connectedAt = now;
  }
  wasConnected = connected;
  if (updateOnly && connected) {
    FirmwareUpdate::Status install; firmwareUpdate.status(install);
    bootScreen.setProgress(install.total ? (int)((uint64_t)install.received * 100 / install.total) : -1);
    drawBoot(boot::Kind::Installing);
    return;
  }
  bool synced = connected && ultime::timeStatus() == timeSet;
  static bool bootChecked = false;
  if (synced && !bootChecked) { bootChecked = true; if (firmwareUpdate.checkDue()) firmwareUpdate.checkNow(); }
  if (!synced) syncedAt = 0;
  else if (!syncedAt) syncedAt = now;
  // The settings page and telnet already answer from the moment the link is up, so the address is shown
  // on every screen from here on
  if (!connected) drawBoot(portalActive ? boot::Kind::Setup : boot::Kind::Connecting);
  else if (welcomeMode) {  // no time sync or face here: the welcome screens run as soon as the link is up
    if (!welcomeAt) {
      welcomeAt = now ? now : 1;
      char url[32];
      snprintf(url, sizeof(url), "HTTP://%s", ipText);
      bootScreen.setSettingsUrl(url);
    }
    bootScreen.setWelcomeVisited(welcomeVisited);
    drawBoot(boot::Kind::Welcome);
  }
  else if (!synced) drawBoot(now - connectedAt > kSyncGiveUpMs ? boot::Kind::Error : boot::Kind::Syncing, ipText);
  else if (noticeAt && now - noticeAt >= kUpdateNoticeMs) return;  // notice over: leave the panel to the face
  else if (now - syncedAt >= kConnectedMs && now - connectedAt >= kAddressMinMs && updateAvailable()) {
    if (!noticeAt) noticeAt = now ? now : 1;
    drawBoot(boot::Kind::Update);
  }
  else drawBoot(boot::Kind::Connected, ipText);
}

// The mDNS name is set up again after every GOT_IP, so ultimatrix.local survives a router restart
static bool memoryShed = false;  // set once out-of-memory errors showed up: mDNS stays off until the next restart (telnet returns when memory is free)
#ifdef UM_WLED_OVERLAY
static bool wledBusy(uint32_t now) { return wled::wanted(now); }  // WLED has the panel: no new background network work (update checks, geo-IP); the feeds park themselves
#else
static bool wledBusy(uint32_t) { return false; }
#endif
#ifdef UM_GAMEPAD
static bool btMode() { return gamepad::active(); }  // the Bluetooth controller runs: everything but Home Assistant's MQTT link stays off to keep the heap
#else
static bool btMode() { return false; }
#endif
#if defined(UM_PONG) || defined(UM_INVADERS)
static uint32_t playHoldUntil = 0;   // somebody is steering a game: what is not needed for it stays quiet until 20 s after the last move
static bool playing(uint32_t now) { return playHoldUntil && (int32_t)(playHoldUntil - now) > 0; }
static void playSteer(int dir) {   // a move from the web page, over HTTP or the WebSocket
  if (face) face->steer(dir);
  playHoldUntil = dir == -1 ? 0 : (millis() + 20000) | 1;
}
#else
static bool playing(uint32_t) { return false; }
#endif
static bool telnetLow = false;  // telnet is off for now because memory is short
static bool telnetStarted = false;  // telnet comes up after the face starts, like mDNS
static bool updateChecking = false;  // mDNS is left off while an update check runs: its RAM goes to the TLS session
static char mdnsHost[64] = "ultimatrix";
static char mdnsIdentity[24] = "";  // unique service instance; never the user-editable name
static std::atomic<bool> mdnsPeerSeen{false};
static bool mdnsMultiple = false;  // once detected, keep the suffix until restart to avoid changing bookmarks again
static uint32_t mdnsInspectAt = 0;

static void mdnsPeer(mdns_result_t *result) {
  // Runs on the mDNS task: do not call mDNS APIs or touch settings here.
  for (; result; result = result->next)
    if (result->instance_name && strcmp(result->instance_name, mdnsIdentity)) mdnsPeerSeen = true;
}

static void mdnsBaseName(char *out, size_t size) {
  const auto &s = *Settings::getInstance();
  if (!s.mdnsUseName) { strlcpy(out, "ultimatrix", size); return; }
  size_t used = 0;
  bool separator = false;
  for (const uint8_t *c = reinterpret_cast<const uint8_t *>(s.clockName.c_str()); *c && used + 1 < size; ++c) {
    if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')) {
      if (separator && used && used + 2 < size) out[used++] = '-';
      out[used++] = *c >= 'A' && *c <= 'Z' ? *c + ('a' - 'A') : *c;
      separator = false;
    } else separator = true;
  }
  out[used] = 0;
  if (!used) strlcpy(out, "ultimatrix", size);
}
static uint32_t mdnsStartedAt = 0;  // for the telnet "mdns" command
static char mdnsServices[48] = "";  // which services registered, for the telnet "mdns" command
static void startMdns() {
  if (memoryShed || updateChecking || !(face || welcomeMode)) return;  // after the face: what can be stopped again (mDNS, telnet) sits above the permanent allocations, so stopping it leaves one big free block
  mdnsRetryAt = millis() + 15000;
  if (mdnsStarted) MDNS.end();
  char base[49];
  mdnsBaseName(base, sizeof(base));
  uint8_t mac[6]; WiFi.macAddress(mac);
  snprintf(mdnsIdentity, sizeof(mdnsIdentity), "um-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  if (mdnsMultiple) snprintf(mdnsHost, sizeof(mdnsHost), "%s-%02x%02x%02x", base, mac[3], mac[4], mac[5]);
  else strlcpy(mdnsHost, base, sizeof(mdnsHost));
  mdnsStarted = MDNS.begin(mdnsHost);
  if (mdnsStarted) {
    MDNS.setInstanceName(Settings::getInstance()->clockName.c_str());
    // All faces advertise this service, regardless of whether MQTT or WLED is enabled.
    // A unique instance avoids service-name conflicts while the hostnames are being resolved.
    if (mdns_service_add(mdnsIdentity, "_ultimatrix", "_tcp", 80, nullptr, 0) == ESP_OK)
      mdns_browse_new("_ultimatrix", "_tcp", mdnsPeer);
    const bool httpOk = MDNS.addService("http", "tcp", 80);
    snprintf(mdnsServices, sizeof(mdnsServices), "http %s", httpOk ? "ok" : "FAILED");
    if (Settings::getInstance()->telnet) MDNS.addService("telnet", "tcp", 23);
#ifdef UM_WLED_OVERLAY
    if (wled::enabled()) {  // Home Assistant finds the clock as a WLED device through this record
      const bool wledOk = wled::mdnsAdd();
      size_t n = strlen(mdnsServices);
      snprintf(mdnsServices + n, sizeof(mdnsServices) - n, ", wled %s", wledOk ? "ok" : "FAILED");
    }
#endif
  }
  mdnsStartedAt = millis();
  mdnsInspectAt = mdnsStartedAt + 1000;
  consoleLog("mDNS: %s.local %s (%s)\n", mdnsHost, mdnsStarted ? "up" : "FAILED", mdnsServices);
}

static void watchMdns(uint32_t now) {
  if (!mdnsStarted || (int32_t)(now - mdnsInspectAt) < 0) return;
  mdnsInspectAt = now + 1000;
  char actual[MDNS_NAME_BUF_LEN] = "";
  const bool renamed = mdns_hostname_get(actual) == ESP_OK && strcmp(actual, mdnsHost);
  if (!mdnsMultiple && (mdnsPeerSeen.load() || renamed)) {
    mdnsMultiple = true;
    consoleLog("mDNS: another clock or hostname conflict detected; adding MAC suffix\n");
    startMdns();
  } else if (renamed) strlcpy(mdnsHost, actual, sizeof(mdnsHost));
}

#define UM_TELNET_COMMON_HELP "  nvs  show how full the settings storage is\r\n  mdns  show the mDNS name and records (mdns restart: announce again)\r\n  update  download signed application update\r\n  update-status  show update progress\r\n  birthday  show the birthday screen for a minute (again to stop)\r\n"
#if defined(UM_MARIO) || defined(UM_LUIGI)
#define UM_TELNET_HELP "  world   pick the world: world list, world demo, world rotate, world <name>\r\n"
#elif defined(UM_F1)
#define UM_TELNET_HELP "  sim     next simulated screen, then back to real data (s)\r\n  next    the same (n)\r\n"
#else
#define UM_TELNET_HELP ""
#endif

// The update command answers once the memory check is known; the watcher reports that result later and asks about a reboot.
enum class TelnetUpdate { None, Watching, Asking };
static TelnetUpdate telnetUpdate = TelnetUpdate::None;

static void telnetWatch(Print *out) {
  if (!out) { telnetUpdate = TelnetUpdate::None; return; }  // nobody connected: forget the pending question
  if (telnetUpdate != TelnetUpdate::Watching) return;
  FirmwareUpdate::Status status; firmwareUpdate.status(status);
  if (status.lowMemory) { telnetUpdate = TelnetUpdate::None; firmwareUpdate.scheduleAfterReboot(nullptr); restartAt = millis() + 1000; out->printf("%s\r\n", status.message); }
  else if (status.ready || (!status.busy && status.failed)) { telnetUpdate = TelnetUpdate::None; out->printf("%s\r\n> ", status.message); }
  else if (!status.busy) telnetUpdate = TelnetUpdate::None;
}

static bool telnetCommand(const char *c, Print &out) {
  if (!strcasecmp(c, "nvs")) {
    nvs_stats_t stats = {};
    nvs_get_stats(nullptr, &stats);
    out.printf("NVS: %u of %u entries used, %u free (%u namespaces)\r\n", (unsigned)stats.used_entries, (unsigned)stats.total_entries, (unsigned)stats.free_entries, (unsigned)stats.namespace_count);
    nvs_iterator_t it = nullptr;   // the keys and how large each one is, to see what fills it
    for (esp_err_t err = nvs_entry_find("nvs", nullptr, NVS_TYPE_ANY, &it); err == ESP_OK; err = nvs_entry_next(&it)) {
      nvs_entry_info_t info; nvs_entry_info(it, &info);
      size_t length = 0;
      if (info.type == NVS_TYPE_BLOB || info.type == NVS_TYPE_STR) { nvs_handle_t h; if (nvs_open(info.namespace_name, NVS_READONLY, &h) == ESP_OK) { if (info.type == NVS_TYPE_BLOB) nvs_get_blob(h, info.key, nullptr, &length); else nvs_get_str(h, info.key, nullptr, &length); nvs_close(h); } }
      out.printf("  %s/%s type %d %u bytes\r\n", info.namespace_name, info.key, (int)info.type, (unsigned)length);
    }
    if (it) nvs_release_iterator(it);
    return true;
  }
  if (!strncasecmp(c, "mdns", 4) && (!c[4] || c[4] == ' ')) {  // "mdns" reports, "mdns restart" announces again
    if (strstr(c, "restart")) startMdns();
    out.printf("mDNS: %s.local %s%s%s; registered: %s; started %u s ago; Wi-Fi %d dBm ch %d\r\n", mdnsHost, mdnsStarted ? "up" : "DOWN", memoryShed ? " (stopped after memory trouble)" : "", updateChecking ? " (paused during an update check)" : "", mdnsServices, (unsigned)((millis() - mdnsStartedAt) / 1000), (int)WiFi.RSSI(), (int)WiFi.channel());
#ifdef UM_WLED_OVERLAY
    { uint32_t n[9]; wled::realtimeCounts(n); out.printf("UDP received/valid: DDP %u/%u, UDP %u/%u, Art-Net %u/%u, E1.31 %u/%u\r\n", (unsigned)n[0], (unsigned)n[4], (unsigned)n[1], (unsigned)n[5], (unsigned)n[2], (unsigned)n[6], (unsigned)n[3], (unsigned)n[7]); }
    out.printf("WLED: %s; _wled._tcp record %s; UDP listeners %s; %u packets, %u dropped; free heap %u\r\n", wled::enabled() ? "on" : "off", wled::mdnsAdded() ? "in the responder" : "absent", wled::realtimeListening() ? "open" : "closed", (unsigned)wled::realtimePackets(), (unsigned)wled::realtimeDropped(), (unsigned)ESP.getFreeHeap());
#endif
    return true;
  }
#ifdef UM_WLED_OVERLAY
  if (!strncasecmp(c, "udp", 3) && (!c[3] || c[3] == ' ')) {  // "udp off" / "udp on": the realtime listeners, for tests
    const char *arg = c + 3;
    while (*arg == ' ') ++arg;
    if (!strcasecmp(arg, "off")) wled::setEnabled(false);
    else if (!strcasecmp(arg, "on")) wled::setEnabled(true);
    out.printf("WLED: %s, UDP listeners %s (usage: udp on|off)\r\n", wled::enabled() ? "on" : "off", wled::realtimeListening() ? "open" : "closed");
    return true;
  }
#endif
  if (telnetUpdate == TelnetUpdate::Asking) {
    if (!strcasecmp(c, "y") || !strcasecmp(c, "yes")) {
      telnetUpdate = TelnetUpdate::None;
      firmwareUpdate.scheduleAfterReboot(nullptr); restartAt = millis() + 1000;
      out.print("Restarting to install the update.\r\n"); return true;
    }
    if (!strcasecmp(c, "n") || !strcasecmp(c, "no")) { telnetUpdate = TelnetUpdate::None; out.print("Update cancelled.\r\n"); return true; }
  }
  if (!strcasecmp(c, "update")) {
    bool accepted = firmwareUpdate.request();
    FirmwareUpdate::Status status; firmwareUpdate.status(status);
    if (accepted) { telnetUpdate = TelnetUpdate::Watching; out.print("Update started: checking memory. The result follows here.\r\n"); }
    else if (status.lowMemory) { firmwareUpdate.scheduleAfterReboot(nullptr); restartAt = millis() + 1000; out.printf("%s\r\n", status.message); }
    else out.printf("Update not started: %s\r\n", status.message);
    return true;
  }
  if (!strcasecmp(c, "birthday")) {
    bool on = !birthdayPreviewing();
    birthdayPreviewUntil = on ? millis() + 60000 : 0;
    out.print(on ? "Showing the birthday screen for a minute (again to stop).\r\n" : "Birthday screen stopped.\r\n");
    return true;
  }
  if (!strcasecmp(c, "update-status")) {
    FirmwareUpdate::Status status; firmwareUpdate.status(status);
    out.printf("%s (%lu/%lu bytes)\r\n", status.message, (unsigned long)status.received, (unsigned long)status.total); return true;
  }
#if defined(UM_MARIO) || defined(UM_LUIGI)
  if (!strncasecmp(c, "world", 5) && (!c[5] || c[5] == ' ')) {
    // The same byte the settings page writes: 1 rotates, 10 is the demo, 0 and 2-9 pin a world
    static const struct { const char *name; uint8_t value; } kWorlds[] = {
      {"overground", 0}, {"rotate", 1}, {"underground", 2}, {"castle", 3}, {"snow", 4},
      {"underwater", 5}, {"cloud", 6}, {"ghost", 7}, {"lava", 8}, {"airship", 9}, {"demo", 10}};
    auto &s = *Settings::getInstance();
    const char *arg = c + 5;
    while (*arg == ' ') ++arg;
    if (!*arg || !strcasecmp(arg, "list")) {
      for (const auto &w : kWorlds) out.printf("  %-12s%s\r\n", w.name, w.value == s.numberFont ? " <- now" : "");
      out.print("Usage: world <name>\r\n"); return true;
    }
    for (const auto &w : kWorlds) {
      if (strncasecmp(arg, w.name, strlen(arg)) || strlen(arg) < 2) continue;  // a unique start of the name is enough
      s.numberFont = w.value;
      out.printf(s.save() ? "World: %s\r\n" : "World: %s (could not be saved)\r\n", w.name);
      return true;
    }
    out.print("Unknown world. Type world list.\r\n"); return true;
  }
#endif
#ifdef UM_F1
  if (!face) { out.print("Not ready yet.\r\n"); return true; }
  auto is = [&](const char *a, const char *b) { return !strcasecmp(c, a) || !strcasecmp(c, b); };
  if (is("sim", "s")) { out.printf("%s\r\n", face->simulate()); return true; }
  if (is("next", "n")) { out.printf("%s\r\n", face->simulateNext()); return true; }
#endif
  return false;
}

UM_GZIP_ASSET(settingsHtmlGz, "settings_html")
UM_GZIP_ASSET(themeCssGz, "theme_css")
UM_GZIP_ASSET(settingsJsGz, "settings_js")
UM_GZIP_ASSET(authJsGz, "auth_js")
extern const char manifestJson[] asm("_binary_manifest_webmanifest_start");
#define UM_ICON(sym) \
  extern const uint8_t sym##Start[] asm("_binary_" #sym "_png_start"); \
  extern const uint8_t sym##End[] asm("_binary_" #sym "_png_end")
UM_ICON(icon_192); UM_ICON(icon_512); UM_ICON(icon_maskable_192); UM_ICON(apple_touch_icon); UM_ICON(favicon_32);
#define UM_FACE(sym) UM_ICON(sym)
UM_FACE(football);
UM_FACE(f1);
UM_FACE(motogp);
UM_FACE(tetris);
UM_FACE(word);
UM_FACE(mario);
UM_FACE(luigi);
UM_FACE(pacman);
UM_FACE(invaders);
UM_FACE(pong);
UM_FACE(home);
static void sendIcon(const uint8_t *start, const uint8_t *end) {
  server.sendHeader("Cache-Control", "public, max-age=86400");
  server.send_P(200, "image/png", (const char *)start, end - start);
}
static bool settingsServerStarted = false;

static String jsonText(const String &value) {
  String out = "\"";
  for (size_t i = 0; i < value.length(); ++i) {
    char c = value[i];
    if (c == '\\' || c == '\"') { out += '\\'; out += c; }
    else if ((uint8_t)c < 32) { char code[7]; snprintf(code, sizeof(code), "\\u%04x", (unsigned)(uint8_t)c); out += code; }
    else out += c;
  }
  return out + "\"";
}

static String selectedValues(const char *name) {
  String result;
  for (int i = 0; i < server.args(); ++i) {
    if (server.argName(i) != name) continue;
    if (result.length()) result += ',';
    result += server.arg(i);
    if (result.length() > 400) break;
  }
  return result;
}

static void settingsPage() {
#ifdef UM_WLED_OVERLAY
  if (wled::wantsLightPage(server)) { server.sendHeader("Location", "/wled"); server.send(302, "text/plain", ""); return; }
#endif
  welcomeVisited = true;  // the welcome screens drop the QR code once somebody has opened the page
  sendGzip(server, "text/html; charset=utf-8", settingsHtmlGz, settingsHtmlGzEnd); }

static String minuteText(unsigned minute) {
  char text[6];
  snprintf(text, sizeof(text), "%02u:%02u", (minute / 60) % 24, minute % 60);
  return text;
}

static bool welcomeHidden() {
  Preferences prefs;
  bool hidden = false;
  if (prefs.begin("ultimatrix", true)) { hidden = prefs.getBool("welcomeHidden", false); prefs.end(); }
  return hidden;
}

static void settingsState() {
  const auto &s = *Settings::getInstance();
  String json;
  json.reserve(1500);
  json = "{\"mode\":\"" UM_MODE_NAME "\",\"modeId\":\"" UM_MODE_ID "\",\"timeSynced\":";
  json += ultime::timeStatus() == timeSet ? "true" : "false";
  json += ",\"lightReading\":" + String(displayControl.lightReading());
  json += ",\"nightActive\":" + String(displayControl.nightActive() ? "true" : "false");
  json += ",\"nvsFree\":" + String(nvsFreeEntries);
  json += ",\"pinSet\":" + String(adminpin::isSet() ? "true" : "false");
  json += ",\"nightWhy\":\"" + String(!Settings::getInstance()->nightClock ? "" : displayControl.roomDark() ? "dark" : displayControl.nightScheduled() ? "schedule" : "") + "\"";
  {
    int rise = 0, set = 0;
    if (s.hasLocation && ultime::timeStatus() == timeSet && sun::times(s.latitude, s.longitude, time(nullptr), rise, set) == sun::Result::Normal)
      json += ",\"sunTimes\":" + jsonText("Today: sunrise " + minuteText(rise) + ", sunset " + minuteText(set) + ".");
  }
  json += ",\"network\":{\"ssid\":" + jsonText(WiFi.SSID()) + ",\"ip\":" + jsonText(WiFi.localIP().toString()) + ",\"hostname\":" + jsonText(String(mdnsHost) + ".local") + "},\"welcomeHidden\":" + String(welcomeHidden() ? "true" : "false") + ",\"welcomeMode\":" + String(welcomeMode ? "true" : "false") + ",\"settings\":{";
  json += "\"accessScope\":" + String(s.accessScope) + ",\"anyHost\":" + String(s.anyHost ? "true" : "false") + ",\"mdnsUseName\":" + String(s.mdnsUseName ? "true" : "false") + ",\"clockName\":" + jsonText(s.clockName) + ",";
  json += "\"brightness\":" + String(s.brightness) + ",\"rotation\":" + String(s.rotation);
  json += ",\"use24h\":" + String(s.use24h ? "true" : "false") + ",\"ntpServer\":" + jsonText(s.ntpServer);
  json += ",\"autoBrightness\":" + String(s.autoBrightness ? "true" : "false") + ",\"ldrPin\":" + String(s.ldrPin);
  json += ",\"minimumBrightness\":" + String(s.minimumBrightness) + ",\"ldrDark\":" + String(s.ldrDark) + ",\"ldrLight\":" + String(s.ldrLight);
  json += ",\"nightEnabled\":" + String(s.nightEnabled ? "true" : "false") + ",\"nightStart\":" + jsonText(minuteText(s.nightStart));
  json += ",\"nightEnd\":" + jsonText(minuteText(s.nightEnd)) + ",\"nightBrightness\":" + String(s.nightBrightness);
  json += ",\"nightSun\":" + String(s.nightSun ? "true" : "false") + ",\"nightClock\":" + String(s.nightClock ? "true" : "false");
  json += ",\"timezone\":" + jsonText(s.timezone) + ",\"autoTimezone\":" + String(s.manualTimezone ? "false" : "true") + ",\"matchSecs\":" + String(s.matchSecs);
  json += ",\"resultMins\":" + String(s.resultMins) + ",\"timeStyle\":" + String(s.timeStyle);
  json += ",\"dither\":" + String(s.dither ? "true" : "false") + ",\"swapBlueGreen\":" + String(s.swapBlueGreen ? "true" : "false") + ",\"telnet\":" + String(s.telnet ? "true" : "false") + ",\"autoUpdate\":" + String(s.autoUpdate ? "true" : "false");
  json += ",\"birthdays\":" + jsonText(s.birthdays);
#ifdef UM_SPORTS_SWITCH
  json += ",\"sportsOrder\":" + jsonText(s.sportsOrder) + ",\"sportsHome\":" + jsonText(s.sportsHome) + ",\"sportsLead\":" + String(s.sportsLead);
  json += ",\"sportsOthers\":" + String(s.sportsOthers ? "true" : "false");
  json += ",\"sportsRacing\":" + String(s.sportsRacing) + ",\"sportsFav\":" + String(s.sportsFav ? "true" : "false") + ",\"sportsMax\":" + String(s.sportsMax) + ",\"sportsHold\":" + String(s.sportsHold);
#endif
  json += ",\"fbLeagues\":" + jsonText(s.footballLeagues) + ",\"fbTeams\":" + jsonText(s.footballTeams);
  json += ",\"f1Drivers\":" + jsonText(s.f1Drivers);
  json += ",\"showRankings\":" + String(s.showRankings ? "true" : "false");
#ifdef UM_FOOTBALL_ONLY
  json += ",\"footballOthers\":" + String(s.footballOthers ? "true" : "false");
#endif
  json += ",\"ballRoll\":" + String(s.ballRoll ? "true" : "false");
  json += ",\"numberFont\":" + String(s.numberFont == 1 ? "true" : "false");
  json += ",\"wordLanguage\":" + String(s.numberFont);
#ifdef UM_WORD
  json += ",\"wordAnimation\":" + String(s.timeStyle & 15) + ",\"wordDate\":" + String(s.timeStyle >> 4);
#endif
#ifdef UM_PONG
  json += ",\"pongBackdrop\":" + String(s.pongBackdrop);
  json += ",\"pongSpeed\":" + String(s.pongSpeed) + ",\"pongLevel\":" + String(s.pongLevel) + ",\"pongDate\":" + String(s.pongDate) + ",\"pongDay\":" + String(s.pongDay);
#if defined(UM_PONG) || defined(UM_INVADERS)
  json += ",\"arcHours\":" + String(s.arcHours) + ",\"arcMinutes\":" + String(s.arcMinutes) + ",\"arcScore\":" + String(s.arcScore);
#endif
#endif
#ifdef UM_PACMAN
  json += ",\"color\":" + String(s.color) + ",\"dotColor\":" + String(s.dotColor);
#ifdef UM_GAMEPAD
  json += String(",\"btPad\":") + (gamepad::active() ? "true" : "false");
#endif
  json += ",\"pacmanClockColor\":" + String(s.pacmanClockColor) + ",\"clockMinutesColor\":" + String(s.clockMinutesColor) + ",\"clockShade\":" + String(s.clockShade);
  const char *ghostNames[] = {"ghost1Color", "ghost2Color", "ghost3Color", "ghost4Color"};
  for (int i = 0; i < 4; i++) json += ",\"" + String(ghostNames[i]) + "\":" + String(s.ghostColors[i]);
#endif
#if defined(UM_MARIO) || defined(UM_LUIGI)
  json += ",\"marioWorld\":" + String(s.numberFont);
#endif
  json += ",\"f1Flags\":" + String(s.f1Flags ? "true" : "false") + "}}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

static void selectionCatalog() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
#ifdef UM_FOOTBALL_ONLY
  server.sendContent_P(FOOTBALL_LEAGUES_JSON);  // [code, label, name, group]
  server.sendContent("");
  return;
#endif
  server.sendContent("[");
  bool first = true;
#ifdef UM_FOOTBALL_ONLY
  (void)first;
#elif defined(UM_MOTOGP)
  for (const auto &rider : MOTOGP_RIDERS) {
    if (!first) server.sendContent(",");
    first = false;
    server.sendContent("{\"value\":" + jsonText(rider.code) + ",\"label\":" + jsonText(String(rider.name) + " (" + String(rider.number) + ")") + "}");
  }
#elif defined(UM_F1)
  for (const auto &driver : F1_DRIVERS) {
    if (!first) server.sendContent(",");
    first = false;
    server.sendContent("{\"value\":" + jsonText(driver.code) + ",\"label\":" + jsonText(driver.surname) + "}");
  }
#elif defined(UM_WORD)
  for (int i = 0; i < WORD_LANGUAGE_COUNT; ++i) {
    if (!first) server.sendContent(",");
    first = false;
    server.sendContent("{\"value\":" + String(i) + ",\"label\":" + jsonText(WORD_LANGUAGES[i].name) + "}");
  }
#else
  (void)first;
#endif
  server.sendContent("]");
  server.sendContent("");
}

#ifdef UM_FOOTBALL_ONLY
static void footballTeams() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  for (const char *part : FOOTBALL_TEAMS_JSON) server.sendContent_P(part);  // [id, abbreviation, name, 'c'|'n']; the parts split one array
  server.sendContent("");
}
#endif

static void timezones() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("[");
  String chunk;
  chunk.reserve(600);
  for (size_t i = 0; i < ultime::zoneCount(); ++i) {
    if (i) chunk += ',';
    chunk += jsonText(ultime::zoneName(i));
    if (chunk.length() > 500) { server.sendContent(chunk); chunk = ""; }
  }
  chunk += ']';
  server.sendContent(chunk);
  server.sendContent("");
}

static void settingsResponse(int status, const char *message) {
  if (server.uri().startsWith("/api/"))
    server.send(status, "application/json", "{\"message\":" + jsonText(message) + "}");
  else server.send(status, "text/plain; charset=utf-8", message);
}

// Geo-IP lookup (time zone and location) on its own task: the HTTP call can take seconds, and the clock task also
// draws the display and serves the settings page. The result is applied from loop().
static std::atomic<int> geoState{0};  // 0 idle, 1 looking, 2 result waiting
static String geoZone;
static float geoLatitude = 0, geoLongitude = 0;
static bool geoFound = false;
static bool geoWanted = false;  // a lookup is wanted as soon as Wi-Fi is up
static uint32_t geoRetryAt = 0, geoStartedAt = 0;
static uint8_t geoFails = 0;
static bool geoSettled = false;  // this boot has had an answer, or has given up

static void geoTask(void *) {
  String zone; float lat = 0, lon = 0;
  bool found = ultime::detectZone(zone, &lat, &lon);
  geoZone = zone; geoLatitude = lat; geoLongitude = lon; geoFound = found;
  geoState = 2;
  vTaskDelete(nullptr);
}

static void startGeo() {
  if (geoState.load() != 0) return;
  geoState = 1;
  geoStartedAt = millis();
  if (xTaskCreatePinnedToCore(geoTask, "geoip", 4096, nullptr, 1, nullptr, 0) != pdPASS) geoState = 0;
}

static void dismissWelcome() {
  Preferences prefs;
  bool saved = prefs.begin("ultimatrix", false) && prefs.putBool("welcomeHidden", true) > 0;
  prefs.end();
  if (!saved) { settingsResponse(500, "Could not save. Please try again."); return; }
  settingsResponse(200, "Welcome hidden.");
}

static void startClock() {
  Preferences prefs;
  bool saved = prefs.begin("ultimatrix", false) && prefs.putBool("onboarded", true) > 0;  // first: if it fails, the welcome stays
  if (saved) prefs.remove("welcomePending");
  prefs.end();
  if (!saved) { settingsResponse(500, "Could not save. Please try again."); return; }
  settingsResponse(200, "Starting the clock.");
  restartAt = millis() + 1000;
}

static void openWifiSetup() {
  Preferences p;
  if (!p.begin("ultimatrix", false)) { settingsResponse(500, "Could not open Wi-Fi setup."); return; }
  bool saved = p.putBool("setupWifi", true) > 0;
  p.end();
  if (!saved) { settingsResponse(500, "Could not open Wi-Fi setup."); return; }
  settingsResponse(200, "Restarting into Wi-Fi setup.");
  restartAt = millis() + 1000;
}

static bool number(const char *key, long minimum, long maximum, long &value) {
  String text = server.arg(key);
  if (text.isEmpty() || text.length() > 5) return false;
  for (size_t i = 0; i < text.length(); ++i) if (!isDigit(text[i])) return false;
  value = text.toInt();
  return value >= minimum && value <= maximum;
}

static bool clockMinute(const char *key, uint16_t &minute) {
  String value = server.arg(key);
  if (value.length() != 5 || value[2] != ':' || !isDigit(value[0]) || !isDigit(value[1]) || !isDigit(value[3]) || !isDigit(value[4])) return false;
  unsigned hour = (value[0] - '0') * 10 + value[1] - '0';
  unsigned min = (value[3] - '0') * 10 + value[4] - '0';
  if (hour > 23 || min > 59) return false;
  minute = hour * 60 + min;
  return true;
}

static bool privateAddress(IPAddress ip) {
  return ip[0] == 10 || (ip[0] == 172 && (ip[1] & 0xF0) == 16) || (ip[0] == 192 && ip[1] == 168) || (ip[0] == 169 && ip[1] == 254);
}

// Who may reach the web interface (setting "accessScope"): everything, the private networks (RFC 1918, plus link-local), or the clock's own subnet.
static bool accessAllowed(IPAddress ip, uint8_t scope) {
  if (scope == 0 || ip == IPAddress(127, 0, 0, 1)) return true;
  if (scope == 2) {
    const uint32_t mask = (uint32_t)WiFi.subnetMask();
    return ((uint32_t)ip & mask) == ((uint32_t)WiFi.localIP() & mask);
  }
  return privateAddress(ip) || !privateAddress(WiFi.localIP());  // on a network with public addresses (some universities) "private only" cannot mean anything: everything may reach it
}

// With a timezone chosen by hand the clock is not where the network lookup puts it, so sunrise and sunset follow the zone's main city
static bool applyZonePlace(Settings &s) {
  float latitude, longitude;
  if (!s.manualTimezone || !ultime::placeForZone(s.timezone.c_str(), latitude, longitude)) return false;
  if (s.hasLocation && fabsf(latitude - s.latitude) <= 0.05f && fabsf(longitude - s.longitude) <= 0.05f) return false;
  s.latitude = latitude; s.longitude = longitude; s.hasLocation = true;
  return true;
}

static void saveSettings() {
  long bright, rotation;
#ifndef UM_SIMPLE_FACE
  long seconds, results, style;
#endif
  String zone = server.arg("timezone");
  bool autoZone = server.hasArg("autoTimezone");  // the timezone field is disabled (not sent) while this is ticked
  int leagueCount = 0;
  for (int i = 0; i < server.args(); ++i) if (server.argName(i) == "fbLeagues") ++leagueCount;
  if (leagueCount > 10) { settingsResponse(400, "Choose up to 10 competitions."); return; }
  if (!number("brightness", 1, 255, bright)) { settingsResponse(400, "Choose a brightness using the slider."); return; }
  if (!number("rotation", 0, 3, rotation)) { settingsResponse(400, "Choose a display orientation."); return; }
#ifndef UM_SIMPLE_FACE
  if (!number("matchSecs", 3, 60, seconds)) { settingsResponse(400, "Page duration must be between 3 and 60 seconds."); return; }
  if (!number("resultMins", 0, 1440, results)) { settingsResponse(400, "Keep results for between 0 and 1440 minutes."); return; }
  if (!number("timeStyle", 0, 5, style)) { settingsResponse(400, "Choose a clock animation."); return; }
#endif
  if (!autoZone && !ultime::posixForZone(zone.c_str())) { settingsResponse(400, "Choose a supported timezone from the suggestions."); return; }
  if (server.arg("birthdays").length() > Birthdays::SETTING_MAX) { settingsResponse(400, "Too many birthdays. At most 10."); return; }
  if (selectedValues("fbLeagues").length() > 400 || server.arg("fbTeams").length() > 400 || selectedValues("f1Drivers").length() > 400) {
    settingsResponse(400, "Your selection is too long. Shorten it and try again.");
    return;
  }
  Settings candidate = *Settings::getInstance();
  auto &s = candidate;
  // Optional new fields preserve compatibility with existing bookmarked forms.
  if (server.hasArg("clockName")) {
    String name = server.arg("clockName"); name.trim();
    if (!Settings::validClockName(name)) { settingsResponse(400, "Enter a clock name of 1 to 48 bytes, without control characters."); return; }
    s.clockName = name;
    s.mdnsUseName = server.hasArg("mdnsUseName");
    s.anyHost = server.hasArg("anyHost");
    if (server.hasArg("accessScope")) {
      long scope;
      if (!number("accessScope", 0, 2, scope)) { settingsResponse(400, "Choose who may reach the clock."); return; }
      if (!accessAllowed(server.client().remoteIP(), (uint8_t)scope)) { settingsResponse(400, "That would lock this computer out of the clock. Save it from a computer inside that network."); return; }
      s.accessScope = (uint8_t)scope;
    }
  }
  if (server.hasArg("use24h")) {
    long format;
    if (!number("use24h", 0, 1, format)) { settingsResponse(400, "Choose a time format."); return; }
    s.use24h = format == 1;
  }
  if (server.hasArg("ntpServer")) {
    String host = server.arg("ntpServer"); host.trim();
    bool valid = host.length() > 0 && host.length() <= 128;
    for (size_t i = 0; i < host.length(); ++i) valid &= isalnum((unsigned char)host[i]) || host[i] == '.' || host[i] == '-' || host[i] == ':';
    if (!valid) { settingsResponse(400, "Enter a valid time-server hostname or IP address."); return; }
    s.ntpServer = host;
  }
  if (server.hasArg("ldrPin")) {
    long pin, dark, light, minimum;
    if (!number("ldrPin", 32, 39, pin) || (pin != 32 && pin != 33 && pin != 34 && pin != 35 && pin != 36 && pin != 39) ||
        !number("ldrDark", 0, 4094, dark) || !number("ldrLight", 1, 4095, light) || light <= dark ||
        !number("minimumBrightness", 1, 255, minimum) ||
        !clockMinute("nightStart", s.nightStart) || !clockMinute("nightEnd", s.nightEnd)) {
      settingsResponse(400, "Check the light sensor values, brightness limits, and night times."); return;
    }
    s.autoBrightness = server.hasArg("autoBrightness"); s.ldrPin = pin; s.ldrDark = dark; s.ldrLight = light;
    s.minimumBrightness = minimum; s.nightEnabled = server.hasArg("nightEnabled");
    s.nightSun = server.hasArg("nightSun");
    s.nightClock = server.hasArg("nightClock");
    if (s.nightSun && !s.hasLocation) { geoWanted = true; geoFails = 0; }  // sunrise and sunset need the network's location: looked up in the background
    if (s.nightEnabled && !s.nightSun && s.nightStart == s.nightEnd) { settingsResponse(400, "Choose different start and end times for night mode."); return; }
  }
  s.brightness = bright; s.rotation = rotation;
  if (autoZone) { if (s.manualTimezone) { geoWanted = true; geoFails = 0; } }  // the network's zone is applied once it has been looked up
  else s.timezone = zone;
  s.manualTimezone = !autoZone;
  applyZonePlace(s);
#ifndef UM_SIMPLE_FACE
  s.matchSecs = seconds; s.resultMins = results; s.timeStyle = style;
#endif
#if defined(UM_MARIO) || defined(UM_LUIGI)
  if (server.hasArg("marioWorld")) {
    long choice;
    if (!number("marioWorld", 0, 10, choice)) { settingsResponse(400, "Choose a world from the list."); return; }
    s.numberFont = choice;  // 1 rotates, 10 is the demo, 0 and 2-9 pin a world (the byte the Words language and Football digits share)
  }
#endif
#ifdef UM_WORD
  if (server.hasArg("wordLanguage")) {
    long language;
    if (!number("wordLanguage", 0, WORD_LANGUAGE_COUNT - 1, language)) { settingsResponse(400, "Choose a language from the list."); return; }
    s.numberFont = language;  // the Time in Words language shares the football "alternative digits" byte
  }
  if (server.hasArg("wordAnimation")) {
    long animation;
    if (!number("wordAnimation", 0, 5, animation)) { settingsResponse(400, "Choose a word animation from the list."); return; }
    s.timeStyle = (s.timeStyle & 0xF0) | animation;
  }
  if (server.hasArg("wordDate")) {
    long date;
    if (!number("wordDate", 0, 2, date)) { settingsResponse(400, "Choose a date style from the list."); return; }
    s.timeStyle = (s.timeStyle & 15) | (date << 4);
  }
#endif
#ifdef UM_PONG
  if (server.hasArg("pongDate")) {
    long on = 0;
    if (!number("pongDate", 0, 2, on)) { settingsResponse(400, "Choose Hidden, Above or Below."); return; }
    s.pongDate = on;
  }
  if (server.hasArg("pongDay")) {
    long on = 0;
    if (!number("pongDay", 0, 2, on)) { settingsResponse(400, "Choose Hidden, Above or Below."); return; }
    s.pongDay = on;
  }
  if (server.hasArg("pongLevel")) {
    long level = 0;
    if (!number("pongLevel", 0, 8, level)) { settingsResponse(400, "Choose a difficulty from the list."); return; }
    s.pongLevel = level;
  }
  if (server.hasArg("pongSpeed")) {
    long level = 0;
    if (!number("pongSpeed", 0, 4, level)) { settingsResponse(400, "Choose a speed from the list."); return; }
    s.pongSpeed = level;
  }
  if (server.hasArg("pongBackdrop")) {
    long level;
    if (!number("pongBackdrop", 0, 5, level)) { settingsResponse(400, "Choose a brightness from the list."); return; }
    s.pongBackdrop = level;
  }
#endif
#if defined(UM_PONG) || defined(UM_INVADERS)
  const struct { const char *name; uint8_t *value; } arcColours[] = {{"arcHours", &s.arcHours}, {"arcMinutes", &s.arcMinutes}, {"arcScore", &s.arcScore}};
  for (const auto &colour : arcColours) {
    if (!server.hasArg(colour.name)) continue;
    long choice;
    if (!number(colour.name, 0, pacmancolors::COLOR_COUNT - 1, choice)) { settingsResponse(400, "Choose a colour from the list."); return; }
    *colour.value = choice;
  }
#endif
#ifdef UM_PACMAN
  const struct { const char *name; uint8_t *value; uint8_t count; } colours[] = {
    {"pacmanClockColor", &s.pacmanClockColor, pacmancolors::COLOR_COUNT}, {"clockMinutesColor", &s.clockMinutesColor, pacmancolors::COLOR_COUNT},
    {"clockShade", &s.clockShade, 2},
    {"color", &s.color, pacmancolors::WALL_COUNT}, {"dotColor", &s.dotColor, pacmancolors::COLOR_COUNT},
    {"ghost1Color", &s.ghostColors[0], pacmancolors::COLOR_COUNT}, {"ghost2Color", &s.ghostColors[1], pacmancolors::COLOR_COUNT},
    {"ghost3Color", &s.ghostColors[2], pacmancolors::COLOR_COUNT}, {"ghost4Color", &s.ghostColors[3], pacmancolors::COLOR_COUNT}
  };
  for (const auto &colour : colours) {
    if (!server.hasArg(colour.name)) continue;  // older forms keep the saved colours
    long choice;
    if (!number(colour.name, 0, colour.count - 1, choice)) { settingsResponse(400, "Choose a colour from the list."); return; }
    *colour.value = choice;
  }
#endif
  if (server.hasArg("birthdays")) s.birthdays = Birthdays::clean(server.arg("birthdays"));
  s.swapBlueGreen = server.hasArg("swapBlueGreen");
  s.dither = server.hasArg("dither");
  s.telnet = server.hasArg("telnet");
  s.autoUpdate = server.hasArg("autoUpdate");
#ifdef UM_SPORTS_SWITCH
  if (server.hasArg("sportsOrder")) {  // the sports page sends all of its fields together
    s.sportsOrder = server.arg("sportsOrder");
    s.sportsHome = FirmwareUpdate::knownMode(server.arg("sportsHome").c_str()) ? server.arg("sportsHome") : String();
    s.sportsLead = constrain(server.arg("sportsLead").toInt(), 0, 120);
    s.sportsRacing = server.arg("sportsRacing").toInt() == 1 ? 1 : server.arg("sportsRacing").toInt() == 2 ? 2 : 0;
    s.sportsMax = constrain(server.arg("sportsMax").toInt(), 1, 12);
    s.sportsHold = constrain(server.arg("sportsHold").toInt(), 0, 120);
    s.sportsFav = server.hasArg("sportsFav");
    s.sportsOthers = server.hasArg("sportsOthers");
  }
#endif
#ifdef UM_FOOTBALL_ONLY
  s.footballLeagues = selectedValues("fbLeagues"); s.footballTeams = server.arg("fbTeams");
  s.showRankings = server.hasArg("showRankings"); s.footballOthers = server.hasArg("footballOthers"); s.ballRoll = server.hasArg("ballRoll");
  s.numberFont = server.hasArg("numberFont") ? 1 : 0;
#elif defined(UM_F1)
  s.f1Drivers = selectedValues("f1Drivers"); s.f1Flags = server.hasArg("f1Flags");
#endif
  if (!s.save()) {
    settingsResponse(500, "Could not save settings. Please try again.");
    return;
  }
  const auto &previous = *Settings::getInstance();
  bool restart = previous.swapBlueGreen != s.swapBlueGreen;
  bool timeChanged = previous.timezone != s.timezone || previous.use24h != s.use24h || previous.ntpServer != s.ntpServer;
  bool consoleChanged = previous.telnet != s.telnet;
  bool nameChanged = previous.clockName != s.clockName || previous.mdnsUseName != s.mdnsUseName;
  *Settings::getInstance() = candidate;
  if (displayReady) display->setRotation(s.rotation);
  if (timeChanged && face) clockTime.begin(s.timezone.c_str(), s.use24h, s.ntpServer.c_str(), "");
  if (consoleChanged) {
    if (s.telnet && !telnetLow && !btMode()) { telnet.begin(telnetCommand, UM_TELNET_COMMON_HELP UM_TELNET_HELP); telnetStarted = true; }
    else { telnet.stop(); telnetStarted = false; }
    if (WiFi.status() == WL_CONNECTED) startMdns();
  }
  if (nameChanged && !consoleChanged && WiFi.status() == WL_CONNECTED) startMdns();
  String response = "{\"message\":" + jsonText(restart ? "Settings saved. Restarting Ultimatrix." : "Settings saved and applied.");
  response += ",\"restart\":" + String(restart ? "true" : "false") + ",\"network\":{\"hostname\":";
  response += jsonText(String(mdnsHost) + ".local") + ",\"ip\":" + jsonText(WiFi.localIP().toString()) + "}}";
  server.send(200, "application/json", response);
  if (restart) restartAt = millis() + 1000;
}

static void firmwareStatus() {
  FirmwareUpdate::Status status; firmwareUpdate.status(status);
  String json = "{\"version\":" + jsonText(firmwareUpdate.version()) + ",\"message\":" + jsonText(status.message);
  json += ",\"busy\":" + String(status.busy ? "true" : "false") + ",\"ready\":" + String(status.ready ? "true" : "false");
  json += ",\"failed\":" + String(status.failed ? "true" : "false") + ",\"received\":" + String(status.received) + ",\"total\":" + String(status.total);
  json += ",\"lowMemory\":" + String(status.lowMemory ? "true" : "false");
  FirmwareUpdate::Latest latest; firmwareUpdate.latest(latest);
  json += ",\"available\":" + String(latest.available ? "true" : "false") + ",\"latest\":" + jsonText(latest.version) + ",\"checkError\":" + jsonText(latest.error);
  json += ",\"checking\":" + String(latest.checking ? "true" : "false") + ",\"checkedAt\":" + String(latest.checkedAt);
  char updateServer[96]; FirmwareUpdate::server(updateServer, sizeof(updateServer));
  json += ",\"server\":" + jsonText(updateServer);
  json += ",\"freeHeap\":" + String(heap_caps_get_free_size(MALLOC_CAP_8BIT));
  json += ",\"minimumHeap\":" + String(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)) + "}";
  server.sendHeader("Cache-Control", "no-store"); server.send(200, "application/json", json);
}

static void checkFirmware() {
  bool started = firmwareUpdate.checkNow(server.hasArg("force"));
  if (!started && liveDataActive()) {  // the page offers to pause the live data and check anyway
    server.send(429, "application/json", "{\"live\":true,\"message\":\"A live match or session is running. It is not paused to free memory for the update check unless you force it.\"}"); return;
  }
  settingsResponse(started ? 202 : 429, started ? "Stopping feeds before checking for updates." : "Wait a moment before checking again.");
}

// The colour quality level for the settings page: what runs, why it was lowered, and the lock
static void storageState() { server.send(200, "application/json", storage::json()); }
static void storageReformat() {
  if (!storage::reformat()) { server.send(500, "application/json", "{\"message\":\"Could not format the flash disk.\"}"); return; }
  server.send(200, "application/json", "{\"restarting\":true}");
  restartAt = millis() + 1000;  // every face and cache starts again on the empty disk
}
static void storageClear() { storage::clear(); server.send(200, "application/json", storage::json()); }

// Admin PIN: POST /api/auth trades the PIN for a token, POST /api/pin sets, changes or removes it (pin empty = remove).
static void pinLogin() {
  char token[33];
  uint32_t wait = 0;
  int result = adminpin::login(server.arg("pin").c_str(), token, wait);
  if (result == 2) { server.send(429, "application/json", "{\"message\":\"Too many wrong PINs. Try again in " + String(wait) + " seconds.\",\"wait\":" + String(wait) + "}"); return; }
  if (result == 1) { server.send(401, "application/json", "{\"message\":\"Wrong PIN.\"}"); return; }
  server.send(200, "application/json", String("{\"token\":\"") + token + "\"}");
}
static void pinSave() {
  String pin = server.arg("pin");
  if (!pin.length()) {
    if (!adminpin::clear()) { settingsResponse(500, "Could not remove the PIN. Please try again."); return; }
    server.send(200, "application/json", "{\"message\":\"PIN removed.\",\"pinSet\":false}");
    return;
  }
  if (!adminpin::validFormat(pin.c_str())) { settingsResponse(400, "The PIN is 4 to 8 digits."); return; }
  if (!adminpin::set(pin.c_str())) { settingsResponse(500, "Could not save the PIN. Please try again."); return; }
  char token[33];
  adminpin::newToken(token);  // setting the PIN logs everybody out, this browser stays in
  server.send(200, "application/json", String("{\"message\":\"PIN saved.\",\"pinSet\":true,\"token\":\"") + token + "\"}");
}

static void displayStatus() {
  String json = "{\"level\":" + String(panelLevel + 1) + ",\"lock\":" + String(panelState.lock) + ",\"reason\":\"" +
      (panelState.reason == paneltier::Busy ? "busy" : panelState.reason == paneltier::Quiet ? "quiet" : "none") +
      "\",\"since\":" + String(panelState.at) + ",\"retryHour\":" + String(paneltier::kRetryHour) + ",\"levels\":[";
  for (int i = 0; i < paneltier::kCount; i++) { if (i) json += ","; json += "\""; json += paneltier::kTiers[i].name; json += "\""; }
  json += "]}";
  server.send(200, "application/json", json);
}

// reset=1 goes back to the best level; lock=0 is automatic, 1..n stays on that level. Both restart the clock.
static void displaySave() {
  if (server.hasArg("reset")) paneltier::reset(panelState);
  if (server.hasArg("lock")) {
    int lock = server.arg("lock").toInt();
    if (lock < 0 || lock > paneltier::kCount) { settingsResponse(400, "Unknown level."); return; }
    panelState.lock = (uint8_t)lock;
    if (!lock) paneltier::reset(panelState);
    else paneltier::save(panelState);
  }
  settingsResponse(200, "Saved. The clock restarts with the new colour quality.");
  restartAt = millis() + 1000;
}

// What Home Assistant (over MQTT) can ask of the clock: the follow-ups of a changed setting that saveSettings() runs for the settings
// page, and the restart and update actions
static void mqttSettingsChanged(const Settings &previous) {
  auto &s = *Settings::getInstance();
  if (displayReady && previous.rotation != s.rotation) display->setRotation(s.rotation);
  if ((previous.timezone != s.timezone || previous.use24h != s.use24h || previous.ntpServer != s.ntpServer) && face)
    clockTime.begin(s.timezone.c_str(), s.use24h, s.ntpServer.c_str(), "");
  if (previous.telnet != s.telnet) {
    if (s.telnet && !telnetLow && !btMode()) { telnet.begin(telnetCommand, UM_TELNET_COMMON_HELP UM_TELNET_HELP); telnetStarted = true; }
    else { telnet.stop(); telnetStarted = false; }
    if (WiFi.status() == WL_CONNECTED) startMdns();
  }
  if ((previous.timezone != s.timezone || previous.manualTimezone != s.manualTimezone) && applyZonePlace(s)) s.save();
  if ((s.nightSun && !s.hasLocation) || (previous.manualTimezone && !s.manualTimezone)) { geoWanted = true; geoFails = 0; }
  if (previous.swapBlueGreen != s.swapBlueGreen && s.save()) restartAt = millis() + 1000;  // the panel colours are set up at start
}

static bool mqttScreenRow(int16_t y, uint8_t *rgb) {
  return display && display->width() == 64 && display->height() == 64 && display->readDisplayRow(y, rgb);
}
static int mqttLightLevel() { return displayControl.lightReading(); }
static void mqttRestart() { restartAt = millis() + 500; }
static void mqttCheckUpdate() { firmwareUpdate.checkNow(false); }
static void mqttInstallUpdate() { firmwareUpdate.request(); }

// The Face select in Home Assistant: the same signed install as the settings page, started by an explicit choice
static void mqttSwitchFace(const char *mode) {
  if (!FirmwareUpdate::knownMode(mode)) return;
  if (firmwareUpdate.activateStaged(mode)) { consoleLog("[Face] Switching to %s from the spare slot\n", mode); return; }  // prepared ahead of time: only a restart
  const bool accepted = firmwareUpdate.request(mode, false);
  FirmwareUpdate::Status status; firmwareUpdate.status(status);
  if (!accepted && status.lowMemory) {  // too little memory with this face running: restart first and install before the feeds start
    consoleLog("[MQTT] Switching face to %s needs a restart first\n", mode);
    firmwareUpdate.scheduleAfterReboot(mode);
    restartAt = millis() + 1000;
  } else if (!accepted) consoleLog("[MQTT] Face switch refused: %s\n", status.message);
  else consoleLog("[MQTT] Switching face to %s\n", mode);
}

static void mqttUpdateInfo(Mqtt::UpdateInfo &out) {
  FirmwareUpdate::Status status; firmwareUpdate.status(status);
  FirmwareUpdate::Latest latest; firmwareUpdate.latest(latest);
  strlcpy(out.installed, firmwareUpdate.version(), sizeof(out.installed));
  strlcpy(out.latest, latest.available && latest.version[0] ? latest.version : out.installed, sizeof(out.latest));  // equal: Home Assistant shows it up to date
  out.busy = status.busy;
  out.percent = status.busy && status.total ? (int)((uint64_t)status.received * 100 / status.total) : -1;
}

static void mqttState() {
  const Mqtt::Config &c = Mqtt::instance().config();
  char status[80];
  Mqtt::instance().status(status, sizeof(status));
  server.send(200, "application/json", String("{\"enabled\":") + (c.enabled ? "true" : "false") + ",\"screenCamera\":" + (c.screenCamera ? "true" : "false") + ",\"host\":" + jsonText(c.host) + ",\"port\":" + c.port +
              ",\"user\":" + jsonText(c.user) + ",\"passwordSet\":" + (c.pass[0] ? "true" : "false") +
              ",\"connected\":" + (Mqtt::instance().connected() ? "true" : "false") + ",\"status\":" + jsonText(status) + "}");
}

static void mqttSave() {
  Mqtt::Config c = Mqtt::instance().config();
  c.enabled = server.arg("enabled") == "1";
  c.screenCamera = server.arg("screenCamera") == "1";
  String host = server.arg("host"), user = server.arg("user"), password = server.arg("password");
  host.trim();
  if (host.length() && !Mqtt::validHost(host.c_str())) { settingsResponse(400, "Enter the broker as an IP address or a host name."); return; }
  if (c.enabled && host.isEmpty()) { settingsResponse(400, "Enter the address of the MQTT broker first."); return; }
  long port = server.arg("port").length() ? server.arg("port").toInt() : 1883;
  if (port < 1 || port > 65535) { settingsResponse(400, "The port must be between 1 and 65535."); return; }
  if (user.length() >= sizeof(c.user) || password.length() >= sizeof(c.pass)) { settingsResponse(400, "That username or password is too long."); return; }
  if (strcmp(c.host, host.c_str()) != 0 && password.isEmpty()) c.pass[0] = 0;  // a new broker never receives the saved password
  strlcpy(c.host, host.c_str(), sizeof(c.host));
  strlcpy(c.user, user.c_str(), sizeof(c.user));
  c.port = (uint16_t)port;
  if (server.arg("passwordClear") == "1") c.pass[0] = 0;
  else if (password.length()) strlcpy(c.pass, password.c_str(), sizeof(c.pass));  // empty keeps the saved one: it is never sent back
  const bool saved = Mqtt::instance().save(c);
  settingsResponse(saved ? 200 : 500, saved ? "Saved." : "Could not save the MQTT settings.");
}

static void saveUpdateServer() {
  bool saved = firmwareUpdate.setServer(server.arg("url").c_str());
  settingsResponse(saved ? 200 : 400, saved ? "Update source saved." : "Enter an http:// or https:// address, or leave it empty for GitHub.");
}

static void requestFirmwareUpdate() {
  String mode = server.arg("mode");
  if (mode.length() && !FirmwareUpdate::knownMode(mode.c_str())) { settingsResponse(400, "Unknown clock face."); return; }
  if (mode.length() && firmwareUpdate.activateStaged(mode.c_str())) { consoleLog("[Face] Switching to %s from the spare slot\n", mode.c_str()); settingsResponse(202, "Switching to the prepared face. Restarting."); return; }
  bool accepted = firmwareUpdate.request(mode.c_str(), server.hasArg("force"));
  FirmwareUpdate::Status status; firmwareUpdate.status(status);
  if (!accepted && liveDataActive() && !status.lowMemory) {
    server.send(409, "application/json", "{\"live\":true,\"message\":" + jsonText(status.message) + "}"); return;
  }
  if (!accepted && status.lowMemory) {
    server.send(409, "application/json", "{\"lowMemory\":true,\"message\":" + jsonText(status.message) + "}"); return;
  }
  settingsResponse(accepted ? 202 : 409, status.message);
}

// Automatic updates: once a check has found a newer image, install it (started once per boot).
static void autoUpdate(uint32_t now) {
  static bool tried = false;
  static uint32_t nextTry = 0;
  if (tried || !Settings::getInstance()->autoUpdate || !bootDone || portalActive || WiFi.status() != WL_CONNECTED || scheduledMode[0] || wledBusy(now)) return;
  if (now < 120000 || (int32_t)(now - nextTry) < 0) return;
  nextTry = now + 60000;
  FirmwareUpdate::Latest latest; firmwareUpdate.latest(latest);
  if (!latest.available || latest.checking) return;
  FirmwareUpdate::Status status; firmwareUpdate.status(status);
  if (status.busy || status.ready) return;
  // A failure (too little memory included) is retried once by background() with a restart and no face loaded
  if (firmwareUpdate.request()) { tried = true; consoleLog("[Firmware] Automatic update started: %s\n", latest.version); }
}

#ifdef UM_SPORTS_SWITCH
// Sports that may take the panel (Settings > Sports): when one is on, or about to start, the clock installs that sport's face
// with the same signed install as the Face select, and goes back to the home face once nothing is on (SportsPlan.h decides).
static sportsplan::Log sportsLog;

// Prepares the face the clock will need next in the spare slot, so the switch is a restart: the sport that starts within 6 hours, or the
// home face while a sport face we switched to is running. Only when nothing is live and the running image has proved itself.
static void sportsStageAhead(uint32_t now, time_t wall, const sportsplan::Config &plan, const sportsplan::Events (&events)[sportsplan::SPORTS], const sportsplan::Log &log) {
  static uint32_t nextTry = 0, day = 0, stagedToday = 0;
  if ((int32_t)(now - nextTry) < 0 || liveDataActive() || FirmwareUpdate::pendingVerification()) return;
  nextTry = now + 10 * 60000;
  if (day != (uint32_t)(wall / 86400)) { day = (uint32_t)(wall / 86400); stagedToday = 0; }
  if (stagedToday >= 6) return;  // flash wear: a few writes a day at most
  const char *want = nullptr;
  for (uint8_t i = 0; i < plan.orderCount && !want; ++i) {
    const char *mode = sportsplan::kModes[plan.order[i]];
    if (!strcmp(mode, UM_MODE_ID)) continue;
    const sportsplan::Events &e = events[plan.order[i]];
    for (uint8_t k = 0; k < e.count; ++k) if (e.list[k].end > wall && e.list[k].start - wall < 6 * 3600) { want = mode; break; }
  }
  if (!want && log.target[0] && !strcmp(log.target, UM_MODE_ID) && plan.home[0] && strcmp(plan.home, UM_MODE_ID)) want = plan.home;
  if (!want || !FirmwareUpdate::knownMode(want) || firmwareUpdate.spareHolds(want)) return;
  if (firmwareUpdate.stage(want)) { ++stagedToday; consoleLog("[Sports] preparing %s ahead of time\n", want); }
}

static void sportsSwitch(uint32_t now) {
  static uint32_t seenSaves = ~0u, nextCheck = 0;
  sportsplan::Log &log = sportsLog;
  static bool logLoaded = false;
  Settings &s = *Settings::getInstance();
  static bool active = false, settleHome = false, settingsSeen = false;  // settleHome: sports or home face were just chosen, go to the home face once if nothing is on
  static uint32_t settleSince = 0;
  static String lastOrder, lastHome;
  if (updateOnly || (s.sportsOrder.isEmpty() && !active)) return;  // nothing chosen: no object made, no task
  SportsWatch &watch = SportsWatch::instance();
  if (seenSaves != Settings::saveCount()) {
    seenSaves = Settings::saveCount();
    SportsWatch::Config cfg;
    sportsplan::Config plan;
    sportsplan::parseOrder(s.sportsOrder.c_str(), plan);
    for (uint8_t i = 0; i < plan.orderCount; ++i) cfg.sports[plan.order[i]] = true;
    cfg.racing = s.sportsRacing; cfg.favouritesOnly = s.sportsFav; cfg.othersFav = s.sportsOthers;
    strlcpy(cfg.leagues, s.footballLeagues.c_str(), sizeof(cfg.leagues)); strlcpy(cfg.teams, s.footballTeams.c_str(), sizeof(cfg.teams));
    watch.configure(cfg);
    active = !s.sportsOrder.isEmpty();
    if (settingsSeen && active && !s.sportsHome.isEmpty() && (s.sportsOrder != lastOrder || s.sportsHome != lastHome)) { settleHome = true; settleSince = now; }
    lastOrder = s.sportsOrder; lastHome = s.sportsHome; settingsSeen = true;
  }
  if (active) watch.poll();  // the schedule task ends itself while an update check or install runs: start it again
  if (s.sportsOrder.isEmpty() || (int32_t)(now - nextCheck) < 0) return;
  nextCheck = now + 15000;
  if (!bootDone || portalActive || welcomeMode || updateOnly || restartAt || scheduledMode[0] || WiFi.status() != WL_CONNECTED || wledBusy(now)) return;
  FirmwareUpdate::Status status; firmwareUpdate.status(status);
  if (status.busy || status.ready) return;
  Preferences p;
  if (!logLoaded) {
    logLoaded = true;
    if (p.begin("ultimatrix", true)) {
      String v = p.getString("sportsLog", "");
      char target[16] = "", seen[16] = "";
      unsigned day = 0, count = 0; long last = 0, paused = 0;
      if (sscanf(v.c_str(), "%u,%u,%ld,%15[^,],%15[^,],%ld", &day, &count, &last, target, seen, &paused) >= 3) {  // "-" stands for an empty name
        log.day = day; log.count = count; log.last = last; log.pausedUntil = paused;
        strlcpy(log.target, strcmp(target, "-") ? target : "", sizeof(log.target)); strlcpy(log.seen, strcmp(seen, "-") ? seen : "", sizeof(log.seen));
      }
      p.end();
    }
  }
  sportsplan::Config plan;
  sportsplan::parseOrder(s.sportsOrder.c_str(), plan);
  strlcpy(plan.home, s.sportsHome.c_str(), sizeof(plan.home));
  plan.leadSecs = s.sportsLead * 60; plan.maxPerDay = s.sportsMax; plan.holdSecs = s.sportsHold * 60;
  sportsplan::Events events[sportsplan::SPORTS];
  watch.events(events);
  const time_t wall = time(nullptr);
  auto saveLog = [&]() {
    if (!p.begin("ultimatrix", false)) return;
    char v[80];
    snprintf(v, sizeof(v), "%u,%u,%ld,%s,%s,%ld", (unsigned)log.day, (unsigned)log.count, (long)log.last, log.target[0] ? log.target : "-", log.seen[0] ? log.seen : "-", (long)log.pausedUntil);
    p.putString("sportsLog", v);
    p.end();
  };
  if (sportsplan::noticeManual(log, events, wall, plan, UM_MODE_ID)) { saveLog(); consoleLog("[Sports] face chosen by hand: no automatic switch until the events now on are over\n"); }
  if (settleHome) {  // a new setup: the home face is where the clock belongs while nothing is on, wherever it was left
    if (!strcmp(UM_MODE_ID, plan.home) || !plan.orderCount) settleHome = false;
    else {
      bool known = true, anyOn = false;
      for (uint8_t i = 0; i < plan.orderCount; ++i) {
        known &= events[plan.order[i]].checkedAt != 0;
        anyOn |= sportsplan::wanted(events[plan.order[i]], wall, plan, false);
      }
      if (known || now - settleSince > 300000) {  // the schedules are read (or could not be for 5 minutes)
        settleHome = false;
        if (!anyOn && FirmwareUpdate::knownMode(plan.home)) {
          sportsplan::noteSwitch(log, wall, plan.home);
          saveLog();
          consoleLog("[Sports] nothing is on: switching to the home face %s\n", plan.home);
          mqttSwitchFace(plan.home);
          return;
        }
      }
    }
  }
  const char *target = sportsplan::decide(events, wall, plan, log, UM_MODE_ID);
  if (!target) { sportsStageAhead(now, wall, plan, events, log); return; }
  if (!FirmwareUpdate::knownMode(target)) return;
  sportsplan::noteSwitch(log, wall, target);  // counted before the install: a refused one is not retried for the minimum gap
  saveLog();
  consoleLog("[Sports] switching to %s\n", target);
  mqttSwitchFace(target);
}

// Settings > Sports > Prepare a face now: the same staging the clock does by itself ahead of an event
static void sportsPrepare() {
  const String mode = server.arg("mode");
  if (!FirmwareUpdate::knownMode(mode.c_str())) { settingsResponse(400, "Choose a face."); return; }
  if (mode == UM_MODE_ID) { settingsResponse(409, "That face is already running."); return; }
  if (FirmwareUpdate::pendingVerification()) { settingsResponse(409, "The running version has not been confirmed yet; try again in a few minutes."); return; }
  if (firmwareUpdate.spareHolds(mode.c_str())) { settingsResponse(200, "That face is already prepared."); return; }
  if (liveDataActive()) { settingsResponse(409, "A live match or session is running; preparing waits until it has ended."); return; }
  if (firmwareUpdate.stage(mode.c_str())) { settingsResponse(202, "Preparing. The feeds pause for about a minute."); return; }
  FirmwareUpdate::Status status; firmwareUpdate.status(status);
  settingsResponse(409, status.busy ? "An update or preparation is already running." : "Not enough free memory to prepare a face right now.");
}

// The plan on the settings page: what the watcher found and the rules, so the page can show what will happen when
static void sportsInfo() {
  Settings &s = *Settings::getInstance();
  sportsplan::Config plan;
  sportsplan::parseOrder(s.sportsOrder.c_str(), plan);
  plan.holdSecs = s.sportsHold * 60;
  sportsplan::Events events[sportsplan::SPORTS];
  if (plan.orderCount) SportsWatch::instance().events(events);
  const time_t wall = time(nullptr);
  String json = "{\"now\":" + String((long)wall) + ",\"mode\":\"" UM_MODE_ID "\",\"home\":" + jsonText(s.sportsHome) + ",\"lead\":" + String(s.sportsLead * 60);
  json += ",\"hold\":" + String(plan.holdSecs) + ",\"paused\":" + String((long)sportsLog.pausedUntil) + ",\"max\":" + String(s.sportsMax);
  char spare[16]; firmwareUpdate.spareMode(spare, sizeof(spare));
  json += ",\"spare\":" + jsonText(spare);
  json += ",\"today\":" + String(sportsLog.day == (uint32_t)(wall / 86400) ? sportsLog.count : 0) + ",\"order\":[";
  for (uint8_t i = 0; i < plan.orderCount; ++i) json += String(i ? ",\"" : "\"") + sportsplan::kModes[plan.order[i]] + "\"";
  json += "],\"sports\":{";
  for (uint8_t i = 0; i < plan.orderCount; ++i) {
    const sportsplan::Events &e = events[plan.order[i]];
    json += String(i ? ",\"" : "\"") + sportsplan::kModes[plan.order[i]] + "\":{\"checked\":" + String((long)e.checkedAt) + ",\"events\":[";
    for (uint8_t k = 0; k < e.count; ++k)
      json += String(k ? "," : "") + "{\"s\":" + String((long)e.list[k].start) + ",\"e\":" + String((long)e.list[k].end) + ",\"l\":" + (e.list[k].live ? "true" : "false") + ",\"n\":" + jsonText(e.list[k].label) + "}";
    json += "]}";
  }
  json += "}}";
  server.send(200, "application/json", json);
}
#endif

// Low-memory fallback: restart first, then update before the feeds start.
static void rebootForFirmwareUpdate() {
  String mode = server.arg("mode");
  if (mode.length() && !FirmwareUpdate::knownMode(mode.c_str())) { settingsResponse(400, "Unknown clock face."); return; }
  firmwareUpdate.scheduleAfterReboot(mode.c_str());
  settingsResponse(202, "Restarting to install the update.");
  restartAt = millis() + 1000;
}

// Load of each core from the idle tasks' run time over the interval since the last call. Interrupt time (the ring refill)
// runs inside whichever task it interrupted, mostly idle, so it is subtracted separately and shown on its own.
static void reportCpu() {
  static int64_t lastUs = 0;
  static uint32_t lastIdle[2] = {0, 0}, lastRows = 0;
  // Read the two idle counters directly: uxTaskGetSystemState walks every task with interrupts off for about 2.5 ms, which made the panel flicker once a minute
  uint32_t idle[2] = {ulTaskGetIdleRunTimeCounterForCore(0), ulTaskGetIdleRunTimeCounterForCore(1)};
  int64_t now = esp_timer_get_time();
  if (lastUs) {
    uint32_t wall = (uint32_t)(now - lastUs);
    // The idle counters and the wall clock are read a moment apart, so idle can come out a little above 100%
    unsigned busy0 = 100 - std::min(100u, (unsigned)(100ull * (uint32_t)(idle[0] - lastIdle[0]) / wall));
    unsigned busy1 = 100 - std::min(100u, (unsigned)(100ull * (uint32_t)(idle[1] - lastIdle[1]) / wall));
    unsigned ring = 0;
    MatrixPanel_I2S_DMA::RingStats rs = display->getRingStats();
    if (rs.active) ring = (unsigned)(100ull * (uint32_t)(rs.rows - lastRows) * rs.meanCycles / ((uint64_t)getCpuFrequencyMhz() * wall));
    if (rs.active) lastRows = rs.rows;
    consoleLog("[Cpu] core0 busy %u%%, core1 busy %u%% (ring refill %u%% of one core is counted as idle: add it to its core)\n", busy0, busy1, ring);
  }
  lastUs = now;
  lastIdle[0] = idle[0];
  lastIdle[1] = idle[1];
}

// Independent deltas keep MQTT's 10-second samples separate from console reporting.
static void mqttCpuUsage(Mqtt::CpuInfo &out) {
  static int64_t lastUs = 0;
  static uint32_t lastIdle[2] = {0, 0}, lastRows = 0;
  const uint32_t idle[2] = {ulTaskGetIdleRunTimeCounterForCore(0), ulTaskGetIdleRunTimeCounterForCore(1)};
  const int64_t now = esp_timer_get_time();
  const auto rs = display->getRingStats();
  if (lastUs && now > lastUs) {
    const uint64_t wall = now - lastUs;
    unsigned total = 0;
    for (int core = 0; core < 2; ++core)
      total += 100 - std::min(100u, (unsigned)(100ull * (uint32_t)(idle[core] - lastIdle[core]) / wall));
    out.load = (total + 1) / 2;
    // Approximate duty of one core, from the driver's lifetime mean refill cost.
    out.display = rs.active ? std::min(100u, (unsigned)(100ull * (uint32_t)(rs.rows - lastRows) * rs.meanCycles /
        ((uint64_t)getCpuFrequencyMhz() * wall))) : 0;
  }
  lastUs = now;
  lastIdle[0] = idle[0]; lastIdle[1] = idle[1]; lastRows = rs.rows;
}

static void reportMemory() {
  static uint32_t reportedAt = 0;
  static uint32_t lastFailures = 0;
  uint32_t now = millis();
  uint32_t failures = memoryAllocationFailures.load(std::memory_order_relaxed);
  if (reportedAt && now - reportedAt < 60000 && failures == lastFailures) return;
  reportedAt = now ? now : 1;
  lastFailures = failures;
  const uint32_t walkAt = micros();
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const uint32_t walkUs = micros() - walkAt;  // walks every free block with the heap lock held: interrupts on this core wait that long
  consoleLog("[Memory] free=%u min=%u largest=%u dma=%u stack_free=%u failures=%u (largest-block walk %u us) at=%u ms\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
      (unsigned)largest,
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
      (unsigned)uxTaskGetStackHighWaterMark(nullptr), (unsigned)failures, (unsigned)walkUs, (unsigned)millis());
  if (displayReady) reportCpu();
  if (displayReady) {
    MatrixPanel_I2S_DMA::RingStats rs = display->getRingStats();
    if (rs.active)
      consoleLog("[Ring] rows=%u missed=%u mean=%u longest=%u us of %u us per row\n", (unsigned)rs.rows, (unsigned)rs.missed,
          (unsigned)(rs.meanCycles / getCpuFrequencyMhz()), (unsigned)(rs.maxCycles / getCpuFrequencyMhz()), (unsigned)rs.rowPeriodUs);
    if (rs.active)
      consoleLog("[Ring] flips=%u waits=%u wait total=%u ms longest=%u us\n", (unsigned)rs.flips, (unsigned)rs.waits,
          (unsigned)(rs.waitUs / 1000), (unsigned)rs.waitMaxUs);
    if (rs.active)
      consoleLog("[Ring] interrupt gap: longest=%u us, over 1/2/5/10 ms: %u/%u/%u/%u\n", (unsigned)rs.gapMaxUs,
          (unsigned)rs.late[0], (unsigned)rs.late[1], (unsigned)rs.late[2], (unsigned)rs.late[3]);
    static uint32_t lateSeen = 0;
    uint32_t gapUs, pc, at; char task[9];
    while (rs.active && display->nextLateEvent(lateSeen, gapUs, pc, task, &at))
      consoleLog("[Ring] late: at=%u ms gap=%u us pc=0x%08x task=%s\n", (unsigned)at, (unsigned)gapUs, (unsigned)pc, task);
  }
}

// Lowers the panel's colour quality one level and restarts into it. The reason decides whether it is tried again
// later: a busy time (a live match or session) passes, a quiet one does not.
// Live data that ran in the last few minutes still counts as a busy time: allocation failures often land between two live reads
static uint32_t lastLiveAt = 0;
static void noteLiveData() {
  if (liveDataActive() || liveFavouriteOn.load()) lastLiveAt = millis() | 1;
}
static bool recentlyLive() {
  constexpr uint32_t kGraceMs = 10 * 60 * 1000;
  return lastLiveAt && millis() - lastLiveAt < kGraceMs;
}

static void lowerPanelQuality(const char *why) {
  static bool done = false;
  if (done || restartAt || setupRefresh || updateOnly || !paneltier::canLower(panelState)) return;
  done = true;
  noteLiveData();
  bool busy = recentlyLive();
  paneltier::lower(panelState, busy);
  consoleLog("[Panel] %s: lowering the colour quality to level %d (%s), reason %s\n", why, paneltier::active(panelState) + 1,
      paneltier::kTiers[paneltier::active(panelState)].name, busy ? "busy" : "quiet");
  restartAt = millis() + 1500;
}

// Memory safeguard: the first allocation failure after boot stops the telnet server and mDNS to give their RAM back.
// If failures go on after that (or pile up before the boot is done), the panel's colour quality goes one level lower.
static void shedOnLowMemory() {
  static uint32_t seen = 0, strikes = 0;
  uint32_t failures = memoryAllocationFailures.load(std::memory_order_relaxed);
  if (failures == seen) return;
  uint32_t fresh = failures - seen;
  seen = failures;
  FirmwareUpdate::Latest check; firmwareUpdate.latest(check);
  if (check.checking || firmwareUpdating || updateOnly) return;  // an update check runs the heap nearly dry on purpose and copes with it; telnet and mDNS stay
  if (!bootDone) { strikes += fresh; if (strikes >= 6) lowerPanelQuality("allocation failures during boot"); return; }
  if (memoryShed) { strikes += fresh; if (strikes >= 3) lowerPanelQuality("allocation failures after shedding"); return; }
  memoryShed = true;
  telnet.stop(); telnetStarted = false; telnetLow = true;  // telnet comes back by itself once memory is free again (main loop)
  if (mdnsStarted) { MDNS.end(); mdnsStarted = false; }
#ifdef UM_WLED_OVERLAY
  wled::service(false);  // the UDP listeners and the drawing buffer go too
#endif
  consoleLog("[Memory] allocation failures: telnet stopped until memory is free, mDNS until restart (free=%u largest=%u)\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// A lowered level that was caused by a busy time is tried again from 11:00 local time on the next quiet moment, when nothing live is running
static void retryPanelQuality(uint32_t now) {
  static uint32_t nextAt = 0;
  if (!bootDone || updateOnly || restartAt || (int32_t)(now - nextAt) < 0) return;
  nextAt = now + 60000;
  if (liveDataActive() || liveFavouriteOn.load() || !paneltier::retryDue(panelState)) return;
  paneltier::reset(panelState);
  consoleLog("[Panel] trying the best colour quality again\n");
  restartAt = now + 1500;
}

#ifdef UM_HOME
// ---- Home Assistant settings (Home face): GET the saved values, POST a form. The token is write-only. ----
static String haSlotJson(const ha::Slot &slot, const char *entity) {
  return "{\"type\":" + String(slot.type) + ",\"auto\":" + String(slot.autoMode ? "true" : "false") + ",\"entity\":" + jsonText(entity) +
         ",\"unit\":" + jsonText(slot.unit) + ",\"decimals\":" + String(slot.decimals) + ",\"alertMins\":" + String(slot.alertMins) + ",\"flags\":" + String(slot.flags) + "}";
}

static void haState() {
  const ha::Config &c = ha::config();
  char status[64];
  HaClient::instance().status(status, sizeof(status));
  char artNote[48];
  ha::media::artNote(artNote, sizeof(artNote));
  String json;
  json.reserve(2600);
  json = "{\"host\":" + jsonText(c.host) + ",\"port\":" + String(c.port) + ",\"tls\":" + String(c.tls ? "true" : "false");
  json += ",\"tokenSet\":" + String(c.token[0] ? "true" : "false") + ",\"pages\":" + String(c.pages) + ",\"pageSecs\":" + String(c.pageSecs);
  json += ",\"gridImportPositive\":" + String(c.gridImportPositive ? "true" : "false") + ",\"batteryDischargePositive\":" + String(c.batteryDischargePositive ? "true" : "false");
  json += ",\"state\":" + String((int)HaClient::instance().state()) + ",\"status\":" + jsonText(status) + ",\"art\":" + jsonText(artNote) + ",\"tiles\":[";
  for (int i = 0; i < ha::kHomeTiles; i++) json += (i ? "," : "") + haSlotJson(ha::tileSlot(c, i), ha::tileSlot(c, i).entity);
  json += "],\"icons\":[";
  std::unique_ptr<char[]> iconList(new (std::nothrow) char[ha::kIconEntity]);
  for (int i = 0; i < ha::kIcons; i++) {
    if (iconList) ha::iconEntityCopy(i, iconList.get(), ha::kIconEntity);
    json += (i ? "," : "") + haSlotJson(c.icons[i], iconList ? iconList.get() : "");
  }
  json += "],\"energy\":[";
  for (int i = 0; i < ha::E_COUNT; i++) json += (i ? "," : "") + jsonText(c.energy[i]);
  json += "],\"media\":[";
  for (int i = 0; i < ha::kRooms; i++) json += (i ? "," : "") + jsonText(ha::mediaPlayers()[i]);
  json += "],\"washer\":" + haSlotJson(ha::washerAlert(), ha::washerAlert().entity) + ",\"weather\":" + jsonText(ha::weatherEntity()) + ",\"doorbell\":" + jsonText(c.doorbell) + ",\"doorbellFlags\":" + String(c.doorbellFlags) + ",\"alarmFlags\":" + String(c.alarmFlags) + "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

// "http://homeassistant.local:8123/" or "192.168.1.5" into host, port and tls
static bool haParseAddress(String text, ha::Config &c) {
  text.trim();
  if (text.startsWith("https://")) { c.tls = 1; c.port = 443; text.remove(0, 8); }  // a pasted address without a port means the scheme's own port
  else if (text.startsWith("http://")) { c.tls = 0; c.port = 80; text.remove(0, 7); }
  int slash = text.indexOf('/');
  if (slash >= 0) text.remove(slash);
  int colon = text.lastIndexOf(':');
  if (colon >= 0) {
    long port = text.substring(colon + 1).toInt();
    if (port < 1 || port > 65535) return false;
    c.port = port;
    text.remove(colon);
  }
  if (text.isEmpty() || text.length() > ha::kHostMax) return false;
  for (size_t i = 0; i < text.length(); ++i) if (!(isalnum((unsigned char)text[i]) || text[i] == '.' || text[i] == '-')) return false;
  strlcpy(c.host, text.c_str(), sizeof(c.host));
  return true;
}

static bool haEnergyList(const String &name, char *out) {
  String value = server.arg(name);
  value.trim();
  value.toLowerCase();
  value.replace(" ", "");
  if (value.length() >= ha::kEntity || !ha::validEnergyList(value.c_str())) return false;
  strlcpy(out, value.c_str(), ha::kEntity);
  return true;
}

static bool haEntityList(const String &name, char *out, size_t size = ha::kEntity) {
  String value = server.arg(name);
  value.trim();
  value.toLowerCase();
  value.replace(" ", "");
  if (value.length() >= size || !ha::validEntityList(value.c_str())) return false;
  strlcpy(out, value.c_str(), size);
  return true;
}

static bool haEntity(const String &name, char *out) {
  String value = server.arg(name);
  value.trim();
  value.toLowerCase();
  if (value.length() >= ha::kEntity || !ha::validEntity(value.c_str())) return false;
  strlcpy(out, value.c_str(), ha::kEntity);
  return true;
}

// GET /api/ha/suggest?kind=power&q=sol : up to 25 matching entities from Home Assistant, for the settings page's autocomplete
static void haSuggest() {
  constexpr size_t kText = 9000;
  std::unique_ptr<char[]> buffer(new (std::nothrow) char[kText]);  // the clock's stack is small: sizeable things go on the heap
  char error[64] = "";
  server.sendHeader("Cache-Control", "no-store");
  if (!buffer) { server.send(200, "application/json", "{\"error\":\"Out of memory\",\"entities\":[]}"); return; }
  char *text = buffer.get();
  if (!ha::suggest(server.arg("kind").c_str(), server.arg("q").c_str(), text, kText, error, sizeof(error))) {
    server.send(200, "application/json", "{\"error\":" + jsonText(error) + ",\"entities\":[]}");
    return;
  }
  String json = "{\"entities\":[";
  bool first = true;
  for (char *entry = strtok(text, ";"); entry; entry = strtok(nullptr, ";")) {  // id|name|unit
    char *name = strchr(entry, '|');
    if (!name) continue;
    *name++ = 0;
    char *unit = strchr(name, '|');
    if (unit) *unit++ = 0;
    json += String(first ? "" : ",") + "{\"id\":" + jsonText(entry) + ",\"name\":" + jsonText(name) + ",\"unit\":" + jsonText(unit ? unit : "") + "}";
    first = false;
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// POST asks Home Assistant for the sensors of its Energy dashboard; GET returns the answer once it is there
static void haDetectAsk() {
  if (HaClient::instance().state() != HaClient::LIVE) { settingsResponse(409, "The clock is not connected to Home Assistant yet."); return; }
  HaClient::instance().requestPrefs();
  settingsResponse(202, "Asking Home Assistant.");
}

static void haDetectAnswer() {
  std::unique_ptr<HaClient::EnergyPrefs> prefs(new (std::nothrow) HaClient::EnergyPrefs);
  if (!prefs) { settingsResponse(500, "Out of memory."); return; }
  HaClient::instance().prefs(*prefs);
  const char *state = prefs->state == HaClient::EnergyPrefs::READY ? "ready" : prefs->state == HaClient::EnergyPrefs::PENDING ? "pending"
                      : prefs->state == HaClient::EnergyPrefs::FAILED_ASKING ? "failed" : "none";
  String json = String("{\"state\":\"") + state + "\",\"note\":" + jsonText(prefs->note) + ",\"entities\":[";
  for (int i = 0; i < ha::E_COUNT; i++) json += (i ? "," : "") + jsonText(prefs->id[i]);
  json += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

static void haSave() {
  auto c = std::unique_ptr<ha::Config>(new (std::nothrow) ha::Config);
  if (!c) { settingsResponse(500, "Out of memory."); return; }
  *c = ha::config();
  if (server.hasArg("host") && !haParseAddress(server.arg("host"), *c)) { settingsResponse(400, "Enter the address of Home Assistant, for example homeassistant.local:8123."); return; }
  long value;
  if (server.hasArg("port") && !server.arg("port").isEmpty()) {
    if (!number("port", 1, 65535, value)) { settingsResponse(400, "Choose a port between 1 and 65535."); return; }
    c->port = value;
  }
  if (server.hasArg("tls")) c->tls = server.arg("tls") == "1";
  String token = server.arg("token");
  token.trim();
  if (!token.isEmpty()) {
    if (token.length() > ha::kTokenMax) { settingsResponse(400, "That token is too long."); return; }
    for (size_t i = 0; i < token.length(); ++i)
      if (!(isalnum((unsigned char)token[i]) || token[i] == '.' || token[i] == '_' || token[i] == '-')) { settingsResponse(400, "The token contains characters a Home Assistant token never has. Copy it again."); return; }
    strlcpy(c->token, token.c_str(), sizeof(c->token));
  }
  if (token.isEmpty() && (strcmp(c->host, ha::config().host) != 0 || c->port != ha::config().port)) c->token[0] = 0;  // a new address never receives the saved token
  if (server.hasArg("clearToken")) c->token[0] = 0;
  if (server.hasArg("pages")) {
    if (!number("pages", 1, 31, value) || !(value & 15)) { settingsResponse(400, "Choose at least one page."); return; }
    c->pages = value;
  }
  if (server.hasArg("pageSecs")) {
    if (!number("pageSecs", 3, 120, value)) { settingsResponse(400, "Page time must be between 3 and 120 seconds."); return; }
    c->pageSecs = value;
  }
  if (server.hasArg("gridImportPositive")) c->gridImportPositive = server.arg("gridImportPositive") == "1";
  if (server.hasArg("batteryDischargePositive")) c->batteryDischargePositive = server.arg("batteryDischargePositive") == "1";
  ha::Slot extra[ha::kHomeTiles - ha::kTiles];
  for (int i = 0; i < ha::kHomeTiles - ha::kTiles; i++) extra[i] = ha::extraTile(ha::kTiles + i);
  for (int i = 0; i < ha::kHomeTiles; i++) {
    String key = "tile" + String(i);
    if (!server.hasArg(key + "Type")) continue;
    ha::Slot &slot = i < ha::kTiles ? c->tiles[i] : extra[i - ha::kTiles];
    if (!number((key + "Type").c_str(), 0, ha::TILE_TYPES - 1, value)) { settingsResponse(400, "Choose a tile type from the list."); return; }
    slot.type = value;
    slot.autoMode = server.hasArg(key + "Auto") && server.arg(key + "Auto") == "1" && ha::autoAvailable(true, slot.type);
    if (!haEntityList(key + "Entity", slot.entity)) { settingsResponse(400, "Entity IDs look like sensor.living_room_temperature."); return; }
    if (strchr(slot.entity, ',') && !ha::multiTile(slot.type) && slot.type != ha::TILE_EV) { settingsResponse(400, "This tile type takes one entity."); return; }
    if (slot.type == ha::TILE_WEATHER && slot.entity[0] && !slot.autoMode && strncmp(slot.entity, "weather.", 8)) {
      settingsResponse(400, "Weather tiles need a weather entity, for example weather.home."); return;
    }
    String unit = server.arg(key + "Unit");
    unit.trim();
    strlcpy(slot.unit, unit.c_str(), sizeof(slot.unit));
    slot.decimals = server.arg(key + "Decimals") == "1";
  }
  for (int i = 0; i < ha::kIcons; i++) {
    String key = "icon" + String(i);
    if (!server.hasArg(key + "Type")) continue;
    ha::Slot &slot = c->icons[i];
    if (!number((key + "Type").c_str(), 0, ha::ICON_TYPES - 1, value)) { settingsResponse(400, "Choose an icon type from the list."); return; }
    slot.type = value;
    slot.autoMode = server.arg(key + "Auto") == "1" && ha::autoAvailable(false, slot.type);
    std::unique_ptr<char[]> list(new (std::nothrow) char[ha::kIconEntity]);
    if (!list) { settingsResponse(500, "Out of memory."); return; }
    if (!haEntityList(key + "Entity", list.get(), ha::kIconEntity)) { settingsResponse(400, server.arg(key + "Entity").length() >= ha::kIconEntity ? "Too many entities for the clock to store: remove some." : "Entity IDs look like binary_sensor.front_door."); return; }
    if (!ha::setIconEntity(i, list.get())) { settingsResponse(500, "Out of memory."); return; }
    if (!number((key + "Alert").c_str(), 0, 240, value)) value = 0;
    slot.alertMins = value;
    slot.flags = number((key + "Flags").c_str(), 0, 7, value) ? value : 0;
  }
  for (int i = 0; i < ha::E_COUNT; i++) {
    String key = "energy" + String(i);
    if (server.hasArg(key) && !haEnergyList(key, c->energy[i])) { settingsResponse(400, "Entity IDs look like sensor.house_power; several are separated by commas, or write auto."); return; }
  }
  if (number("doorbellFlags", 0, 7, value)) c->doorbellFlags = value;
  if (number("alarmFlags", 0, 7, value)) c->alarmFlags = value;
  if (server.hasArg("doorbell") && !haEntity("doorbell", c->doorbell)) { settingsResponse(400, "Entity IDs look like binary_sensor.doorbell."); return; }
  char rooms[ha::kRooms][ha::kEntity];
  memcpy(rooms, ha::mediaPlayers(), sizeof(rooms));
  for (int i = 0; i < ha::kRooms; i++) {
    const String key = "media" + String(i);
    if (server.hasArg(key) && (!haEntity(key, rooms[i]) || (rooms[i][0] && strncmp(rooms[i], "media_player.", 13)))) {
      settingsResponse(400, "Choose media players, for example media_player.living_room."); return;
    }
  }
  char weather[ha::kEntity];
  strlcpy(weather, ha::weatherEntity(), sizeof(weather));
  if (server.hasArg("weather") && (!haEntity("weather", weather) || (weather[0] && strncmp(weather, "weather.", 8)))) {
    settingsResponse(400, "Choose a weather entity, for example weather.home."); return;
  }
  ha::Slot washer = ha::washerAlert();
  if (server.hasArg("washerEntity") && !haEntity("washerEntity", washer.entity)) {
    settingsResponse(400, "Enter the washer running entity, for example binary_sensor.washer_running."); return;
  }
  if (number("washerFlags", 0, 7, value)) washer.flags = value;
  for (int i = 0; i < ha::kHomeTiles - ha::kTiles; i++) ha::extraTile(ha::kTiles + i) = extra[i];
  ha::washerAlert() = washer;
  ha::config() = *c;
  strlcpy(ha::weatherEntity(), weather, ha::kEntity);
  memcpy(ha::mediaPlayers(), rooms, sizeof(rooms));
  if (!ha::save()) {
    ha::load();  // the clock keeps running on what is stored, not on settings that were refused
    settingsResponse(500, (String("The settings could not be saved (") + ha::saveFailure() + ", flash full?).").c_str());
    return;
  }
  settingsResponse(200, "Saved.");
}
#endif

// DNS rebinding: a web page on another domain can point its name at this clock, and then Origin and Host agree. The page's owner needs a
// real domain, so a host name under a real top-level domain (IANA's list, lib/core/Tlds.h) is refused. Addresses, .local, single words and
// internal names with a made-up ending (.lan, .home, joris.panel) pass; so does AVM's fritz.box. Settings: "Allow public host names" turns it off.
static bool publicHostName(String host) {
  if (host.startsWith("[")) return false;  // an IPv6 address
  const int colon = host.indexOf(':');
  if (colon >= 0) host = host.substring(0, colon);
  host.toLowerCase();
  if (host.endsWith(".")) host.remove(host.length() - 1);
  const int dot = host.lastIndexOf('.');
  if (dot < 0 || host == "fritz.box" || host.endsWith(".fritz.box")) return false;
  const String needle = "|" + host.substring(dot + 1) + "|";
  return strstr(kTlds, needle.c_str()) != nullptr;
}
// The WebServer picks a request's handler when it has read the request line only, before the headers (so Host is still the previous
// request's). The guard therefore takes every request that has a route, remembers the real handler, and decides in handle(), when the
// headers are known: it answers 403, or passes the request on to the real handler (body limits and raw reads are forwarded too).
class HostGuard : public RequestHandler {
public:
  bool canHandle(WebServer &web, HTTPMethod method, const String &uri) override {
    _target = next();
    while (_target && !_target->canHandle(web, method, uri)) _target = _target->next();
    return _target != nullptr;  // no route: the not-found answer (404, OPTIONS) needs no guard
  }
  bool canUpload(WebServer &web, const String &uri) override { return _target && _target->canUpload(web, uri); }
  bool canRaw(WebServer &web, const String &uri) override { return _target && _target->canRaw(web, uri); }
  void upload(WebServer &web, const String &uri, HTTPUpload &up) override { if (_target) _target->upload(web, uri, up); }
  void raw(WebServer &web, const String &uri, HTTPRaw &raw) override { if (_target) _target->raw(web, uri, raw); }
  bool handle(WebServer &web, HTTPMethod method, const String &uri) override {
    const bool clientOk = accessAllowed(web.client().remoteIP(), Settings::getInstance()->accessScope);
    if (!clientOk || (!Settings::getInstance()->anyHost && publicHostName(web.hostHeader()))) {
      web.send(403, "text/plain", clientOk ? "This clock does not answer to public host names. Open it by its address or its .local name."
                                           : "This clock only answers to computers inside its network.");
      return true;
    }
    return _target && _target->process(web, method, uri);
  }
private:
  RequestHandler *_target = nullptr;
};

// POST route with a body limit. The WebServer buffers a whole body before dispatch, so an oversized request is
// answered through its raw-body path instead: the bytes are read in small chunks and dropped.
class BoundedPost : public RequestHandler {
public:
  BoundedPost(const char *uri, WebServer::THandlerFunction fn) : _uri(uri), _fn(fn) {}
  bool canHandle(WebServer &, HTTPMethod method, const String &uri) override { return method == HTTP_POST && uri == _uri; }
  bool canRaw(WebServer &web, const String &) override { return web.clientContentLength() > kMaxBody; }
  void raw(WebServer &, const String &, HTTPRaw &) override {}
  bool handle(WebServer &web, HTTPMethod, const String &) override {
    if (web.clientContentLength() > kMaxBody) { web.send(413, "text/plain", "Request too large."); return true; }
    if (!sameOrigin(web)) { web.send(403, "text/plain", "Cross-site request refused."); return true; }
    if (adminpin::isSet() && !openRoute(_uri) && !adminpin::tokenOk(web.header("X-Token"))) {
      web.send(401, "application/json", "{\"message\":\"Enter the clock PIN.\",\"pin\":true}");
      return true;
    }
    _fn();
    return true;
  }
private:
  // A browser sends Origin on cross-site POSTs: refuse it unless it names this clock. Requests without Origin
  // (Home Assistant, curl, the WLED app) pass, as they are not driven by a web page.
  static bool sameOrigin(WebServer &web) {
    String origin = web.header("Origin");
    if (!origin.length()) return true;
    int scheme = origin.indexOf("://");
    if (scheme < 0) return false;  // "null" (sandboxed or file pages)
    return origin.substring(scheme + 3).equalsIgnoreCase(web.hostHeader());
  }
  // With a PIN set, these stay open: the login itself and everything of the WLED side (the WLED app and Home
  // Assistant's WLED integration, the options, timers and backup, the drawing canvas with saving) plus DLNA control.
  // None of it reaches Wi-Fi, firmware or the other settings. /api/ha/detect only asks Home Assistant for the
  // Energy dashboard's sensors (the page does it by itself on load) and stores nothing.
  static bool openRoute(const String &uri) {
    return uri == "/api/auth" || uri == "/api/ha/detect" || uri == "/json" || uri == "/json/state" || uri.startsWith("/dlna/") ||
           uri == "/api/wled" || uri.startsWith("/api/wled/") || uri.startsWith("/api/canvas/");
  }
  static constexpr int kMaxBody = 8192;  // the largest settings form (selections are capped at 400 characters each) with room to spare
  String _uri;
  WebServer::THandlerFunction _fn;
};

static void postRoute(const char *uri, WebServer::THandlerFunction fn) { server.addHandler(new BoundedPost(uri, fn)); }

// Web requests run on the drawing task, so a submitted frame stays stable while streamed.
static void screenPreview() {
  if (playing(millis())) { server.send(503, "text/plain", "Paused while a game is being played."); return; }
  if (!display || display->width() != 64 || display->height() != 64) {
    server.send(503, "text/plain", "Display preview is not ready."); return;
  }
  uint8_t row[64 * 3];
  if (!display->readDisplayRow(63, row)) {
    server.send(503, "text/plain", "Display preview is not ready."); return;
  }
  // Uncompressed 24-bit BMP: 54-byte header followed by bottom-up BGR rows.
  uint8_t header[54] = {'B', 'M'};
  auto put32 = [&](int at, uint32_t value) { for (int i = 0; i < 4; ++i) header[at + i] = value >> (i * 8); };
  put32(2, sizeof(header) + 64 * 64 * 3); put32(10, sizeof(header)); put32(14, 40);
  put32(18, 64); put32(22, 64); header[26] = 1; header[28] = 24; put32(34, 64 * 64 * 3);
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(sizeof(header) + 64 * 64 * 3);
  server.send(200, "image/bmp", "");
  server.sendContent(reinterpret_cast<const char *>(header), sizeof(header));
  uint8_t batch[64 * 3 * 4]; size_t used = 0;
  for (int y = 63; y >= 0 && server.client().connected(); --y) {
    if (y != 63) display->readDisplayRow(y, row);
    for (int x = 0; x < 64; ++x) std::swap(row[x * 3], row[x * 3 + 2]);
    memcpy(batch + used, row, sizeof(row)); used += sizeof(row);
    if (used == sizeof(batch)) { server.sendContent(reinterpret_cast<const char *>(batch), used); used = 0; }
  }
}


// The startup diagnostic a new OTA image must pass: display up, heap left, and either Wi-Fi setup running or
// the settings server up with the face drawing after its first time sync.
static bool startupHealthy() {
  return displayReady && heap_caps_get_free_size(MALLOC_CAP_8BIT) > 16384 &&
      (portalActive || (settingsServerStarted && (bootDone || updateOnly)));
}

static bool provisioningRestart = false;  // the pending restart is into or out of Wi-Fi setup

static void background() {
  wifi.process();
  if (portalCloseAt && (int32_t)(millis() - portalCloseAt) >= 0) {
    wifi.stopConfigPortal();
    portalCloseAt = 0;
  }
  bool portalNow = wifi.getConfigPortalActive();
  static uint32_t portalLogAt = 0;
  if (portalNow && millis() - portalLogAt >= 5000) {
    portalLogAt = millis();
    wifi_mode_t mode; esp_wifi_get_mode(&mode);
    consoleLog("[Portal] mode %d, stations %u, sta status %d, channel %u, heap %u, largest %u\n", (int)mode, (unsigned)WiFi.softAPgetStationNum(),
        (int)WiFi.status(), (unsigned)WiFi.channel(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  }
  if (setupRefresh && !welcomeMode && !portalNow && !restartAt) { restartAt = millis() + 1000; provisioningRestart = true; consoleLog("[Portal] setup over: restarting out of the fast display\n"); }  // setup is over: restart out of the fast display (the face needs the memory and colour depth)
  if (portalActive && !portalNow && WiFi.status() != WL_CONNECTED) WiFi.begin();
  portalActive = portalNow;
  if (!settingsServerStarted && WiFi.status() == WL_CONNECTED && !wifi.getConfigPortalActive()) {
    server.begin();
    settingsServerStarted = true;
  }
  if (settingsServerStarted) {
    server.handleClient();
  }
  uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED) { reconnectAt = 0; reconnectDelay = 5000; }
  else if (!portalActive && !firmwareUpdating && (int32_t)(now - reconnectAt) >= 0) {
    if (!WiFi.SSID().isEmpty()) WiFi.reconnect();
    reconnectAt = now + reconnectDelay;
    reconnectDelay = std::min<uint32_t>(reconnectDelay * 2, 60000);
  }
  {
    FirmwareUpdate::Latest check; firmwareUpdate.latest(check);
    updateChecking = check.checking;
    if (updateChecking && mdnsStarted) { MDNS.end(); mdnsStarted = false; mdnsRetryAt = now; }  // restarts by itself once the check is over
  }
  {  // telnet needs heap of its own: it goes while memory is short and comes back once there is room again
    static uint32_t lowSince = 0;
    const size_t dataFree = heap_caps_get_free_size(MALLOC_CAP_8BIT), dataBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (telnetLow) {
      if (dataFree >= 30000 && dataBlock >= 14000) { telnetLow = false; lowSince = 0; }
    } else if (telnetStarted && (dataFree < 20000 || dataBlock < 8000)) {
      if (!lowSince) lowSince = now;
      else if ((int32_t)(now - lowSince) >= 3000) {  // short dips do not count
        telnet.stop(); telnetStarted = false; telnetLow = true; lowSince = 0;
        Serial.printf("[Memory] telnet stopped while memory is short (free=%u largest=%u)\n", (unsigned)dataFree, (unsigned)dataBlock);
      }
    } else lowSince = 0;
  }
  if (!telnetStarted && face && !telnetLow && !btMode() && Settings::getInstance()->telnet) { telnetStarted = true; telnet.begin(telnetCommand, UM_TELNET_COMMON_HELP UM_TELNET_HELP); }
  if (!portalActive && WiFi.status() == WL_CONNECTED && !mdnsStarted && (int32_t)(now - mdnsRetryAt) >= 0) startMdns();
  if (!portalActive && WiFi.status() == WL_CONNECTED) watchMdns(now);
#ifdef UM_WLED_OVERLAY
  wled::service(face && bootDone && !updateChecking && !firmwareUpdating && WiFi.status() == WL_CONNECTED, now, updateChecking || firmwareUpdating);  // a memory shed gives the canvas back once (shedOnLowMemory) but does not switch the listeners off for good  // opens the UDP listeners when there is heap for them, closes them when it is needed elsewhere
#endif
  { static uint32_t nvsCheckAt = 0; if ((int32_t)(now - nvsCheckAt) >= 0) { nvsCheckAt = now + 60000; watchNvs(); } }
  firmwareUpdate.confirmBoot(startupHealthy(), now, memoryAllocationFailures.load());
  FirmwareUpdate::Status updateStatus; firmwareUpdate.status(updateStatus);
  if (updateStatus.ready && !restartAt) restartAt = now + 1500;
  if (scheduledMode[0]) {
    // Signed manifest and TLS need a valid clock, so wait for NTP before starting.
    if (WiFi.status() == WL_CONNECTED && time(nullptr) > 1700000000 && !FirmwareUpdate::pendingVerification()) {  // a new image still being verified cannot write another one
      bool started = firmwareUpdate.request(scheduledMode);
      consoleLog("[Firmware] Scheduled update %s\n", started ? "started" : "could not start");
      scheduledMode[0] = 0;
      if (!started) { firmwareUpdating = false; updateOnly = false; }
    }
    else if (now - scheduledSince > 90000) { scheduledMode[0] = 0; firmwareUpdating = false; updateOnly = false; }  // no Wi-Fi: give up and start the feeds
  }
  if (updateOnly && !scheduledMode[0]) {
    firmwareUpdate.status(updateStatus);
    if (!updateStatus.busy && !updateStatus.ready) updateOnly = false;  // the install failed: start the face as usual
  }
  static bool retriedInstall = false;
  if (!retriedInstall && !bootWasScheduled && !restartAt && updateStatus.failed && !updateStatus.busy && !updateStatus.ready &&
      !strstr(updateStatus.message, "already been used") && !strstr(updateStatus.message, "not a matching")) {
    // Any failed install gets one more go after a restart, with no face loaded and the whole heap free
    retriedInstall = true;
    consoleLog("[Firmware] Install failed (%s) Restarting to install without the face loaded.\n", updateStatus.message);
    firmwareUpdate.scheduleAfterReboot(firmwareUpdate.target());
    restartAt = now + 1500;
  }
  firmwareUpdate.poll(now, bootDone && !portalActive && WiFi.status() == WL_CONNECTED && !wledBusy(now) && !playing(now));  // a check that came due waits until WLED lets go
  if (!btMode() && !playing(now)) notice::poll(now);
  if (!btMode() && !playing(now)) dlna::poll(now, bootDone && !portalActive && !welcomeMode && !updateOnly && WiFi.status() == WL_CONNECTED);
  Mqtt::instance().setBusy(playing(now));
  Mqtt::instance().poll(now, bootDone && !portalActive && !welcomeMode && !updateOnly && WiFi.status() == WL_CONNECTED);
  if (!playing(now)) autoUpdate(now);
#ifdef UM_SPORTS_SWITCH
  if (!playing(now)) sportsSwitch(now);
#endif
  reportMemory();
  noteLiveData();
  shedOnLowMemory();
  retryPanelQuality(now);
  telnet.poll();
  if (mdnsDue) { mdnsDue = false; startMdns(); }
  if (restartAt && (int32_t)(millis() - restartAt) >= 0) {
    if (provisioningRestart) FirmwareUpdate::confirmForProvisioning(startupHealthy());
    // A restart does not certify a new image: wait for its startup check (rolled back after the deadline)
    if (!FirmwareUpdate::pendingVerification() || millis() > 90000) ESP.restart();
  }
}


// The NVS partition is only 20 KB and shared by every face, so what no longer belongs to the running firmware goes:
// the namespace of the clock software this one replaced always; the F1 cache and the Home Assistant records of faces that are not running only while fewer than 100 entries are free (so people who switch faces often keep them).
// Wi-Fi, settings and update records are not touched.
static bool nvsHas(const char *name) {
  nvs_iterator_t it = nullptr;
  const bool found = nvs_entry_find("nvs", name, NVS_TYPE_ANY, &it) == ESP_OK;
  if (it) nvs_release_iterator(it);
  return found;
}
static void nvsEraseNamespace(const char *name) {
  if (!nvsHas(name)) return;
  nvs_handle_t handle;
  if (nvs_open(name, NVS_READWRITE, &handle) != ESP_OK) return;
  nvs_erase_all(handle);
  nvs_commit(handle);
  nvs_close(handle);
}
static void nvsEraseKeys(const char *name, const char *const *keys, size_t count) {
  nvs_handle_t handle;
  if (nvs_open(name, NVS_READWRITE, &handle) != ESP_OK) return;
  for (size_t i = 0; i < count; ++i) nvs_erase_key(handle, keys[i]);
  nvs_commit(handle);
  nvs_close(handle);
}
// Every key any face stores in the "ultimatrix" namespace (all faces, as settings survive a face switch).
// Anything else there is left over from older firmware. Add a key here when the code starts storing a new one.
static const char *const kKnownKeys[] = {
  "welcomeHidden", "welcomePending", "onboarded", "setupWifi", "setupRefresh", "adminPin",                                 // main.cpp
  "settingsV2", "clockName", "mdnsUseName", "anyHost", "accessScope", "autoUpdate", "nightClock", "dither", "birthdays", "sports", "sportsLog", "pongBg", "pongSp", "pongLv", "pongDt", "pongDy", "arcHrs", "arcMin", "arcPts",                        // Settings.cpp
  "mqOn", "mqCam", "mqHost", "mqPort", "mqUser", "mqPass",                                                                 // Mqtt.cpp
  "ptier", "preason", "plock", "pat", "ptierv",                                                                   // PanelTier.h
  "wledS", "wledP", "wledT", "searchId", "searchTok",                                                                                    // WledState.cpp
  "haCfg", "haTile4", "haTile5", "haTile6", "haWasher", "haMedia", "haTok", "haEn", "haBarV", "haIcnV", "haWeather",  // HaConfig.cpp
  "btPad",                                                                                                                 // Gamepad.cpp
  "haIcn0", "haIcn1", "haIcn2", "haIcn3", "haIcn4", "haIcn5",
};
static void nvsEraseUnknownKeys() {
  char names[16][16];
  size_t found = 0;
  nvs_iterator_t it = nullptr;
  for (esp_err_t err = nvs_entry_find("nvs", "ultimatrix", NVS_TYPE_ANY, &it); err == ESP_OK && found < 16; err = nvs_entry_next(&it)) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    bool known = false;
    for (const char *key : kKnownKeys) if (!strcmp(key, info.key)) { known = true; break; }
    if (!known) strlcpy(names[found++], info.key, sizeof(names[0]));
  }
  if (it) nvs_release_iterator(it);
  if (!found) return;
  const char *list[16];
  for (size_t i = 0; i < found; ++i) { list[i] = names[i]; consoleLog("[Nvs] removing unused key %s\n", names[i]); }
  nvsEraseKeys("ultimatrix", list, found);
}
static void tidyNvs(bool force = false) {
  nvs_stats_t before = {}, after = {};
  nvs_get_stats(nullptr, &before);
  nvsEraseNamespace("clockwise");  // always: nothing reads it
  // Another face's own records stay for people who switch faces often, and go only when the partition is nearly full
  nvs_stats_t now = {};
  nvs_get_stats(nullptr, &now);
  const bool crowded = force || now.free_entries < 100;
  nvsEraseUnknownKeys();  // every boot: keys no firmware writes any more
#if !defined(UM_F1)
  if (crowded) nvsEraseNamespace("um-f1cache");
#endif
#if !defined(UM_HOME)
  if (crowded) {  // the Home Assistant face's own records (settings, token, tiles, alert and media options)
    static const char *const homeKeys[] = {"haCfg", "haTile4", "haTile5", "haTile6", "haWasher", "haMedia", "haTok", "haEn", "haBarV", "haIcnV", "haWeather",
  "haIcn0", "haIcn1", "haIcn2", "haIcn3", "haIcn4", "haIcn5"};
    nvsEraseKeys("ultimatrix", homeKeys, sizeof(homeKeys) / sizeof(homeKeys[0]));
  }
#endif
  nvs_get_stats(nullptr, &after);
  if (before.used_entries != after.used_entries) consoleLog("[Nvs] removed data that no longer belongs: %u of %u entries used (was %u)\n", (unsigned)after.used_entries, (unsigned)after.total_entries, (unsigned)before.used_entries);
}

// The settings storage is watched while the clock runs: when it gets crowded the leftovers go (also those of other faces),
// and what is still too full is reported to the settings page
static void watchNvs() {
  nvs_stats_t stats = {};
  if (nvs_get_stats(nullptr, &stats) != ESP_OK) return;
  if (stats.free_entries < 100) { tidyNvs(true); nvs_get_stats(nullptr, &stats); }
  nvsFreeEntries = stats.free_entries;
}

void setup() {
  Serial.begin(115200);
#ifdef UM_GAMEPAD
  gamepad::boot();
#endif
  heap_caps_register_failed_alloc_callback(allocationFailed);
  telnet.setWatcher(telnetWatch);
  telnet.setFilter([](IPAddress ip) { return accessAllowed(ip, Settings::getInstance()->accessScope); });
  if (FirmwareUpdate::takeScheduled(scheduledMode, sizeof(scheduledMode))) { firmwareUpdating = true; scheduledSince = millis(); bootWasScheduled = true; updateOnly = true; }  // keeps the feeds off until the update has run
  tidyNvs();
  auto &s = *Settings::getInstance();
  s.load();
#ifdef UM_WLED_OVERLAY
  wled::load();
#ifdef UM_GAMEPAD
  if (gamepad::active()) wled::suspend(true);   // the Bluetooth controller takes the memory of WLED, the canvas and the GIFs
#endif
#endif
#ifdef UM_HOME
  ha::load();
#endif
  firmwareUpdate.begin();  // the cached "update available" result, so the boot screen can mention it
  Mqtt::instance().begin(firmwareUpdate.version(), {mqttSettingsChanged, mqttRestart, mqttCheckUpdate, mqttInstallUpdate, mqttUpdateInfo, mqttScreenRow, mqttCpuUsage, mqttLightLevel, mqttSwitchFace});
  {  // The fast display is only for the Wi-Fi setup QR: when setup was asked for. A start that needs the setup late restarts into it (setupRefresh flag)
    Preferences refreshPrefs;
    bool asked = false;
    if (refreshPrefs.begin("ultimatrix", false)) {
      asked = refreshPrefs.getBool("setupWifi", false);
      setupRefreshRestart = refreshPrefs.getBool("setupRefresh", false);
      welcomeMode = refreshPrefs.getBool("welcomePending", false);  // stays set (every start) until "Start the clock" is pressed
      if (setupRefreshRestart) refreshPrefs.remove("setupRefresh");
      refreshPrefs.end();
    }
    setupRefresh = asked || setupRefreshRestart || welcomeMode;  // no saved network: the portal comes up late and restarts into it (the saved SSID can't be read before Wi-Fi is initialised)
    consoleLog("[Boot] reset reason %d, setupWifi=%d setupRefreshRestart=%d welcomeMode=%d -> %s display\n", (int)esp_reset_reason(), (int)asked, (int)setupRefreshRestart, (int)welcomeMode, setupRefresh ? "fast setup" : "normal");
  }
  HUB75_I2S_CFG panel(layout::width, layout::height, 1);
  panel.gpio.e = 18;
  panel.clkphase = false;
  panel.double_buff = !setupRefresh;  // the setup and welcome display is single buffered (static screens, half the DMA memory)
  {  // The colour quality level (PanelTier.h). The update-only boot has no face and every KB goes to the download: it always takes the last, lightest level
    panelState = paneltier::load();
    panelLevel = updateOnly ? paneltier::kCount - 1 : paneltier::active(panelState);
    const paneltier::Tier &tier = paneltier::kTiers[panelLevel];
    panelLut = cie_lut_for_depth(tier.bits);
    panel.setPixelColorDepthBits(tier.bits);
    panel.i2sspeed = tier.mhz >= 20 ? HUB75_I2S_CFG::HZ_20M : tier.mhz >= 16 ? HUB75_I2S_CFG::HZ_16M : HUB75_I2S_CFG::HZ_8M;
    panel.min_refresh_rate = tier.minRefresh;  // the driver merges the lowest bit planes until this is reached, and each plane it keeps doubles the DMA descriptors
    boot::setQuality(tier.iconPercent, tier.uFloor, tier.gradient);
    consoleLog("[Panel] level %d of %d: %s, state level %u reason %u lock %u\n", panelLevel + 1, paneltier::kCount, tier.name, panelState.level, panelState.reason, panelState.lock);
  }
#ifndef UM_RING_ROWS
#define UM_RING_ROWS 8
#endif
  if (!setupRefresh) panel.ring_rows = UM_RING_ROWS;  // streaming mode: 8 rows of bit planes are kept, the rest is generated from a compact framebuffer (0 = keep every row)

  if (setupRefresh) {
    panel.i2sspeed = HUB75_I2S_CFG::HZ_20M;
    panel.min_refresh_rate = 255;
    panel.setPixelColorDepthBits(2);  // the setup QR is white on black: fewer bit planes give the fastest refresh, so cameras see no stripes (the library's minimum). The restart after setup returns to UM_PIXEL_DEPTH_BITS
  }
  if (s.swapBlueGreen) {
    panel.gpio.b1 = 26; panel.gpio.b2 = 12;
    panel.gpio.g1 = 27; panel.gpio.g2 = 13;
  }
  display = new MatrixPanel_I2S_DMA(panel);
  if (display->begin()) {
    displayReady = true;
    boot::setPlot888(plotDithered);
    bootScreen.setPlainBackdrop(setupRefresh);  // the setup display: no dim U behind the icons
    boot::setLevelMap(bootMap);
    appliedBrightness = panelBrightness(s.brightness);
    appliedDither = s.dither;
    display->setDither(s.dither);
    display->setBrightness8(appliedBrightness);
    display->setRotation(s.rotation);
    bootAt = millis();
    if (setupRefresh) drawBoot(boot::Kind::Connecting);  // the setup display skips the logo
    else {
      while (millis() - bootAt < kLogoFadeMs) { drawBoot(boot::Kind::Logo); delay(10); }  // the fade-in is drawn here: setup below is too busy to tick it
      drawBoot(boot::Kind::Logo);
    }
  } else {
    // No panel: a new image that cannot drive it goes back; a proven one stays up headless so Wi-Fi, settings and updates can repair it
    consoleLog("Display allocation failed\n");
    if (!setupRefresh && !updateOnly && paneltier::canLower(panelState)) {  // too heavy for this level: one lighter and restart
      paneltier::lower(panelState, false);
      consoleLog("[Panel] the display did not start: level %d next\n", paneltier::active(panelState) + 1);
      delay(200);
      ESP.restart();
    }
    FirmwareUpdate::rollbackIfPending();
  }
  {  // the router lists each clock under its own name: two clocks would otherwise both be "ultimatrix"
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char dhcpName[24];
    snprintf(dhcpName, sizeof(dhcpName), "ultimatrix-%02x%02x", mac[4], mac[5]);
    WiFi.setHostname(dhcpName);
  }
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);  // as Clockwise does, before any mode change; the STA event handler below re-applies it
  // Modem power saving (the default) delays packets by up to a beacon interval; disable it on every (re)connect.
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t) {
    WiFi.setSleep(false);
    wifi_ps_type_t ps; esp_wifi_get_ps(&ps);
    consoleLog("Wi-Fi power save: %d (0 = off)\n", (int)ps);
    mdnsDue = true;
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  wifi.setDebugOutput(true, WM_DEBUG_NOTIFY);  // portal requests and connection state on serial, never credentials
  WiFi.onEvent([](arduino_event_id_t id, arduino_event_info_t info) {
    switch (id) {
      case ARDUINO_EVENT_WIFI_AP_START: consoleLog("[AP] started, ip %s\n", WiFi.softAPIP().toString().c_str()); break;
      case ARDUINO_EVENT_WIFI_AP_STACONNECTED: consoleLog("[AP] phone joined, %u station(s)\n", (unsigned)WiFi.softAPgetStationNum()); break;
      case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: consoleLog("[AP] phone left, %u station(s)\n", (unsigned)WiFi.softAPgetStationNum()); break;
      case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED: consoleLog("[AP] DHCP gave %s\n", IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str()); break;
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: consoleLog("[STA] disconnected, reason %u\n", (unsigned)info.wifi_sta_disconnected.reason); break;
      case ARDUINO_EVENT_WIFI_STA_CONNECTED: consoleLog("[STA] connected on channel %u\n", (unsigned)info.wifi_sta_connected.channel); break;
      default: break;
    }
  });
  wifi.setConfigPortalBlocking(false);
  wifi.setTitle("Ultimatrix");
  wifi.setClass("ultimatrix-portal");
  wifi.setDisableConfigPortal(false);
  wifi.setSaveConfigCallback([]() {
    Preferences first;  // the first setup ever: the next start shows the settings-page QR code once
    if (first.begin("ultimatrix", false)) { if (!first.getBool("onboarded", false)) first.putBool("welcomePending", true); first.end(); }
    if (WiFi.status() == WL_CONNECTED) portalCloseAt = millis() + 10000;
  });
  wifi.setCustomHeadElement(R"HTML(<link rel='stylesheet' href='/theme.css'>
<script>function busy(t){if(document.getElementById('busy'))return;var d=document.createElement('div');d.id='busy';d.className='busy';d.innerHTML='<div><i class="spin"></i><p>'+t+'</p></div>';document.body.appendChild(d)}
addEventListener('submit',function(e){if(/\/wifi$/.test(e.target.getAttribute('action')||''))busy('Scanning for networks…')});
addEventListener('click',function(e){var a=e.target.closest&&e.target.closest('a');if(a&&/\/wifi$/.test(a.getAttribute('href')||''))busy('Scanning for networks…')});
addEventListener('pageshow',function(){var b=document.getElementById('busy');if(b)b.remove()})</script>
<script>addEventListener('DOMContentLoaded',function(){document.querySelectorAll('.msg').forEach(function(m){
if(!/Saving Credentials/.test(m.textContent))return;
function link(u){return '<a href="'+u+'" target="_blank" rel="noopener">'+u+'</a> (<a href="#" data-copy="'+u+'">copy</a>)'}
var intro=document.querySelector('.portal-intro');if(intro)intro.style.display='none';
function show(ip){m.innerHTML='<b>Wi-Fi saved.</b><br>The clock will restart soon. Reconnect to your normal Wi-Fi, then open '+link('http://ultimatrix.local/')+(ip?' or '+link('http://'+ip+'/'):'')+'.'}
show('');m.addEventListener('click',function(e){var u=e.target.getAttribute('data-copy');if(!u)return;e.preventDefault();var t=document.createElement('textarea');t.value=u;document.body.appendChild(t);t.select();try{if(document.execCommand('copy'))e.target.textContent='copied'}catch(_){}t.remove()});
var attempts=0,timer=setInterval(function(){if(++attempts>30){clearInterval(timer);return}fetch('/setup-ip',{cache:'no-store'}).then(function(r){return r.text()}).then(function(ip){if(!/^\d+\.\d+\.\d+\.\d+$/.test(ip))return;show(ip);clearInterval(timer)}).catch(function(){})},1000)
})})</script>)HTML");
  wifi.setCustomBodyHeader("<div class='portal-brand'><img class='brand-logo' src='/icon-192.png' alt='' width='40' height='40'>Ultimatrix<span class='brand-dot'>.</span></div><h2 class='portal-title'>Wi-Fi <span>setup</span></h2><p class='portal-intro'>Select a network and enter its password.</p>");
  wifi.setCustomBodyFooter("<p class='portal-foot'>Your saved network is kept until you choose another one.<br>Ultimatrix · LED Matrix Clock</p>");
  const char *portalMenu[] = {"wifi", "info", "exit"};
  wifi.setMenu(portalMenu, 3);
  wifi.setWebServerCallback([]() {
    wifi.server->on("/setup-ip", HTTP_GET, []() {
      bool connected = WiFi.status() == WL_CONNECTED;
      wifi.server->sendHeader("Cache-Control", "no-store");
      wifi.server->send(200, "text/plain", connected ? WiFi.localIP().toString() : String(""));
      if (connected && !portalIpServed) { portalIpServed = true; portalCloseAt = millis() + 10000; }
    });
    wifi.server->on("/theme.css", HTTP_GET, []() { sendGzip(*wifi.server, "text/css; charset=utf-8", themeCssGz, themeCssGzEnd); });
    wifi.server->on("/icon-192.png", HTTP_GET, []() {
      wifi.server->sendHeader("Cache-Control", "public, max-age=86400");
      wifi.server->send_P(200, "image/png", (const char *)icon_192Start, icon_192End - icon_192Start);
    });
    wifi.server->on("/favicon.ico", HTTP_GET, []() { wifi.server->send_P(200, "image/png", (const char *)favicon_32Start, favicon_32End - favicon_32Start); });
  });
  wifi.setAPCallback([](WiFiManager *) {
    IPAddress ip = WiFi.softAPIP();
    char address[16];
    snprintf(address, sizeof(address), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    bootScreen.setAccessPoint(kApName, nullptr, address);
    WiFi.setSleep(false);  // no modem power saving while the setup AP is up
    portalActive = true;
    if (!setupRefresh) {  // the portal came up late (saved network gone): restart into the fast display for the QR code
      Preferences prefs;
      if (prefs.begin("ultimatrix", false)) { prefs.putBool("setupRefresh", true); prefs.end(); }
      consoleLog("[WiFi] Restarting into the fast display mode for the setup QR code\n");
      FirmwareUpdate::confirmForProvisioning(startupHealthy());
      ESP.restart();
    }
  });
  // Optional admin PIN, asked once during the first setup. Not offered again when one is set, so a nearby phone
  // cannot replace it by forcing the setup page to open.
  static WiFiManagerParameter pinField("pin", "Admin PIN (optional): 4 to 8 digits that protect the clock's settings. Leave empty for none.", "", 8, "inputmode='numeric' pattern='[0-9]*' autocomplete='off' placeholder='No PIN'");
  if (!adminpin::isSet()) {
    wifi.addParameter(&pinField);
    wifi.setSaveConfigCallback([]() {
      if (adminpin::validFormat(pinField.getValue()) && adminpin::set(pinField.getValue())) consoleLog("[WiFi] Admin PIN set during setup\n");
    });
  }
  wifi.setWaitCallback(bootTick);  // keep the connecting pulse going while a saved network is tried
  wifi.setConnectTimeout(40);  // seconds per attempt, as the core's own wait: without a timeout the wait does not call bootTick, so the pulse froze
  wifi.setConnectRetries(3);  // three attempts before the setup portal: a router that is still starting after a power cut should not bring up the QR code
  Preferences setupPrefs;
  bool forceSetup = false;
  if (setupPrefs.begin("ultimatrix", false)) {
    forceSetup = setupPrefs.getBool("setupWifi", false);
    if (forceSetup) setupPrefs.putBool("setupWifi", false);
    setupPrefs.end();
  }
  if (forceSetup || setupRefreshRestart) wifi.startConfigPortal(kApName);  // after the restart for the fast display, go straight to the portal
  else wifi.autoConnect(kApName);
  static const char *headerKeys[] = {"Origin", "Sec-Fetch-Site", "Sec-WebSocket-Key", "User-Agent", "X-Token"};
  server.addHandler(new HostGuard());  // first: every request, GET and POST
  server.collectHeaders(headerKeys, 5);  // Origin and X-Token: see BoundedPost, Sec-Fetch-Site: /win; the others are read by the WLED overlay
  server.on("/", HTTP_GET, settingsPage);
  postRoute("/settings", saveSettings);
  server.on("/theme.css", HTTP_GET, []() { sendGzip(server, "text/css; charset=utf-8", themeCssGz, themeCssGzEnd); });
  server.on("/auth.js", HTTP_GET, []() { sendGzip(server, "application/javascript; charset=utf-8", authJsGz, authJsGzEnd); });
  server.on("/settings.js", HTTP_GET, []() { sendGzip(server, "application/javascript; charset=utf-8", settingsJsGz, settingsJsGzEnd); });
  server.on("/manifest.webmanifest", HTTP_GET, []() { server.send_P(200, "application/manifest+json", manifestJson); });
  server.on("/icon-192.png", HTTP_GET, []() { sendIcon(icon_192Start, icon_192End); });
  server.on("/icon-512.png", HTTP_GET, []() { sendIcon(icon_512Start, icon_512End); });
  server.on("/icon-maskable-192.png", HTTP_GET, []() { sendIcon(icon_maskable_192Start, icon_maskable_192End); });
  server.on("/apple-touch-icon.png", HTTP_GET, []() { sendIcon(apple_touch_iconStart, apple_touch_iconEnd); });
  server.on("/favicon-32.png", HTTP_GET, []() { sendIcon(favicon_32Start, favicon_32End); });
  server.on("/favicon.ico", HTTP_GET, []() { sendIcon(favicon_32Start, favicon_32End); });
  server.on("/faces/football.png", HTTP_GET, []() { sendIcon(footballStart, footballEnd); });
  server.on("/faces/f1.png", HTTP_GET, []() { sendIcon(f1Start, f1End); });
  server.on("/faces/motogp.png", HTTP_GET, []() { sendIcon(motogpStart, motogpEnd); });
  server.on("/faces/tetris.png", HTTP_GET, []() { sendIcon(tetrisStart, tetrisEnd); });
  server.on("/faces/word.png", HTTP_GET, []() { sendIcon(wordStart, wordEnd); });
  server.on("/faces/mario.png", HTTP_GET, []() { sendIcon(marioStart, marioEnd); });
  server.on("/faces/luigi.png", HTTP_GET, []() { sendIcon(luigiStart, luigiEnd); });
  server.on("/faces/pacman.png", HTTP_GET, []() { sendIcon(pacmanStart, pacmanEnd); });
  server.on("/faces/invaders.png", HTTP_GET, []() { sendIcon(invadersStart, invadersEnd); });
  server.on("/faces/pong.png", HTTP_GET, []() { sendIcon(pongStart, pongEnd); });
  server.on("/faces/home.png", HTTP_GET, []() { sendIcon(homeStart, homeEnd); });
  server.on("/api/screen", HTTP_GET, screenPreview);
  server.on("/api/settings", HTTP_GET, settingsState);
  postRoute("/api/settings", saveSettings);
#if defined(UM_PACMAN) || defined(UM_INVADERS) || defined(UM_PONG)
  postRoute("/api/play/steer", []() {   // play the face yourself: dir 0 up, 1 right, 2 down, 3 left, -1 back to the computer (what each does is up to the face)
    const int dir = server.arg("dir").toInt();
#if defined(UM_PONG) || defined(UM_INVADERS)
    playSteer(dir);
#else
    if (face) face->steer(dir);
#endif
    server.send(204);
  });
#if defined(UM_PONG) || defined(UM_INVADERS)
  playsocket::attach(server, playSteer);   // the same codes over a WebSocket: GET /play
#endif
#endif
#ifdef UM_PACMAN
  postRoute("/api/pacman/steer", []() {   // the old path, for a settings page that is still cached
    const int dir = server.arg("dir").toInt();
    if (face) face->steer(dir);
    server.send(204);
  });
#ifdef UM_GAMEPAD
  server.on("/api/gamepad", HTTP_GET, []() {   // Bluetooth game controller: state, name, number of bonded controllers
    String json = "{\"enabled\":" + String(gamepad::enabled() ? "true" : "false") + ",\"state\":\"" + String(gamepad::state()) + "\",\"name\":" + jsonText(gamepad::name()) + ",\"bonded\":" + String(gamepad::bonded()) + "}";
    server.send(200, "application/json", json);
  });
  postRoute("/api/gamepad/enable", []() {   // the memory is set aside (or given back) at the start, so this restarts the clock
    const bool on = server.arg("on").toInt() != 0;
    bool saved = gamepad::setEnabled(on);
    if (!saved) { tidyNvs(true); saved = gamepad::setEnabled(on); }   // a full settings storage: drop what no longer belongs, once
    if (!saved) { server.send(507, "text/plain", "The settings storage is full."); return; }
    server.send(204); restartAt = millis() + 800;
  });
  postRoute("/api/gamepad/pair", []() { gamepad::pair(); server.send(204); });   // when the controller is not running yet this restarts the clock into it
  postRoute("/api/gamepad/forget", []() { gamepad::forget(); server.send(204); });
#endif
#endif
  server.on("/api/catalog", HTTP_GET, selectionCatalog);
#ifdef UM_FOOTBALL_ONLY
  server.on("/api/teams", HTTP_GET, footballTeams);
#endif
  server.on("/api/timezones", HTTP_GET, timezones);
  postRoute("/api/wifi/setup", openWifiSetup);
  postRoute("/api/auth", pinLogin);
  postRoute("/api/pin", pinSave);
  postRoute("/api/welcome/dismiss", dismissWelcome);
  postRoute("/api/welcome/start", startClock);
  server.on("/api/firmware", HTTP_GET, firmwareStatus);
  postRoute("/api/firmware", requestFirmwareUpdate);
  postRoute("/api/firmware/reboot", rebootForFirmwareUpdate);
  postRoute("/api/firmware/check", checkFirmware);
  postRoute("/api/firmware/server", saveUpdateServer);
#ifdef UM_SPORTS_SWITCH
  server.on("/api/sports", HTTP_GET, sportsInfo);
  postRoute("/api/sports/prepare", sportsPrepare);
#endif
  server.on("/api/storage", HTTP_GET, storageState);
  postRoute("/api/storage/clear", storageClear);
  postRoute("/api/storage/reformat", storageReformat);
  server.on("/api/display", HTTP_GET, displayStatus);
  postRoute("/api/display", displaySave);
  server.on("/api/mqtt", HTTP_GET, mqttState);
  postRoute("/api/mqtt", mqttSave);
#ifdef UM_WLED_OVERLAY
  if (!wled::suspendedNow()) wled::apiAttach(server, postRoute);
#endif
  if (!btMode()) dlna::attach(server, postRoute);
#ifdef UM_HOME
  server.on("/api/ha", HTTP_GET, haState);
  postRoute("/api/ha", haSave);
  server.on("/api/ha/suggest", HTTP_GET, haSuggest);
  server.on("/api/ha/detect", HTTP_GET, haDetectAnswer);
  postRoute("/api/ha/detect", haDetectAsk);
#endif
  backgroundWork() = background;
}

void loop() {
  uint32_t loopStart = millis();
  background();
  uint32_t backgroundMs = millis() - loopStart;
  if (portalActive && backgroundMs > 40) consoleLog("[Portal] background() took %u ms\n", (unsigned)backgroundMs);
  if (!displayReady) { delay(50); return; }
  uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED && !portalActive && !welcomeMode && !updateOnly) {
    auto &s = *Settings::getInstance();
    if (!geoSettled && !geoWanted && geoState.load() == 0) {  // once per start: the network's zone and location
      if (applyZonePlace(s)) s.save();  // clocks saved before the zone's place was used
      geoSettled = !(!s.manualTimezone || !s.hasLocation);
      geoWanted = !geoSettled;
    }
    if (geoWanted && geoState.load() == 0 && (int32_t)(now - geoRetryAt) >= 0 && !wledBusy(now)) { geoWanted = false; startGeo(); }
    if (geoState.load() == 2) {
      if (geoFound) {
        bool changed = false, zoneChanged = false;
        if (!s.manualTimezone && geoZone != s.timezone) { s.timezone = geoZone; changed = zoneChanged = true; }
        float zoneLatitude, zoneLongitude;
        const bool zonePlaced = s.manualTimezone && ultime::placeForZone(s.timezone.c_str(), zoneLatitude, zoneLongitude);  // the chosen zone keeps its own place
        if (!zonePlaced && (!s.hasLocation || fabsf(geoLatitude - s.latitude) > 0.05f || fabsf(geoLongitude - s.longitude) > 0.05f)) {
          s.latitude = geoLatitude; s.longitude = geoLongitude; s.hasLocation = true; changed = true;
        }
        if (changed) s.save();
        if (zoneChanged && face) clockTime.begin(s.timezone.c_str(), s.use24h, s.ntpServer.c_str(), "");
        geoSettled = true; geoFails = 0;
      } else if (++geoFails < 5) { geoWanted = true; geoRetryAt = now + 60000; }
      else geoSettled = true;
      geoState = 0;
    }
  }
  // Without a known location (the first start) the face waits a few seconds for the lookup, so it does not start in the wrong zone
  bool geoHold = !Settings::getInstance()->hasLocation && !geoSettled && now - geoStartedAt < 6000 && geoState.load() != 2;
  static bool updateTimeStarted = false;
  if (updateOnly && !updateTimeStarted && !portalActive && WiFi.status() == WL_CONNECTED) {  // the signed manifest needs the time
    auto &s = *Settings::getInstance();
    clockTime.begin(s.timezone.c_str(), s.use24h, s.ntpServer.c_str(), "");
    updateTimeStarted = true;
  }
  if (!face && !updateOnly && !portalActive && !welcomeMode && WiFi.status() == WL_CONNECTED && !geoHold) {
    auto &s = *Settings::getInstance();
    clockTime.begin(s.timezone.c_str(), s.use24h, s.ntpServer.c_str(), "");
    face = new Clockface(display);
    face->setup(&clockTime);
#ifdef UM_GAMEPAD
    gamepad::begin();
#endif
#ifdef UM_WLED_OVERLAY
    wled::attachPanel(*display);
#endif
    consoleLog("Ultimatrix %s settings: http://%s/ (http://%s.local/)\n", UM_MODE_NAME, WiFi.localIP().toString().c_str(), mdnsHost);
  }
#if defined(UM_PONG) || defined(UM_INVADERS)
  playsocket::service();   // the game controller socket is read on every pass, not only on frames
#endif
  if ((int32_t)(now - nextFrame) >= 0) {
    uint8_t brightness = displayControl.update(*Settings::getInstance(), now, ultime::timeStatus() == timeSet,
        clockTime.getHour24() * 60 + clockTime.getMinute(), !bootDone || portalActive, face && face->handlesNight());
    if ((face && face->alertFullBrightness()) || notice::fullBrightness(now)) brightness = 255;  // a popup that asked for full brightness
    uint8_t panel = panelBrightness(brightness);
    const bool deepDim = displayControl.nightDimmed(face && face->handlesNight());  // the red night clock may dim past the safe window width
    if (panel != appliedBrightness || deepDim != appliedDeepDim) {
      const uint8_t dimFloor = display->brightnessFloor();
      display->setDeepDim(deepDim);
      display->setBrightness8(panel);
      appliedDeepDim = deepDim;
      if (panel < dimFloor || appliedBrightness < dimFloor) { nightClock.reset(); if (face) face->invalidate(); }  // dimming below the floor changes the pixel values; the night clock draws once a minute
      appliedBrightness = panel;
    }
    {  // the setting, unless the face knows better (Pac-Man never, Mario and Luigi per world); the gain table is rebuilt with or without the pattern, then the face repaints
      const int policy = face ? face->ditherPolicy() : -1;
      const bool dither = policy < 0 ? Settings::getInstance()->dither : policy == 1;
      if (dither != appliedDither) {
        appliedDither = dither;
        display->setDither(dither);
        display->setBrightness8(appliedBrightness);
        nightClock.reset(); if (face) face->invalidate();
      }
    }
    bootTick();  // before the check below: it stamps connectedAt on the connect
    if (face && !portalActive && !bootDone && syncedAt) {
      uint32_t holdAt = millis();  // not the loop's `now`: bootTick() just stamped connectedAt/syncedAt/noticeAt with a later time, and an unsigned difference to it would wrap and end the notice at once
      if (bootHoldOver(holdAt)) { bootDone = true; consoleLog("[Boot] done at %u ms\n", (unsigned)holdAt); }
    }
    const char *mqttDisplayMode = "boot";
    if (bootDone) {  // keeps drawing during an update: the feeds are parked, the face shows its last snapshot
      // A face that paints its own night (Mario, Luigi) follows the sun for its sky and only gives way to the red clock in a dark room
      face->poll();  // the face follows its data while the night clock is up too
#ifdef UM_WLED_OVERLAY
      wled::poll(clockTime.getHour24(), clockTime.getMinute(), clockTime.getSecond(), clockTime.is24hFormat(), clockTime.getWeekday(), now);
      bool wledDrawn = false;
#endif
      bool dlnaDrawn = false;
      static bool mqttPixelsShown = false;
      if (mqttPixelsShown && !Mqtt::instance().pixelsActive()) { nightClock.reset(); face->invalidate(); }
      mqttPixelsShown = Mqtt::instance().pixelsActive();
      const bool popup = face->alertOverridesNight();  // an alert that must show through the night clock and the birthday screen
      const bool night = displayControl.nightClockShown(face->handlesNight()) && !popup;
      if (!btMode() && !playing(now) && notice::active(now, displayControl.nightDimmed(face->handlesNight()) && !popup)) {  // a message sent from Home Assistant takes the panel, whatever the face; a normal one waits out the night
        mqttDisplayMode = "notification";
        notice::draw(*display, now);
        noticeShown = true;
      } else if (Mqtt::instance().drawPixels(*display)) {
        mqttDisplayMode = "pixels";
      } else if (night) {
        mqttDisplayMode = "clock";
        nightClock.draw(*display, clockTime.getHour(), clockTime.getMinute(), clockTime.is24hFormat());
        nightShown = true;
      } else if (!playing(now) && dlna::active()) {  // a picture played from Home Assistant stays until Stop, below notices and the night clock
        mqttDisplayMode = "image";
        dlna::draw(*display);
        dlnaDrawn = dlnaShown = true;
#ifdef UM_WLED_OVERLAY
      } else if (wled::wanted(now)) {  // WLED content takes the panel below the Notice and the night clock; the face is not updated meanwhile
        mqttDisplayMode = "wled";
        wled::draw(*display, now);
        wledDrawn = wledShown = true;
#endif
      } else if (const Birthdays::Entry *birthday = popup ? nullptr : birthdayToday()) {
        char time[16];
        snprintf(time, sizeof(time), clockTime.is24hFormat() ? "%02d:%02d" : "%d:%02d", clockTime.getHour(), clockTime.getMinute());
        int age = birthday->year ? clockTime.getYear() - birthday->year : 0;
        mqttDisplayMode = "birthday";
        birthday::party::draw(*display, now, birthday->name, age, time);
        display->flipDMABuffer();
        birthdayShown = true;
      } else {
        if (nightShown || birthdayShown || noticeShown || dlnaShown
#ifdef UM_WLED_OVERLAY
            || wledShown
#endif
        ) {
          nightShown = birthdayShown = noticeShown = dlnaShown = false;
#ifdef UM_WLED_OVERLAY
          wledShown = false;
#endif
          nightClock.reset(); face->invalidate();
        }
        face->setNight(face->handlesNight() ? displayControl.sunDown() : displayControl.nightActive());
        mqttDisplayMode = "clock";
#ifdef UM_GAMEPAD
        { gamepad::poll(); if (gamepad::restartWanted() && !restartAt) restartAt = millis() + 800; int dir; if (gamepad::take(dir)) face->steer(dir); }
#endif
        face->update();
      }
      if (!dlnaDrawn) dlna::hidden();
#ifdef UM_WLED_OVERLAY
      if (!wledDrawn) wled::hidden();  // something above took the panel, or the content ended: the listeners stop drawing before anyone else does
#endif
    }
    Mqtt::instance().afterFrame(mqttDisplayMode, now);
#ifdef UM_WLED_OVERLAY
    static bool wledPace = false;  // effects and streams run at up to 50 frames per second while WLED has the panel
    wledPace = bootDone && wledShown && wled::shown();
#endif
#ifdef UM_FOOTBALL_ONLY
    // The goal animation paces itself (40 ms frames by wall time): poll fast so no frame is late or skipped
    nextFrame = millis() + (face && face->animating() ? 4 : layout::frameMs);
#elif defined(UM_SCANNER)
    nextFrame = millis() + 20;
#elif defined(UM_PONG) || defined(UM_INVADERS)
    nextFrame = millis() + 16;  // games: about 60 frames per second, so the movement is smooth
#elif defined(UM_MARIO) || defined(UM_LUIGI)
    nextFrame = millis() + 33;  // smoother plumber animation at about 30 frames per second
#else
    nextFrame = millis() + layout::frameMs;
#endif
    if (birthdayShown) nextFrame = millis() + kBirthdayFrameMs;
#ifdef UM_WLED_OVERLAY
    if (wledPace) nextFrame = millis() + 20;
#endif
    if (portalActive && millis() - loopStart - backgroundMs > 40) consoleLog("[Portal] frame took %u ms\n", (unsigned)(millis() - loopStart - backgroundMs));
  }
  delay(portalActive ? 1 : 2);  // the setup page and DNS answers wait on this loop
}

// ESP-IDF owns startup. The rendering task stays on core 1; sport services
// retain their existing background tasks on core 0 during migration.
static void clockTask(void *) {
  setup();
  for (;;) loop();
}

extern "C" void app_main() {
  initArduino();
  if (xTaskCreatePinnedToCore(clockTask, "ultimatrix", 8192, nullptr, 1, nullptr, 1) != pdPASS)
    ESP_LOGE("ultimatrix", "Could not allocate the clock task");
}
