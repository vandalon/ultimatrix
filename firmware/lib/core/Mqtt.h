// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once
#include <Arduino.h>
#include <atomic>
#include <mutex>
#include <mqtt_client.h>
#include "Notice.h"
#include "Settings.h"

// Optional link to Home Assistant over MQTT, off until it is switched on. Home Assistant finds the clock by its
// discovery messages: the standard settings (brightness, night mode, display, time, light sensor, updates), a
// notification, restart and update buttons, an update entity for installing new firmware, and two diagnostics.
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
  struct UpdateInfo { char installed[32], latest[32]; bool busy; int percent; };  // percent < 0: not known
  // What the clock does for Home Assistant, supplied by main.cpp (it owns the display, the time and the updater)
  struct Hooks {
    void (*changed)(const Settings &previous);  // settings were changed from outside the settings page: apply the follow-ups
    void (*restart)();
    void (*checkUpdate)();
    void (*installUpdate)();
    void (*updateInfo)(UpdateInfo &out);
  };
  static Mqtt &instance();
  static bool validHost(const char *host);
  void begin(const char *version, const Hooks &hooks);  // loads the saved setup
  const Config &config() const { return _cfg; }
  bool save(const Config &next);  // stores it and reconnects; the clock task only
  // Call every frame: starts and stops the client, applies commands, publishes changes. Cheap while off.
  void poll(uint32_t now, bool online);
  void status(char *out, size_t size) const;  // for the settings page
  bool connected() const { return _connected.load(); }
private:
  struct Command { uint8_t entity; char value[48]; };
  static constexpr uint8_t kQueue = 8;
  Mqtt() = default;
  void start();
  void stop();
  void handle(int32_t id, esp_mqtt_event_handle_t event);
  static void onEvent(void *self, esp_event_base_t, int32_t id, void *data);
  void applyCommands(uint32_t now);
  void applyCommand(const Command &command, Settings &s);
  void publishNext(uint32_t now);
  void publishState(uint32_t now);
  bool publish(const char *topic, const char *payload, bool retain);
  Config _cfg;
  Hooks _hooks = {};
  char _id[8] = "", _base[24] = "", _version[32] = "", _error[48] = "";
  esp_mqtt_client_handle_t _client = nullptr;
  std::atomic<bool> _connected{false};
  std::atomic<uint8_t> _step{0};  // discovery messages still to send after a connect
  std::mutex _lock;
  Command _queue[kQueue];  // commands waiting for poll()
  uint8_t _queued = 0;
  bool _hasNotice = false;
  char _noticeText[notice::kPayloadMax] = "";
  bool _restart = false, _stateDue = true, _updateDue = true;
  uint32_t _saveAt = 0, _stateAt = 0, _updateAt = 0, _stateSignature = 0, _updateSignature = 0;
};
