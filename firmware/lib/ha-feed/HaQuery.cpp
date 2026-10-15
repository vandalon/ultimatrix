// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "HaQuery.h"

#include <Arduino.h>
#include <atomic>
#include <memory>
#include <new>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <string.h>
#include "HaConfig.h"

extern std::atomic<bool> firmwareUpdating;

namespace ha {

namespace {

// The "for s in ..." part of the template for each kind of entity
const char *loopFor(const char *kind) {
  struct Kind { const char *name, *loop; };
  static const Kind kinds[] = {
    {"power", "states.sensor if s.attributes.device_class == 'power'"},
    {"energy", "states.sensor if s.attributes.device_class == 'energy'"},
    {"battery", "states.sensor if s.attributes.device_class == 'battery'"},
    {"temperature", "states.sensor if s.attributes.device_class == 'temperature'"},
    {"humidity", "states.sensor if s.attributes.device_class == 'humidity'"},
    {"person", "states.person"},
    {"weather", "states.weather"},
    {"battery_status", "states if s.attributes.device_class == 'battery'"},
    {"leak", "states.binary_sensor if s.attributes.device_class == 'moisture'"},
    {"blinds", "states.cover if s.attributes.device_class in ['blind','curtain','shade','shutter']"},
    {"media", "states.media_player"},
    {"climate", "states.climate"},
    {"co2", "states.sensor if s.attributes.device_class == 'carbon_dioxide'"},
    {"rain", "states.sensor if s.attributes.device_class in ['precipitation','precipitation_intensity']"},
    {"price", "states.sensor if s.attributes.device_class == 'monetary' or s.attributes.unit_of_measurement is defined"},
    {"presence", "states.binary_sensor if s.attributes.device_class in ['occupancy','motion','presence']"},
    {"door", "states.binary_sensor if s.attributes.device_class in ['door','opening']"},
    {"window", "states.binary_sensor if s.attributes.device_class == 'window'"},
    {"garage", "states.cover if s.attributes.device_class == 'garage'"},
    {"light", "states.light"},
    {"lock", "states if s.domain in ['alarm_control_panel','lock']"},
    {"plug", "states.binary_sensor if s.attributes.device_class == 'plug'"},
    {"charging", "states.binary_sensor if s.attributes.device_class == 'battery_charging'"},
    {"binary", "states.binary_sensor"},
    {"sensor", "states.sensor"},
  };
  for (const Kind &k : kinds)
    if (!strcmp(kind, k.name)) return k.loop;
  return "states if s.domain in ['sensor','binary_sensor','switch','input_boolean','group','light','cover','climate','person']";
}

}  // namespace

bool suggest(const char *kind, const char *typed, char *out, size_t capacity, char *error, size_t errorSize) {
  out[0] = 0;
  if (!configured()) { strlcpy(error, "Home Assistant is not set up yet", errorSize); return false; }
  if (WiFi.status() != WL_CONNECTED || firmwareUpdating) { strlcpy(error, "Not available now", errorSize); return false; }
  const Config &c = config();

  // What was typed is reduced to the characters of an entity ID, so it can sit between quotes in the template
  char q[28];
  size_t n = 0;
  for (const char *t = typed; *t && n + 1 < sizeof(q); ++t) {
    const char ch = *t >= 'A' && *t <= 'Z' ? *t + 32 : *t;
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.') q[n++] = ch;
  }
  q[n] = 0;

  // The clock's own task has a small stack: everything of size lives on the heap
  std::unique_ptr<char[]> body(new (std::nothrow) char[900]);
  // Declared before the HTTP client so they are destroyed after it: its destructor still talks to them
  std::unique_ptr<WiFiClient> plain;
  std::unique_ptr<WiFiClientSecure> secure;
  std::unique_ptr<HTTPClient> http(new (std::nothrow) HTTPClient);
  if (!body || !http) { strlcpy(error, "Out of memory", errorSize); return false; }
  snprintf(body.get(), 900,
           "{\"template\":\"{%% set ns = namespace(r=[]) %%}{%% for s in %s %%}{%% if '%s' in s.entity_id or '%s' in s.name|lower %%}"
           "{%% set ns.r = ns.r + [s.entity_id ~ '|' ~ (s.name|replace(';',' ')|replace('|',' ')|truncate(28,true,'')) ~ '|' ~ (s.attributes.unit_of_measurement or '')] %%}"
           "{%% endif %%}{%% endfor %%}{{ ns.r[:%d]|join(';') }}\"}",
           loopFor(kind), q, q, n ? 25 : 120);  // nothing typed: the whole list, which the settings page keeps for filtering

  bool started;
  if (c.tls) {
    secure.reset(new (std::nothrow) WiFiClientSecure);
    if (!secure) { strlcpy(error, "Out of memory", errorSize); return false; }
    secure->setInsecure();
    started = http->begin(*secure, c.host, c.port, "/api/template", true);
  } else {
    plain.reset(new (std::nothrow) WiFiClient);
    if (!plain) { strlcpy(error, "Out of memory", errorSize); return false; }
    started = http->begin(*plain, c.host, c.port, "/api/template");
  }
  if (!started) { strlcpy(error, "Cannot reach Home Assistant", errorSize); return false; }
  http->setTimeout(4000);
  http->addHeader("Content-Type", "application/json");
  std::unique_ptr<char[]> bearer(new (std::nothrow) char[kTokenMax + 8]);
  if (!bearer) { http->end(); strlcpy(error, "Out of memory", errorSize); return false; }
  snprintf(bearer.get(), kTokenMax + 8, "Bearer %s", c.token);
  http->addHeader("Authorization", bearer.get());
  const int code = http->POST((uint8_t *)body.get(), strlen(body.get()));
  if (code != 200) {
    strlcpy(error, code == 401 ? "Home Assistant refused the token" : code > 0 ? "Home Assistant could not answer" : "Cannot reach Home Assistant", errorSize);
    http->end();
    return false;
  }
  String text = http->getString();
  http->end();
  if (text.length() >= capacity) text.remove(capacity - 1);
  strlcpy(out, text.c_str(), capacity);
  return true;
}

}  // namespace ha
