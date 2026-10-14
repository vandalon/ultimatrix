#pragma once
#include <Arduino.h>
#include <Settings.h>

class DisplayControl {
public:
  uint8_t update(const Settings &settings, uint32_t now, bool timeSynced, unsigned localMinute, bool setupScreen,
                 bool faceHandlesNight = false);
  int lightReading() const { return _count ? (int)((_sum + _count / 2) / _count) : -1; }
  // Scheduled night hours, or a room darker than the dark reading: the night clock replaces the face
  bool nightActive() const { return _night || _dark; }
  // Only the dark room (the light sensor): for a face that paints its own night, this is what brings the red night clock
  bool roomDark() const { return _dark; }
  // The sun is down at the clock's location, whatever the night settings say: a face that paints its own night follows this
  bool sunDown() const { return _sunDown; }
private:
  uint32_t _sampleAt = 0;
  static constexpr uint8_t kWindow = 20;  // 20 samples at 100 ms: a 2 s moving average
  uint16_t _samples[kWindow] = {};
  uint32_t _sum = 0;
  uint8_t _count = 0, _next = 0, _pin = 0;
  int _level = -1;  // the auto brightness actually applied: it ignores small sensor noise and eases towards bigger changes
  bool _night = false, _dark = false, _sunDown = false;
};
