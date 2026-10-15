// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Clockface.h"

#include <algorithm>
#include <math.h>
#include <string.h>
#include "HaClient.h"
#include "HaConfig.h"

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
    default: return SI_NONE;
  }
}

// What an icon looks like when something is lit: open things are orange, running things blue, lights amber, a secured house green
uint8_t litTone(uint8_t type) {
  switch (type) {
    case ha::ICON_WASHER: case ha::ICON_DRYER: case ha::ICON_CHARGER: return T_BLUE;
    case ha::ICON_LIGHTS: return T_AMBER;
    case ha::ICON_LOCK: return T_GREEN;
    default: return T_ORANGE;
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
  if (ok && fabsf(value) < 1.0f) snprintf(out, size, "%dW", (int)fminf(999, lroundf(fabsf(value) * 1000)));  // under a kilowatt: plain watts
  else if (ok) snprintf(out, size, "%.1fKW", fabsf(value));
  else strlcpy(out, "-", size);
}

void kilowattHours(char *out, size_t size, bool ok, float value) {
  if (ok) snprintf(out, size, "%.1f", value < 0 ? -value : value);
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
  if (_haveBefore && _now.doorbell && !_before.doorbell && _alert != ALERT_ALARM) raise(ALERT_BELL, now, "DOORBELL", "RINGING", config.doorbellFlags, kBellMs);
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
  if (!count) pages[count++] = SCREEN_HOME;
  return count;
}

void Clockface::fillHome(Frame &frame) const {
  const ha::Config &config = ha::config();
  for (int i = 0; i < ha::kTiles; i++) {
    const ha::Slot &slot = config.tiles[i];
    if (!slotUsable(slot, true)) continue;
    const bool ok = _now.tileOk >> i & 1;
    const float value = _now.tile[i];
    char *text = frame.tileText[i];
    switch (slot.type) {
      case ha::TILE_TEMP:
        frame.tileIcon[i] = TI_THERMOMETER; frame.tileTone[i] = T_ORANGE;
        if (ok) snprintf(text, 8, "%d`", (int)lroundf(value)); else strlcpy(text, "-", 8);
        break;
      case ha::TILE_HUMIDITY:
        frame.tileIcon[i] = TI_DROP; frame.tileTone[i] = T_BLUE;
        if (ok) snprintf(text, 8, "%d%%", (int)lroundf(value)); else strlcpy(text, "-", 8);
        break;
      case ha::TILE_PEOPLE:
        frame.tileIcon[i] = TI_PERSON; frame.tileTone[i] = T_GREEN;
        if (ok) snprintf(text, 8, "%d", (int)lroundf(value)); else strlcpy(text, "-", 8);
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
  for (int i = 0; i < ha::kIcons; i++) {
    const ha::Slot &slot = config.icons[i];
    frame.icon[i] = slotUsable(slot, false) ? smallIconOf(slot.type) : (uint8_t)SI_NONE;
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
  refreshEnergy();
  const uint32_t now = millis();
  followSnapshot(now);
  if (_alert != ALERT_NONE && _alertUntil && (int32_t)(now - _alertUntil) >= 0) _alert = ALERT_NONE;
  collectHours(_dateTime->getDay() * 24 + _dateTime->getHour24(), _dateTime->getHour24(), _dateTime->getMinute());
}

void Clockface::update() {
  const uint32_t now = millis();

  uint8_t pages[2];
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
  frame.page = pages[_pageIndex];
  frame.pageCount = count;
  frame.pageIndex = _pageIndex;
  snprintf(frame.time, sizeof(frame.time), _dateTime->is24hFormat() ? "%02d:%02d" : "%d:%02d", _dateTime->getHour(), _dateTime->getMinute());
  snprintf(frame.date, sizeof(frame.date), "%02d.%02d", std::min(99, std::max(0, _dateTime->getDay())), std::min(99, std::max(0, _dateTime->getMonth())));
  if (!_haveNow) {
    fillStatus(frame);
  } else {
    if (frame.page == SCREEN_ENERGY) fillEnergy(frame);
    else fillHome(frame);
    if (_alert != ALERT_NONE) {
      frame.alert = _alert;
      frame.alertFlash = (_alertFlags & ha::AF_FLASH) != 0;
      frame.alertPhase = (now / (frame.alertFlash ? 250 : 500)) & 1;
      strlcpy(frame.line1, _line1, sizeof(frame.line1));
      strlcpy(frame.line2, _line2, sizeof(frame.line2));
    }
  }
  if (_haveDrawn && memcmp(&frame, &_drawn, sizeof(frame)) == 0) return;
  _drawn = frame;
  _haveDrawn = true;
  draw(*_display, frame);
  _display->flipDMABuffer();
}
