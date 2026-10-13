#pragma once
#include <Arduino.h>
#include <Settings.h>

class DisplayControl {
public:
  uint8_t update(const Settings &settings, uint32_t now, bool timeSynced, unsigned localMinute, bool setupScreen);
  int lightReading() const { return _count ? (int)((_sum + _count / 2) / _count) : -1; }
  // Scheduled night hours, or a room darker than the dark reading: the night clock replaces the face
  bool nightActive() const { return _night || _dark; }
private:
  uint32_t _sampleAt = 0;
  static constexpr uint8_t kWindow = 20;  // 20 samples at 100 ms: a 2 s moving average
  uint16_t _samples[kWindow] = {};
  uint32_t _sum = 0;
  uint8_t _count = 0, _next = 0, _pin = 0;
  bool _night = false, _dark = false;
};
