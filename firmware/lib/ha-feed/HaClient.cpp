// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "HaClient.h"
#include "HaPrices.h"
#include <algorithm>
#include <math.h>
#include <time.h>

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_random.h>
#include <memory>
#include <new>
#include "Console.h"
#include "HaMedia.h"
#include "HaDevices.h"

#include <NetworkBudget.h>

namespace {

constexpr size_t kTemplateMax = 9000;  // the template and the message around it, on the heap only while they are sent
constexpr size_t kMessageMax = 4096;    // a received message. The Energy dashboard's answer is longer but only its first part is read; history is asked for one statistic at a time
constexpr uint32_t kPingEveryMs = 25000, kSilenceMs = 75000, kStatisticsEveryMs = 60000, kHistoryEveryMs = 600000;

// The id a message of ours starts with: {"id":N,...
int idOf(const char *message) { return strncmp(message, "{\"id\":", 6) == 0 ? atoi(message + 6) : 0; }

// One WebSocket over a connected client: masked text out, whole messages in. Control frames are handled underneath.
struct Socket {
  WiFiClient &client;
  uint32_t lastData = 0;
  const char *why = "";  // what ended the last read, for the log

  explicit Socket(WiFiClient &c) : client(c) { lastData = millis(); }

  bool send(uint8_t opcode, const uint8_t *data, size_t length) {
    std::unique_ptr<uint8_t[]> frame(new (std::nothrow) uint8_t[length + 14]);
    if (!frame) return false;
    uint32_t mask = esp_random();
    size_t at = 0;
    frame[at++] = 0x80 | opcode;
    if (length < 126) {
      frame[at++] = 0x80 | length;
    } else {
      frame[at++] = 0x80 | 126;
      frame[at++] = length >> 8;
      frame[at++] = length & 0xFF;
    }
    memcpy(frame.get() + at, &mask, 4);
    at += 4;
    for (size_t i = 0; i < length; i++) frame[at + i] = data[i] ^ ((uint8_t *)&mask)[i % 4];
    return client.write(frame.get(), at + length) == at + length;
  }
  bool text(const char *message, size_t length) { return send(0x1, (const uint8_t *)message, length); }
  bool text(const char *message) { return text(message, strlen(message)); }

  // One byte, waiting up to waitMs for it; -1 on timeout or a closed connection
  int byte(uint32_t waitMs) {
    uint32_t start = millis();
    while (!client.available()) {
      if (!client.connected() || feedsHalted()) return -1;
      if (millis() - start >= waitMs) return -1;
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    int b = client.read();
    if (b >= 0) lastData = millis();
    return b;
  }
  int fail(const char *reason) { why = reason; return -1; }

  // The next text message into out (null terminated). >0 its length, 0 nothing yet, -1 connection lost, -2 message too big (skipped)
  int message(char *out, size_t capacity, uint32_t waitMs) {
    size_t used = 0;
    bool overflow = false;
    int first = byte(waitMs);
    if (first < 0) return client.connected() && !feedsHalted() ? 0 : fail("the server closed the connection");
    int b0 = first;
    for (;;) {
      int b1 = byte(5000);
      if (b1 < 0) return fail("a message stopped half way");
      uint8_t opcode = b0 & 0x0F;
      bool final = b0 & 0x80;
      uint64_t length = b1 & 0x7F;
      int extra = length == 126 ? 2 : length == 127 ? 8 : 0;
      if (extra) length = 0;  // the real length follows in 2 or 8 bytes
      for (int i = 0; i < extra; i++) {
        int b = byte(5000);
        if (b < 0) return fail("a message stopped half way");
        length = length << 8 | b;
      }
      if (b1 & 0x80) for (int i = 0; i < 4; i++) if (byte(5000) < 0) return fail("a message stopped half way");  // servers do not mask
      if (opcode == 0x8) return fail("the server sent a close frame");
      uint8_t control[125];
      bool isControl = opcode >= 0x8;
      for (uint64_t i = 0; i < length; i++) {
        int b = byte(5000);
        if (b < 0) return fail("a message stopped half way");
        if (isControl) { if (i < sizeof(control)) control[i] = b; }
        else if (used + 1 < capacity) out[used++] = b;
        else overflow = true;
      }
      if (opcode == 0x9) send(0xA, control, length > sizeof(control) ? sizeof(control) : length);
      if (isControl && used == 0) return 0;  // a ping between messages: answered, nothing else to say (waiting here for the next frame dropped the connection)
      if (!isControl && final) {
        out[used] = 0;
        return overflow ? -2 : (int)used;
      }
      // the next frame of a fragmented message, or after a control frame inside one
      b0 = byte(5000);
      if (b0 < 0) return fail("a message stopped half way");
    }
  }
};

// The text between `key` and the next quote, for the string values of our own short messages
bool stringAfter(const char *message, const char *key, char *out, size_t size) {
  const char *at = strstr(message, key);
  if (!at) return false;
  at += strlen(key);
  size_t n = 0;
  while (*at && *at != '"' && n + 1 < size) out[n++] = *at++;
  out[n] = 0;
  return true;
}

// Whether the message's own "type" is `name`. Home Assistant writes it before any payload, so the first one is the message's:
// searching the whole text would also match a sensor name or template result that happens to contain the same words.
bool typeIs(const char *message, const char *name) {
  const char *at = strstr(message, "\"type\":\"");
  if (!at) return false;
  at += 8;
  const size_t length = strlen(name);
  return !strncmp(at, name, length) && at[length] == '"';
}

}  // namespace

HaClient &HaClient::instance() {
  static HaClient *client = new HaClient;  // on the heap: 2.6 KB off the static RAM budget, and not reserved by an update-only boot
  return *client;
}

void HaClient::begin() {
  static uint32_t retryAt = 0;
  if (_started || (int32_t)(millis() - retryAt) < 0) return;
  _started = true;
  if (createFeedTask(task, "homeassistant", 8192, this, 1, nullptr, 0) != pdPASS) {
    _started = false;
    retryAt = millis() + 5000;  // poll() asks every frame: try again in a few seconds
    consoleLog("[HA] could not start the connection task\n");
  }
}

void HaClient::task(void *self) { static_cast<HaClient *>(self)->run(); }

void HaClient::set(State state, const char *text) {
  std::lock_guard<std::mutex> guard(_lock);
  _state = state;
  strlcpy(_status, text, sizeof(_status));
}

bool HaClient::energyIds(char (*out)[ha::kEntity]) {
  std::lock_guard<std::mutex> guard(_lock);
  if (!_resolvedOk) return false;
  memcpy(out, _resolved, sizeof(_resolved));
  return true;
}

int HaClient::dayKey(time_t when) {
  struct tm local;
  localtime_r(&when, &local);
  return (local.tm_year + 1900) * 400 + local.tm_yday;
}

void HaClient::outlook(Outlook &out) {
  std::lock_guard<std::mutex> guard(_lock);
  out = _outlook;
}

void HaClient::history(History &out) {
  std::lock_guard<std::mutex> guard(_lock);
  out = _history;
}

bool HaClient::snapshot(ha::Snapshot &out) {
  std::lock_guard<std::mutex> guard(_lock);
  if (!_valid) return false;
  out = _snapshot;
  return true;
}

void HaClient::requestPrefs() {
  { std::lock_guard<std::mutex> guard(_lock); _prefs.state = EnergyPrefs::PENDING; _prefs.note[0] = 0; }
  _wantPrefs = true;
}

void HaClient::prefs(EnergyPrefs &out) {
  std::lock_guard<std::mutex> guard(_lock);
  out = _prefs;
}

void HaClient::status(char *out, size_t size) {
  std::lock_guard<std::mutex> guard(_lock);
  strlcpy(out, _status, size);
}

void HaClient::run() {
  uint32_t pause = 3000, seenVersion = 0;
  bool rejected = false;  // the token was refused: wait for the settings to change instead of trying again
  for (;;) {
    const uint32_t version = ha::version();
    if (version != seenVersion) { seenVersion = version; rejected = false; pause = 3000; }
    if (!ha::configured()) {
      set(IDLE, "Not set up");
    } else if (WiFi.status() != WL_CONNECTED) {
      set(IDLE, "Waiting for Wi-Fi");
    } else if (feedsHalted()) {
      set(IDLE, firmwareUpdating ? "Paused for an update" : "Paused for a picture or WLED");
      _started = false;  // the task parks and is deleted by reapFeedTasks(), which gives back its 8 KB stack; Clockface::poll() calls begin() again when the hold is over
      stopFeedTask();
    } else if (!rejected) {
      const bool ended = session(version);
      ha::media::clear();  // the Now playing page must not outlive the connection that said something was playing
      if (ended) pause = 3000;
      else {
        bool changed = ha::version() != version;
        { std::lock_guard<std::mutex> guard(_lock); _valid = false; }
        if (_state == FAILED && strstr(_status, "token")) rejected = true;
        if (changed) { pause = 500; }
      }
      if (!rejected) {
        vTaskDelay(pdMS_TO_TICKS(pause));
        pause = pause >= 60000 ? 60000 : pause * 2;
        continue;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// One connection from the handshake until it fails, the settings change or an update starts. True when it reached LIVE.
bool HaClient::session(uint32_t version) {
  std::unique_ptr<ha::Config> config(new (std::nothrow) ha::Config);
  if (!config) { set(FAILED, "Out of memory"); return false; }
  *config = ha::config();

  set(CONNECTING, "Connecting");
  std::unique_ptr<WiFiClientSecure> secure;
  WiFiClient plain;
  WiFiClient *client = &plain;
  if (config->tls) {
    secure.reset(new (std::nothrow) WiFiClientSecure);
    if (!secure) { set(FAILED, "Out of memory"); return false; }
    secure->setInsecure();
    client = secure.get();
  }
  client->setTimeout(8000);
  if (!client->connect(config->host, config->port ? config->port : 8123)) {
    char reason[96] = "";
    int code = secure ? secure->lastError(reason, sizeof(reason)) : 0;  // the TLS library's own words
    consoleLog("[HA] cannot connect to %s:%u over %s: %s (%d)\n", config->host, (unsigned)config->port, config->tls ? "https" : "http", reason[0] ? reason : "no answer", code);
    if (!secure) set(FAILED, "Cannot reach Home Assistant");
    else if (code == -0x7780) set(FAILED, "The server refused the TLS handshake");  // a fatal alert from the peer
    else if (code == -0x7200) set(FAILED, "That port does not speak https");
    else set(FAILED, "Cannot open a secure connection");
    return false;
  }

  char request[288];  // about 155 bytes of text, a host of up to 63 and a port
  snprintf(request, sizeof(request),
           "GET /api/websocket HTTP/1.1\r\nHost: %s:%u\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n",
           config->host, (unsigned)config->port);
  client->print(request);
  {
    // Status line, then headers up to the empty line. A line is read to its end whatever its length (the part
    // that does not fit is dropped), so a long header is not taken for the empty line that ends them.
    char line[96];
    bool upgraded = false, first = true;
    for (;;) {
      size_t stored = 0;
      bool ended = false;
      for (;;) {
        // One byte at a time with its own wait: readBytes() on the TLS client gives up as soon as nothing is pending.
        int got = -1;
        for (uint32_t waited = millis(); got < 0 && millis() - waited < 8000;) {
          if (client->available()) got = client->read();
          else if (!client->connected()) break;
          else delay(2);
        }
        if (got < 0) break;
        if (got == '\n') { ended = true; break; }
        if (stored < sizeof(line) - 1) line[stored++] = (char)got;
      }
      if (!ended) break;  // closed or timed out
      line[stored] = 0;
      if (first) { upgraded = strstr(line, " 101") != nullptr; first = false; if (!upgraded) break; continue; }
      if (stored == 0 || (stored == 1 && line[0] == '\r')) break;  // the empty line
    }
    if (!upgraded) { set(FAILED, "Not a Home Assistant WebSocket"); client->stop(); return false; }
  }

  Socket socket(*client);
  std::unique_ptr<char[]> message(new (std::nothrow) char[kMessageMax]);
  if (!message) { set(FAILED, "Out of memory"); client->stop(); return false; }

  bool live = false;
  int heard = 0;
  const uint32_t began = millis();
  uint32_t lastPing = millis();
  uint32_t lastId = 100, prefsId = 0, statsId = 0, histFirst = 0, lastStatistics = 0, lastHistory = 0;
  constexpr int kHistoryAsks = 8;
  constexpr uint8_t kGridOutField = 100;  // a history slot that holds a grid export meter (kWh per hour), not one of the power sensors
  struct HistorySlot { char id[ha::kEntity]; uint8_t field; uint32_t mask; float v[ha::kHistoryBars]; };
  struct HistoryAsk { HistorySlot slot[kHistoryAsks]; int count; uint32_t hour; };
  std::unique_ptr<HistoryAsk> hist;  // the statistics asked for, one request each, until all have answered
  int histCount = 0, histGot = 0;
  // The expected usage: the hourly means of the same weekday in each of the last weeks, one week at a time
  constexpr int kMaxWeeks = 26;
  struct WeekAsk { HistorySlot slot[kHistoryAsks]; int count, week, weeks, good, got, shift; uint32_t first, last; int day; float use[kMaxWeeks][ha::kHistoryBars], out[kMaxWeeks][ha::kHistoryBars]; uint32_t mask[kMaxWeeks], outMask[kMaxWeeks]; int age[kMaxWeeks]; };
  std::unique_ptr<WeekAsk> weekAsk;
  int predictedDay = -1, predictedNextDay = -1, forecastDay = -1;
  uint32_t lastPredict = 0, lastForecast = 0, forecastId = 0, lastPeak = 0, peakId = 0;
  int peakDay = -1;
  // Last year's daily usage, to see how the usage moves from the last weeks to the coming ones at this time of year
  uint32_t yearId = 0, yearFirst = 0;
  bool yearDue = false;
  int yearDay = -1, yearCount = 0;
  char yearIds[kHistoryAsks][ha::kEntity] = {};
  char peakIds[kHistoryAsks][ha::kEntity] = {};
  int peakCount = 0;
  // All meters of a field added up: use (the house, or what grid, solar and battery deliver together) and solar
  auto combine = [&](const HistorySlot *slots, int count, float *use, float *solar, float *battery, float *grid, uint32_t &useMask, uint32_t &solarMask, uint32_t &batteryMask, uint32_t &gridMask) {
    const int gridSign = config->gridImportPositive ? 1 : -1, batterySign = config->batteryDischargePositive ? 1 : -1;
    auto add = [&](int entity, float sign, float *into, uint32_t &mask) {  // all meters of a field, added up
      for (int k = 0; k < count; k++) {
        const HistorySlot &one = slots[k];
        if (one.field != entity) continue;
        for (int i = 0; i < ha::kHistoryBars; i++) if (one.mask >> i & 1) { into[i] += sign * one.v[i]; mask |= 1u << i; }
      }
    };
    add(ha::E_SOLAR, 1, solar, solarMask);
    add(ha::E_BATTERY, batterySign, battery, batteryMask);  // positive: the battery delivers
    add(ha::E_GRID, gridSign, grid, gridMask);  // positive: importing
    if (config->energy[ha::E_USAGE][0]) add(ha::E_USAGE, 1, use, useMask);
    else {  // no house sensor: what the grid, the solar panels and the battery deliver together
      for (int i = 0; i < ha::kHistoryBars; i++) {
        if (!(gridMask >> i & 1)) continue;
        use[i] = fmaxf(0, grid[i] + (solarMask >> i & 1 ? fmaxf(0, solar[i]) : 0) + (batteryMask >> i & 1 ? battery[i] : 0));
        useMask |= 1u << i;
      }
    }
  };
  // Asks for the hourly power means of one earlier day (the same weekday, week + 1 weeks back), every statistic separately
  auto askWeek = [&](WeekAsk &w) {
    const time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    t.tm_mday -= 7 * (w.week + 1) - w.shift;  // shift 1: the weekday after it, which is tomorrow's weekday
    t.tm_hour = t.tm_min = t.tm_sec = 0;
    t.tm_isdst = -1;
    const time_t from = mktime(&t);
    struct tm next = t;
    next.tm_mday += 1;
    next.tm_isdst = -1;
    const time_t to = mktime(&next);
    struct tm a, b;
    gmtime_r(&from, &a);
    gmtime_r(&to, &b);
    w.last = (uint32_t)(to - 3600);
    w.got = 0;
    w.first = lastId + 1;
    std::unique_ptr<char[]> text(new (std::nothrow) char[420]);
    if (!text) return;
    for (int k = 0; k < w.count; k++) {
      const bool meter = w.slot[k].field == kGridOutField;  // the to-grid meter: its kWh per hour, not a power mean
      snprintf(text.get(), 420, "{\"id\":%u,\"type\":\"recorder/statistics_during_period\",\"start_time\":\"%04d-%02d-%02dT%02d:%02d:00+00:00\",\"end_time\":\"%04d-%02d-%02dT%02d:%02d:00+00:00\",\"period\":\"hour\",\"types\":[\"%s\"],\"units\":{\"%s\":\"%s\"},\"statistic_ids\":[\"%s\"]}",
               (unsigned)++lastId, a.tm_year + 1900, a.tm_mon + 1, a.tm_mday, a.tm_hour, a.tm_min, b.tm_year + 1900, b.tm_mon + 1, b.tm_mday, b.tm_hour, b.tm_min, meter ? "change" : "mean", meter ? "energy" : "power", meter ? "kWh" : "kW", w.slot[k].id);
      socket.text(text.get());
    }
  };
  bool historyAsked = false;  // Home Assistant wants every id higher than the one before
  bool statisticsAsked = false;
  char statisticsMidnight[sizeof(ha::Snapshot::midnight)] = "";
  bool resolving = false;
  uint32_t templateId = 0, mediaId = 0, devicesId = 0, pricesId = 0;
  char gridTo[ha::kEntity] = "";  // meters to subtract from the grid's kWh
  auto subscribe = [&]() -> bool {
    set(SUBSCRIBING, "Subscribing");
    {
      std::lock_guard<std::mutex> guard(_lock);
      memcpy(_resolved, config->energy, sizeof(_resolved));
      _resolvedOk = true;
      _resolvedVersion++;
    }
    std::unique_ptr<char[]> body(new (std::nothrow) char[kTemplateMax + 120]);
    if (!body) { set(FAILED, "Out of memory"); return false; }
    templateId = ++lastId;  // Home Assistant wants every id higher than the one before, so this is not always 1
    char head[80];
    size_t at = snprintf(head, sizeof(head), "{\"id\":%u,\"type\":\"render_template\",\"report_errors\":true,\"template\":\"", (unsigned)templateId);
    memcpy(body.get(), head, at);
    size_t built = ha::buildTemplate(*config, body.get() + at, kTemplateMax);
    if (!built) { set(FAILED, "Template too long"); return false; }
    at += built;
    memcpy(body.get() + at, "\"}", 3);
    socket.text(body.get(), at + 2);
    // The Now playing page has a template of its own: a player starting or a new track arrives at once, and without the page nothing is asked
    mediaId = 0;
    if (config->pages & (ha::PAGE_MEDIA | ha::PAGE_ART_BACKDROP)) {
      mediaId = ++lastId;
      at = snprintf(head, sizeof(head), "{\"id\":%u,\"type\":\"render_template\",\"template\":\"", (unsigned)mediaId);
      memcpy(body.get(), head, at);
      built = ha::media::buildTemplate(*config, body.get() + at, kTemplateMax);
      if (built) { at += built; memcpy(body.get() + at, "\"}", 3); socket.text(body.get(), at + 2); }
      else { mediaId = 0; ha::media::clear(); }
    }
    devicesId = 0;
    if (config->pages & ha::PAGE_DEVICES) {
      devicesId = ++lastId;
      at = snprintf(head, sizeof(head), "{\"id\":%u,\"type\":\"render_template\",\"template\":\"", (unsigned)devicesId);
      memcpy(body.get(), head, at);
      built = ha::devices::buildTemplate(body.get() + at, kTemplateMax);
      if (built) { at += built; memcpy(body.get() + at, "\"}", 3); socket.text(body.get(), at + 2); }
      else devicesId = 0;
    }
    // The tariff of the Energy dashboard: its hourly prices are in the sensor's attributes
    pricesId = 0;
    const char *priceEntity = ha::autoPriceEntity();
    if ((config->pages & ha::PAGE_ENERGY) && priceEntity[0]) {
      pricesId = ++lastId;
      at = snprintf(head, sizeof(head), "{\"id\":%u,\"type\":\"render_template\",\"template\":\"", (unsigned)pricesId);
      memcpy(body.get(), head, at);
      built = ha::prices::buildTemplate(priceEntity, body.get() + at, kTemplateMax);
      if (built) { at += built; memcpy(body.get() + at, "\"}", 3); socket.text(body.get(), at + 2); }
      else pricesId = 0;
    }
    if (!pricesId) ha::prices::clear();
    return true;
  };
  set(AUTHENTICATING, "Signing in");
  for (;;) {
    if (feedsHalted() || ha::version() != version) break;
    int n = socket.message(message.get(), kMessageMax, 1000);
    if (n < 0 && n != -2) {
      consoleLog("[HA] connection ended after %u s (%s): %s\n", (unsigned)((millis() - began) / 1000), live ? "was live" : "never live", socket.why);
      if (_state != FAILED) set(FAILED, "Connection lost");
      break;
    }
    if (n == -2 && prefsId && (uint32_t)idOf(message.get()) == prefsId) n = (int)strlen(message.get());  // the Energy dashboard's answer is mostly its device list; the sources that are read come first
    if (n == 0 || n == -2) {
      if (millis() - socket.lastData > kSilenceMs) { set(FAILED, "Connection lost"); break; }
      if (live && _wantPrefs.exchange(false)) {
        char ask[56];
        prefsId = ++lastId;
        snprintf(ask, sizeof(ask), "{\"id\":%u,\"type\":\"energy/get_prefs\"}", (unsigned)prefsId);
        socket.text(ask);
      }
      if (live && (!statisticsAsked || millis() - lastStatistics >= kStatisticsEveryMs)) {
        // Today's kWh from the recorder: the Energy dashboard's meters count for ever, so what is shown is their change since midnight
        char midnight[sizeof(ha::Snapshot::midnight)];
        { std::lock_guard<std::mutex> guard(_lock); strlcpy(midnight, _snapshot.midnight, sizeof(midnight)); }
        std::unique_ptr<char[]> ask(new (std::nothrow) char[640]);
        int ids = 0;
        if (ask && midnight[0]) {
          statsId = ++lastId;
          int at = snprintf(ask.get(), 640, "{\"id\":%u,\"type\":\"recorder/statistics_during_period\",\"start_time\":\"%s\",\"period\":\"day\",\"types\":[\"change\"],\"units\":{\"energy\":\"kWh\"},\"statistic_ids\":[", (unsigned)statsId, midnight);
          for (int k = 0; k < 4; k++) {
            // A field can hold several meters: each is asked for, and the answers are added up
            char id[ha::kEntity];
            for (const char *list = config->energy[ha::E_USAGE_KWH + k]; ha::nextListed(list, id, sizeof(id)) && at < 540;)
              at += snprintf(ask.get() + at, 640 - at, "%s\"%s\"", ids++ ? "," : "", id);
          }
          char id[ha::kEntity];
          for (const char *list = gridTo; ha::nextListed(list, id, sizeof(id)) && at < 540;)
            at += snprintf(ask.get() + at, 640 - at, "%s\"%s\"", ids++ ? "," : "", id);
          if (ids) { snprintf(ask.get() + at, 640 - at, "]}"); socket.text(ask.get()); }
        }
        statisticsAsked = true;
        lastStatistics = millis();
      }
      if (live && time(nullptr) > 1700000000 && (!historyAsked || millis() - lastHistory >= kHistoryEveryMs)) {
        // The last hours of power, for the bars: the means of the recorder's long-term statistics. One request per statistic: an answer for all of them would not fit the message buffer
        std::unique_ptr<char[]> ask(new (std::nothrow) char[360]);
        std::unique_ptr<HistoryAsk> next(new (std::nothrow) HistoryAsk{});
        hist.reset();
        histCount = histGot = 0;
        if (ask && next) {
          const time_t from = (time(nullptr) / 3600 - (ha::kHistoryBars - 1)) * 3600;
          struct tm utc;
          gmtime_r(&from, &utc);
          next->hour = (uint32_t)(time(nullptr) / 3600 * 3600);
          histFirst = lastId + 1;
          const int power[4] = {ha::E_USAGE, ha::E_SOLAR, ha::E_GRID, ha::E_BATTERY};
          for (int e : power) {
            char id[ha::kEntity];
            for (const char *list = config->energy[e]; ha::nextListed(list, id, sizeof(id)) && next->count < kHistoryAsks;) {
              HistorySlot &slot = next->slot[next->count++];
              strlcpy(slot.id, id, sizeof(slot.id));
              slot.field = (uint8_t)e;
              snprintf(ask.get(), 360, "{\"id\":%u,\"type\":\"recorder/statistics_during_period\",\"start_time\":\"%04d-%02d-%02dT%02d:00:00+00:00\",\"period\":\"hour\",\"types\":[\"mean\"],\"units\":{\"power\":\"kW\"},\"statistic_ids\":[\"%s\"]}",
                       (unsigned)++lastId, utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, id);
              socket.text(ask.get());
            }
          }
          // What was sent to the grid, from the Energy dashboard's own meters: an hour of both exporting and importing does not cancel out there
          char id[ha::kEntity];
          for (const char *list = gridTo; ha::nextListed(list, id, sizeof(id)) && next->count < kHistoryAsks;) {
            HistorySlot &slot = next->slot[next->count++];
            strlcpy(slot.id, id, sizeof(slot.id));
            slot.field = kGridOutField;
            snprintf(ask.get(), 360, "{\"id\":%u,\"type\":\"recorder/statistics_during_period\",\"start_time\":\"%04d-%02d-%02dT%02d:00:00+00:00\",\"period\":\"hour\",\"types\":[\"change\"],\"units\":{\"energy\":\"kWh\"},\"statistic_ids\":[\"%s\"]}",
                     (unsigned)++lastId, utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, id);
            socket.text(ask.get());
          }
          histCount = next->count;
          if (histCount) hist = std::move(next);
        }
        historyAsked = true;
        lastHistory = millis();
      }
      // The expected usage of today: the same weekday of the last weeks, once a day, one week at a time
      if (live && !hist && !weekAsk && time(nullptr) > 1700000000 && (!lastPredict || millis() - lastPredict >= 600000) &&
          (config->energy[ha::E_USAGE][0] || config->energy[ha::E_GRID][0]) && (predictedDay != dayKey(time(nullptr)) || predictedNextDay != dayKey(time(nullptr)))) {
        lastPredict = millis();
        std::unique_ptr<WeekAsk> next(new (std::nothrow) WeekAsk{});
        if (next) {
          next->weeks = std::min(ha::predictWeeks(), kMaxWeeks);
          next->day = dayKey(time(nullptr));
          next->shift = predictedDay == next->day ? 1 : 0;  // today first, then tomorrow
          const int power[4] = {ha::E_USAGE, ha::E_SOLAR, ha::E_GRID, ha::E_BATTERY};
          for (int e : power) {
            char id[ha::kEntity];
            for (const char *list = config->energy[e]; ha::nextListed(list, id, sizeof(id)) && next->count < kHistoryAsks;) {
              strlcpy(next->slot[next->count].id, id, sizeof(id));
              next->slot[next->count++].field = (uint8_t)e;
            }
          }
          char meterId[ha::kEntity];
          for (const char *list = gridTo; ha::nextListed(list, meterId, sizeof(meterId)) && next->count < kHistoryAsks;) {  // what was sent to the grid, as the Energy dashboard counts it
            strlcpy(next->slot[next->count].id, meterId, sizeof(meterId));
            next->slot[next->count++].field = kGridOutField;
          }
          if (next->count && next->weeks > 0) { weekAsk = std::move(next); askWeek(*weekAsk); }
        }
      }
      // The highest usage of the last month or so, for the colour of the house row: the recorder's monthly maximum of the house sensor
      if (live && !peakId && time(nullptr) > 1700000000 && config->energy[ha::E_USAGE][0] && peakDay != dayKey(time(nullptr)) && (!lastPeak || millis() - lastPeak >= 600000)) {
        lastPeak = millis();
        std::unique_ptr<char[]> ask(new (std::nothrow) char[900]);
        if (ask) {
          const time_t from = time(nullptr) - 31 * 86400;
          struct tm utc;
          gmtime_r(&from, &utc);
          int at = snprintf(ask.get(), 900, "{\"id\":%u,\"type\":\"recorder/statistics_during_period\",\"start_time\":\"%04d-%02d-%02dT00:00:00+00:00\",\"period\":\"month\",\"types\":[\"max\"],\"units\":{\"power\":\"kW\"},\"statistic_ids\":[",
                            (unsigned)(peakId = ++lastId), utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
          peakCount = 0;
          char id[ha::kEntity];
          for (const char *list = config->energy[ha::E_USAGE]; ha::nextListed(list, id, sizeof(id)) && peakCount < kHistoryAsks && at < 800;) {
            strlcpy(peakIds[peakCount], id, ha::kEntity);
            at += snprintf(ask.get() + at, 900 - at, "%s\"%s\"", peakCount++ ? "," : "", id);
          }
          snprintf(ask.get() + at, 900 - at, "]}");
          if (peakCount) socket.text(ask.get()); else peakId = 0;
        } else {
          peakId = 0;
        }
      }
      // Last year's daily usage from 4 weeks before this day to 2 weeks after it, once the prediction is in
      if (live && yearDue && !yearId && !weekAsk) {
        yearDue = false;
        std::unique_ptr<char[]> ask(new (std::nothrow) char[900]);
        if (ask) {
          const time_t from = time(nullptr) - (365 + 28) * 86400, to = from + 42 * 86400;
          struct tm a, b;
          gmtime_r(&from, &a);
          gmtime_r(&to, &b);
          yearFirst = (uint32_t)(from / 86400 * 86400);
          yearDay = dayKey(time(nullptr));
          int at = snprintf(ask.get(), 900, "{\"id\":%u,\"type\":\"recorder/statistics_during_period\",\"start_time\":\"%04d-%02d-%02dT00:00:00+00:00\",\"end_time\":\"%04d-%02d-%02dT00:00:00+00:00\",\"period\":\"day\",\"types\":[\"mean\"],\"units\":{\"power\":\"kW\"},\"statistic_ids\":[",
                            (unsigned)(yearId = ++lastId), a.tm_year + 1900, a.tm_mon + 1, a.tm_mday, b.tm_year + 1900, b.tm_mon + 1, b.tm_mday);
          yearCount = 0;
          char id[ha::kEntity];
          for (const char *list = config->energy[ha::E_USAGE]; ha::nextListed(list, id, sizeof(id)) && yearCount < kHistoryAsks && at < 800;) {
            strlcpy(yearIds[yearCount], id, ha::kEntity);
            at += snprintf(ask.get() + at, 900 - at, "%s\"%s\"", yearCount++ ? "," : "", id);
          }
          snprintf(ask.get() + at, 900 - at, "]}");
          if (yearCount) socket.text(ask.get()); else yearId = 0;
        }
      }
      // The solar forecast of the Energy dashboard, when there are solar panels
      if (live && config->energy[ha::E_SOLAR][0] && time(nullptr) > 1700000000 && (!lastForecast || millis() - lastForecast >= 1800000) &&
          (forecastDay != dayKey(time(nullptr)) || millis() - lastForecast >= 10800000)) {
        char ask[64];
        snprintf(ask, sizeof(ask), "{\"id\":%u,\"type\":\"energy/solar_forecast\"}", (unsigned)(forecastId = ++lastId));
        socket.text(ask);
        lastForecast = millis();
      }
      if (millis() - lastPing >= kPingEveryMs && live) {
        char ping[48];
        snprintf(ping, sizeof(ping), "{\"id\":%u,\"type\":\"ping\"}", (unsigned)++lastId);
        socket.text(ping);
        lastPing = millis();
      }
      continue;
    }
    const char *text = message.get();
    if (heard < 6) { heard++; char head[72]; strlcpy(head, text, sizeof(head)); consoleLog("[HA] received %d bytes: %s\n", n, head); }  // Home Assistant's own words, never what we send
    if (typeIs(text, "auth_required")) {
      int length = snprintf(nullptr, 0, "{\"type\":\"auth\",\"access_token\":\"%s\"}", config->token);
      std::unique_ptr<char[]> auth(new (std::nothrow) char[length + 1]);
      if (!auth) { set(FAILED, "Out of memory"); break; }
      snprintf(auth.get(), length + 1, "{\"type\":\"auth\",\"access_token\":\"%s\"}", config->token);
      socket.text(auth.get(), length);
    } else if (typeIs(text, "auth_invalid")) {
      set(FAILED, "Home Assistant refused the token");
      break;
    } else if (typeIs(text, "auth_ok")) {
      // Energy fields set to "auto" are the sensors of the Energy dashboard: ask for them first, then subscribe
      bool wantsAuto = false;
      for (int i = 0; i < ha::E_COUNT; i++) wantsAuto |= !strcmp(config->energy[i], "auto");
      for (int i = 0; i < ha::kHomeTiles; i++) wantsAuto |= ha::tileSlot(*config, i).type == ha::TILE_PRICE && ha::tileSlot(*config, i).autoMode;
      if (wantsAuto) {
        char ask[56];
        prefsId = ++lastId;
        resolving = true;
        snprintf(ask, sizeof(ask), "{\"id\":%u,\"type\":\"energy/get_prefs\"}", (unsigned)prefsId);
        socket.text(ask);
      } else if (!subscribe()) break;
    } else if (prefsId && (uint32_t)idOf(text) == prefsId) {  // energy/get_prefs
      EnergyPrefs found;
      if (strstr(text, "\"success\":false")) {
        found.state = EnergyPrefs::FAILED_ASKING;
        char reason[32];
        if (strstr(text, "unauthorized") || strstr(text, "admin")) strlcpy(found.note, "Needs a token of an administrator", sizeof(found.note));
        else if (stringAfter(text, "\"message\":\"", reason, sizeof(reason))) snprintf(found.note, sizeof(found.note), "Refused: %.30s", reason);
        else strlcpy(found.note, "Home Assistant refused", sizeof(found.note));
      } else if (ha::parseEnergyPrefs(text, found.id, found.gridTo, found.price)) {
        found.state = EnergyPrefs::READY;
      } else {
        found.state = EnergyPrefs::FAILED_ASKING;
        strlcpy(found.note, "No sensors in the Energy dashboard yet", sizeof(found.note));
      }
      { std::lock_guard<std::mutex> guard(_lock); _prefs = found; }
      if (resolving) {
        resolving = false;
        ha::setAutoPrice(found.state == EnergyPrefs::READY ? found.price : "");
        if (found.state == EnergyPrefs::READY && !strcmp(config->energy[ha::E_GRID_KWH], "auto")) strlcpy(gridTo, found.gridTo, sizeof(gridTo));
        for (int i = 0; i < ha::E_COUNT; i++)
          if (!strcmp(config->energy[i], "auto")) strlcpy(config->energy[i], found.state == EnergyPrefs::READY ? found.id[i] : "", ha::kEntity);
        if (!subscribe()) break;
      }
    } else if (hist && (uint32_t)idOf(text) >= histFirst && (uint32_t)idOf(text) < histFirst + (uint32_t)histCount) {  // hourly power means of one statistic
      HistorySlot &slot = hist->slot[(uint32_t)idOf(text) - histFirst];
      slot.mask = strstr(text, "\"success\":false") ? 0 : ha::parseHourly(text, slot.id, hist->hour, slot.v, slot.field == kGridOutField ? "change" : "mean");
      if (++histGot >= histCount) {
        const uint32_t hour = hist->hour;
        float use[ha::kHistoryBars] = {}, solar[ha::kHistoryBars] = {}, battery[ha::kHistoryBars] = {}, grid[ha::kHistoryBars] = {};
        uint32_t useMask = 0, solarMask = 0, batteryMask = 0, gridMask = 0;
        combine(hist->slot, hist->count, use, solar, battery, grid, useMask, solarMask, batteryMask, gridMask);
        float gridOut[ha::kHistoryBars] = {};
        uint32_t gridOutMask = 0;
        for (int k = 0; k < hist->count; k++) {
          if (hist->slot[k].field != kGridOutField) continue;
          for (int i = 0; i < ha::kHistoryBars; i++) if (hist->slot[k].mask >> i & 1) { gridOut[i] += fmaxf(0, hist->slot[k].v[i]); gridOutMask |= 1u << i; }
        }
        hist.reset();
        histCount = histGot = 0;
        std::lock_guard<std::mutex> guard(_lock);
        memcpy(_history.use, use, sizeof(use));
        memcpy(_history.solar, solar, sizeof(solar));
        memcpy(_history.battery, battery, sizeof(battery));
        memcpy(_history.grid, grid, sizeof(grid));
        memcpy(_history.gridOut, gridOut, sizeof(gridOut));
        _history.gridOutMask = gridOutMask;
        _history.useMask = useMask; _history.solarMask = solarMask; _history.batteryMask = batteryMask; _history.gridMask = gridMask; _history.hour = hour;
        _history.version++;
        _historyVersion = _history.version;
      }
    } else if (weekAsk && (uint32_t)idOf(text) >= weekAsk->first && (uint32_t)idOf(text) < weekAsk->first + (uint32_t)weekAsk->count) {  // one earlier day
      WeekAsk &w = *weekAsk;
      HistorySlot &slot = w.slot[(uint32_t)idOf(text) - w.first];
      slot.mask = strstr(text, "\"success\":false") ? 0 : ha::parseHourly(text, slot.id, w.last, slot.v, slot.field == kGridOutField ? "change" : "mean");
      if (++w.got >= w.count) {
        float use[ha::kHistoryBars] = {}, solar[ha::kHistoryBars] = {}, battery[ha::kHistoryBars] = {}, grid[ha::kHistoryBars] = {};
        uint32_t useMask = 0, solarMask = 0, batteryMask = 0, gridMask = 0;
        combine(w.slot, w.count, use, solar, battery, grid, useMask, solarMask, batteryMask, gridMask);
        // A day away from home has a steady power (little difference between its highest and lowest hour): it is skipped and the next week back is taken
        bool away = __builtin_popcount(useMask) < 20;
        if (!away) {
          float lo = 1e9f, hi = 0, sum = 0;
          int hours = 0;
          for (int i = 0; i < ha::kHistoryBars; i++) if (useMask >> i & 1) { lo = fminf(lo, use[i]); hi = fmaxf(hi, use[i]); sum += use[i]; hours++; }
          away = sum <= 0 || hi - lo < 0.7f * sum / hours;
        }
        consoleLog("[HA] prediction week %d: %d hours%s\n", w.week + 1, __builtin_popcount(useMask), away ? ", skipped (away or no data)" : "");
        if (!away) {
          memcpy(w.use[w.good], use, sizeof(use));
          w.mask[w.good] = useMask;
          memset(w.out[w.good], 0, sizeof(w.out[0]));
          w.outMask[w.good] = 0;
          for (int k = 0; k < w.count; k++) {
            if (w.slot[k].field != kGridOutField) continue;
            for (int i = 0; i < ha::kHistoryBars; i++) if (w.slot[k].mask >> i & 1) { w.out[w.good][i] += fmaxf(0, w.slot[k].v[i]); w.outMask[w.good] |= 1u << i; }
          }
          w.age[w.good++] = w.week;
        }
        if (++w.week < kMaxWeeks && w.good < w.weeks) {
          askWeek(w);
        } else {
          // Days with very low usage are left out as well: below 60% of what the busier half of the days used
          float total[kMaxWeeks];
          int days = w.good;
          for (int k = 0; k < days; k++) {
            float sum = 0;
            for (int i = 0; i < ha::kHistoryBars; i++) if (w.mask[k] >> i & 1) sum += w.use[k][i];
            total[k] = sum;
          }
          float sorted[kMaxWeeks];
          memcpy(sorted, total, sizeof(float) * days);
          std::sort(sorted, sorted + days, [](float x, float y) { return x > y; });
          float busy = 0;
          const int half = (days + 1) / 2;
          for (int k = 0; k < half; k++) busy += sorted[k];
          if (half) busy /= half;
          // The recent weeks count most: a week's weight halves every 3 weeks back, so the seasons move the prediction along
          float mean[ha::kHistoryBars] = {}, weight[ha::kHistoryBars] = {}, sent[ha::kHistoryBars] = {}, sentWeight[ha::kHistoryBars] = {};
          int kept = 0;
          for (int k = 0; k < days; k++) {
            if (total[k] < 0.6f * busy) continue;
            kept++;
            const float wt = powf(0.5f, w.age[k] / 3.0f);
            for (int i = 0; i < ha::kHistoryBars; i++) if (w.mask[k] >> i & 1) { mean[i] += wt * w.use[k][i]; weight[i] += wt; }
            for (int i = 0; i < ha::kHistoryBars; i++) if (w.outMask[k] >> i & 1) { sent[i] += wt * w.out[k][i]; sentWeight[i] += wt; }
          }
          uint32_t sentMask = 0;
          for (int i = 0; i < ha::kHistoryBars; i++) if (sentWeight[i] > 0 && kept >= 2) { sent[i] /= sentWeight[i]; sentMask |= 1u << i; }
          uint32_t mask = 0;
          if (kept >= 2) for (int i = 0; i < ha::kHistoryBars; i++) if (weight[i] > 0) { mean[i] /= weight[i]; mask |= 1u << i; }
          consoleLog("[HA] prediction: %d usable days, %d kept, %s\n", days, kept, mask ? "ready" : "not enough");
          const int forDay = w.day;
          const bool tomorrow = w.shift != 0;
          weekAsk.reset();
          std::lock_guard<std::mutex> guard(_lock);
          if (tomorrow) {  // the coming day: no level correction or yearly trend, just the usual weekday
            predictedNextDay = forDay;
            memcpy(_outlook.nextUse, mean, sizeof(mean));
            memcpy(_outlook.nextOut, sent, sizeof(sent));
            _outlook.nextOutMask = sentMask;
            _outlook.nextUseMask = mask; _outlook.nextDay = forDay;
            _outlookVersion++;
            lastPredict = 0;
          } else {
            predictedDay = forDay;
            lastPredict = 0;  // tomorrow follows at once
            memcpy(_outlook.use, mean, sizeof(mean));
            memcpy(_outlook.out, sent, sizeof(sent));
            _outlook.outMask = sentMask;
            _outlook.useMask = mask; _outlook.useDay = forDay; _outlook.useDays = (uint32_t)kept;
            _outlookVersion++;
            yearDue = mask != 0;  // then last year's trend
          }
        }
      }
    } else if (yearId && (uint32_t)idOf(text) == yearId) {  // last year's daily usage: how much the usage moves from the last 4 weeks to the coming 2
      yearId = 0;
      if (!strstr(text, "\"success\":false")) {
        float sum[42] = {};
        int have[42] = {};
        for (int k = 0; k < yearCount; k++) {
          float one[42];
          for (float &v : one) v = -1.f;
          ha::parseDaily(text, yearIds[k], yearFirst, 42, one);
          for (int i = 0; i < 42; i++) if (one[i] >= 0) { sum[i] += one[i]; have[i]++; }
        }
        float before = 0, after = 0;
        int nb = 0, na = 0;
        for (int i = 0; i < 28; i++) if (have[i]) { before += sum[i]; nb++; }
        for (int i = 28; i < 42; i++) if (have[i]) { after += sum[i]; na++; }
        if (nb >= 14 && na >= 7 && before > 0) {
          const float ratio = fminf(1.25f, fmaxf(0.8f, (after / na) / (before / nb)));
          std::lock_guard<std::mutex> guard(_lock);
          if (_outlook.useDay == yearDay) {
            for (int i = 0; i < ha::kHistoryBars; i++) _outlook.use[i] *= ratio;
            _outlookVersion++;
          }
        }
      }
    } else if (peakId && (uint32_t)idOf(text) == peakId) {  // the monthly maxima of the house sensor(s)
      peakId = 0;
      float peak = 0;
      if (!strstr(text, "\"success\":false")) for (int k = 0; k < peakCount; k++) peak += ha::parseMaximum(text, peakIds[k]);
      if (peak > 0.2f) {
        peakDay = dayKey(time(nullptr));
        std::lock_guard<std::mutex> guard(_lock);
        _outlook.peak = peak; _outlook.peakDay = peakDay;
        _outlookVersion++;
      }
    } else if (forecastId && (uint32_t)idOf(text) == forecastId) {  // energy/solar_forecast
      forecastId = 0;
      if (!strstr(text, "\"success\":false")) {
        const time_t now = time(nullptr);
        struct tm local;
        localtime_r(&now, &local);
        char date[40];
        snprintf(date, sizeof(date), "%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
        float kw[24] = {};
        const uint32_t mask = ha::parseSolarForecast(text, date, kw);
        if (mask) {
          forecastDay = dayKey(now);
          std::lock_guard<std::mutex> guard(_lock);
          memcpy(_outlook.solar, kw, sizeof(kw));
          _outlook.solarMask = mask; _outlook.solarDay = forecastDay;
          _outlookVersion++;
        }
      }
    } else if (statsId && (uint32_t)idOf(text) == statsId) {  // recorder/statistics_during_period
      float values[4] = {};
      uint8_t got = 0;
      if (!strstr(text, "\"success\":false")) {
        for (int k = 0; k < 4; k++) {
          char id[1][ha::kEntity];
          float sum = 0;
          bool any = false;
          for (const char *list = config->energy[ha::E_USAGE_KWH + k]; ha::nextListed(list, id[0], sizeof(id[0]));) {
            float one = 0;
            if (ha::parseStatistics(text, id, 1, &one)) { sum += one; any = true; }
          }
          if (any) { values[k] = sum; got |= 1 << k; }
        }
        // What the grid received (production) is taken off what it delivered: the grid's net kWh of today
        if (gridTo[0] && (got >> 3 & 1)) {
          char id[1][ha::kEntity];
          for (const char *list = gridTo; ha::nextListed(list, id[0], sizeof(id[0]));) {
            float one = 0;
            if (ha::parseStatistics(text, id, 1, &one)) values[3] -= one;
          }
        }
      }
      std::lock_guard<std::mutex> guard(_lock);
      for (int k = 0; k < 4; k++) {
        _snapshot.energyOk &= ~(1 << (ha::E_USAGE_KWH + k));
        if (got >> k & 1) { _today[k] = values[k]; _snapshot.energy[ha::E_USAGE_KWH + k] = values[k]; _snapshot.energyOk |= 1 << (ha::E_USAGE_KWH + k); }
      }
      _todayOk = got;
      _updates++;
    } else if (typeIs(text, "result") && templateId && (uint32_t)idOf(text) == templateId) {
      if (strstr(text, "\"success\":false")) {
        char reason[48];
        if (!stringAfter(text, "\"message\":\"", reason, sizeof(reason))) strlcpy(reason, "request refused", sizeof(reason));
        char shown[64];
        snprintf(shown, sizeof(shown), "Rejected: %s", reason);
        set(FAILED, shown);
        break;
      }
    } else if (mediaId && (uint32_t)idOf(text) == mediaId) {
      const char *result = strstr(text, "\"result\":\"");
      if (result && typeIs(text, "event")) {
        std::unique_ptr<char[]> line(new (std::nothrow) char[1800]);
        std::unique_ptr<ha::media::Snapshot> found(new (std::nothrow) ha::media::Snapshot);
        if (line && found) {
          ha::media::unescape(result + 10, line.get(), 1800);
          ha::media::parseResult(line.get(), *found);
          ha::media::publish(*found);
        }
      }
    } else if (pricesId && (uint32_t)idOf(text) == pricesId) {
      const char *result = strstr(text, "\"result\":\"");
      if (result && typeIs(text, "event")) {
        char line[640];
        ha::media::unescape(result + 10, line, sizeof(line));
        ha::prices::Snapshot found;
        ha::prices::parseResult(line, found);
        ha::prices::publish(found);
      }
    } else if (devicesId && (uint32_t)idOf(text) == devicesId) {
      const char *result = strstr(text, "\"result\":\"");
      if (result && typeIs(text, "event")) {
        std::unique_ptr<char[]> line(new (std::nothrow) char[ha::devices::kLowPool + ha::devices::kOfflinePool + 100]);  // both lists, as long as the snapshot keeps them
        std::unique_ptr<ha::devices::Snapshot> found(new (std::nothrow) ha::devices::Snapshot);
        if (line && found) {
          ha::media::unescape(result + 10, line.get(), ha::devices::kLowPool + ha::devices::kOfflinePool + 100);
          ha::devices::parseResult(line.get(), *found);
          ha::devices::publish(*found);
        }
      }
    } else if (typeIs(text, "event") && templateId && (uint32_t)idOf(text) == templateId) {
      const char *result = strstr(text, "\"result\":\"");
      if (result) {
        result += 10;
        const char *end = strchr(result, '"');
        ha::Snapshot value;
        if (end && ha::parseResult(result, end - result, value)) {
          {
            std::lock_guard<std::mutex> guard(_lock);
            if (strcmp(statisticsMidnight, value.midnight)) {
              // A new day (or connection) must not reuse the previous day's totals or pending reply.
              strlcpy(statisticsMidnight, value.midnight, sizeof(statisticsMidnight));
              _todayOk = 0;
              statsId = 0;
              statisticsAsked = false;
            }
            for (int k = 0; k < 4; k++)  // today's kWh come from the statistics when they have an answer
              if (_todayOk >> k & 1) { value.energy[ha::E_USAGE_KWH + k] = _today[k]; value.energyOk |= 1 << (ha::E_USAGE_KWH + k); }
            _snapshot = value;
            _valid = true;
          }
          _updates++;
          if (!live) { live = true; set(LIVE, "Connected"); consoleLog("[HA] connected, live\n"); }
        } else {
          set(FAILED, "Unexpected answer from the template");
          break;
        }
      } else {
        char reason[48];
        if (!stringAfter(text, "\"error\":\"", reason, sizeof(reason))) strlcpy(reason, "template error", sizeof(reason));
        char shown[64];
        snprintf(shown, sizeof(shown), "Template: %s", reason);
        set(FAILED, shown);
        break;
      }
    }
  }
  client->stop();
  return live;
}
