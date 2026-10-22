// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Clockface.h"

#include <algorithm>
#include <atomic>
#include <math.h>
#include <string.h>
#include "FaceKit.h"
#include "HaClient.h"
#include "NetworkBudget.h"
#include "HaConfig.h"

extern std::atomic<int> firmwareInstallPercent;

using namespace home;

namespace {

constexpr uint32_t kBellMs = 12000, kDoneMs = 10000, kOpenMs = 10000, kOpenRepeatMs = 10 * 60 * 1000;

const char *iconName(uint8_t type) {
  switch (type) {
    case ha::ICON_DOOR: return "DOOR";
    case ha::ICON_WINDOW: return "WINDOW";
    case ha::ICON_GARAGE: return "GARAGE";
    case ha::ICON_WASHER: return "WASHER";
    case ha::ICON_DRYER: return "DRYER";
    case ha::ICON_LIGHTS: return "LIGHTS";
    case ha::ICON_LOCK: return "LOCK";
    default: return "CHARGER";
  }
}

uint8_t smallIconOf(uint8_t type) {
  switch (type) {
    case ha::ICON_DOOR: return SI_DOOR;
    case ha::ICON_WINDOW: return SI_WINDOW;
    case ha::ICON_GARAGE: return SI_GARAGE;
    case ha::ICON_WASHER: return SI_WASHER;
    case ha::ICON_DRYER: return SI_DRYER;
    case ha::ICON_LIGHTS: return SI_BULB;
    case ha::ICON_LOCK: return SI_LOCK;
    case ha::ICON_CHARGER: return SI_PLUG;
    case ha::ICON_PRESENCE: return SI_PRESENCE;
    case ha::ICON_LOW_BATTERY: return SI_LOW_BATTERY;
    case ha::ICON_OFFLINE: return SI_OFFLINE;
    case ha::ICON_LEAK: return SI_LEAK;
    case ha::ICON_BLINDS: return SI_BLINDS;
    case ha::ICON_MEDIA: return SI_MEDIA;
    case ha::ICON_HEATING: return SI_HEATING;
    default: return SI_NONE;
  }
}

// What an icon looks like when something is lit: one colour for what is on (blue), so the bottom row stays calm; only things that need attention have their own
uint8_t litTone(uint8_t type) {
  switch (type) {
    case ha::ICON_OFFLINE: case ha::ICON_LEAK: return T_RED;
    case ha::ICON_LOW_BATTERY: case ha::ICON_LOCK: return T_AMBER;
    default: return T_BLUE;
  }
}

bool isOpenType(uint8_t type) { return type == ha::ICON_DOOR || type == ha::ICON_WINDOW || type == ha::ICON_GARAGE; }
bool isRunType(uint8_t type) { return type == ha::ICON_WASHER || type == ha::ICON_DRYER || type == ha::ICON_CHARGER; }

bool slotUsable(const ha::Slot &slot, bool tile) {
  return slot.entity[0] || (slot.autoMode && ha::autoAvailable(tile, slot.type));
}

// Which energy sensors are in use: the saved ones, with "auto" taken from what the connection found in the Energy dashboard
bool energyOn[ha::E_COUNT] = {};
uint32_t energySeen = 0xffffffff, configSeen = 0xffffffff;

void refreshEnergy() {
  HaClient &client = HaClient::instance();
  if (energySeen == client.energyVersion() && configSeen == ha::version()) return;
  energySeen = client.energyVersion();
  configSeen = ha::version();
  static char ids[ha::E_COUNT][ha::kEntity];  // static: the clock's stack is small
  const bool resolved = client.energyIds(ids);
  const ha::Config &config = ha::config();
  for (int i = 0; i < ha::E_COUNT; i++) {
    const char *id = resolved ? ids[i] : config.energy[i];
    energyOn[i] = id[0] && strcmp(id, "auto") != 0;
  }
}

bool energyConfigured(const ha::Config &, int entity) { return energyOn[entity]; }

// Without a house power sensor the usage is what the house draws from the grid, solar and battery together
bool usageShown(const ha::Config &config) { return energyConfigured(config, ha::E_USAGE) || energyConfigured(config, ha::E_GRID); }

void deriveUsage(ha::Snapshot &now) {
  const ha::Config &config = ha::config();
  if (energyConfigured(config, ha::E_USAGE) || !energyConfigured(config, ha::E_GRID) || !(now.energyOk >> ha::E_GRID & 1)) return;
  float sum = now.energy[ha::E_GRID] * (config.gridImportPositive ? 1 : -1);
  if (energyConfigured(config, ha::E_SOLAR)) {
    if (!(now.energyOk >> ha::E_SOLAR & 1)) return;
    sum += fmaxf(0, now.energy[ha::E_SOLAR]);
  }
  if (energyConfigured(config, ha::E_BATTERY)) {
    if (!(now.energyOk >> ha::E_BATTERY & 1)) return;
    sum += now.energy[ha::E_BATTERY] * (config.batteryDischargePositive ? 1 : -1);
  }
  now.energy[ha::E_USAGE] = fmaxf(0, sum);
  now.energyOk |= 1 << ha::E_USAGE;
}

void kilowatts(char *out, size_t size, bool ok, float value) {
  if (ok) snprintf(out, size, "%.1fKW", fabsf(value));  // always kilowatts: the same unit on every row
  else strlcpy(out, "-", size);
}

void kilowattHours(char *out, size_t size, bool ok, float value) {
  const float kwh = value < 0 ? -value : value;
  if (ok) snprintf(out, size, kwh < 9.95f ? "%.1f" : "%.0f", kwh);  // from 10 up no decimal: a second whole digit would run into the icon
  else strlcpy(out, "-", size);
}

}  // namespace

Clockface::Clockface(MatrixPanel_I2S_DMA *display) : _display(display) {}

void Clockface::setup(ClockTime *dateTime) {
  _dateTime = dateTime;
  _haveDrawn = false;
  _pageIndex = 0;
  _pageSince = millis();
  _haveNow = _haveBefore = false;
  _alert = ALERT_NONE;
  _alertFlags = 0;
  memset(_openAlertAt, 0, sizeof(_openAlertAt));
  feedTrimFn = [](void *) { ha::media::release(); };  // a picture (notice, DLNA) needs the heap: the 8 KB album cover goes back and is fetched again afterwards
  HaClient::instance().begin();
}

void Clockface::raise(uint8_t kind, uint32_t now, const char *line1, const char *line2, uint8_t flags, uint32_t ms) {
  _alert = kind;
  _alertFlags = flags;
  _alertUntil = ms ? now + ms : 0;
  strlcpy(_line1, line1, sizeof(_line1));
  strlcpy(_line2, line2, sizeof(_line2));
}

// Takes in a new result from Home Assistant: the alerts it leads to and what the history keeps
void Clockface::followSnapshot(uint32_t now) {
  HaClient &client = HaClient::instance();
  if (client.state() != HaClient::LIVE) {
    _haveNow = _haveBefore = false;
    if (_alert == ALERT_ALARM) _alert = ALERT_NONE;
    _seenUpdates = client.updates();
    return;
  }
  if (client.updates() == _seenUpdates && _haveNow) return;
  ha::Snapshot value;
  if (!client.snapshot(value)) return;
  _seenUpdates = client.updates();
  _before = _now;
  _haveBefore = _haveNow;
  _now = value;
  deriveUsage(_now);
  _haveNow = true;

  const ha::Config &config = ha::config();
  if (_now.alarm) {
    if (_alert != ALERT_ALARM) raise(ALERT_ALARM, now, "ALARM", "TRIGGERED", config.alarmFlags, 0);
  } else if (_alert == ALERT_ALARM) {
    _alert = ALERT_NONE;
  }
  if (_haveBefore && _now.doorbell && !_before.doorbell && _alert != ALERT_ALARM) raise(ALERT_BELL, now, "DOORBELL", "", config.doorbellFlags, kBellMs);
  if (_alert != ALERT_ALARM && _haveBefore && _before.washerOk && _before.washerCount && _now.washerOk && !_now.washerCount) {
    raise(ALERT_WASHER, now, "WASHER", "DONE", ha::washerAlert().flags, kDoneMs);
  }
  for (int i = 0; i < ha::kIcons; i++) {
    const uint8_t type = config.icons[i].type;
    const bool lit = (_now.iconOk >> i & 1) && _now.iconCount[i] > 0;
    const bool wasLit = _haveBefore && (_before.iconOk >> i & 1) && _before.iconCount[i] > 0;
    if (_alert == ALERT_ALARM) break;
    if (isRunType(type) && wasLit && !lit && (_now.iconOk >> i & 1)) {
      raise(ALERT_WASHER, now, iconName(type), type == ha::ICON_CHARGER ? "CHARGED" : "DONE", config.icons[i].flags, kDoneMs);
    } else if (isOpenType(type) && config.icons[i].alertMins) {
      if (!lit) {
        _openAlertAt[i] = 0;
      } else if (_now.iconAge[i] >= config.icons[i].alertMins && (_openAlertAt[i] == 0 || now - _openAlertAt[i] >= kOpenRepeatMs)) {
        char age[12];
        snprintf(age, sizeof(age), "OPEN %uM", (unsigned)(_now.iconAge[i] > 99 ? 99 : _now.iconAge[i]));
        raise(type == ha::ICON_GARAGE ? ALERT_GARAGE : ALERT_OPEN, now, iconName(type), age, config.icons[i].flags, kOpenMs);
        _openAlertAt[i] = now ? now : 1;
      }
    }
  }
}

// One average per hour, kept in memory: the bars on the Energy page. The newest bar is the hour in progress.
void Clockface::collectHours(int hourKey, int hour24, int minute) {
  if (hourKey != _hourKey) {
    if (_hourKey >= 0) {
      memmove(_use, _use + 1, sizeof(_use) - sizeof(_use[0]));
      memmove(_solar, _solar + 1, sizeof(_solar) - sizeof(_solar[0]));
      _use[kBars - 1] = _solar[kBars - 1] = 0;
    }
    _hourKey = hourKey;
    _useSum = _solarSum = 0;
    _samples = 0;
  }
  // The hours before this one come from Home Assistant's recorder, so the bars are there after a restart too
  HaClient &client = HaClient::instance();
  const uint32_t thisHour = (uint32_t)(time(nullptr) / 3600 * 3600);
  if (client.historyVersion() != _historySeen) {
    client.history(_past);
    _historySeen = _past.version;
    const int late = _past.hour <= thisHour ? (int)((thisHour - _past.hour) / 3600) : 0;  // hours since the answer was made
    for (int i = 0; i < kBars - 1; i++) {
      const int at = ha::kHistoryBars - 1 - (kBars - 1 - i) - late;
      if (at < 0) continue;
      if (_past.useMask >> at & 1) _use[i] = _past.use[at];
      if (_past.solarMask >> at & 1) _solar[i] = _past.solar[at];
    }
  }
  // Today's usage in kWh: the hourly means since midnight plus the hour in progress
  _useToday = 0;
  _useTodayOk = false;
  if (_past.version && _past.hour <= thisHour) {
    const int late = (int)((thisHour - _past.hour) / 3600);
    for (int k = 1; k <= hour24; k++) {
      const int at = ha::kHistoryBars - 1 - k - late;
      if (at >= 0 && (_past.useMask >> at & 1)) { _useToday += _past.use[at]; _useTodayOk = true; }
    }
    // Before the first completed hour there are no bars for today yet.
    if (hour24 == 0 && _haveNow && (_now.energyOk >> ha::E_USAGE & 1)) _useTodayOk = true;
    if (_useTodayOk && _haveNow && (_now.energyOk >> ha::E_USAGE & 1)) _useToday += _now.energy[ha::E_USAGE] * minute / 60.0f;
  }
  if (!_haveNow) return;
  if (_now.energyOk >> ha::E_USAGE & 1) {
    _useSum += _now.energy[ha::E_USAGE];
    _use[kBars - 1] = _samples ? _useSum / (_samples + 1) : _now.energy[ha::E_USAGE];
  }
  if (_now.energyOk >> ha::E_SOLAR & 1) {
    _solarSum += _now.energy[ha::E_SOLAR];
    _solar[kBars - 1] = _samples ? _solarSum / (_samples + 1) : _now.energy[ha::E_SOLAR];
  }
  _samples++;
}

int Clockface::pagesAvailable(uint8_t *pages) const {
  refreshEnergy();
  const ha::Config &config = ha::config();
  int count = 0;
  // The Energy page comes first, so it is what the clock starts on (while no energy entity is set it is left out)
  bool energy = false;
  for (int i = 0; i < ha::E_COUNT; i++) energy |= energyConfigured(config, i);
  if ((config.pages & ha::PAGE_ENERGY) && energy) pages[count++] = SCREEN_ENERGY;
  if (config.pages & ha::PAGE_HOME) pages[count++] = SCREEN_HOME;
  if ((config.pages & ha::PAGE_MEDIA) && _media.count) pages[count++] = SCREEN_MEDIA;  // only while something is playing
  if ((config.pages & ha::PAGE_DEVICES) && (_devices.lowTotal || _devices.offlineTotal)) pages[count++] = SCREEN_DEVICES;  // only while something is low or offline
  if (!count) pages[count++] = SCREEN_HOME;
  return count;
}

void Clockface::fillHome(Frame &frame) const {
  const ha::Config &config = ha::config();
  for (int i = 0; i < ha::kHomeTiles; i++) {
    const ha::Slot &slot = ha::tileSlot(config, i);
    if (slot.type == ha::TILE_NONE) continue;
    const bool ok = _now.tileOk >> i & 1;
    const float value = _now.tile[i];
    char *text = frame.tileText[i];
    switch (slot.type) {
      case ha::TILE_NONE: continue;
      case ha::TILE_WEATHER:
      case ha::TILE_TEMP:
        frame.tileIcon[i] = slot.type == ha::TILE_WEATHER ? TI_WEATHER : TI_THERMOMETER;
        frame.tileTone[i] = slot.type == ha::TILE_WEATHER ? T_SUN : T_ORANGE;
        strlcpy(frame.weather[i], _now.weather[i], sizeof(frame.weather[i]));
        if (ok) snprintf(text, 8, "%d`", (int)lroundf(value)); else strlcpy(text, "-", 8);
        break;
      case ha::TILE_OUTDOOR_HUMIDITY:
      case ha::TILE_HUMIDITY:
        frame.tileIcon[i] = slot.type == ha::TILE_OUTDOOR_HUMIDITY ? TI_OUTDOOR_DROP : TI_DROP; frame.tileTone[i] = T_BLUE;
        if (ok) snprintf(text, 8, "%d%%", (int)lroundf(value)); else strlcpy(text, "-", 8);
        break;
      case ha::TILE_PEOPLE:
        frame.tileIcon[i] = TI_PERSON; frame.tileTone[i] = T_GREEN;
        if (ok) snprintf(text, 8, "%d", (int)lroundf(value)); else strlcpy(text, "-", 8);
        break;
      case ha::TILE_CO2:
        frame.tileIcon[i] = TI_CO2; frame.tileTone[i] = T_GREEN;
        if (ok) snprintf(text, 8, "%u", (unsigned)(uint16_t)lroundf(fminf(9999, fmaxf(0, value)))); else strlcpy(text, "-", 8);
        break;
      case ha::TILE_EV: {  // the charge, plus 1000 when plugged in and 2000 when charging
        const int code = (int)((value + 0.5f) / 1000), charge = (int)lroundf(value - code * 1000);
        frame.tileIcon[i] = code & 2 ? TI_CAR_CHARGING : code & 1 ? TI_CAR_PLUGGED : TI_CAR; frame.tileTone[i] = T_GREEN;
        if (ok && charge <= 100) snprintf(text, 8, "%d%%", charge < 0 ? 0 : charge); else strlcpy(text, "-", 8);
        break;
      }
      case ha::TILE_RAIN:
      case ha::TILE_PRICE:
        frame.tileIcon[i] = slot.type == ha::TILE_RAIN ? TI_RAIN : TI_PRICE;
        frame.tileTone[i] = slot.type == ha::TILE_RAIN ? T_BLUE : T_AMBER;
        if (ok) snprintf(text, 8, slot.type == ha::TILE_RAIN ? (fabsf(value) < 100 ? "%.1f" : "%.0f") : (fabsf(value) < 10 ? "%.2f" : fabsf(value) < 100 ? "%.1f" : "%.0f"), value); else strlcpy(text, "-", 8);
        break;
      case ha::TILE_POWER:
        frame.tileIcon[i] = TI_BOLT; frame.tileTone[i] = T_AMBER;
        if (!ok) strlcpy(text, "-", 8);
        else if (fabsf(value) < 1.0f) snprintf(text, 8, "%dW", (int)fminf(999.0f, fmaxf(0.0f, lroundf(fabsf(value) * 1000))));
        else snprintf(text, 8, "%.1fKW", fabsf(value));
        break;
      default: {
        frame.tileIcon[i] = TI_RING; frame.tileTone[i] = T_MUTE;
        char unit[5];
        strlcpy(unit, slot.unit, sizeof(unit));
        for (char *c = unit; *c; ++c) if (*c >= 'a' && *c <= 'z') *c -= 32;
        if (!ok) strlcpy(text, "-", 8);
        else if (slot.decimals) snprintf(text, 8, "%.1f%s", value, unit);
        else snprintf(text, 8, "%d%s", (int)lroundf(value), unit);
        break;
      }
    }
  }
  frame.iconOk = _now.iconOk;
  memcpy(frame.iconCount, _now.iconCount, sizeof(frame.iconCount));
  for (int i = 0; i < ha::kIcons; i++) {
    const ha::Slot &slot = config.icons[i];
    frame.icon[i] = (slotUsable(slot, false) || ha::hasIconEntity(i)) ? smallIconOf(slot.type) : (uint8_t)SI_NONE;
    const bool lit = (_now.iconOk >> i & 1) && _now.iconCount[i] > 0;
    frame.iconTone[i] = lit ? litTone(slot.type) : (uint8_t)T_DIM;
  }
}

void Clockface::fillEnergy(Frame &frame) const {
  const ha::Config &config = ha::config();
  auto have = [&](int e) { return (_now.energyOk >> e & 1) != 0; };
  auto value = [&](int e) { return _now.energy[e]; };

  if (usageShown(config)) {
    frame.rowIcon[0] = SI_HOUSE; frame.rowTone[0] = T_AMBER;
    kilowatts(frame.left[0], 8, have(ha::E_USAGE), value(ha::E_USAGE));
    if (have(ha::E_USAGE_KWH) || !_useTodayOk) kilowattHours(frame.right[0], 8, have(ha::E_USAGE_KWH), value(ha::E_USAGE_KWH));
    else kilowattHours(frame.right[0], 8, true, _useToday);  // no meter: worked out from the hourly means
  }
  // The middle row is solar; with no solar power it is the battery when that is giving power to the house
  const bool batteryOn = energyConfigured(config, ha::E_BATTERY);
  const float discharge = have(ha::E_BATTERY) ? value(ha::E_BATTERY) * (config.batteryDischargePositive ? 1 : -1) : 0;
  const bool solarOn = energyConfigured(config, ha::E_SOLAR);
  const bool producing = solarOn && have(ha::E_SOLAR) && value(ha::E_SOLAR) >= 0.05f;
  if (batteryOn && ((!solarOn) || (!producing && discharge > 0.05f))) {
    frame.rowIcon[1] = SI_BATTERY; frame.rowTone[1] = T_BATTERY;
    frame.batterySoc = have(ha::E_SOC) ? (uint8_t)fminf(fmaxf(value(ha::E_SOC), 0), 100) : 0;
    kilowatts(frame.left[1], 8, have(ha::E_BATTERY), discharge);
    kilowattHours(frame.right[1], 8, have(ha::E_BATTERY_KWH), value(ha::E_BATTERY_KWH));
  } else if (solarOn) {
    frame.rowIcon[1] = SI_SUN; frame.rowTone[1] = T_SUN;
    kilowatts(frame.left[1], 8, have(ha::E_SOLAR), value(ha::E_SOLAR));
    kilowattHours(frame.right[1], 8, have(ha::E_SOLAR_KWH), value(ha::E_SOLAR_KWH));
  }
  if (energyConfigured(config, ha::E_GRID)) {
    const float grid = have(ha::E_GRID) ? value(ha::E_GRID) * (config.gridImportPositive ? 1 : -1) : 0;  // positive: importing
    frame.rowIcon[2] = SI_POLE;
    frame.rowTone[2] = !have(ha::E_GRID) || fabsf(grid) < 0.05f ? T_MUTE : grid < 0 ? T_GREEN : T_ORANGE;
    kilowatts(frame.left[2], 8, have(ha::E_GRID), grid);
    kilowattHours(frame.right[2], 8, have(ha::E_GRID_KWH), value(ha::E_GRID_KWH));
  }
  float top = 1.0f;
  for (int i = 0; i < kBars; i++) top = fmaxf(top, fmaxf(_use[i], _solar[i]));
  for (int i = 0; i < kBars; i++) {
    if (usageShown(config) && _use[i] > 0) frame.useBars[i] = (uint8_t)fminf(12, fmaxf(1, lroundf(_use[i] / top * 12)));
    if (solarOn && _solar[i] > 0) frame.solarBars[i] = (uint8_t)fminf(12, fmaxf(1, lroundf(_solar[i] / top * 12)));
  }
}

void Clockface::fillStatus(Frame &frame) const {
  frame.page = SCREEN_STATUS;
  if (!ha::configured()) {
    strlcpy(frame.line1, "SET UP", sizeof(frame.line1));
    strlcpy(frame.line2, "IN SETTINGS", sizeof(frame.line2));
    return;
  }
  char status[64];
  HaClient::instance().status(status, sizeof(status));
  if (HaClient::instance().state() == HaClient::FAILED) {
    strlcpy(frame.line1, "NO HA", sizeof(frame.line1));
    strlcpy(frame.line2, strstr(status, "token") ? "BAD TOKEN" : strstr(status, "Template") || strstr(status, "Rejected") ? "BAD SETUP" : "NO ANSWER", sizeof(frame.line2));
  } else {
    strlcpy(frame.line1, "CONNECTING", sizeof(frame.line1));
    strlcpy(frame.line2, "TO HA", sizeof(frame.line2));
  }
}

// Every frame, also while the night clock is on the panel: the alerts and the hourly history carry on
void Clockface::poll() {
  if (!feedsHalted()) HaClient::instance().begin();  // restarts the connection task after a held picture or an update
  refreshEnergy();
  const uint32_t now = millis();
  followSnapshot(now);
  if (_alert != ALERT_NONE && _alertUntil && (int32_t)(now - _alertUntil) >= 0) _alert = ALERT_NONE;
  collectHours(_dateTime->getDay() * 24 + _dateTime->getHour24(), _dateTime->getHour24(), _dateTime->getMinute());
}

void Clockface::fillMedia(Frame &frame, uint32_t now) {
  if (!_media.count) return;
  const ha::media::Player &p = _media.player[0];  // the first of the chosen players that is playing
  strlcpy(frame.title, p.title, sizeof(frame.title));
  strlcpy(frame.artist, p.artist, sizeof(frame.artist));
  // A line wider than the panel scrolls; every new track or room starts from the left
  char key[sizeof(_scrollKey)];
  snprintf(key, sizeof(key), "%s", p.title);
  if (strcmp(key, _scrollKey)) { strlcpy(_scrollKey, key, sizeof(_scrollKey)); _scrollSince = now; }
  if (strlen(p.title) > 13 || strlen(p.artist) > 13) frame.scrollTick = 1 + (now - _scrollSince) / 70 % 60000;
  if (p.duration) {
    const uint32_t played = std::min<uint32_t>(p.duration, p.position + (now - p.at) / 1000);
    frame.progress = 1 + played * kArt / p.duration;
  }
  const uint16_t *cover = nullptr;
  uint16_t id = 0;
  if (ha::media::art(p.picture, &cover, &id) == ha::media::ART_READY) { frame.art = cover; frame.artVersion = id; }
}

void Clockface::fillDevices(Frame &frame, uint32_t now) const {
  int n = 0;
  auto add = [&](const char *name, uint8_t kind) { strlcpy(frame.device[n], name, sizeof(frame.device[n])); frame.deviceKind[n++] = kind; };
  // Seven rows: when they do not all fit the last one counts what is left
  const int total = _devices.lowShown + _devices.offlineShown;
  const int room = total > 7 ? 6 : 7;
  for (int i = 0; i < _devices.lowShown && n < room; i++) add(_devices.low[i], SI_LOW_BATTERY);
  for (int i = 0; i < _devices.offlineShown && n < room; i++) add(_devices.offline[i], SI_OFFLINE);
  const int left = _devices.lowTotal + _devices.offlineTotal - n;
  if (left > 0) { char more[25]; snprintf(more, sizeof(more), "+%u MORE", (unsigned)std::min(left, 99)); add(more, SI_NONE); }
  for (int i = 0; i < n; i++)  // a name wider than the panel scrolls
    if (strlen(frame.device[i]) > 12) { frame.scrollTick = 1 + (now - _devicesSince) / 70 % 60000; break; }
}

void Clockface::update() {
  const uint32_t now = millis();
  if (ha::media::version() != _mediaSeen) { _mediaSeen = ha::media::version(); ha::media::snapshot(_media); }
  if (!_media.count && !_sliding && !_drawn.art) ha::media::release();
  if (ha::devices::version() != _devicesSeen) { _devicesSince = now; _devicesSeen = ha::devices::version(); ha::devices::snapshot(_devices); }

  uint8_t pages[4];
  const int count = pagesAvailable(pages);
  if (_pageIndex >= count) _pageIndex = 0;
  if (count > 1 && now - _pageSince >= (uint32_t)ha::config().pageSecs * 1000) {
    _pageIndex = (_pageIndex + 1) % count;
    _pageSince = now;
  } else if (count == 1) {
    _pageSince = now;
  }

  Frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.colonLevel = (uint8_t)(46 + 209 * (0.5f - 0.5f * cosf(((now / 33 * 33) % 3000) * (2.0f * 3.14159265f / 3000))));
  frame.page = pages[_pageIndex];
  frame.pageCount = count;
  frame.pageIndex = _pageIndex;
  snprintf(frame.time, sizeof(frame.time), _dateTime->is24hFormat() ? "%02d:%02d" : "%d:%02d", _dateTime->getHour(), _dateTime->getMinute());
  static const char *const kMonths[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  const int month = std::min(12, std::max(1, _dateTime->getMonth()));
  snprintf(frame.date, sizeof(frame.date), "%s %d", kMonths[month - 1], std::min(31, std::max(1, _dateTime->getDay())));
  if (const int install = firmwareInstallPercent.load(); install >= 0) {  // only a real install: a check or preparing a face ahead of time leaves the screen alone
    frame.page = SCREEN_STATUS;
    strlcpy(frame.line1, "UPDATING", sizeof(frame.line1));
    snprintf(frame.line2, sizeof(frame.line2), "%d%%", std::min(install, 100));
  } else if (!_haveNow) {
    fillStatus(frame);
  } else {
    if (frame.page == SCREEN_ENERGY) fillEnergy(frame);
    else if (frame.page == SCREEN_MEDIA) fillMedia(frame, now);
    else if (frame.page == SCREEN_DEVICES) fillDevices(frame, now);
    else fillHome(frame);
    if ((ha::config().pages & ha::PAGE_ART_BACKDROP) && frame.page == SCREEN_MEDIA && frame.art) frame.backdrop = 1;  // so a slide to or from it keeps the art still
    if ((ha::config().pages & ha::PAGE_ART_BACKDROP) && frame.page != SCREEN_MEDIA && _media.count) {
      const uint16_t *cover = nullptr;
      uint16_t id = 0;
      if (ha::media::art(_media.player[0].picture, &cover, &id) == ha::media::ART_READY) { frame.art = cover; frame.artVersion = id; frame.backdrop = 1; }
    }
    if (_alert != ALERT_NONE) {
      frame.alert = _alert;
      frame.alertFlash = (_alertFlags & ha::AF_FLASH) != 0;
      frame.alertPhase = (now / (frame.alertFlash ? 250 : 500)) & 1;
      strlcpy(frame.line1, _line1, sizeof(frame.line1));
      strlcpy(frame.line2, _line2, sizeof(frame.line2));
    }
  }
  constexpr uint32_t kSlideMs = 450;
  if (!_sliding && _haveDrawn && canSlide(_drawn, frame)) { _slideFrom = _drawn; _slideStart = now; _sliding = true; }
  if (_sliding) {
    const uint32_t elapsed = now - _slideStart;
    _drawn = frame;
    if (elapsed < kSlideMs) {
      drawSlide(*_display, _slideFrom, frame, facekit::ease(elapsed, kSlideMs));
      _display->flipDMABuffer();
      return;
    }
    _sliding = false;
    _haveDrawn = false;  // the last frame is drawn in full below
  }
  if (_haveDrawn && memcmp(&frame, &_drawn, sizeof(frame)) == 0) return;
  _drawn = frame;
  _haveDrawn = true;
  draw(*_display, frame);
  _display->flipDMABuffer();
}
