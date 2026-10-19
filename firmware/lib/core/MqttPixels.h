// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md.
#pragma once
#include <memory>
#include <mutex>
#include <mqtt_client.h>

class MatrixPanel_I2S_DMA;

// MQTT task assembles bounded messages; the clock task validates and commits whole frames.
class MqttPixels {
public:
  static constexpr size_t kMaxPayload = 12288;
  void cancelReceive();
  void receive(esp_mqtt_event_handle_t event, bool first);
  void poll(uint32_t now, bool enabled);
  bool draw(MatrixPanel_I2S_DMA &display);
  bool active() const { return bool(_frame); }
  bool takeStatus(char *out, size_t size);
private:
  void setStatus(const char *message);
  void apply(const char *json, uint32_t now);
  std::mutex _mutex;
  std::unique_ptr<char[]> _incoming, _pending;
  size_t _size = 0, _received = 0;
  uint32_t _receiveAt = 0, _until = 0;
  bool _timed = false;
  std::unique_ptr<uint16_t[]> _frame;
  char _status[96] = "";
};
