#include "DisplayControl.h"

uint8_t DisplayControl::update(const Settings &s, uint32_t now, bool synced, unsigned minute, bool setupScreen) {
  unsigned value = s.brightness;
  if (!s.autoBrightness) _count = _next = 0, _sum = 0;  // restart the average when auto brightness is switched back on
  else if (now - _sampleAt >= 100) {
    _sampleAt = now;
    if (s.ldrPin != _pin) { _pin = s.ldrPin; _count = _next = 0; _sum = 0; }
    if (_count == kWindow) _sum -= _samples[_next]; else ++_count;
    _samples[_next] = analogRead(s.ldrPin);
    _sum += _samples[_next];
    _next = (_next + 1) % kWindow;
  }
  if (s.autoBrightness && _count && s.ldrLight > s.ldrDark) {
    long sample = constrain((long)lightReading(), (long)s.ldrDark, (long)s.ldrLight);
    value = map(sample, s.ldrDark, s.ldrLight, std::min(s.minimumBrightness, s.brightness), s.brightness);
  }
  if (s.autoBrightness && _count >= kWindow / 2) {
    // Hysteresis: dark below the dark reading, light again a little above it
    long margin = std::max<long>(10, (s.ldrLight - s.ldrDark) / 20), reading = lightReading();
    if (reading < s.ldrDark) _dark = true;
    else if (reading > s.ldrDark + margin) _dark = false;
  } else _dark = false;
  _night = s.nightEnabled && synced && s.nightStart != s.nightEnd &&
      (s.nightStart < s.nightEnd ? minute >= s.nightStart && minute < s.nightEnd : minute >= s.nightStart || minute < s.nightEnd);
  if (nightActive()) value = s.nightBrightness;  // the red night clock has its own brightness
  // Keep provisioning QR and startup text readable, even during scheduled dimming.
  if (setupScreen) value = std::max<unsigned>(value, 32);
  return (uint8_t)constrain(value, 1U, 255U);
}
