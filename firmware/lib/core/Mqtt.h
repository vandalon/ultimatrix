// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once
#include <Arduino.h>
#include <atomic>
#include <mutex>
#include <mqtt_client.h>
#include "Notice.h"

// Optional link to Home Assistant over MQTT, off until it is switched on. Home Assistant finds the clock by its
// discovery messages: brightness, auto brightness, night clock, a notification text and two diagnostics.
// The client runs in the esp-mqtt task and only queues what arrives; poll() applies it on the clock task, so the
// settings are touched from one task only.
class Mqtt {
public:
  struct Config {
    bool enabled = false;
    char host[64] = "";
    uint16_t port = 1883;
    char user[33] = "";
    char pass[65] = "";
  };
  static Mqtt &instance();
  static bool validHost(const char *host);
  void begin(const char *version);  // loads the saved setup
  const Config &config() const { return _cfg; }
  bool save(const Config &next);  // stores it and reconnects; the clock task only
  // Call every frame: starts and stops the client, applies commands, publishes changes. Cheap while off.
  void poll(uint32_t now, bool online);
  void status(char *out, size_t size) const;  // for the settings page
  bool connected() const { return _connected.load(); }
private:
  Mqtt() = default;
  void start();
  void stop();
  void handle(int32_t id, esp_mqtt_event_handle_t event);
  static void onEvent(void *self, esp_event_base_t, int32_t id, void *data);
  void applyCommands(uint32_t now);
  void publishNext(uint32_t now);
  bool publish(const char *topic, const char *payload, bool retain);
  Config _cfg;
  char _id[8] = "", _base[24] = "", _version[32] = "", _error[48] = "";
  esp_mqtt_client_handle_t _client = nullptr;
  std::atomic<bool> _connected{false};
  std::atomic<uint8_t> _step{0};  // discovery messages still to send after a connect
  std::mutex _lock;
  int _brightness = 0;      // commands waiting for poll(); 0 or -1 mean none
  int8_t _auto = -1, _night = -1;
  bool _hasNotice = false;
  char _noticeText[notice::kPayloadMax] = "";
  bool _restart = false, _stateDue = true;
  uint32_t _saveAt = 0, _stateAt = 0;
  char _lastState[24] = "";
};
