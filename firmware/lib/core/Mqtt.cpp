// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Mqtt.h"
#include <Preferences.h>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_mac.h>
#include <memory>
#include <new>
#include "Notice.h"
#include "Settings.h"

namespace {
constexpr const char *kDiscovery = "homeassistant";
constexpr uint8_t kEntities = 6;
constexpr uint32_t kSaveDelayMs = 30000;   // commands from automations change settings often: store them once they settle
constexpr uint32_t kStateEveryMs = 60000;  // the diagnostics move all the time, so state goes out at most this often
constexpr size_t kPayload = 720;

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

void Mqtt::begin(const char *version) {
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
      auto is = [&](const char *name) { return keyLen == (int)strlen(name) && strncmp(key, name, keyLen) == 0; };
      char value[sizeof(_noticeText)];
      const int n = min(event->data_len, (int)sizeof(value) - 1);
      memcpy(value, event->data, n);
      value[n] = 0;
      std::lock_guard<std::mutex> guard(_lock);
      if (is("brightness/set")) { int b = atoi(value); _brightness = b < 1 ? 1 : b > 255 ? 255 : b; }
      else if (is("auto_brightness/set")) _auto = onOff(value);
      else if (is("night_clock/set")) _night = onOff(value);
      else if (is("notify/set")) { strlcpy(_noticeText, value, sizeof(_noticeText)); _hasNotice = true; }
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
  cfg.buffer.size = 768;
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
  _stateDue = true;
  _lastState[0] = 0;
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

void Mqtt::applyCommands(uint32_t now) {
  int brightness;
  int8_t autoOn, night;
  char text[sizeof(_noticeText)];
  bool notice;
  {
    std::lock_guard<std::mutex> guard(_lock);
    brightness = _brightness; autoOn = _auto; night = _night; notice = _hasNotice;
    if (notice) memcpy(text, _noticeText, sizeof(text));
    _brightness = 0; _auto = _night = -1; _hasNotice = false;
  }
  auto &s = *Settings::getInstance();
  bool changed = false;
  if (brightness > 0 && s.brightness != brightness) { s.brightness = (uint8_t)brightness; changed = true; }
  if (autoOn >= 0 && s.autoBrightness != (autoOn == 1)) { s.autoBrightness = autoOn == 1; changed = true; }
  if (night >= 0 && s.nightClock != (night == 1)) { s.nightClock = night == 1; changed = true; }
  if (changed) { _saveAt = (now + kSaveDelayMs) | 1; _stateDue = true; }
  if (notice) {
    notice::Message message;
    const char *plain = text;
    cJSON *json = text[0] == '{' ? cJSON_Parse(text) : nullptr;
    if (json) {
      auto string = [&](const char *key) -> const char * {
        const cJSON *item = cJSON_GetObjectItem(json, key);
        return cJSON_IsString(item) ? item->valuestring : nullptr;
      };
      const char *body = string("message");
      if (!body) body = string("text");
      plain = body ? body : "";
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
    notice::show(message, now);
    if (json) cJSON_Delete(json);
  }
  if (_saveAt && (int32_t)(now - _saveAt) >= 0) { _saveAt = 0; s.save(); }
}

// One message per call, so a connect never takes long on the clock task. Steps 1..kEntities are the discovery
// messages, then the "online" status, then the state.
void Mqtt::publishNext(uint32_t now) {
  const uint8_t step = _step.load();
  char topic[96], status[40];
  snprintf(status, sizeof(status), "%s/status", _base);
  if (step >= 1 && step <= kEntities) {
    std::unique_ptr<char[]> payload(new (std::nothrow) char[kPayload]);
    if (!payload) return;
    static const char *const kComponent[kEntities] = {"number", "switch", "switch", "notify", "sensor", "sensor"};
    static const char *const kKey[kEntities] = {"brightness", "auto_brightness", "night_clock", "notify", "wifi_signal", "free_memory"};
    static const char *const kBody[kEntities] = {
        "\"name\":\"Brightness\",\"stat_t\":\"~/state\",\"val_tpl\":\"{{value_json.brightness}}\",\"cmd_t\":\"~/brightness/set\","
        "\"min\":1,\"max\":255,\"step\":1,\"mode\":\"slider\"",
        "\"name\":\"Auto brightness\",\"stat_t\":\"~/state\",\"val_tpl\":\"{{value_json.auto}}\",\"cmd_t\":\"~/auto_brightness/set\"",
        "\"name\":\"Night clock\",\"stat_t\":\"~/state\",\"val_tpl\":\"{{value_json.night}}\",\"cmd_t\":\"~/night_clock/set\"",
        "\"name\":\"Notification\",\"cmd_t\":\"~/notify/set\"",
        "\"name\":\"Wi-Fi signal\",\"stat_t\":\"~/state\",\"val_tpl\":\"{{value_json.rssi}}\",\"dev_cla\":\"signal_strength\","
        "\"unit_of_meas\":\"dBm\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\"",
        "\"name\":\"Free memory\",\"stat_t\":\"~/state\",\"val_tpl\":\"{{value_json.heap}}\",\"unit_of_meas\":\"kB\","
        "\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\""};
    const int i = step - 1;
    const IPAddress ip = WiFi.localIP();
    snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config", kDiscovery, kComponent[i], _id, kKey[i]);
    snprintf(payload.get(), kPayload,
             "{%s,\"~\":\"%s\",\"avty_t\":\"~/status\",\"uniq_id\":\"ultimatrix_%s_%s\","
             "\"dev\":{\"ids\":[\"ultimatrix_%s\"],\"name\":\"Ultimatrix\",\"mf\":\"Ultimatrix\",\"mdl\":\"%s\",\"sw\":\"%s\","
             "\"cu\":\"http://%u.%u.%u.%u/\"}}",
             kBody[i], _base, _id, kKey[i], _id, UM_MODE_NAME, _version, ip[0], ip[1], ip[2], ip[3]);
    if (i == 3) {  // earlier builds announced the notification as a text entity: clear that retained announcement
      char old[96];
      snprintf(old, sizeof(old), "%s/text/%s/%s/config", kDiscovery, _id, kKey[i]);
      publish(old, "", true);
    }
    if (publish(topic, payload.get(), true)) _step = step + 1;
    return;
  }
  if (step == kEntities + 1) {
    if (publish(status, "online", true)) { _step = step + 1; _stateDue = true; }
    return;
  }
  // State: at once when a setting changes, and once a minute for the moving diagnostics
  const auto &s = *Settings::getInstance();
  char key[24];
  snprintf(key, sizeof(key), "%u,%d,%d", (unsigned)s.brightness, (int)s.autoBrightness, (int)s.nightClock);
  if (!_stateDue && strcmp(key, _lastState) == 0 && (int32_t)(now - _stateAt) < (int32_t)kStateEveryMs) return;
  char json[112];
  snprintf(json, sizeof(json), "{\"brightness\":%u,\"auto\":\"%s\",\"night\":\"%s\",\"rssi\":%d,\"heap\":%u}",
           (unsigned)s.brightness, s.autoBrightness ? "ON" : "OFF", s.nightClock ? "ON" : "OFF", (int)WiFi.RSSI(),
           (unsigned)(ESP.getFreeHeap() / 1024));
  snprintf(topic, sizeof(topic), "%s/state", _base);
  if (publish(topic, json, true)) {
    _stateDue = false;
    _stateAt = now;
    strlcpy(_lastState, key, sizeof(_lastState));
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
  if (_connected.load() && _step.load() > 0) publishNext(now);
}
