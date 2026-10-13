#include <Arduino.h>
#include <atomic>
#include <esp_heap_caps.h>
#include <esp_log.h>
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
#include <DisplayControl.h>
#include <NightClock.h>
#include <FirmwareUpdate.h>
#ifdef UM_FOOTBALL_ONLY
#include <FootballCatalog.h>
#elif defined(UM_MOTOGP)
#include <MotoGpRiders.h>
#elif defined(UM_F1)
#include <F1Drivers.h>
#elif defined(UM_WORD)
#include <WordLanguages.h>
#endif
#include <BackgroundWait.h>
#include <BootScreen.h>
#include <Console.h>
#include <TelnetConsole.h>
#include <strings.h>

// Temporary service flags used by the existing sport data modules during migration.
std::atomic<uint32_t> memoryAllocationFailures{0};
static bool displayReady = false;

static void allocationFailed(size_t, uint32_t, const char *) {
  memoryAllocationFailures.fetch_add(1, std::memory_order_relaxed);
}

volatile bool firmwareUpdating = false;
volatile bool liveEventOn = false;
volatile bool faceBootDone = false;

static MatrixPanel_I2S_DMA *display = nullptr;
static Clockface *face = nullptr;
static ClockTime clockTime;
static WebServer server(80);
static WiFiManager wifi;
static uint32_t nextFrame = 0;
static uint32_t restartAt = 0;
static char scheduledMode[16] = "";  // update chosen before a low-memory reboot; started once Wi-Fi is up
static uint32_t scheduledSince = 0;
static DisplayControl displayControl;
static NightClock nightClock;
static bool nightShown = false;
static FirmwareUpdate firmwareUpdate;
static uint8_t appliedBrightness = 0;

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
static uint8_t bootMap[256];  // a level at full scale -> the input that looks like it at bootScale
static uint8_t bootInverse[256];  // output level -> smallest input that gives at least it

static void setBootScale(unsigned scale) {
  if (scale == bootScale) return;
  if (!bootScale) {
    for (int o = 0; o < 256; o++) {
      int v = 255;
      for (int i = 0; i < 256; i++) if (lumConvTab[i] >= o) { v = i; break; }
      bootInverse[o] = (uint8_t)v;
    }
  }
  bootScale = scale;
  for (int v = 0; v < 256; v++) bootMap[v] = bootInverse[(lumConvTab[v] * scale + 128) >> 8];
  bootScreen.invalidate();
}

// The panel brightness for a user setting, rescaling the boot screens to match
static uint8_t panelBrightness(uint8_t userBrightness) {
  if (bootDone) { setBootScale(256); return userBrightness; }
  if (portalActive) { setBootScale(256); return kBootPanelBrightness; }  // the setup QR is shown at full brightness whatever the setting, so phones scan it reliably
  uint8_t panel = userBrightness > kBootPanelBrightness ? userBrightness : kBootPanelBrightness;
  setBootScale(userBrightness * 256u / panel);
  return panel;
}

// The logo's gradient: the wanted output level is kept fractional and ordered-dithered across the lattice (8x8 Bayer),
// then mapped back to the input that gives it, for gradient steps finer than the panel's whole levels.
static void plotDithered(Adafruit_GFX &gfx, int x, int y, uint16_t r, uint16_t g, uint16_t b) {
  unsigned lx = x >> 1, ly = y >> 1, lxy = lx ^ ly;  // lattice coordinates; 8x8 Bayer index 0..63 by bit interleave
  unsigned index = ((ly & 1) << 5) | ((lxy & 1) << 4) | (((ly >> 1) & 1) << 3) | (((lxy >> 1) & 1) << 2) | (((ly >> 2) & 1) << 1) | ((lxy >> 2) & 1);
  unsigned threshold = index * 4;  // 0..252 of a level's 256
  auto channel = [&](uint16_t in) -> uint8_t {
    unsigned i = in >> 8, f = in & 255;
    if (i > 255) i = 255;
    unsigned next = i < 255 ? lumConvTab[i + 1] : lumConvTab[i];
    unsigned out = (((lumConvTab[i] * 256 + (next - lumConvTab[i]) * f) * bootScale >> 8) + threshold) >> 8;
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
  if (now - bootAt < kLogoMs) { drawBoot(boot::Kind::Logo); return; }
  static bool wasConnected = false;
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) {
    IPAddress ip = WiFi.localIP();
    snprintf(ipText, sizeof(ipText), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    connectedAt = now;
  }
  wasConnected = connected;
  bool synced = connected && ultime::timeStatus() == timeSet;
  static bool bootChecked = false;
  if (synced && !bootChecked) { bootChecked = true; if (firmwareUpdate.checkDue()) firmwareUpdate.checkNow(); }
  if (!synced) syncedAt = 0;
  else if (!syncedAt) syncedAt = now;
  // The settings page and telnet already answer from the moment the link is up, so the address is shown
  // on every screen from here on
  if (!connected) drawBoot(portalActive ? boot::Kind::Setup : boot::Kind::Connecting);
  else if (!synced) drawBoot(now - connectedAt > kSyncGiveUpMs ? boot::Kind::Error : boot::Kind::Syncing, ipText);
  else if (noticeAt && now - noticeAt >= kUpdateNoticeMs) return;  // notice over: leave the panel to the face
  else if (now - syncedAt >= kConnectedMs && now - connectedAt >= kAddressMinMs && updateAvailable()) {
    if (!noticeAt) noticeAt = now ? now : 1;
    drawBoot(boot::Kind::Update);
  }
  else drawBoot(boot::Kind::Connected, ipText);
}

// The mDNS name is set up again after every GOT_IP, so ultimatrix.local survives a router restart
static void startMdns() {
  mdnsRetryAt = millis() + 15000;
  if (mdnsStarted) MDNS.end();
  mdnsStarted = MDNS.begin("ultimatrix");
  if (mdnsStarted) {
    MDNS.setInstanceName("Ultimatrix " UM_MODE_NAME);
    MDNS.addService("http", "tcp", 80);
    if (Settings::getInstance()->telnet) MDNS.addService("telnet", "tcp", 23);
  }
  consoleLog("mDNS: ultimatrix.local %s\n", mdnsStarted ? "up" : "FAILED");
}

#ifdef UM_FOOTBALL_ONLY
#define UM_TELNET_HELP "  goal    a made-up goal (g)\r\n  yellow  a yellow card (y)\r\n  red     a red card (d)\r\n  sub     a substitution (w)\r\n" \
                       "  sim     next simulated screen (s)\r\n  next    the same (n)\r\n"
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
  if (status.lowMemory) { telnetUpdate = TelnetUpdate::Asking; out->printf("%s (y/n)\r\n> ", status.message); }
  else if (status.ready || (!status.busy && status.failed)) { telnetUpdate = TelnetUpdate::None; out->printf("%s\r\n> ", status.message); }
  else if (!status.busy) telnetUpdate = TelnetUpdate::None;
}

static bool telnetCommand(const char *c, Print &out) {
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
    else if (status.lowMemory) { telnetUpdate = TelnetUpdate::Asking; out.printf("%s (y/n)\r\n", status.message); }
    else out.printf("Update not started: %s\r\n", status.message);
    return true;
  }
  if (!strcasecmp(c, "update-status")) {
    FirmwareUpdate::Status status; firmwareUpdate.status(status);
    out.printf("%s (%lu/%lu bytes)\r\n", status.message, (unsigned long)status.received, (unsigned long)status.total); return true;
  }
#if defined(UM_FOOTBALL_ONLY) || defined(UM_F1)
  if (!face) { out.print("Not ready yet.\r\n"); return true; }
  auto is = [&](const char *a, const char *b) { return !strcasecmp(c, a) || !strcasecmp(c, b); };
  if (is("sim", "s")) { out.printf("%s\r\n", face->simulate()); return true; }
  if (is("next", "n")) { out.printf("%s\r\n", face->simulateNext()); return true; }
#ifdef UM_FOOTBALL_ONLY
  if (is("goal", "g")) { face->testGoal(); out.print("Goal.\r\n"); return true; }
  if (is("yellow", "y")) { face->testIncident('y'); out.print("Yellow card.\r\n"); return true; }
  if (is("red", "d")) { face->testIncident('r'); out.print("Red card.\r\n"); return true; }
  if (is("sub", "w")) { face->testIncident('s'); out.print("Substitution.\r\n"); return true; }
#endif
#endif
  return false;
}

extern const char settingsHtml[] asm("_binary_settings_html_start");
extern const char themeCss[] asm("_binary_theme_css_start");
extern const char settingsJs[] asm("_binary_settings_js_start");
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

static void settingsPage() { server.send_P(200, "text/html; charset=utf-8", settingsHtml); }

static String minuteText(unsigned minute) {
  char text[6];
  snprintf(text, sizeof(text), "%02u:%02u", (minute / 60) % 24, minute % 60);
  return text;
}

static void settingsState() {
  const auto &s = *Settings::getInstance();
  String json;
  json.reserve(1500);
  json = "{\"mode\":\"" UM_MODE_NAME "\",\"modeId\":\"" UM_MODE_ID "\",\"timeSynced\":";
  json += ultime::timeStatus() == timeSet ? "true" : "false";
  json += ",\"lightReading\":" + String(displayControl.lightReading());
  json += ",\"nightActive\":" + String(displayControl.nightActive() ? "true" : "false");
  json += ",\"network\":{\"ssid\":" + jsonText(WiFi.SSID()) + ",\"ip\":" + jsonText(WiFi.localIP().toString()) + "},\"settings\":{";
  json += "\"brightness\":" + String(s.brightness) + ",\"rotation\":" + String(s.rotation);
  json += ",\"use24h\":" + String(s.use24h ? "true" : "false") + ",\"ntpServer\":" + jsonText(s.ntpServer);
  json += ",\"autoBrightness\":" + String(s.autoBrightness ? "true" : "false") + ",\"ldrPin\":" + String(s.ldrPin);
  json += ",\"minimumBrightness\":" + String(s.minimumBrightness) + ",\"ldrDark\":" + String(s.ldrDark) + ",\"ldrLight\":" + String(s.ldrLight);
  json += ",\"nightEnabled\":" + String(s.nightEnabled ? "true" : "false") + ",\"nightStart\":" + jsonText(minuteText(s.nightStart));
  json += ",\"nightEnd\":" + jsonText(minuteText(s.nightEnd)) + ",\"nightBrightness\":" + String(s.nightBrightness);
  json += ",\"timezone\":" + jsonText(s.timezone) + ",\"autoTimezone\":" + String(s.manualTimezone ? "false" : "true") + ",\"matchSecs\":" + String(s.matchSecs);
  json += ",\"resultMins\":" + String(s.resultMins) + ",\"timeStyle\":" + String(s.timeStyle);
  json += ",\"swapBlueGreen\":" + String(s.swapBlueGreen ? "true" : "false") + ",\"telnet\":" + String(s.telnet ? "true" : "false");
  json += ",\"fbLeagues\":" + jsonText(s.footballLeagues) + ",\"fbTeams\":" + jsonText(s.footballTeams);
  json += ",\"f1Drivers\":" + jsonText(s.f1Drivers);
  json += ",\"showRankings\":" + String(s.showRankings ? "true" : "false");
  json += ",\"ballRoll\":" + String(s.ballRoll ? "true" : "false");
  json += ",\"numberFont\":" + String(s.numberFont == 1 ? "true" : "false");
  json += ",\"wordLanguage\":" + String(s.numberFont);
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
  bool first = true;
  server.sendContent("[");
#ifdef UM_FOOTBALL_ONLY
#elif defined(UM_MOTOGP)
  (void)first;
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

static void timezones() {
#ifdef UM_FOOTBALL_ONLY
static void footballTeams() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  for (const char *part : FOOTBALL_TEAMS_JSON) server.sendContent_P(part);  // [id, abbreviation, name, 'c'|'n']; the parts split one array
  server.sendContent("");
}
#endif

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
  if (selectedValues("fbLeagues").length() > 400 || server.arg("fbTeams").length() > 400 || selectedValues("f1Drivers").length() > 400) {
    settingsResponse(400, "Your selection is too long. Shorten it and try again.");
    return;
  }
  Settings candidate = *Settings::getInstance();
  auto &s = candidate;
  // Optional new fields preserve compatibility with existing bookmarked forms.
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
    long pin, dark, light, minimum, night;
    if (!number("ldrPin", 32, 39, pin) || (pin != 32 && pin != 33 && pin != 34 && pin != 35 && pin != 36 && pin != 39) ||
        !number("ldrDark", 0, 4094, dark) || !number("ldrLight", 1, 4095, light) || light <= dark ||
        !number("minimumBrightness", 1, 255, minimum) || !number("nightBrightness", 1, 255, night) ||
        !clockMinute("nightStart", s.nightStart) || !clockMinute("nightEnd", s.nightEnd)) {
      settingsResponse(400, "Check the light sensor values, brightness limits, and night times."); return;
    }
    s.autoBrightness = server.hasArg("autoBrightness"); s.ldrPin = pin; s.ldrDark = dark; s.ldrLight = light;
    s.minimumBrightness = minimum; s.nightBrightness = night; s.nightEnabled = server.hasArg("nightEnabled");
    if (s.nightEnabled && s.nightStart == s.nightEnd) { settingsResponse(400, "Choose different start and end times for night mode."); return; }
  }
  s.brightness = bright; s.rotation = rotation;
  if (autoZone) { String found; if (s.manualTimezone && ultime::detectZone(found)) s.timezone = found; }
  else s.timezone = zone;
  s.manualTimezone = !autoZone;
#ifndef UM_SIMPLE_FACE
  s.matchSecs = seconds; s.resultMins = results; s.timeStyle = style;
#endif
#ifdef UM_WORD
  if (server.hasArg("wordLanguage")) {
    long language;
    if (!number("wordLanguage", 0, WORD_LANGUAGE_COUNT - 1, language)) { settingsResponse(400, "Choose a language from the list."); return; }
    s.numberFont = language;  // the Time in Words language shares the football "alternative digits" byte
  }
#endif
  s.swapBlueGreen = server.hasArg("swapBlueGreen");
  s.telnet = server.hasArg("telnet");
#ifdef UM_FOOTBALL_ONLY
  s.footballLeagues = selectedValues("fbLeagues"); s.footballTeams = server.arg("fbTeams");
  s.showRankings = server.hasArg("showRankings"); s.ballRoll = server.hasArg("ballRoll");
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
  *Settings::getInstance() = candidate;
  if (display) display->setRotation(s.rotation);
  if (timeChanged && face) clockTime.begin(s.timezone.c_str(), s.use24h, s.ntpServer.c_str(), "");
  if (consoleChanged) {
    if (s.telnet) telnet.begin(telnetCommand, "  update  download signed application update\r\n  update-status  show update progress\r\n" UM_TELNET_HELP);
    else telnet.stop();
    if (WiFi.status() == WL_CONNECTED) startMdns();
  }
  server.send(200, "application/json", restart ? "{\"message\":\"Settings saved. Restarting Ultimatrix.\",\"restart\":true}" : "{\"message\":\"Settings saved and applied.\",\"restart\":false}");
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
  json += ",\"freeHeap\":" + String(heap_caps_get_free_size(MALLOC_CAP_8BIT));
  json += ",\"minimumHeap\":" + String(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)) + "}";
  server.sendHeader("Cache-Control", "no-store"); server.send(200, "application/json", json);
}

static void checkFirmware() {
  bool started = firmwareUpdate.checkNow();
  settingsResponse(started ? 202 : 429, started ? "Checking for updates." : "Wait a moment before checking again.");
}

static void requestFirmwareUpdate() {
  String mode = server.arg("mode");
  if (mode.length() && !FirmwareUpdate::knownMode(mode.c_str())) { settingsResponse(400, "Unknown clock face."); return; }
  bool accepted = firmwareUpdate.request(mode.c_str());
  FirmwareUpdate::Status status; firmwareUpdate.status(status);
  if (!accepted && status.lowMemory) {
    server.send(409, "application/json", "{\"lowMemory\":true,\"message\":" + jsonText(status.message) + "}"); return;
  }
  settingsResponse(accepted ? 202 : 409, status.message);
}

// Low-memory fallback: restart first, then update before the feeds start.
static void rebootForFirmwareUpdate() {
  String mode = server.arg("mode");
  if (mode.length() && !FirmwareUpdate::knownMode(mode.c_str())) { settingsResponse(400, "Unknown clock face."); return; }
  firmwareUpdate.scheduleAfterReboot(mode.c_str());
  settingsResponse(202, "Restarting to install the update.");
  restartAt = millis() + 1000;
}

static void reportMemory() {
  static uint32_t reportedAt = 0;
  static uint32_t lastFailures = 0;
  uint32_t now = millis();
  uint32_t failures = memoryAllocationFailures.load(std::memory_order_relaxed);
  if (reportedAt && now - reportedAt < 60000 && failures == lastFailures) return;
  reportedAt = now ? now : 1;
  lastFailures = failures;
  consoleLog("[Memory] free=%u min=%u largest=%u dma=%u stack_free=%u failures=%u\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
      (unsigned)uxTaskGetStackHighWaterMark(nullptr), (unsigned)failures);
}

static void background() {
  wifi.process();
  if (portalCloseAt && (int32_t)(millis() - portalCloseAt) >= 0) {
    wifi.stopConfigPortal();
    portalCloseAt = 0;
  }
  bool portalNow = wifi.getConfigPortalActive();
  if (setupRefresh && !portalNow && !restartAt) restartAt = millis() + 1000;  // setup is over: restart out of the fast display (the face needs the memory and colour depth)
  if (portalActive && !portalNow && WiFi.status() != WL_CONNECTED) WiFi.begin();
  portalActive = portalNow;
  if (!settingsServerStarted && WiFi.status() == WL_CONNECTED && !wifi.getConfigPortalActive()) {
    server.begin();
    settingsServerStarted = true;
  }
  if (settingsServerStarted) server.handleClient();
  uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED) { reconnectAt = 0; reconnectDelay = 5000; }
  else if (!portalActive && !firmwareUpdating && (int32_t)(now - reconnectAt) >= 0) {
    if (!WiFi.SSID().isEmpty()) WiFi.reconnect();
    reconnectAt = now + reconnectDelay;
    reconnectDelay = std::min<uint32_t>(reconnectDelay * 2, 60000);
  }
  if (!portalActive && WiFi.status() == WL_CONNECTED && !mdnsStarted && (int32_t)(now - mdnsRetryAt) >= 0) startMdns();
  firmwareUpdate.confirmBoot(displayReady && (settingsServerStarted || portalActive) && heap_caps_get_free_size(MALLOC_CAP_8BIT) > 16384,
      now, memoryAllocationFailures.load());
  FirmwareUpdate::Status updateStatus; firmwareUpdate.status(updateStatus);
  if (updateStatus.ready && !restartAt) restartAt = now + 1500;
  if (scheduledMode[0]) {
    if (WiFi.status() == WL_CONNECTED) { firmwareUpdate.request(scheduledMode); scheduledMode[0] = 0; }
    else if (now - scheduledSince > 90000) { scheduledMode[0] = 0; firmwareUpdating = false; }  // no Wi-Fi: give up and start the feeds
  }
  firmwareUpdate.poll(now, bootDone && !portalActive && WiFi.status() == WL_CONNECTED);
  reportMemory();
  telnet.poll();
  if (mdnsDue) { mdnsDue = false; startMdns(); }
  if (restartAt && (int32_t)(millis() - restartAt) >= 0) { FirmwareUpdate::confirmBeforeRestart(); ESP.restart(); }
}

void setup() {
  Serial.begin(115200);
  heap_caps_register_failed_alloc_callback(allocationFailed);
  telnet.setWatcher(telnetWatch);
  if (FirmwareUpdate::takeScheduled(scheduledMode, sizeof(scheduledMode))) { firmwareUpdating = true; scheduledSince = millis(); }  // keeps the feeds off until the update has run
  auto &s = *Settings::getInstance();
  s.load();
  firmwareUpdate.begin();  // the cached "update available" result, so the boot screen can mention it
  {  // The fast display is only for the Wi-Fi setup QR: when setup was asked for. A start that needs the setup late restarts into it (setupRefresh flag)
    Preferences refreshPrefs;
    bool asked = false;
    if (refreshPrefs.begin("ultimatrix", false)) {
      asked = refreshPrefs.getBool("setupWifi", false);
      setupRefreshRestart = refreshPrefs.getBool("setupRefresh", false);
      if (setupRefreshRestart) refreshPrefs.remove("setupRefresh");
      refreshPrefs.end();
    }
    setupRefresh = asked || setupRefreshRestart;  // no saved network: the portal comes up late and restarts into it (the saved SSID can't be read before Wi-Fi is initialised)
  }
  HUB75_I2S_CFG panel(layout::width, layout::height, 1);
  panel.gpio.e = 18;
  panel.clkphase = false;
  panel.double_buff = true;
  panel.setPixelColorDepthBits(UM_PIXEL_DEPTH_BITS);
  panel.i2sspeed = HUB75_I2S_CFG::HZ_16M;  // the driver drops low bit planes (lsbMsbTransitionBit) to reach 60 Hz, and each plane kept halves the DMA descriptor memory: 8 bit at 20 MHz needs 4096 descriptors per buffer and ran out of heap
  if (setupRefresh) {
    panel.i2sspeed = HUB75_I2S_CFG::HZ_20M;
    panel.min_refresh_rate = 255;
  }
  if (s.swapBlueGreen) {
    panel.gpio.b1 = 26; panel.gpio.b2 = 12;
    panel.gpio.g1 = 27; panel.gpio.g2 = 13;
  }
  display = new MatrixPanel_I2S_DMA(panel);
  if (!display->begin()) { consoleLog("Display allocation failed\n"); return; }
  displayReady = true;
  boot::setPlot888(plotDithered);
  boot::setLevelMap(bootMap);
  appliedBrightness = panelBrightness(s.brightness);
  display->setBrightness8(appliedBrightness);
  display->setRotation(s.rotation);
  bootAt = millis();
  while (millis() - bootAt < kLogoFadeMs) { drawBoot(boot::Kind::Logo); delay(10); }  // the fade-in is drawn here: setup below is too busy to tick it
  drawBoot(boot::Kind::Logo);
  WiFi.setHostname("ultimatrix");
  WiFi.setAutoReconnect(true);
  // Modem power saving (the default) delays packets by up to a beacon interval; disable it on every (re)connect.
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t) {
    WiFi.setSleep(false);
    wifi_ps_type_t ps; esp_wifi_get_ps(&ps);
    consoleLog("Wi-Fi power save: %d (0 = off)\n", (int)ps);
    mdnsDue = true;
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  wifi.setConfigPortalBlocking(false);
  wifi.setTitle("Ultimatrix");
  wifi.setClass("ultimatrix-portal");
  wifi.setDisableConfigPortal(false);
  wifi.setSaveConfigCallback([]() {
    if (WiFi.status() == WL_CONNECTED) portalCloseAt = millis() + 10000;
  });
  wifi.setCustomHeadElement(R"HTML(<link rel='stylesheet' href='/theme.css'>
<script>addEventListener('DOMContentLoaded',function(){document.querySelectorAll('.msg').forEach(function(m){
if(!/Saving Credentials/.test(m.textContent))return;
function link(u){return '<a href="'+u+'">'+u+'</a> (<a href="#" data-copy="'+u+'">copy</a>)'}
function show(ip){m.innerHTML='<b>Wi-Fi saved.</b><br>Reconnect to your normal Wi-Fi, then open '+link('http://ultimatrix.local/')+(ip?' or '+link('http://'+ip+'/'):'')+'.'}
show('');m.addEventListener('click',function(e){var u=e.target.getAttribute('data-copy');if(!u)return;e.preventDefault();var t=document.createElement('textarea');t.value=u;document.body.appendChild(t);t.select();try{if(document.execCommand('copy'))e.target.textContent='copied'}catch(_){}t.remove()});
var attempts=0,timer=setInterval(function(){if(++attempts>30){clearInterval(timer);return}fetch('/setup-ip',{cache:'no-store'}).then(function(r){return r.text()}).then(function(ip){if(!/^\d+\.\d+\.\d+\.\d+$/.test(ip))return;show(ip);clearInterval(timer)}).catch(function(){})},1000)
})})</script>)HTML");
  wifi.setCustomBodyHeader("<div class='portal-brand'><span class='matrix-mark'>▦</span>Ultimatrix.</div><h2 class='portal-title'>Wi-Fi setup</h2><p class='portal-intro'>Select a network and enter its password.</p>");
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
    wifi.server->on("/theme.css", HTTP_GET, []() { wifi.server->send_P(200, "text/css; charset=utf-8", themeCss); });
    wifi.server->on("/favicon.ico", HTTP_GET, []() { wifi.server->send_P(200, "image/png", (const char *)favicon_32Start, favicon_32End - favicon_32Start); });
  });
  wifi.setAPCallback([](WiFiManager *) {
    IPAddress ip = WiFi.softAPIP();
    char address[16];
    snprintf(address, sizeof(address), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    bootScreen.setAccessPoint(kApName, nullptr, address);
    portalActive = true;
    if (!setupRefresh) {  // the portal came up late (saved network gone): restart into the fast display for the QR code
      Preferences prefs;
      if (prefs.begin("ultimatrix", false)) { prefs.putBool("setupRefresh", true); prefs.end(); }
      consoleLog("[WiFi] Restarting into the fast display mode for the setup QR code\n");
      FirmwareUpdate::confirmBeforeRestart();
      ESP.restart();
    }
  });
  wifi.setWaitCallback(bootTick);  // keep the connecting pulse going while a saved network is tried
  wifi.setConnectTimeout(60);  // seconds, as the core's own wait: without a timeout the wait does not call bootTick, so the pulse froze
  Preferences setupPrefs;
  bool forceSetup = false;
  if (setupPrefs.begin("ultimatrix", false)) {
    forceSetup = setupPrefs.getBool("setupWifi", false);
    if (forceSetup) setupPrefs.putBool("setupWifi", false);
    setupPrefs.end();
  }
  if (forceSetup || setupRefreshRestart) wifi.startConfigPortal(kApName);  // after the restart for the fast display, go straight to the portal
  else wifi.autoConnect(kApName);
  server.on("/", HTTP_GET, settingsPage);
  server.on("/settings", HTTP_POST, saveSettings);
  server.on("/theme.css", HTTP_GET, []() { server.send_P(200, "text/css; charset=utf-8", themeCss); });
  server.on("/settings.js", HTTP_GET, []() { server.send_P(200, "application/javascript; charset=utf-8", settingsJs); });
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
  server.on("/api/settings", HTTP_GET, settingsState);
  server.on("/api/settings", HTTP_POST, saveSettings);
  server.on("/api/catalog", HTTP_GET, selectionCatalog);
  server.on("/api/timezones", HTTP_GET, timezones);
  server.on("/api/wifi/setup", HTTP_POST, openWifiSetup);
  server.on("/api/firmware", HTTP_GET, firmwareStatus);
  server.on("/api/firmware", HTTP_POST, requestFirmwareUpdate);
  server.on("/api/firmware/reboot", HTTP_POST, rebootForFirmwareUpdate);
  server.on("/api/firmware/check", HTTP_POST, checkFirmware);
  if (s.telnet) telnet.begin(telnetCommand, "  update  download signed application update\r\n  update-status  show update progress\r\n" UM_TELNET_HELP);
  backgroundWork() = background;
}
#ifdef UM_FOOTBALL_ONLY
  server.on("/api/teams", HTTP_GET, footballTeams);
#endif

void loop() {
  background();
  if (!displayReady) { delay(50); return; }
  uint32_t now = millis();
  if (!face && !portalActive && WiFi.status() == WL_CONNECTED) {
    auto &s = *Settings::getInstance();
    String zone;  // unless a zone was typed in on the settings page, the network's location decides; the page shows the result
    if (!s.manualTimezone && ultime::detectZone(zone) && zone != s.timezone) { s.timezone = zone; s.save(); }
    clockTime.begin(s.timezone.c_str(), s.use24h, s.ntpServer.c_str(), "");
    face = new Clockface(display);
    face->setup(&clockTime);
    consoleLog("Ultimatrix %s settings: http://%s/ (http://ultimatrix.local/)\n", UM_MODE_NAME, WiFi.localIP().toString().c_str());
  }
  if ((int32_t)(now - nextFrame) >= 0) {
    uint8_t brightness = displayControl.update(*Settings::getInstance(), now, ultime::timeStatus() == timeSet,
        clockTime.getHour24() * 60 + clockTime.getMinute(), !bootDone || portalActive);
    uint8_t panel = panelBrightness(brightness);
    if (panel != appliedBrightness) { display->setBrightness8(panel); appliedBrightness = panel; }
    bootTick();  // before the check below: it stamps connectedAt on the connect
    if (face && !portalActive && !bootDone && syncedAt && bootHoldOver(now)) { bootDone = true; Serial.printf("[Boot] done at %u ms\n", (unsigned)now); }
    if (bootDone) {  // keeps drawing during an update: the feeds are parked, the face shows its last snapshot
      if (displayControl.nightActive() && !face->handlesNight()) {
        nightClock.draw(*display, clockTime.getHour(), clockTime.getMinute(), clockTime.is24hFormat());
        nightShown = true;
      } else {
        if (nightShown) { nightShown = false; nightClock.reset(); face->invalidate(); }
        face->setNight(displayControl.nightActive());
        face->update();
      }
    }
    nextFrame = millis() + layout::frameMs;
  }
  delay(2);
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
