#include "DisplayControl.h"
#include "SunTimes.h"

uint8_t DisplayControl::update(const Settings &s, uint32_t now, bool synced, unsigned minute, bool setupScreen,
                              bool faceHandlesNight) {
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
    long target = map(sample, s.ldrDark, s.ldrLight, std::min(s.minimumBrightness, s.brightness), s.brightness);
    // Sensor noise would nudge the panel up and down by a level or two: hold still inside a small deadband, ease towards real changes
    long diff = target - _level;
    if (_level < 0 || target <= 1 || target >= 255) _level = target;
    else if (std::abs(diff) > 3) _level += diff / 8 ? diff / 8 : (diff > 0 ? 1 : -1);
    value = _level;
  } else _level = -1;
  if (s.autoBrightness && _count >= kWindow / 2) {
    // Hysteresis: dark below the dark reading, light again a little above it (a tenth of it, so it stays close to the slider's dark value)
    long margin = std::max<long>(5, s.ldrDark / 10), reading = lightReading();
    if (reading < s.ldrDark) _dark = true;
    else if (reading > s.ldrDark + margin) _dark = false;
  } else _dark = false;
  _night = s.nightEnabled && synced && s.nightStart != s.nightEnd &&
      (s.nightStart < s.nightEnd ? minute >= s.nightStart && minute < s.nightEnd : minute >= s.nightStart || minute < s.nightEnd);
  // Sunset to sunrise at the clock's location, once it is known; until then the fixed hours above still apply.
  // The sun moves slowly: worked out once a minute (and again when the location changes), not at the frame rate.
  static bool sunDown = false;
  static long sunMinute = -1;
  static float sunLat = 0, sunLon = 0;
  if (s.hasLocation && synced) {
    const long thisMinute = (long)(time(nullptr) / 60);
    if (thisMinute != sunMinute || s.latitude != sunLat || s.longitude != sunLon) {
      sunDown = sun::isNight(s.latitude, s.longitude, time(nullptr), minute);
      sunMinute = thisMinute; sunLat = s.latitude; sunLon = s.longitude;
    }
  } else sunMinute = -1;
  if (s.nightEnabled && s.nightSun && s.hasLocation && synced) _night = sunDown;
  _clock = s.nightClock;  // switched off: the face stays up at night, still dimmed to the minimum brightness
  _sunDown = s.hasLocation && synced && sunDown;
  // The red night clock follows the minimum brightness; a face that paints its own night keeps its brightness until the room is dark
  if (faceHandlesNight ? _dark : nightActive()) value = std::min(s.minimumBrightness, s.brightness);
  // Keep provisioning QR and startup text readable, even during scheduled dimming.
  if (setupScreen) value = std::max<unsigned>(value, 32);
  return (uint8_t)constrain(value, 1U, 255U);
}
