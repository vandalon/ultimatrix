// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Mqtt.h"
#include "Console.h"
#include "ScreenPng.h"
#include <Preferences.h>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_mac.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <stdarg.h>
#include <memory>
#include <new>
#include "TimeService.h"
#ifdef UM_WLED_OVERLAY
#include <Wled.h>
#endif

namespace {
constexpr const char *kDiscovery = "homeassistant";
constexpr uint32_t kSaveDelayMs = 30000;   // commands from automations change settings often: store them once they settle
constexpr uint32_t kStateEveryMs = 60000;
constexpr uint32_t kLightEveryMs = 10000;  // the diagnostics move all the time, so state goes out at most this often
constexpr size_t kPayload = 800;

struct Color { const char *name; uint16_t rgb; };
constexpr Color kColors[] = {{"white", 0xFFFF}, {"red", 0xF800}, {"green", 0x07E0}, {"blue", 0x041F}, {"yellow", 0xFFE0},
                             {"orange", 0xFD20}, {"purple", 0xA01F}, {"cyan", 0x07FF}};

uint16_t parseColor(const char *text) {
  if (text[0] == '#' && strlen(text) == 7) {
    char *end = nullptr;
    unsigned long rgb = strtoul(text + 1, &end, 16);
    if (*end == 0) return ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F);
  }
  for (const Color &c : kColors) if (strcasecmp(text, c.name) == 0) return c.rgb;
  return 0xFFFF;
}

// "ON"/"OFF" from Home Assistant: 1, 0, or -1 for anything else
int8_t onOff(const char *text) {
  if (strcasecmp(text, "ON") == 0) return 1;
  if (strcasecmp(text, "OFF") == 0) return 0;
  return -1;
}

// The entities Home Assistant gets. A key is the topic part (<base>/<key>/set) and the name of its value in the state message.
enum EntityId : uint8_t {
  E_BRIGHTNESS, E_AUTO_BRIGHTNESS, E_NIGHT_CLOCK, E_NOTIFY, E_MIN_BRIGHTNESS, E_NIGHT_MODE, E_NIGHT_SUN, E_NIGHT_START, E_NIGHT_END,
  E_ROTATION, E_TIME_24H, E_AUTO_TIMEZONE, E_TIMEZONE, E_NTP_SERVER, E_LDR_DARK, E_LDR_LIGHT, E_SWAP, E_AUTO_UPDATE, E_TELNET,
  E_RESTART, E_CHECK_UPDATE, E_UPDATE, E_LIGHT, E_WIFI, E_MEMORY, E_SCREEN, E_CPU, E_DISPLAY_CPU, E_FACE,
#ifdef UM_WLED_OVERLAY
  E_CANVAS,
#endif
  E_COUNT
};
enum Flag : uint8_t { STATE = 1, COMMAND = 2, CONFIG = 4, DIAGNOSTIC = 8, CUSTOM = 16 };  // CUSTOM: its extra part brings its own state topic
struct Entity { const char *component, *key, *name; uint8_t flags; const char *extra; };
constexpr const char *kSlider = "\"min\":1,\"max\":255,\"step\":1,\"mode\":\"slider\"";
constexpr const char *kClock = "\"min\":5,\"max\":5,\"pattern\":\"^([01][0-9]|2[0-3]):[0-5][0-9]$\"";
const Entity kEntities[E_COUNT] = {
    {"number", "brightness", "Brightness", STATE | COMMAND, kSlider},
    {"switch", "auto_brightness", "Auto brightness", STATE | COMMAND, ""},
    {"switch", "night_clock", "Night clock", STATE | COMMAND, ""},
    {"notify", "notify", "Notification", COMMAND, ""},
    {"number", "min_brightness", "Minimum brightness", STATE | COMMAND | CONFIG, kSlider},
    {"switch", "night_mode", "Night mode", STATE | COMMAND | CONFIG, ""},
    {"switch", "night_sun", "Night from sunset to sunrise", STATE | COMMAND | CONFIG, ""},
    {"text", "night_start", "Night starts", STATE | COMMAND | CONFIG, kClock},
    {"text", "night_end", "Night ends", STATE | COMMAND | CONFIG, kClock},
    {"select", "rotation", "Display orientation", STATE | COMMAND | CONFIG, "\"options\":[\"0\",\"90\",\"180\",\"270\"]"},
    {"switch", "time_24h", "24-hour clock", STATE | COMMAND | CONFIG, ""},
    {"switch", "auto_timezone", "Automatic time zone", STATE | COMMAND | CONFIG, ""},
    {"text", "timezone", "Time zone", STATE | COMMAND | CONFIG, "\"min\":1,\"max\":47"},
    {"text", "ntp_server", "Time server", STATE | COMMAND | CONFIG, "\"min\":1,\"max\":47"},
    {"number", "ldr_dark", "Light sensor: dark level", STATE | COMMAND | CONFIG, "\"min\":0,\"max\":4094,\"step\":1,\"mode\":\"box\""},
    {"number", "ldr_light", "Light sensor: bright level", STATE | COMMAND | CONFIG, "\"min\":1,\"max\":4095,\"step\":1,\"mode\":\"box\""},
    {"switch", "swap_blue_green", "Swap blue and green (restarts)", STATE | COMMAND | CONFIG, ""},
    {"switch", "auto_update", "Automatic updates", STATE | COMMAND | CONFIG, ""},
    {"switch", "telnet", "Remote console", STATE | COMMAND | CONFIG, ""},
    {"button", "restart", "Restart", COMMAND | CONFIG, "\"dev_cla\":\"restart\""},
    {"button", "check_update", "Check for updates", COMMAND | CONFIG, ""},
    {"update", "update", "Firmware", COMMAND | CUSTOM, "\"stat_t\":\"~/update\",\"payload_install\":\"INSTALL\",\"dev_cla\":\"firmware\""},
    {"sensor", "light_level", "Light level", STATE, "\"stat_cla\":\"measurement\",\"icon\":\"mdi:brightness-5\""},
    {"sensor", "wifi_signal", "Wi-Fi signal", STATE | DIAGNOSTIC, "\"dev_cla\":\"signal_strength\",\"unit_of_meas\":\"dBm\",\"stat_cla\":\"measurement\""},
    {"sensor", "free_memory", "Free memory", STATE | DIAGNOSTIC, "\"unit_of_meas\":\"kB\",\"stat_cla\":\"measurement\""},
    {"camera", "screen", "Screen", CUSTOM, "\"topic\":\"~/screen\",\"encoding\":\"\""},
    {"sensor", "cpu_usage", "CPU usage", STATE | DIAGNOSTIC, "\"unit_of_meas\":\"%\",\"stat_cla\":\"measurement\",\"icon\":\"mdi:chip\""},
    {"sensor", "display_cpu", "Display refill CPU", STATE | DIAGNOSTIC, "\"unit_of_meas\":\"%\",\"stat_cla\":\"measurement\",\"icon\":\"mdi:led-on\""},
    {"select", "face", "Face", STATE | COMMAND | CONFIG, "\"options\":[\"Home\",\"Football\",\"F1\",\"MotoGP\",\"Tetris\",\"Words\",\"Mario\",\"Luigi\",\"Pac-Man\"],\"icon\":\"mdi:swap-horizontal\""},
#ifdef UM_WLED_OVERLAY
    {"select", "canvas", "Saved picture", COMMAND | CUSTOM, "\"stat_t\":\"~/canvas\",\"icon\":\"mdi:image\""},
#endif
};

// Appends formatted text to a buffer, stopping quietly when it is full
struct Out {
  char *buf;
  size_t size, len = 0;
  void add(const char *format, ...) __attribute__((format(printf, 2, 3))) {
    if (len >= size) return;
    va_list args;
    va_start(args, format);
    int n = vsnprintf(buf + len, size - len, format, args);
    va_end(args);
    if (n > 0) len = min(len + (size_t)n, size - 1);
  }
};

uint32_t mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
uint32_t mixText(uint32_t h, const char *s) {
  for (; *s; s++) h = mix(h, (uint8_t)*s);
  return mix(h, 0xFF);
}
// A fingerprint of everything Home Assistant shows from the settings: a change is published at once
uint32_t signature(const Settings &s) {
  uint32_t h = 2166136261u;
  const uint32_t numbers[] = {s.brightness, s.autoBrightness, s.nightClock, s.minimumBrightness, s.nightEnabled, s.nightSun, s.nightStart,
                              s.nightEnd, s.rotation, s.use24h, s.manualTimezone, s.ldrDark, s.ldrLight, s.swapBlueGreen, s.autoUpdate, s.telnet};
  for (uint32_t n : numbers) h = mix(h, n);
  h = mixText(h, s.timezone.c_str());
  return mixText(h, s.ntpServer.c_str());
}

bool clockMinutes(const char *text, uint16_t &minutes) {
  if (strlen(text) != 5 || text[2] != ':' || !isdigit((unsigned char)text[0]) || !isdigit((unsigned char)text[1]) ||
      !isdigit((unsigned char)text[3]) || !isdigit((unsigned char)text[4])) return false;
  const int hour = (text[0] - '0') * 10 + text[1] - '0', minute = (text[3] - '0') * 10 + text[4] - '0';
  if (hour > 23 || minute > 59) return false;
  minutes = hour * 60 + minute;
  return true;
}
}

// On the heap: the faces have little static RAM to spare
Mqtt &Mqtt::instance() { static Mqtt *value = new Mqtt; return *value; }

bool Mqtt::validHost(const char *host) {
  size_t n = strlen(host);
  if (n == 0 || n >= sizeof(Config::host)) return false;
  for (size_t i = 0; i < n; i++) {
    char c = host[i];
    if (!(isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_')) return false;
  }
  return true;
}

void Mqtt::begin(const char *version, const Hooks &hooks) {
  _hooks = hooks;
  strlcpy(_version, version, sizeof(_version));
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(_id, sizeof(_id), "%02x%02x%02x", mac[3], mac[4], mac[5]);
  snprintf(_base, sizeof(_base), "ultimatrix/%s", _id);
  Preferences p;
  if (!p.begin("ultimatrix", true)) return;
  _cfg.enabled = p.getBool("mqOn", false);
  p.getString("mqHost", _cfg.host, sizeof(_cfg.host));
  _cfg.port = p.getUShort("mqPort", 1883);
  p.getString("mqUser", _cfg.user, sizeof(_cfg.user));
  p.getString("mqPass", _cfg.pass, sizeof(_cfg.pass));
  p.end();
  if (!validHost(_cfg.host)) _cfg.host[0] = 0;
  if (_cfg.port == 0) _cfg.port = 1883;
}

bool Mqtt::save(const Config &next) {
  Preferences p;
  if (!p.begin("ultimatrix", false)) return false;
  bool ok = p.putBool("mqOn", next.enabled) && p.putUShort("mqPort", next.port);  // an empty string stores 0 bytes, so only these count
  p.putString("mqHost", next.host);
  p.putString("mqUser", next.user);
  p.putString("mqPass", next.pass);
  p.end();
  if (!ok) return false;
  _cfg = next;
  _restart = true;
  _error[0] = 0;
  return true;
}

void Mqtt::status(char *out, size_t size) const {
  if (!_cfg.enabled) snprintf(out, size, "Off.");
  else if (!_cfg.host[0]) snprintf(out, size, "Enter the broker address.");
  else if (_connected.load()) snprintf(out, size, "Connected to %s.", _cfg.host);
  else if (_error[0]) snprintf(out, size, "%s", _error);
  else snprintf(out, size, "Connecting to %s…", _cfg.host);
}

void Mqtt::onEvent(void *self, esp_event_base_t, int32_t id, void *data) {
  static_cast<Mqtt *>(self)->handle(id, static_cast<esp_mqtt_event_handle_t>(data));
}

// Runs in the esp-mqtt task: keep it short, and leave the work to poll()
void Mqtt::handle(int32_t id, esp_mqtt_event_handle_t event) {
  switch (id) {
    case MQTT_EVENT_CONNECTED: {
      char filter[40];
      snprintf(filter, sizeof(filter), "%s/+/set", _base);
      esp_mqtt_client_subscribe_single(_client, filter, 0);
      _error[0] = 0;
      _screenSent = false;
      _canvasRefresh = true;
      _step = 1;  // 0 is idle; the discovery messages follow from poll()
      _connected = true;
      break;
    }
    case MQTT_EVENT_DISCONNECTED:
      _connected = false;
      break;
    case MQTT_EVENT_ERROR:
      if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        int code = event->error_handle->connect_return_code;
        strlcpy(_error, code == MQTT_CONNECTION_REFUSE_BAD_USERNAME || code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED
                            ? "The broker refused the username or password." : "The broker refused the connection.", sizeof(_error));
      } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
        strlcpy(_error, "Cannot reach the broker. Check the address and port.", sizeof(_error));
      }
      break;
    case MQTT_EVENT_DATA: {
      if (!event->topic || event->current_data_offset != 0 || event->data_len != event->total_data_len) break;  // one piece only
      const size_t baseLen = strlen(_base);
      if (event->topic_len <= (int)baseLen + 1 || strncmp(event->topic, _base, baseLen) != 0 || event->topic[baseLen] != '/') break;
      const char *key = event->topic + baseLen + 1;
      const int keyLen = event->topic_len - (int)baseLen - 1;
      if (keyLen < 5 || strncmp(key + keyLen - 4, "/set", 4) != 0) break;
      const int nameLen = keyLen - 4;
      if (nameLen == 6 && strncmp(key, "screen", 6) == 0) {
        _screenSent = false;  // a viewer requests the current frame, even if unchanged
        break;
      }
      int entity = -1;
      for (int i = 0; i < E_COUNT; i++)
        if ((kEntities[i].flags & COMMAND) && (int)strlen(kEntities[i].key) == nameLen && strncmp(kEntities[i].key, key, nameLen) == 0) { entity = i; break; }
      if (entity < 0) break;
      char value[sizeof(_noticeText)];
      if (entity == E_NOTIFY && event->data_len >= (int)sizeof(value)) break;  // never treat truncated JSON as a text notice
      const int n = min(event->data_len, (int)sizeof(value) - 1);
      memcpy(value, event->data, n);
      value[n] = 0;
      std::lock_guard<std::mutex> guard(_lock);
      if (entity == E_NOTIFY) { strlcpy(_noticeText, value, sizeof(_noticeText)); _hasNotice = true; }
      else if (_queued < kQueue) {
        _queue[_queued].entity = entity;
        strlcpy(_queue[_queued].value, value, sizeof(_queue[_queued].value));
        _queued++;
      }
      break;
    }
    default: break;
  }
}

void Mqtt::start() {
  char status[40], clientId[24];
  snprintf(status, sizeof(status), "%s/status", _base);
  snprintf(clientId, sizeof(clientId), "ultimatrix-%s", _id);
  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.hostname = _cfg.host;
  cfg.broker.address.port = _cfg.port;
  cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
  if (_cfg.user[0]) {
    cfg.credentials.username = _cfg.user;
    cfg.credentials.authentication.password = _cfg.pass;
  }
  cfg.credentials.client_id = clientId;
  cfg.session.last_will.topic = status;
  cfg.session.last_will.msg = "offline";
  cfg.session.last_will.retain = true;
  cfg.session.keepalive = 60;
  cfg.network.reconnect_timeout_ms = 15000;
  cfg.network.timeout_ms = 5000;
  cfg.task.stack_size = 4096;
  cfg.task.priority = 2;
  cfg.buffer.size = 1024;
  cfg.buffer.out_size = 1024;
  _client = esp_mqtt_client_init(&cfg);  // copies the strings it is given
  if (!_client) { strlcpy(_error, "Not enough memory to connect.", sizeof(_error)); return; }
  esp_mqtt_client_register_event(_client, MQTT_EVENT_ANY, onEvent, this);
  if (esp_mqtt_client_start(_client) != ESP_OK) {
    esp_mqtt_client_destroy(_client);
    _client = nullptr;
    strlcpy(_error, "Could not start the MQTT client.", sizeof(_error));
    return;
  }
  _stateDue = _updateDue = true;
}

void Mqtt::stop() {
  if (!_client) return;
  if (_connected.load()) {  // a clean goodbye does not trigger the last will, so say it
    char status[40];
    snprintf(status, sizeof(status), "%s/status", _base);
    esp_mqtt_client_publish(_client, status, "offline", 0, 0, 1);
  }
  esp_mqtt_client_destroy(_client);
  _client = nullptr;
  _connected = false;
  _step = 0;
}

bool Mqtt::publish(const char *topic, const char *payload, bool retain) {
  return esp_mqtt_client_enqueue(_client, topic, payload, 0, 0, retain, true) >= 0;
}

// The Face select: the label Home Assistant shows and the mode name the updater knows
struct FaceChoice { const char *label, *mode; };
constexpr FaceChoice kFaces[] = {{"Home", "home"}, {"Football", "football"}, {"F1", "f1"}, {"MotoGP", "motogp"}, {"Tetris", "tetris"},
                                 {"Words", "word"}, {"Mario", "mario"}, {"Luigi", "luigi"}, {"Pac-Man", "pacman"}};

// One command from Home Assistant: validated like the settings page does, then applied to the settings (the caller
// notices the change and runs the follow-ups). Bad values are ignored.
void Mqtt::applyCommand(const Command &command, Settings &s) {
  const char *value = command.value;
  const int8_t on = onOff(value);
  int number = atoi(value);
  uint16_t minutes;
  switch (command.entity) {
#ifdef UM_WLED_OVERLAY
    case E_CANVAS: {
      if (!strcmp(value, "Off")) { _canvasWanted = -1; wled::state().on = false; wled::realtimeStop(); wled::lookChanged(); return; }
      const int slot = value[0] - '1';
      char name[17], label[24];
      if (!wled::enabled() || slot < 0 || slot >= wled::kDrawingSlots || !wled::canvas::slotName(slot, name, sizeof(name))) return;
      snprintf(label, sizeof(label), "%d. %s", slot + 1, name);
      if (strcmp(value, label)) return;
      _canvasWanted = slot; _canvasUntil = millis() + 10000;
      wled::state().fx = wled::kPaintEffect; wled::state().on = true;
      wled::options().cycleSec = 0;
      wled::inputActivity(); wled::lookChanged();
      return;
    }
#endif
    case E_FACE:  // an explicit choice only: the face now running is ignored, anything unknown is dropped
      if (!_hooks.switchFace || !strcmp(value, UM_MODE_NAME)) return;
      for (const FaceChoice &face : kFaces) if (!strcmp(value, face.label)) { _hooks.switchFace(face.mode); return; }
      return;
    case E_BRIGHTNESS: s.brightness = constrain(number, 1, 255); break;
    case E_MIN_BRIGHTNESS: s.minimumBrightness = constrain(number, 1, 255); break;
    case E_AUTO_BRIGHTNESS: if (on >= 0) s.autoBrightness = on; break;
    case E_NIGHT_CLOCK: if (on >= 0) s.nightClock = on; break;
    case E_NIGHT_MODE: if (on >= 0) s.nightEnabled = on; break;
    case E_NIGHT_SUN: if (on >= 0) s.nightSun = on; break;
    case E_NIGHT_START: if (clockMinutes(value, minutes)) s.nightStart = minutes; break;
    case E_NIGHT_END: if (clockMinutes(value, minutes)) s.nightEnd = minutes; break;
    case E_ROTATION: if (number >= 0 && number <= 270 && number % 90 == 0) s.rotation = number / 90; break;
    case E_TIME_24H: if (on >= 0) s.use24h = on; break;
    case E_AUTO_TIMEZONE: if (on >= 0) s.manualTimezone = !on; break;
    case E_TIMEZONE:
      if (ultime::posixForZone(value)) { s.timezone = value; s.manualTimezone = true; }
      break;
    case E_NTP_SERVER: {
      bool valid = value[0] != 0;
      for (const char *c = value; *c; c++) valid &= isalnum((unsigned char)*c) || *c == '.' || *c == '-' || *c == ':';
      if (valid) s.ntpServer = value;
      break;
    }
    case E_LDR_DARK: if (number >= 0 && number <= 4094 && number < s.ldrLight) s.ldrDark = number; break;
    case E_LDR_LIGHT: if (number >= 1 && number <= 4095 && number > s.ldrDark) s.ldrLight = number; break;
    case E_SWAP: if (on >= 0) s.swapBlueGreen = on; break;
    case E_AUTO_UPDATE: if (on >= 0) s.autoUpdate = on; break;
    case E_TELNET: if (on >= 0) s.telnet = on; break;
    case E_RESTART: if (_hooks.restart) _hooks.restart(); break;
    case E_CHECK_UPDATE: if (_hooks.checkUpdate) _hooks.checkUpdate(); break;
    case E_UPDATE: if (strcasecmp(value, "INSTALL") == 0 && _hooks.installUpdate) _hooks.installUpdate(); break;
    default: break;
  }
}

void Mqtt::applyCommands(uint32_t now) {
  Command batch[kQueue];
  uint8_t count;
  bool notice;
  char text[sizeof(_noticeText)];
  {
    std::lock_guard<std::mutex> guard(_lock);
    count = _queued;
    memcpy(batch, _queue, sizeof(Command) * count);
    notice = _hasNotice;
    if (notice) memcpy(text, _noticeText, sizeof(text));
    _queued = 0;
    _hasNotice = false;
  }
  if (count) {
    auto &s = *Settings::getInstance();
    const Settings previous = s;
    const uint32_t before = signature(s);
    for (uint8_t i = 0; i < count; i++) applyCommand(batch[i], s);
    if (signature(s) != before) {
      if (_hooks.changed) _hooks.changed(previous);
      _saveAt = (now + kSaveDelayMs) | 1;
      _stateDue = true;
    }
  }
  if (notice) {
    notice::Message message;
    const char *plain = text;
    bool invalidImage = false;
    cJSON *json = text[0] == '{' ? cJSON_Parse(text) : nullptr;
    if (json) {
      auto string = [&](const char *key) -> const char * {
        const cJSON *item = cJSON_GetObjectItem(json, key);
        return cJSON_IsString(item) ? item->valuestring : nullptr;
      };
      const char *body = string("message");
      if (!body) body = string("text");
      plain = body ? body : "";
      if (const char *image = string("image")) {
        if (strlen(image) < sizeof(message.image) && (!strncmp(image, "http://", 7) || !strncmp(image, "https://", 8)))
          strlcpy(message.image, image, sizeof(message.image));
        else { invalidImage = true; consoleLog("[Notice] Image URL must be HTTP(S), at most 256 characters\n"); }
      }
      if (const char *scale = string("scale")) message.imageFill = !strcasecmp(scale, "fill");
      if (const char *title = string("title")) strlcpy(message.title, title, sizeof(message.title));
      if (const char *color = string("color")) message.color = parseColor(color);
      if (const char *priority = string("priority")) message.priority = strcasecmp(priority, "high") == 0 ? notice::PRIORITY_HIGH : notice::PRIORITY_NORMAL;
      const cJSON *seconds = cJSON_GetObjectItem(json, "seconds");
      if (cJSON_IsNumber(seconds)) message.seconds = (uint16_t)constrain((int)seconds->valuedouble, 2, 300);
    } else {  // plain text: "!" first means high priority, "Title|message" splits off a title
      while (*plain == ' ') plain++;
      if (*plain == '!') {
        message.priority = notice::PRIORITY_HIGH;
        plain++;
        while (*plain == ' ') plain++;
      }
      if (const char *bar = strchr(plain, '|')) {
        strlcpy(message.title, plain, min((size_t)(bar - plain) + 1, sizeof(message.title)));
        for (size_t n = strlen(message.title); n > 0 && message.title[n - 1] == ' '; n--) message.title[n - 1] = 0;
        plain = bar + 1;
        while (*plain == ' ') plain++;
      }
    }
    strlcpy(message.text, plain, sizeof(message.text));
    if (!invalidImage || message.title[0] || message.text[0]) notice::show(message, now);
    if (json) cJSON_Delete(json);
  }
  if (_saveAt && (int32_t)(now - _saveAt) >= 0) { _saveAt = 0; Settings::getInstance()->save(); }
}

// One message per call, so a connect never takes long on the clock task. Steps 1..E_COUNT are the discovery messages,
// then the "online" status; after that the state and the update information.
void Mqtt::publishNext(uint32_t now) {
  const uint8_t step = _step.load();
  if (step > E_COUNT + 1) { publishState(now); return; }
  char topic[96];
  if (step == E_COUNT + 1) {
    snprintf(topic, sizeof(topic), "%s/status", _base);
    if (publish(topic, "online", true)) { _step = step + 1; _stateDue = _updateDue = true; }
    return;
  }
  std::unique_ptr<char[]> payload(new (std::nothrow) char[kPayload]);
  if (!payload) return;
  const Entity &e = kEntities[step - 1];
  const IPAddress ip = WiFi.localIP();
  Out out{payload.get(), kPayload};
  out.add("{\"name\":\"%s\"", e.name);
  if ((e.flags & STATE) && !(e.flags & CUSTOM)) out.add(",\"stat_t\":\"~/state\",\"val_tpl\":\"{{value_json.%s}}\"", e.key);
  if (e.flags & COMMAND) out.add(",\"cmd_t\":\"~/%s/set\"", e.key);
#ifdef UM_WLED_OVERLAY
  if (step - 1 == E_CANVAS) {
    out.add(",\"options\":[\"Off\"");
    for (int i = 0; i < wled::kDrawingSlots; i++) {
      char name[17];
      if (wled::canvas::slotName(i, name, sizeof(name))) out.add(",\"%d. %s\"", i + 1, name);
    }
    out.add("]");
  }
#endif
  if (e.extra[0]) out.add(",%s", e.extra);
  if (e.flags & CONFIG) out.add(",\"ent_cat\":\"config\"");
  if (e.flags & DIAGNOSTIC) out.add(",\"ent_cat\":\"diagnostic\"");
  out.add(",\"~\":\"%s\",\"avty_t\":\"~/status\",\"uniq_id\":\"ultimatrix_%s_%s\",", _base, _id, e.key);
  out.add("\"dev\":{\"ids\":[\"ultimatrix_%s\"],\"name\":\"Ultimatrix\",\"mf\":\"Ultimatrix\",\"mdl\":\"%s\",\"sw\":\"%s\",", _id, UM_MODE_NAME, _version);
  out.add("\"cu\":\"http://%u.%u.%u.%u/\"}}", ip[0], ip[1], ip[2], ip[3]);
  snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config", kDiscovery, e.component, _id, e.key);
  if (step - 1 == E_NOTIFY) {  // earlier builds announced the notification as a text entity: clear that
    char old[96];
    snprintf(old, sizeof(old), "%s/text/%s/%s/config", kDiscovery, _id, e.key);
    publish(old, "", true);
  }
  // Written to the socket at once, one message per call: queued messages go out about one a second and are dropped after 30 s,
  // which lost the last entities of a long discovery run
  if (esp_mqtt_client_publish(_client, topic, payload.get(), 0, 0, 1) >= 0) {
    _step = step + 1;
  } else {
    static uint32_t loggedAt = 0;
    if (millis() - loggedAt > 5000) { loggedAt = millis(); consoleLog("[MQTT] Discovery of %s could not be sent (payload %u bytes)\n", e.key, (unsigned)out.len); }
  }
}

// The settings go out as one retained message whenever one changes and once a minute (the diagnostics move); the update
// entity has its own message: when the versions or progress change, and once a minute.
void Mqtt::publishState(uint32_t now) {
  const auto &s = *Settings::getInstance();
  char topic[96];
  const uint32_t sig = signature(s);
  const int light = _hooks.lightLevel ? _hooks.lightLevel() : -1;
  // A moving light reading goes out every 10 s, so the sensor levels can be tuned from Home Assistant
  const uint32_t every = light >= 0 || _hooks.cpuUsage ? kLightEveryMs : kStateEveryMs;
  if (_stateDue || sig != _stateSignature || (int32_t)(now - _stateAt) >= (int32_t)every) {
    std::unique_ptr<char[]> json(new (std::nothrow) char[kPayload]);
    if (!json) return;
    Out out{json.get(), kPayload};
    auto onoff = [](bool value) { return value ? "ON" : "OFF"; };
    out.add("{\"brightness\":%u,\"auto_brightness\":\"%s\",\"night_clock\":\"%s\",\"min_brightness\":%u,", (unsigned)s.brightness,
            onoff(s.autoBrightness), onoff(s.nightClock), (unsigned)s.minimumBrightness);
    out.add("\"night_mode\":\"%s\",\"night_sun\":\"%s\",\"night_start\":\"%02u:%02u\",\"night_end\":\"%02u:%02u\",", onoff(s.nightEnabled),
            onoff(s.nightSun), s.nightStart / 60u, s.nightStart % 60u, s.nightEnd / 60u, s.nightEnd % 60u);
    out.add("\"rotation\":\"%u\",\"time_24h\":\"%s\",\"auto_timezone\":\"%s\",\"timezone\":\"%s\",\"ntp_server\":\"%s\",", (unsigned)s.rotation * 90,
            onoff(s.use24h), onoff(!s.manualTimezone), s.timezone.c_str(), s.ntpServer.c_str());
    out.add("\"ldr_dark\":%u,\"ldr_light\":%u,\"swap_blue_green\":\"%s\",\"auto_update\":\"%s\",\"telnet\":\"%s\",", (unsigned)s.ldrDark,
            (unsigned)s.ldrLight, onoff(s.swapBlueGreen), onoff(s.autoUpdate), onoff(s.telnet));
    if (light >= 0) out.add("\"light_level\":%d,", light);
    else out.add("\"light_level\":null,");
    CpuInfo cpu;
    if (_hooks.cpuUsage) _hooks.cpuUsage(cpu);
    if (cpu.load >= 0) out.add("\"cpu_usage\":%d,", cpu.load);
    else out.add("\"cpu_usage\":null,");
    if (cpu.display >= 0) out.add("\"display_cpu\":%d,", cpu.display);
    else out.add("\"display_cpu\":null,");
    out.add("\"face\":\"%s\",\"wifi_signal\":%d,\"free_memory\":%u}", UM_MODE_NAME, (int)WiFi.RSSI(), (unsigned)(ESP.getFreeHeap() / 1024));
    snprintf(topic, sizeof(topic), "%s/state", _base);
    if (publish(topic, json.get(), true)) { _stateDue = false; _stateAt = now; _stateSignature = sig; }
    return;
  }
  UpdateInfo info = {};
  if (!_hooks.updateInfo) return;
  _hooks.updateInfo(info);
  uint32_t updateSig = mixText(mixText(2166136261u, info.installed), info.latest);
  updateSig = mix(mix(updateSig, info.busy), (uint32_t)info.percent);
  if (_updateDue || updateSig != _updateSignature || (int32_t)(now - _updateAt) >= (int32_t)kStateEveryMs) {
    char json[240], percent[12] = "null";
    if (info.busy && info.percent >= 0) snprintf(percent, sizeof(percent), "%d", info.percent);
    snprintf(json, sizeof(json), "{\"installed_version\":\"%s\",\"latest_version\":\"%s\",\"title\":\"Ultimatrix %s\",\"in_progress\":%s,\"update_percentage\":%s}",
             info.installed, info.latest, UM_MODE_NAME, info.busy ? "true" : "false", percent);
    snprintf(topic, sizeof(topic), "%s/update", _base);
    if (publish(topic, json, true)) { _updateDue = false; _updateAt = now; _updateSignature = updateSig; }
  }
}

// One indexed RGB332 image per MQTT message (192x192 PNG, bounded BMP fallback).
// No permanent framebuffer, no retained image,
// and no image backlog: leave space for commands, notices and GIF uploads.
void Mqtt::publishScreen(uint32_t now) {
  if (!_hooks.screenRow || now - _screenAt < 1000) return;
  _screenAt = now;
  constexpr size_t headerSize = 54 + 256 * 4;  // header plus BGR0 palette
  constexpr size_t rowSize = 64;  // already aligned to four bytes
  constexpr size_t size = headerSize + 64 * rowSize;
  if (esp_mqtt_client_get_outbox_size(_client) != 0) return;
  const size_t free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  // Allow the image and MQTT queue copy, keeping 16 KiB for other work.
  // Each image needs its own block; neither needs the old RGB888-sized block.
  static uint32_t memoryLogAt = 0;
  if (free < 2 * size + 16384 || largest < size + 1024) {
    if (!memoryLogAt || now - memoryLogAt >= 30000) {
      ESP_LOGW("MQTT camera", "Waiting for memory: free=%u largest=%u", (unsigned)free, (unsigned)largest);
      memoryLogAt = now;
    }
    return;
  }
  memoryLogAt = 0;
  uint8_t row[64 * 3];
  const auto index = [](const uint8_t *rgb) -> uint8_t {
    return (rgb[0] & 0xE0) | ((rgb[1] >> 3) & 0x1C) | (rgb[2] >> 6);
  };
  uint32_t hash = 2166136261u;
  for (int y = 63; y >= 0; --y) {
    if (!_hooks.screenRow(y, row)) return;
    for (int x = 0; x < 64; ++x) hash = mix(hash, index(row + x * 3));
  }
  if (_screenSent && hash == _screenSignature) return;
  // Count compressed output first, so the only allocation is its exact size.
  // Cap PNG at 8 KiB; detailed frames or a tight heap keep the 64x64 BMP path.
  const size_t pngSize = screenpng::encode(_hooks.screenRow, nullptr, 0);
  const bool png = pngSize && pngSize <= 8192 && free >= 2 * pngSize + 16384 && largest >= pngSize + 1024;
  size_t imageSize = png ? pngSize : size;
  std::unique_ptr<uint8_t[]> image(new (std::nothrow) uint8_t[imageSize]);
  if (!image) return;
  if (png) {
    imageSize = screenpng::encode(_hooks.screenRow, image.get(), imageSize);
    if (!imageSize) return;
  } else {
    memset(image.get(), 0, headerSize);
    image[0] = 'B'; image[1] = 'M';
    auto put32 = [&](int at, uint32_t value) {
      for (int i = 0; i < 4; ++i) image[at + i] = value >> (i * 8);
    };
    put32(2, size); put32(10, headerSize); put32(14, 40);
    put32(18, 64); put32(22, 64); image[26] = 1; image[28] = 8;
    put32(34, 64 * rowSize);
    put32(46, 256);
    for (unsigned i = 0; i < 256; ++i) {
      image[54 + i * 4] = (i & 3) * 255 / 3;
      image[54 + i * 4 + 1] = ((i >> 2) & 7) * 255 / 7;
      image[54 + i * 4 + 2] = (i >> 5) * 255 / 7;
    }
    for (int y = 63; y >= 0; --y) {
      if (!_hooks.screenRow(y, row)) return;
      uint8_t *dst = image.get() + headerSize + (63 - y) * rowSize;
      for (int x = 0; x < 64; ++x) {
        dst[x] = index(row + x * 3);
      }
    }
  }
  char topic[40];
  snprintf(topic, sizeof(topic), "%s/screen", _base);
  if (esp_mqtt_client_enqueue(_client, topic, reinterpret_cast<const char *>(image.get()), imageSize, 0, false, true) >= 0) {
    _screenSignature = hash;
    _screenSent = true;
  }
}

void Mqtt::poll(uint32_t now, bool online) {
  if (!_cfg.enabled || !online || !_cfg.host[0]) {
    if (_client) stop();
    _restart = false;
    return;
  }
  if (_restart) { stop(); _restart = false; }
  if (!_client) { start(); return; }
  applyCommands(now);
#ifdef UM_WLED_OVERLAY
  if (_canvasWanted >= 0) {
    if (!wled::state().on || !wled::enabled()) _canvasWanted = -1;
    else if (wled::feedsQuiet() && wled::canvas::loadSlot(_canvasWanted)) { _canvasWanted = -1; wled::lookChanged(); }
    else if ((int32_t)(now - _canvasUntil) >= 0) { _canvasWanted = -1; wled::state().on = false; wled::lookChanged(); consoleLog("[MQTT] Could not load saved picture\n"); }
  }
  if (_canvasRefresh.exchange(false)) { _canvasHash = 0; _canvasAt = 0; }
  if (_connected && (!_canvasAt || now - _canvasAt >= 5000)) {
    _canvasAt = now | 1;
    uint32_t hash = 2166136261u;
    char selected[24] = "Off";
    for (int i = 0; i < wled::kDrawingSlots; i++) {
      char name[17];
      if (!wled::canvas::slotName(i, name, sizeof(name))) continue;
      hash = mixText(mix(hash, i), name);
      if (wled::state().on && wled::state().fx == wled::kPaintEffect && wled::canvas::currentSlot() == i)
        snprintf(selected, sizeof(selected), "%d. %s", i + 1, name);
    }
    if (hash != _canvasHash && _step > E_COUNT) { _step = E_CANVAS + 1; _canvasHash = hash; }
    char topic[40]; snprintf(topic, sizeof(topic), "%s/canvas", _base);
    publish(topic, selected, true);
  }
#endif
  if (_connected.load() && _step.load() > 0) {
    publishNext(now);
    if (_step.load() > E_COUNT + 1) publishScreen(now);
  }
}
