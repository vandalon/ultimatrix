// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Wled.h"
#include <CacheBlob.h>
#include <Preferences.h>
#include <Settings.h>
#include <SunTimes.h>

namespace wled {
namespace {
constexpr uint32_t kSaveDelayMs = 8000;  // an app dragging a slider changes the state many times a second: store it once it settles
constexpr uint16_t kVersion = 5;

State current;
Options chosen;
Preset presets[kPresets];
Timer timers[kTimers];
uint32_t saveAt = 0;
bool loaded = false;

// What survives a restart: the look, not the night light or the applied preset
struct Saved {
  uint8_t on, bri, transition, segOn, segBri, fx, sx, ix, pal, c1, c2, c3;
  uint8_t col[3][3];
  char text[kTextLength + 1];
  Options options;
};

// Eased values: brightness in 1/256 steps, colours the same
int32_t shownBri = 0;
int32_t shownCol[3][3];
uint8_t outCol[3][3];
uint32_t lastMs = 0;
bool started = false;

// Playlist runner
int plIndex = 0, plRound = 0;
uint32_t plNextMs = 0;

ClockNow clockValue{0, 0, 0, true};

void persist() {
  Preferences prefs;
  if (!prefs.begin("ultimatrix", false)) return;
  Saved saved{current.on, current.bri, current.transition, current.segOn, current.segBri, current.fx, current.sx, current.ix, current.pal,
              current.c1, current.c2, current.c3, {}, {}, chosen};
  memcpy(saved.col, current.col, sizeof(saved.col));
  memcpy(saved.text, current.name, sizeof(saved.text));
  cacheblob::put(prefs, "wledS", saved, kVersion);
  // NVS is small and shared by every face: presets and timers take space only once there are any
  bool anyPreset = false, anyTimer = false;
  for (const Preset &p : presets) anyPreset = anyPreset || p.used;
  for (const Timer &t : timers) anyTimer = anyTimer || t.used;
  if (anyPreset) cacheblob::put(prefs, "wledP", presets, kVersion); else if (prefs.isKey("wledP")) prefs.remove("wledP");
  if (anyTimer) cacheblob::put(prefs, "wledT", timers, kVersion); else if (prefs.isKey("wledT")) prefs.remove("wledT");
  prefs.end();
}

// Takes a look from a preset into the live state
void applyLook(const Preset &p) {
  current.on = true; current.bri = p.bri;  // a preset always brings the light up: it is the way to start WLED content on a clock face
  if (effectKnown(p.fx)) { if (p.fx != current.fx) restartEffect(); current.fx = p.fx; }
  current.sx = p.sx; current.ix = p.ix; current.pal = p.pal;
  memcpy(current.col, p.col, sizeof(current.col));
  if (p.fx == kPaintEffect && p.ps[0]) canvas::loadSlot(p.ps[0] - 1);  // a preset of the Paint effect remembers its saved drawing
}

bool playlistStep(int index, uint32_t now) {
  const Preset *list = preset(current.pl);
  if (!list || !list->playlist || index >= list->count) return false;
  const Preset *step = preset(list->ps[index]);
  if (step && !step->playlist) { applyLook(*step); current.ps = list->ps[index]; }
  plIndex = index;
  const uint32_t duration = (uint32_t)list->dur[index] * 100;
  plNextMs = now + (duration ? duration : 1000);
  return true;
}

void playlistTick(uint32_t now) {
  if (current.pl < 0 || (int32_t)(now - plNextMs) < 0) return;
  const Preset *list = preset(current.pl);
  if (!list || !list->playlist || !list->count) { current.pl = -1; return; }
  int next = plIndex + 1;
  if (next >= list->count) {
    next = 0;
    ++plRound;
    if (list->repeat && plRound >= list->repeat) {  // finished: apply the end preset, if any
      const int end = list->end;
      current.pl = -1;
      if (end) { if (const Preset *last = preset(end)) { if (!last->playlist) { applyLook(*last); current.ps = end; } } }
      touched();
      return;
    }
  }
  playlistStep(next, now);
}
}  // namespace

State &state() { return current; }
Options &options() { return chosen; }

void load() {
  if (loaded) return;
  loaded = true;
  Preferences prefs;
  if (prefs.begin("ultimatrix", true)) {
    Saved saved;
    if (cacheblob::get(prefs, "wledS", saved, kVersion)) {
      current.bri = saved.bri; current.transition = saved.transition; current.segOn = saved.segOn;
      current.segBri = saved.segBri; current.fx = saved.fx; current.sx = saved.sx; current.ix = saved.ix; current.pal = saved.pal;
      current.c1 = saved.c1; current.c2 = saved.c2; current.c3 = saved.c3;
      memcpy(current.col, saved.col, sizeof(current.col));
      memcpy(current.name, saved.text, sizeof(current.name));
      current.name[kTextLength] = 0;
      chosen = saved.options;
    }
    cacheblob::get(prefs, "wledP", presets, kVersion);
    cacheblob::get(prefs, "wledT", timers, kVersion);
    prefs.end();
  }
  for (Preset &p : presets) { cacheblob::terminate(p.name); if (p.count > kPlaylistSteps) p.count = kPlaylistSteps; }
  if (chosen.bootEffect != 255) current.fx = chosen.bootEffect;
  if (!effectKnown(current.fx)) current.fx = 0;
  current.on = false;  // a restart goes back to the face: WLED content is started again by a command
}

uint32_t stamp = 1;
uint32_t changeStamp() { return stamp; }
void touched() { ++stamp; saveAt = millis() + kSaveDelayMs; if (!saveAt) saveAt = 1; }

void saveIfDue(uint32_t now) {
  if (saveAt && (int32_t)(now - saveAt) >= 0) { saveAt = 0; persist(); }
}

void markChanged() { lookChanged(); }
void lookChanged() { current.ps = -1; current.pl = -1; touched(); }

const Preset *preset(int id) { return id >= 1 && id <= kPresets && presets[id - 1].used ? &presets[id - 1] : nullptr; }

bool savePreset(int id, const char *name) {
  if (id < 1 || id > kPresets) return false;
  Preset &p = presets[id - 1];
  memset(&p, 0, sizeof(p));
  p.used = 1;
  p.on = current.on; p.bri = current.bri; p.fx = current.fx; p.sx = current.sx; p.ix = current.ix; p.pal = current.pal;
  memcpy(p.col, current.col, sizeof(p.col));
  if (current.fx == kPaintEffect && canvas::currentSlot() >= 0) p.ps[0] = canvas::currentSlot() + 1;
  snprintf(p.name, sizeof(p.name), "%s", name && *name ? name : "Preset");
  current.ps = id;
  persist();
  return true;
}

bool savePlaylist(int id, const char *name, const uint8_t *ps, const uint16_t *dur, int count, int repeat, int end) {
  if (id < 1 || id > kPresets || count < 1) return false;
  if (count > kPlaylistSteps) count = kPlaylistSteps;
  Preset &p = presets[id - 1];
  memset(&p, 0, sizeof(p));
  p.used = 1; p.playlist = 1; p.count = count; p.repeat = repeat < 0 ? 0 : repeat > 255 ? 255 : repeat; p.end = end < 0 || end > kPresets ? 0 : end;
  for (int i = 0; i < count; ++i) { p.ps[i] = ps[i]; p.dur[i] = dur[i]; }
  snprintf(p.name, sizeof(p.name), "%s", name && *name ? name : "Playlist");
  persist();
  return true;
}

bool setPreset(int id, const Preset &value) {
  if (id < 1 || id > kPresets) return false;
  presets[id - 1] = value;
  presets[id - 1].used = 1;
  cacheblob::terminate(presets[id - 1].name);
  if (presets[id - 1].count > kPlaylistSteps) presets[id - 1].count = kPlaylistSteps;
  touched();
  return true;
}

bool deletePreset(int id) {
  if (id < 1 || id > kPresets) return false;
  memset(&presets[id - 1], 0, sizeof(Preset));
  if (current.ps == id) current.ps = -1;
  if (current.pl == id) current.pl = -1;
  persist();
  return true;
}

bool applyPreset(int id) {
  const Preset *p = preset(id);
  if (!p) return false;
  if (p->playlist) {
    current.pl = id;
    plRound = 0;
    if (!playlistStep(0, millis())) { current.pl = -1; return false; }
  } else {
    current.pl = -1;
    applyLook(*p);
    current.ps = id;
  }
  touched();
  return true;
}

const Timer *timer(int index) { return index >= 0 && index < kTimers && timers[index].used ? &timers[index] : nullptr; }

bool setTimers(const Timer *list, int count) {
  memset(timers, 0, sizeof(timers));
  for (int i = 0; i < count && i < kTimers; ++i) { timers[i] = list[i]; timers[i].used = 1; }
  persist();
  return true;
}

void timersTick(int minuteOfDay, int weekday, uint32_t) {
  static int lastKey = -1;
  const int key = weekday * 1440 + minuteOfDay;
  if (key == lastKey) return;
  lastKey = key;
  int rise = -1, set = -1;
  auto &settings = *Settings::getInstance();
  for (const Timer &t : timers) {
    if (!t.used || !(t.days & (1 << weekday))) continue;
    int target = t.hour * 60 + t.minute;
    if (t.type) {
      if (rise < 0 && (!settings.hasLocation || sun::times(settings.latitude, settings.longitude, time(nullptr), rise, set) != sun::Result::Normal)) { rise = set = -2; }
      if (rise < 0) continue;  // no location, or a polar day or night
      target = ((t.type == 1 ? rise : set) + t.offset) % 1440;
      if (target < 0) target += 1440;
    }
    if (target != minuteOfDay) continue;
    if (t.action == 0) { current.on = false; lookChanged(); }
    else if (t.action == 1) { current.on = true; lookChanged(); }
    else applyPreset(t.preset);
  }
}

void setClock(int hour, int minute, int second, bool h24) { clockValue = {hour, minute, second, h24}; }
ClockNow clockNow() { return clockValue; }

void sunriseRamp(uint8_t p, uint8_t out[3]) {
  // dark red -> red-orange -> orange -> warm white in thirds
  static const uint8_t stops[4][3] = {{12, 0, 0}, {255, 40, 0}, {255, 140, 20}, {255, 235, 170}};
  const int segment = p / 85, f = (p % 85) * 3;
  const int s = segment > 2 ? 2 : segment;
  for (int c = 0; c < 3; ++c) out[c] = (uint8_t)(stops[s][c] + (((int)stops[s + 1][c] - stops[s][c]) * f >> 8));
}

const uint8_t *displayColor(int slot) { return outCol[slot < 0 || slot > 2 ? 0 : slot]; }

uint16_t advance(uint32_t now) {
  uint32_t dt = started ? now - lastMs : 0;
  if (dt > 200) dt = 200;
  lastMs = now;
  State &s = current;
  bool sunrise = false;
  uint8_t ramp[3] = {0, 0, 0};
  if (s.nl.on) {  // night light: wait the duration, fading to the target on the way unless the mode is instant
    const uint32_t total = (uint32_t)s.nl.duration * 60000UL, elapsed = now - s.nl.startMs;
    if (elapsed >= total) {
      s.bri = s.nl.target;
      if (!s.nl.target) s.on = false;
      s.nl.on = false;
      touched();
    } else if (s.nl.mode != 0) {
      s.bri = (uint8_t)(s.nl.startBri + ((int32_t)s.nl.target - (int32_t)s.nl.startBri) * (int64_t)elapsed / (int64_t)total);
      if (s.nl.mode == 3) { sunrise = true; sunriseRamp((uint8_t)((uint64_t)elapsed * 255 / total), ramp); }
    }
  }
  playlistTick(now);
  const int32_t targetBri = (s.on && s.segOn) ? (int32_t)s.bri * s.segBri * 256 / 255 : 0;
  const uint32_t fade = (uint32_t)(s.once >= 0 ? s.once : s.transition) * 100;
  const int32_t step = fade ? (int32_t)((uint64_t)dt * 65280 / fade) : 0x7FFFFFFF;
  if (!started) { shownBri = targetBri; for (int i = 0; i < 3; ++i) for (int c = 0; c < 3; ++c) shownCol[i][c] = s.col[i][c] << 8; started = true; }
  auto approach = [&](int32_t &value, int32_t target, int32_t by) {
    if (value < target) value = (target - value > by) ? value + by : target;
    else if (value > target) value = (value - target > by) ? value - by : target;
  };
  approach(shownBri, targetBri, step);
  for (int i = 0; i < 3; ++i)
    for (int c = 0; c < 3; ++c) {
      if (sunrise && i == 0) shownCol[0][c] = ramp[c] << 8;  // the wake-up colour follows the clock, not a fade
      else approach(shownCol[i][c], s.col[i][c] << 8, step);
      outCol[i][c] = shownCol[i][c] >> 8;
    }
  bool settled = shownBri == targetBri;
  for (int i = 0; i < 3; ++i) for (int c = 0; c < 3; ++c) settled = settled && shownCol[i][c] == (s.col[i][c] << 8);
  if (settled) s.once = -1;  // a one-off fade ends once the change has arrived
  return (uint16_t)((shownBri + 127) / 255);  // 0..256
}

}  // namespace wled
