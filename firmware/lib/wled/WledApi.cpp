// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Wled.h"
#include "GzipAsset.h"
#include "Settings.h"
#include <esp_heap_caps.h>
#include <ESPmDNS.h>
#include <mdns.h>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_random.h>
#include <memory>
#include <utility>
#include <vector>
#include <stdarg.h>
#include <stdlib.h>

// The part of WLED's HTTP API that the WLED apps and Home Assistant's WLED integration (python-wled) use: /json (state,
// info, effects, palettes), /json/si, /json/state (GET and POST), /json/eff|effects, /json/pal|palettes, /json/fxdata,
// /presets.json and the small /win?... query API. Responses stream in chunks so no JSON document is built on the heap.
UM_GZIP_ASSET(wledHtmlGz, "wled_html")

namespace wled {
namespace {
constexpr const char *kReportedVersion = "0.15.0";  // what apps and Home Assistant compare (they need 0.14.0 or newer); the real firmware version is "um" in /json/info
constexpr uint32_t kBuild = 2410160;

WebServer *srv = nullptr;
bool engage = false;       // the request started WLED content (an effect, a colour, a pixel): the light comes on unless the request says "on":false
uint32_t presetStamp = 1;  // "fs.pmt": changes whenever the presets do, so Home Assistant fetches them again

class Out {
public:
  explicit Out(String &sink) : _sink(&sink) {}  // collects the document in a string (the WebSocket)
  explicit Out(const char *download = nullptr) {
    srv->setContentLength(CONTENT_LENGTH_UNKNOWN);
    if (download) srv->sendHeader("Content-Disposition", String("attachment; filename=") + download);
    srv->sendHeader("Access-Control-Allow-Origin", "*");
    srv->sendHeader("Cache-Control", "no-store");
    srv->send(200, "application/json", "");
  }
  void add(const char *text) { append(text, strlen(text)); }
  void addf(const char *format, ...) __attribute__((format(printf, 2, 3))) {
    char line[256];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    append(line, n < 0 ? 0 : (n >= (int)sizeof(line) ? sizeof(line) - 1 : n));
  }
  void text(const char *value) {  // a JSON string
    add("\"");
    for (const char *p = value; *p; ++p) {
      if (*p == '"' || *p == '\\') { const char pair[2] = {'\\', *p}; append(pair, 2); }
      else if ((uint8_t)*p >= 0x20) append(p, 1);
    }
    add("\"");
  }
  void done() { flush(); if (!_sink) srv->sendContent(""); }
private:
  void append(const char *data, size_t length) {
    if (_sink) { _sink->concat(data, length); return; }
    if (length > sizeof(_buffer)) { flush(); srv->sendContent(data, length); return; }
    if (_used + length > sizeof(_buffer)) flush();
    memcpy(_buffer + _used, data, length);
    _used += length;
  }
  void flush() { if (_used) { srv->sendContent(_buffer, _used); _used = 0; } }
  char _buffer[400];
  size_t _used = 0;
  String *_sink = nullptr;
};

void macText(char *out) {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(out, 13, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void writeColors(Out &o, const uint8_t col[3][3]) {
  o.add("[");
  for (int i = 0; i < 3; ++i) o.addf("%s[%u,%u,%u]", i ? "," : "", col[i][0], col[i][1], col[i][2]);
  o.add("]");
}

void writeState(Out &o) {
  const State &s = state();
  int remaining = -1;
  if (s.nl.on) {
    const uint32_t total = (uint32_t)s.nl.duration * 60, elapsed = (millis() - s.nl.startMs) / 1000;
    remaining = elapsed >= total ? 0 : (int)(total - elapsed);
  }
  o.addf("{\"on\":%s,\"bri\":%u,\"transition\":%u,\"ps\":%d,\"pl\":%d,\"ledmap\":0,", s.on ? "true" : "false", s.bri, s.transition, s.ps, s.pl);
  o.addf("\"nl\":{\"on\":%s,\"dur\":%u,\"mode\":%u,\"tbri\":%u,\"rem\":%d},", s.nl.on ? "true" : "false", s.nl.duration, s.nl.mode, s.nl.target, remaining);
  o.addf("\"udpn\":{\"send\":%s,\"recv\":%s,\"sgrp\":%u,\"rgrp\":%u},\"lor\":0,\"mainseg\":0,", s.udpSend ? "true" : "false", s.udpRecv ? "true" : "false", s.udpSend ? 1 : 0, s.udpRecv ? 1 : 0);
  o.add("\"seg\":[{\"id\":0,\"start\":0,\"stop\":64,\"startY\":0,\"stopY\":64,\"len\":4096,\"grp\":1,\"spc\":0,\"of\":0,");
  o.addf("\"on\":%s,\"frz\":false,\"bri\":%u,\"cct\":127,\"set\":0,\"lc\":1,\"n\":", s.segOn ? "true" : "false", s.segBri);
  o.text(s.name);
  o.add(",\"col\":");
  writeColors(o, s.col);
  o.addf(",\"fx\":%u,\"sx\":%u,\"ix\":%u,\"pal\":%u,\"c1\":%u,\"c2\":%u,\"c3\":%u,", s.fx, s.sx, s.ix, s.pal, s.c1, s.c2, s.c3);
  o.add("\"sel\":true,\"rev\":false,\"mi\":false,\"rY\":false,\"mY\":false,\"tp\":false,\"o1\":false,\"o2\":false,\"o3\":false,\"si\":0,\"m12\":0}]}");
}

void writeInfo(Out &o) {
  char mac[13];
  macText(mac);
  const uint32_t now = millis();
  const bool live = realtimeActive(now);
  o.addf("{\"ver\":\"%s\",\"vid\":%u,\"cn\":\"Ultimatrix\",\"um\":", kReportedVersion, (unsigned)kBuild);
  o.text(esp_app_get_description()->version);
  o.add(",\"leds\":{\"count\":4096,\"pwr\":0,\"fps\":30,\"maxpwr\":0,\"maxseg\":1,\"seglc\":[1],\"lc\":1,\"rgbw\":false,\"wv\":false,\"cct\":0,\"matrix\":{\"w\":64,\"h\":64}},");
  o.add("\"str\":false,\"name\":");
  o.text(Settings::getInstance()->clockName.c_str());
  o.addf(",\"udpport\":21324,\"simple\":false,\"live\":%s,\"liveseg\":-1,\"lm\":", live ? "true" : "false");
  o.text(live ? realtimeSource() : "");
  o.add(",\"lip\":\"\",");
  o.addf("\"fxcount\":%d,\"palcount\":%d,\"cpalcount\":0,", effectSlots(), paletteSlots());
  o.addf("\"wifi\":{\"bssid\":\"%s\",\"rssi\":%d,\"signal\":%d,\"channel\":%d},", WiFi.BSSIDstr().c_str(), (int)WiFi.RSSI(),
         (int)constrain(2 * (WiFi.RSSI() + 100), 0, 100), (int)WiFi.channel());
  o.addf("\"fs\":{\"u\":1,\"t\":1,\"pmt\":%u},\"ndc\":0,\"arch\":\"esp32\",\"core\":\"%s\",\"lwip\":0,\"freeheap\":%u,\"uptime\":%u,\"time\":\"\",\"opt\":0,",
         (unsigned)presetStamp, ESP.getSdkVersion(), (unsigned)ESP.getFreeHeap(), (unsigned)(now / 1000));
  o.addf("\"brand\":\"Ultimatrix\",\"product\":\"LED Matrix Clock\",\"mac\":\"%s\",\"ip\":\"%s\"}", mac, WiFi.localIP().toString().c_str());
}

void writeEffects(Out &o) {
  o.add("[");
  for (int i = 0; i < effectSlots(); ++i) { if (i) o.add(","); o.text(effectName(i)); }
  o.add("]");
}

void writePalettes(Out &o) {
  o.add("[");
  for (int i = 0; i < paletteSlots(); ++i) { if (i) o.add(","); o.text(paletteName(i)); }
  o.add("]");
}

void writeEffectData(Out &o) {
  o.add("[");
  for (int i = 0; i < effectSlots(); ++i) { if (i) o.add(","); o.text(effectData(i)); }
  o.add("]");
}

void writePresets(Out &o) {
  o.add("{\"0\":{}");
  for (int id = 1; id <= kPresets; ++id) {
    const Preset *p = preset(id);
    if (!p) continue;
    o.addf(",\"%d\":{\"n\":", id);
    o.text(p->name);
    if (p->playlist) {
      o.add(",\"playlist\":{\"ps\":[");
      for (int i = 0; i < p->count; ++i) o.addf("%s%u", i ? "," : "", p->ps[i]);
      o.add("],\"dur\":[");
      for (int i = 0; i < p->count; ++i) o.addf("%s%u", i ? "," : "", p->dur[i]);
      o.addf("],\"transition\":[7],\"repeat\":%u,\"end\":%u,\"r\":0}}", p->repeat, p->end);
      continue;
    }
    o.addf(",\"on\":%s,\"bri\":%u,\"seg\":[{\"id\":0,\"fx\":%u,\"sx\":%u,\"ix\":%u,\"pal\":%u,\"col\":", p->on ? "true" : "false", p->bri, p->fx, p->sx, p->ix, p->pal);
    writeColors(o, p->col);
    o.add("}]}");
  }
  o.add("}");
}

void writeTimers(Out &o) {
  o.add("[");
  bool first = true;
  for (int i = 0; i < kTimers; ++i) {
    const Timer *t = timer(i);
    if (!t) continue;
    o.addf("%s{\"h\":%u,\"m\":%u,\"d\":%u,\"t\":%u,\"a\":%u,\"p\":%u,\"o\":%d}", first ? "" : ",", t->hour, t->minute, t->days, t->type, t->action, t->preset, t->offset);
    first = false;
  }
  o.add("]");
}

// ---- Applying a request ----
int number(const cJSON *item, int fallback) { return cJSON_IsNumber(item) ? (int)item->valuedouble : fallback; }
uint8_t byte(const cJSON *item, uint8_t fallback) { return (uint8_t)constrain(number(item, fallback), 0, 255); }

// true/false, or the string "t" for toggle
bool flag(const cJSON *item, bool current, bool &changed) {
  bool value = current;
  if (cJSON_IsBool(item)) value = cJSON_IsTrue(item);
  else if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] == 't') value = !current;
  else return current;
  changed = changed || value != current;
  return value;
}

bool hexByte(const char *s, uint8_t &out) {
  uint8_t v = 0;
  for (int i = 0; i < 2; ++i) {
    const char c = s[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10; else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else return false;
  }
  out = v;
  return true;
}

bool parseColor(const cJSON *item, uint8_t out[3]) {
  if (cJSON_IsArray(item)) {
    if (cJSON_GetArraySize(item) < 3) return false;
    for (int i = 0; i < 3; ++i) out[i] = byte(cJSON_GetArrayItem(item, i), out[i]);
    return true;
  }
  if (cJSON_IsString(item) && item->valuestring && strlen(item->valuestring) >= 6) {
    uint8_t c[3];
    if (!hexByte(item->valuestring, c[0]) || !hexByte(item->valuestring + 2, c[1]) || !hexByte(item->valuestring + 4, c[2])) return false;
    memcpy(out, c, 3);
    return true;
  }
  return false;
}

void applySegment(const cJSON *seg, bool &changed) {
  State &s = state();
  s.segOn = flag(cJSON_GetObjectItem(seg, "on"), s.segOn, changed);
  const cJSON *item;
  if ((item = cJSON_GetObjectItem(seg, "bri")) && cJSON_IsNumber(item)) { s.segBri = byte(item, s.segBri); changed = true; }
  const cJSON *col = cJSON_GetObjectItem(seg, "col");
  if (cJSON_IsArray(col))
    for (int i = 0; i < 3 && i < cJSON_GetArraySize(col); ++i) if (parseColor(cJSON_GetArrayItem(col, i), s.col[i])) { changed = true; engage = true; }
  if ((item = cJSON_GetObjectItem(seg, "fx"))) {
    int fx = -1;
    if (cJSON_IsNumber(item)) fx = (int)item->valuedouble;
    else if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] == 'r') {
      for (int tries = 0; tries < 40 && !effectKnown(fx); ++tries) fx = esp_random() % effectSlots();
    }
    if (effectKnown(fx)) { if (fx != s.fx) { s.fx = fx; restartEffect(); } changed = true; engage = true; }
  }
  if ((item = cJSON_GetObjectItem(seg, "sx")) && cJSON_IsNumber(item)) { s.sx = byte(item, s.sx); changed = true; }
  if ((item = cJSON_GetObjectItem(seg, "ix")) && cJSON_IsNumber(item)) { s.ix = byte(item, s.ix); changed = true; }
  if ((item = cJSON_GetObjectItem(seg, "pal")) && cJSON_IsNumber(item)) { const int pal = number(item, 0); if (pal >= 0 && pal < paletteSlots()) { s.pal = pal; changed = true; } }
  if ((item = cJSON_GetObjectItem(seg, "c1")) && cJSON_IsNumber(item)) { s.c1 = byte(item, s.c1); changed = true; }
  if ((item = cJSON_GetObjectItem(seg, "c2")) && cJSON_IsNumber(item)) { s.c2 = byte(item, s.c2); changed = true; }
  if ((item = cJSON_GetObjectItem(seg, "c3")) && cJSON_IsNumber(item)) { s.c3 = byte(item, s.c3); changed = true; }
  const cJSON *leds = cJSON_GetObjectItem(seg, "i");
  if (cJSON_IsArray(leds)) {  // individual LEDs: index, colour | start, stop, colour | ... land in the canvas, which Paint shows
    const int count = cJSON_GetArraySize(leds);
    for (int at = 0; at < count;) {
      const cJSON *first = cJSON_GetArrayItem(leds, at);
      if (!cJSON_IsNumber(first)) { ++at; continue; }
      int start = number(first, 0), stop = start + 1, colourAt = at + 1;
      if (at + 1 < count && cJSON_IsNumber(cJSON_GetArrayItem(leds, at + 1))) { stop = number(cJSON_GetArrayItem(leds, at + 1), start + 1); colourAt = at + 2; }
      uint8_t rgb[3] = {0, 0, 0};
      if (colourAt < count && parseColor(cJSON_GetArrayItem(leds, colourAt), rgb)) {
        for (int i = start; i < stop && i < kPixels; ++i) canvas::setPixel(i, rgb[0], rgb[1], rgb[2]);
        if (s.fx != kPaintEffect) { s.fx = kPaintEffect; restartEffect(); }
        changed = true; engage = true;
      }
      at = colourAt + 1;
    }
  }
  if ((item = cJSON_GetObjectItem(seg, "n")) && cJSON_IsString(item) && item->valuestring) { snprintf(s.name, sizeof(s.name), "%s", item->valuestring); changed = true; }
}

// Returns true when the request asked for the state back ("v")
bool applyJson(const cJSON *root) {
  for (const cJSON *item = root->child; item; item = item->next) {
    if (item->string && strcmp(item->string, "v")) { inputActivity(); break; }
  }
  State &s = state();
  bool changed = false, presetGiven = false;
  engage = false;
  const cJSON *item;
  if ((item = cJSON_GetObjectItem(root, "transition")) && cJSON_IsNumber(item)) s.transition = byte(item, s.transition);
  if ((item = cJSON_GetObjectItem(root, "tt")) && cJSON_IsNumber(item)) s.once = constrain(number(item, 0), 0, 255);
  if (((item = cJSON_GetObjectItem(root, "ps")) || (item = cJSON_GetObjectItem(root, "pl"))) && cJSON_IsNumber(item) && number(item, -1) > 0) presetGiven = applyPreset(number(item, -1));
  const bool onGiven = cJSON_GetObjectItem(root, "on") != nullptr;
  s.on = flag(cJSON_GetObjectItem(root, "on"), s.on, changed);
  if ((item = cJSON_GetObjectItem(root, "bri")) && cJSON_IsNumber(item)) {
    const int bri = number(item, s.bri);
    if (bri > 0) s.bri = constrain(bri, 0, 255);
    changed = true;
  }
  const cJSON *nl = cJSON_GetObjectItem(root, "nl");
  if (cJSON_IsObject(nl)) {
    if ((item = cJSON_GetObjectItem(nl, "dur")) && cJSON_IsNumber(item)) s.nl.duration = (uint8_t)constrain(number(item, 60), 1, 255);
    if ((item = cJSON_GetObjectItem(nl, "mode")) && cJSON_IsNumber(item)) s.nl.mode = (uint8_t)constrain(number(item, 1), 0, 3);
    if ((item = cJSON_GetObjectItem(nl, "tbri")) && cJSON_IsNumber(item)) s.nl.target = byte(item, 0);
    bool toggled = false;
    const bool on = flag(cJSON_GetObjectItem(nl, "on"), s.nl.on, toggled);
    if (on && !s.nl.on) { s.nl.startMs = millis(); s.nl.startBri = s.bri; s.on = true; }
    s.nl.on = on;
  }
  const cJSON *seg = cJSON_GetObjectItem(root, "seg");
  if (cJSON_IsArray(seg)) {
    int index = 0;
    for (const cJSON *entry = seg->child; entry; entry = entry->next, ++index)
      if (cJSON_IsObject(entry) && number(cJSON_GetObjectItem(entry, "id"), index) == 0) applySegment(entry, changed);
  } else if (cJSON_IsObject(seg) && number(cJSON_GetObjectItem(seg, "id"), 0) == 0) applySegment(seg, changed);
  if (engage && !onGiven) s.on = true;
  if ((item = cJSON_GetObjectItem(root, "live")) && cJSON_IsBool(item) && !cJSON_IsTrue(item)) realtimeStop();
  const cJSON *udpn = cJSON_GetObjectItem(root, "udpn");
  if (cJSON_IsObject(udpn)) {
    bool unused = false;
    s.udpSend = flag(cJSON_GetObjectItem(udpn, "send"), s.udpSend, unused);
    s.udpRecv = flag(cJSON_GetObjectItem(udpn, "recv"), s.udpRecv, unused);
    if ((item = cJSON_GetObjectItem(udpn, "rgrp")) && cJSON_IsNumber(item)) s.udpRecv = number(item, 0) != 0;
    if ((item = cJSON_GetObjectItem(udpn, "sgrp")) && cJSON_IsNumber(item)) s.udpSend = number(item, 0) != 0;
  }
  if (changed && !presetGiven) lookChanged();
  if ((item = cJSON_GetObjectItem(root, "psave")) && cJSON_IsNumber(item)) {
    const cJSON *name = cJSON_GetObjectItem(root, "n"), *list = cJSON_GetObjectItem(root, "playlist");
    const char *label = cJSON_IsString(name) ? name->valuestring : "";
    bool saved;
    if (cJSON_IsObject(list)) {
      uint8_t ps[kPlaylistSteps] = {};
      uint16_t dur[kPlaylistSteps] = {};
      const cJSON *psList = cJSON_GetObjectItem(list, "ps"), *durList = cJSON_GetObjectItem(list, "dur");
      const int count = cJSON_IsArray(psList) ? cJSON_GetArraySize(psList) : 0;
      for (int i = 0; i < count && i < kPlaylistSteps; ++i) {
        ps[i] = byte(cJSON_GetArrayItem(psList, i), 0);
        const cJSON *d = cJSON_IsArray(durList) ? cJSON_GetArrayItem(durList, i) : durList;  // a single number applies to every step
        dur[i] = (uint16_t)constrain(number(d, 100), 1, 65535);
      }
      saved = savePlaylist(number(item, 0), label, ps, dur, count, number(cJSON_GetObjectItem(list, "repeat"), 0), number(cJSON_GetObjectItem(list, "end"), 0));
    } else saved = savePreset(number(item, 0), label);
    if (saved) ++presetStamp;
  }
  if ((item = cJSON_GetObjectItem(root, "pdel")) && cJSON_IsNumber(item) && deletePreset(number(item, 0))) ++presetStamp;
  touched();
  const cJSON *verbose = cJSON_GetObjectItem(root, "v");
  return cJSON_IsTrue(verbose);
}

// ---- Routes ----
void sendState() { Out o; writeState(o); o.done(); }
void sendInfo() { Out o; writeInfo(o); o.done(); }
void sendFull() {
  Out o;
  o.add("{\"state\":"); writeState(o);
  o.add(",\"info\":"); writeInfo(o);
  o.add(",\"effects\":"); writeEffects(o);
  o.add(",\"palettes\":"); writePalettes(o);
  o.add("}");
  o.done();
}
void sendStateInfo() {
  Out o;
  o.add("{\"state\":"); writeState(o);
  o.add(",\"info\":"); writeInfo(o);
  o.add("}");
  o.done();
}
void sendEffects() { Out o; writeEffects(o); o.done(); }
void sendPalettes() { Out o; writePalettes(o); o.done(); }
void sendEffectData() { Out o; writeEffectData(o); o.done(); }
void sendPresets() { Out o; writePresets(o); o.done(); }
void sendNodes() { srv->sendHeader("Access-Control-Allow-Origin", "*"); srv->send(200, "application/json", "{\"nodes\":[]}"); }
void sendConfig() {
  Out o;
  o.add("{\"id\":{\"name\":");
  o.text(Settings::getInstance()->clockName.c_str());
  o.add("}}"); o.done();
}

void postState() {
  if (!enabled()) { srv->send(404, "application/json", "{\"error\":\"WLED is off in the settings\"}"); return; }
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(srv->arg("plain").c_str()), cJSON_Delete);
  if (!root || !cJSON_IsObject(root.get())) { srv->send(400, "application/json", "{\"error\":9}"); return; }
  if (applyJson(root.get())) { sendState(); return; }
  srv->sendHeader("Access-Control-Allow-Origin", "*");
  srv->send(200, "application/json", "{\"success\":true}");
}

// /json accepts the state document too (the apps post there as well)
void postJson() {
  if (!enabled()) { srv->send(404, "application/json", "{\"error\":\"WLED is off in the settings\"}"); return; }
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(srv->arg("plain").c_str()), cJSON_Delete);
  if (!root || !cJSON_IsObject(root.get())) { srv->send(400, "application/json", "{\"error\":9}"); return; }
  const cJSON *inner = cJSON_GetObjectItem(root.get(), "state");
  if (applyJson(cJSON_IsObject(inner) ? inner : root.get())) { sendState(); return; }
  srv->sendHeader("Access-Control-Allow-Origin", "*");
  srv->send(200, "application/json", "{\"success\":true}");
}

// The query API: /win?A=128&FX=9&SX=100&R=255&G=0&B=0&T=1 ...
void win() {
  // /win changes state on a GET, which any web page can fire with an <img> tag: browsers label that cross-site
  if (srv->header("Sec-Fetch-Site") == "cross-site") { srv->send(403, "text/plain", "Cross-site request refused."); return; }
  if (srv->args()) inputActivity();
  if (!enabled()) { srv->send(404, "text/plain", "WLED is off in the settings"); return; }
  State &s = state();
  bool changed = false, fxGiven = false;
  if (srv->hasArg("T")) { const int t = srv->arg("T").toInt(); const bool on = t == 2 ? !s.on : t != 0; changed = changed || on != s.on; s.on = on; }
  if (srv->hasArg("A")) { const int bri = srv->arg("A").toInt(); if (bri > 0) s.bri = constrain(bri, 0, 255); changed = true; }
  if (srv->hasArg("FX")) { const int fx = srv->arg("FX").toInt(); if (effectKnown(fx)) { if (fx != s.fx) { s.fx = fx; restartEffect(); } changed = true; fxGiven = true; } }
  if (srv->hasArg("SX")) { s.sx = constrain(srv->arg("SX").toInt(), 0, 255); changed = true; }
  if (srv->hasArg("IX")) { s.ix = constrain(srv->arg("IX").toInt(), 0, 255); changed = true; }
  if (srv->hasArg("FP")) { const int pal = srv->arg("FP").toInt(); if (pal >= 0 && pal < paletteSlots()) { s.pal = pal; changed = true; } }
  static const char *const channels[2][3] = {{"R", "G", "B"}, {"R2", "G2", "B2"}};
  for (int slot = 0; slot < 2; ++slot)
    for (int c = 0; c < 3; ++c) if (srv->hasArg(channels[slot][c])) { s.col[slot][c] = constrain(srv->arg(channels[slot][c]).toInt(), 0, 255); changed = true; }
  if (srv->hasArg("TT")) s.once = constrain(srv->arg("TT").toInt() / 100, 0, 255);
  bool presetGiven = false;
  if (srv->hasArg("PL")) presetGiven = applyPreset(srv->arg("PL").toInt());
  if (srv->hasArg("NL")) {
    const int minutes = srv->arg("NL").toInt();
    s.nl.on = minutes > 0;
    if (minutes > 0) { s.nl.duration = constrain(minutes, 1, 255); s.nl.startMs = millis(); s.nl.startBri = s.bri; }
  }
  if (fxGiven && !srv->hasArg("T")) s.on = true;
  if (changed && !presetGiven) lookChanged();
  touched();
  char xml[160];
  snprintf(xml, sizeof(xml), "<?xml version=\"1.0\" ?><vs><ac>%u</ac><fx>%u</fx><sx>%u</sx><ix>%u</ix><fp>%u</fp></vs>", s.on ? s.bri : 0, s.fx, s.sx, s.ix, s.pal);
  srv->send(200, "text/xml", xml);
}


// ---- The overlay's own options for the settings page ----
void optionsState() {
  const Options &o = options();
  String json = String("{\"enabled\":") + (o.enabled ? "true" : "false") + ",\"listening\":" + (realtimeListening() ? "true" : "false") +
                ",\"timeoutMin\":" + o.timeoutMin + ",\"ignoreLive\":" + (o.ignoreLive ? "true" : "false") + ",\"bootEffect\":" + (o.bootEffect == 255 ? -1 : o.bootEffect) +
                ",\"on\":" + (state().on ? "true" : "false") + ",\"source\":\"" + realtimeSource() + "\",\"live\":" + (realtimeActive(millis()) ? "true" : "false") +
                ",\"packets\":" + realtimePackets() + ",\"dropped\":" + realtimeDropped() + ",\"freeheap\":" + (unsigned)ESP.getFreeHeap() + ",\"data\":" + (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT) + ",\"block\":" + (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) + ",\"effects\":[";
  bool first = true;
  for (int i = 0; i < effectSlots(); ++i) {
    if (!effectKnown(i)) continue;
    json += String(first ? "" : ",") + "[" + i + ",\"" + effectName(i) + "\"]";
    first = false;
  }
  srv->send(200, "application/json", json + "]}");
}

void optionsSave() {
  Options &o = options();
  if (srv->hasArg("timeoutMin")) o.timeoutMin = (uint8_t)constrain(srv->arg("timeoutMin").toInt(), 0, 255);
  if (srv->hasArg("ignoreLive")) o.ignoreLive = srv->arg("ignoreLive") == "1";
  if (srv->hasArg("bootEffect")) {
    const int fx = srv->arg("bootEffect").toInt();
    if (fx < 0) o.bootEffect = 255;
    else if (effectKnown(fx)) o.bootEffect = fx;
    else { srv->send(400, "application/json", "{\"message\":\"Unknown effect.\"}"); return; }
  }
  if (srv->hasArg("enabled")) {
    const bool on = srv->arg("enabled") == "1";
    if (on != (o.enabled != 0)) {
      if (!setEnabled(on)) { o.enabled = 0; srv->send(507, "application/json", "{\"message\":\"Not enough free memory to turn WLED on.\"}"); return; }
      if (on) mdnsAdd(); else mdnsRemove();
    }
  }
  touched();
  srv->send(200, "application/json", "{\"message\":\"Saved.\"}");
}

// ---- Timers, image and backup (this face's own endpoints for the settings page) ----
void timersState() { Out o; o.add("{\"timers\":"); writeTimers(o); o.add("}"); o.done(); }

bool parseTimers(const cJSON *list, Timer *out, int &count) {
  count = 0;
  if (!cJSON_IsArray(list)) return false;
  for (const cJSON *entry = list->child; entry && count < kTimers; entry = entry->next) {
    if (!cJSON_IsObject(entry)) continue;
    Timer &t = out[count];
    memset(&t, 0, sizeof(t));
    t.hour = (uint8_t)constrain(number(cJSON_GetObjectItem(entry, "h"), 0), 0, 23);
    t.minute = (uint8_t)constrain(number(cJSON_GetObjectItem(entry, "m"), 0), 0, 59);
    t.days = (uint8_t)(number(cJSON_GetObjectItem(entry, "d"), 127) & 127);
    t.type = (uint8_t)constrain(number(cJSON_GetObjectItem(entry, "t"), 0), 0, 2);
    t.action = (uint8_t)constrain(number(cJSON_GetObjectItem(entry, "a"), 0), 0, 2);
    t.preset = (uint8_t)constrain(number(cJSON_GetObjectItem(entry, "p"), 0), 0, kPresets);
    t.offset = (int16_t)constrain(number(cJSON_GetObjectItem(entry, "o"), 0), -720, 720);
    ++count;
  }
  return true;
}

void timersSave() {
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(srv->arg("plain").c_str()), cJSON_Delete);
  Timer list[kTimers];
  int count = 0;
  if (!root || !parseTimers(cJSON_IsArray(root.get()) ? root.get() : cJSON_GetObjectItem(root.get(), "timers"), list, count)) {
    srv->send(400, "application/json", "{\"message\":\"Send a list of timers.\"}");
    return;
  }
  setTimers(list, count);
  srv->send(200, "application/json", "{\"message\":\"Saved.\"}");
}

bool parsePreset(const cJSON *entry, Preset &p) {
  if (!cJSON_IsObject(entry)) return false;
  memset(&p, 0, sizeof(p));
  const cJSON *item = cJSON_GetObjectItem(entry, "n");
  snprintf(p.name, sizeof(p.name), "%s", cJSON_IsString(item) && item->valuestring ? item->valuestring : "Preset");
  const cJSON *list = cJSON_GetObjectItem(entry, "playlist");
  if (cJSON_IsObject(list)) {
    const cJSON *psList = cJSON_GetObjectItem(list, "ps"), *durList = cJSON_GetObjectItem(list, "dur");
    const int count = cJSON_IsArray(psList) ? cJSON_GetArraySize(psList) : 0;
    if (count < 1) return false;
    p.playlist = 1;
    p.count = count > kPlaylistSteps ? kPlaylistSteps : count;
    for (int i = 0; i < p.count; ++i) {
      p.ps[i] = byte(cJSON_GetArrayItem(psList, i), 0);
      p.dur[i] = (uint16_t)constrain(number(cJSON_IsArray(durList) ? cJSON_GetArrayItem(durList, i) : durList, 100), 1, 65535);
    }
    p.repeat = byte(cJSON_GetObjectItem(list, "repeat"), 0);
    p.end = (uint8_t)constrain(number(cJSON_GetObjectItem(list, "end"), 0), 0, kPresets);
    return true;
  }
  p.on = cJSON_IsFalse(cJSON_GetObjectItem(entry, "on")) ? 0 : 1;
  p.bri = byte(cJSON_GetObjectItem(entry, "bri"), 128);
  const cJSON *seg = cJSON_GetObjectItem(entry, "seg");
  if (cJSON_IsArray(seg)) seg = cJSON_GetArrayItem(seg, 0);
  if (!cJSON_IsObject(seg)) seg = entry;
  const int fx = number(cJSON_GetObjectItem(seg, "fx"), 0);
  p.fx = effectKnown(fx) ? fx : 0;
  p.sx = byte(cJSON_GetObjectItem(seg, "sx"), 128);
  p.ix = byte(cJSON_GetObjectItem(seg, "ix"), 128);
  p.pal = (uint8_t)constrain(number(cJSON_GetObjectItem(seg, "pal"), 0), 0, paletteSlots() - 1);
  const cJSON *col = cJSON_GetObjectItem(seg, "col");
  for (int i = 0; i < 3 && cJSON_IsArray(col) && i < cJSON_GetArraySize(col); ++i) parseColor(cJSON_GetArrayItem(col, i), p.col[i]);
  return true;
}

// Everything this face stores, as one file: options, the look, presets and playlists, timers
void backupDownload() {
  Out o("ultimatrix-wled-backup.json");
  const Options &op = options();
  o.addf("{\"format\":\"ultimatrix-wled\",\"version\":1,\"options\":{\"timeoutMin\":%u,\"bootEffect\":%d},\"state\":", op.timeoutMin, op.bootEffect == 255 ? -1 : op.bootEffect);
  writeState(o);
  o.add(",\"presets\":"); writePresets(o);
  o.add(",\"timers\":"); writeTimers(o);
  o.add("}");
  o.done();
}

void backupRestore() {
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(srv->arg("plain").c_str()), cJSON_Delete);
  const cJSON *format = root ? cJSON_GetObjectItem(root.get(), "format") : nullptr;
  if (!cJSON_IsString(format) || strcmp(format->valuestring, "ultimatrix-wled") != 0) {
    srv->send(400, "application/json", "{\"message\":\"That is not a backup file from this clock face.\"}");
    return;
  }
  // Parse the presets first: nothing is touched when none of them is usable (a damaged file must not wipe what the clock has)
  const cJSON *presets = cJSON_GetObjectItem(root.get(), "presets");
  std::vector<std::pair<int, Preset>> parsed;
  if (cJSON_IsObject(presets)) {
    int slots = 0;  // entries with a real preset number (the "0" placeholder of an empty list does not count)
    for (const cJSON *entry = presets->child; entry; entry = entry->next) {
      const int id = entry->string ? atoi(entry->string) : 0;
      Preset p;
      if (id < 1 || id > kPresets) continue;
      slots++;
      if (parsePreset(entry, p)) parsed.emplace_back(id, p);
    }
    if (slots && parsed.empty()) {
      srv->send(400, "application/json", "{\"message\":\"The presets in that file are not usable. Nothing was changed.\"}");
      return;
    }
  }
  const cJSON *op = cJSON_GetObjectItem(root.get(), "options");
  if (cJSON_IsObject(op)) {
    options().timeoutMin = (uint8_t)constrain(number(cJSON_GetObjectItem(op, "timeoutMin"), 0), 0, 255);
    const int fx = number(cJSON_GetObjectItem(op, "bootEffect"), -1);
    options().bootEffect = fx >= 0 && effectKnown(fx) ? fx : 255;
  }
  if (cJSON_IsObject(presets)) {
    for (int id = 1; id <= kPresets; ++id) deletePreset(id);
    for (const auto &entry : parsed) setPreset(entry.first, entry.second);
    ++presetStamp;
  }
  const cJSON *timers = cJSON_GetObjectItem(root.get(), "timers");
  Timer list[kTimers];
  int count = 0;
  if (parseTimers(timers, list, count)) setTimers(list, count);
  const cJSON *state = cJSON_GetObjectItem(root.get(), "state");
  if (cJSON_IsObject(state)) applyJson(state);
  touched();
  srv->send(200, "application/json", "{\"message\":\"Restored.\"}");
}
}  // namespace

// The WebSocket's side of the API: the whole state and info as one document, and a command document applied
void apiStateInfo(String &out) {
  Out o(out);
  o.add("{\"state\":"); writeState(o);
  o.add(",\"info\":"); writeInfo(o);
  o.add("}");
}

void apiApplyText(const char *text) {
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(text), cJSON_Delete);
  if (!root || !cJSON_IsObject(root.get())) return;
  const cJSON *inner = cJSON_GetObjectItem(root.get(), "state");
  applyJson(cJSON_IsObject(inner) ? inner : root.get());
}

void apiAttach(WebServer &server, void (*postRoute)(const char *, WebServer::THandlerFunction)) {
  // (the prototype's per-request header logging is gone: at 115200 baud a full serial buffer stalls the loop that also draws)
  canvas::attach(server, postRoute);
  wsAttach(server);
  server.on("/wled", HTTP_GET, []() { sendGzip(*srv, "text/html; charset=utf-8", wledHtmlGz, wledHtmlGzEnd); });
  srv = &server;
  // Browsers and the WLED app's web view ask OPTIONS before a JSON POST: answer it like WLED does
  server.onNotFound([]() {
    if (srv->method() == HTTP_OPTIONS) {
      srv->sendHeader("Access-Control-Allow-Origin", "*");
      srv->sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
      srv->sendHeader("Access-Control-Allow-Headers", "*");
      srv->send(200, "text/plain", "");
      return;
    }
    srv->send(404, "text/html", String("Not found: ") + srv->uri());
  });
  load();
  server.on("/json", HTTP_GET, sendFull);
  server.on("/json/state", HTTP_GET, sendState);
  server.on("/json/info", HTTP_GET, sendInfo);
  server.on("/json/si", HTTP_GET, sendStateInfo);
  server.on("/json/eff", HTTP_GET, sendEffects);
  server.on("/json/effects", HTTP_GET, sendEffects);
  server.on("/json/pal", HTTP_GET, sendPalettes);
  server.on("/json/palettes", HTTP_GET, sendPalettes);
  server.on("/json/fxdata", HTTP_GET, sendEffectData);
  server.on("/json/nodes", HTTP_GET, sendNodes);
  server.on("/json/cfg", HTTP_GET, sendConfig);
  server.on("/presets.json", HTTP_GET, sendPresets);
  server.on("/win", HTTP_GET, win);
  postRoute("/json/state", postState);
  postRoute("/json", postJson);
  server.on("/api/wled/timers", HTTP_GET, timersState);
  postRoute("/api/wled/timers", timersSave);
  server.on("/api/wled/backup", HTTP_GET, backupDownload);
  postRoute("/api/wled/backup", backupRestore);
  server.on("/api/wled", HTTP_GET, optionsState);
  postRoute("/api/wled", optionsSave);
}

static bool mdnsRegistered = false;

bool mdnsAdd() {
  if (!enabled()) return false;
  char mac[13];
  macText(mac);
  mdnsRegistered = MDNS.addService("wled", "tcp", 80);
  if (mdnsRegistered) MDNS.addServiceTxt("wled", "tcp", "mac", (const char *)mac);
  return mdnsRegistered;
}

void mdnsRemove() {
  if (mdnsRegistered) mdns_service_remove("_wled", "_tcp");
  mdnsRegistered = false;
}

// What the responder holds right now (for the telnet "mdns" command): the record is asked from the mDNS library, not remembered
bool mdnsAdded() { return mdnsRegistered && mdns_service_exists("_wled", "_tcp", nullptr); }

}  // namespace wled

namespace wled {
// The WLED app opens "/" in a web view: it gets the light page instead of the clock settings ("/?settings" always shows the settings)
bool wantsLightPage(WebServer &server) {
  if (!enabled() || server.hasArg("settings")) return false;
  String agent = server.header("User-Agent");
  agent.toLowerCase();
  return agent.indexOf("wled") >= 0;
}
}  // namespace wled
