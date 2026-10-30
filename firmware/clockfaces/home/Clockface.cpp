// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "ColonBlink.h"
#include "Clockface.h"

#include <algorithm>
#include <atomic>
#include <math.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <esp_heap_caps.h>
#include "Console.h"
#include "FaceKit.h"
#include "HaClient.h"
#include "HaPrices.h"
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
  if (ok && fabsf(value) < 9.9995f) snprintf(out, size, "%dW", (int)fminf(9999.0f, lroundf(fabsf(value) * 1000)));  // watts up to 9999: the same unit on the rows that usually show
  else if (ok) snprintf(out, size, "%.1fKW", fabsf(value));
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

// One average per hour of today, kept in memory: the bars on the Energy page (index = hour of the day). The hour in progress is the running mean.
void Clockface::collectHours(int hourKey, int hour24, int minute) {
  _hour24 = hour24;
  if (hourKey / 24 != _dayStamp) {  // a new day starts empty
    const bool nextDay = _dayStamp == hourKey / 24 - 1;
    _dayStamp = hourKey / 24;
    if (nextDay) {
      memcpy(_prevUse, _use, sizeof(_use)); memcpy(_prevSolar, _solar, sizeof(_solar));
      memcpy(_prevBattery, _battery, sizeof(_battery)); memcpy(_prevGrid, _grid, sizeof(_grid));
      memcpy(_prevGridOut, _gridOut, sizeof(_gridOut));
    } else {
      memset(_prevUse, 0, sizeof(_prevUse)); memset(_prevSolar, 0, sizeof(_prevSolar));
      memset(_prevBattery, 0, sizeof(_prevBattery)); memset(_prevGrid, 0, sizeof(_prevGrid));
      for (float &v : _prevGridOut) v = -1.f;
    }
    for (float &v : _gridOut) v = -1.f;
    memset(_use, 0, sizeof(_use));
    memset(_solar, 0, sizeof(_solar));
    memset(_battery, 0, sizeof(_battery));
    memset(_grid, 0, sizeof(_grid));
    _priceMask = 0;
    _hourApplied = -1;
    _hourKey = -1;
  }
  if (hourKey != _hourKey) {
    _hourKey = hourKey;
    _useSum = _solarSum = _batterySum = _gridSum = 0;
    _samples = 0;
  }
  // The hours before this one come from Home Assistant's recorder, so the bars are there after a restart too
  HaClient &client = HaClient::instance();
  const uint32_t thisHour = (uint32_t)(time(nullptr) / 3600 * 3600);
  if (client.historyVersion() != _historySeen) {
    client.history(_past);
    _historySeen = _past.version;
    _hourApplied = -1;
  }
  if (_past.version && _hourApplied != hourKey) {
    _hourApplied = hourKey;
    const int late = _past.hour <= thisHour ? (int)((thisHour - _past.hour) / 3600) : 0;  // hours since the answer was made
    for (int h = 0; h < 24; h++) {  // yesterday's hours that the history still covers
      const int at = ha::kHistoryBars - 1 - (hour24 + 24 - h) + late;
      if (at < 0 || at > ha::kHistoryBars - 2 || h < hour24 + 14) continue;
      if (_past.useMask >> at & 1) _prevUse[h] = _past.use[at];
      if (_past.solarMask >> at & 1) _prevSolar[h] = _past.solar[at];
      if (_past.batteryMask >> at & 1) _prevBattery[h] = _past.battery[at];
      if (_past.gridMask >> at & 1) _prevGrid[h] = _past.grid[at];
      _prevGridOut[h] = _past.gridOutMask >> at & 1 ? _past.gridOut[at] : -1.f;
    }
    for (int h = 0; h < hour24; h++) {
      const int at = ha::kHistoryBars - 1 - (hour24 - h) + late;
      if (at < 0 || at > ha::kHistoryBars - 2) continue;  // the last one was still running when it was asked: keep what was measured here
      if (_past.useMask >> at & 1) _use[h] = _past.use[at];
      if (_past.solarMask >> at & 1) _solar[h] = _past.solar[at];
      if (_past.batteryMask >> at & 1) _battery[h] = _past.battery[at];
      if (_past.gridMask >> at & 1) _grid[h] = _past.grid[at];
      _gridOut[h] = _past.gridOutMask >> at & 1 ? _past.gridOut[at] : -1.f;
    }
  }
  if (client.outlookVersion() != _outlookSeen) {
    _outlookSeen = client.outlookVersion();
    client.outlook(_outlook);
  }
  // The tariff of each hour of today, from the price sensor's attributes
  _priceMask = _priceNextMask = 0;
  ha::prices::Snapshot tariff;
  if (ha::prices::snapshot(tariff) && tariff.day == hourKey / 24) {
    for (int h = 0; h < 24; h++) {
      if (tariff.mask >> h & 1) { _price[h] = tariff.hour[h]; _priceMask |= 1u << h; }
      if (tariff.nextMask >> h & 1) { _priceNext[h] = tariff.next[h]; _priceNextMask |= 1u << h; }
    }
  }
  // Today's usage in kWh: the hourly means since midnight plus the hour in progress
  _useToday = 0;
  _useTodayOk = false;
  if (_past.version && _past.hour <= thisHour) {
    const int late = (int)((thisHour - _past.hour) / 3600);
    for (int k = 1; k <= hour24; k++) {
      const int at = ha::kHistoryBars - 1 - k + late;
      if (at >= 0 && at < ha::kHistoryBars && (_past.useMask >> at & 1)) { _useToday += _past.use[at]; _useTodayOk = true; }
    }
    // Before the first completed hour there are no bars for today yet.
    if (hour24 == 0 && _haveNow && (_now.energyOk >> ha::E_USAGE & 1)) _useTodayOk = true;
    if (_useTodayOk && _haveNow && (_now.energyOk >> ha::E_USAGE & 1)) _useToday += _now.energy[ha::E_USAGE] * minute / 60.0f;
  }
  if (!_haveNow) return;
  if (_now.energyOk >> ha::E_USAGE & 1) {
    _useSum += _now.energy[ha::E_USAGE];
    _use[hour24] = _samples ? _useSum / (_samples + 1) : _now.energy[ha::E_USAGE];
  }
  if (_now.energyOk >> ha::E_SOLAR & 1) {
    _solarSum += _now.energy[ha::E_SOLAR];
    _solar[hour24] = _samples ? _solarSum / (_samples + 1) : _now.energy[ha::E_SOLAR];
  }
  if (_now.energyOk >> ha::E_BATTERY & 1) {
    _batterySum += _now.energy[ha::E_BATTERY] * (ha::config().batteryDischargePositive ? 1 : -1);
    _battery[hour24] = _samples ? _batterySum / (_samples + 1) : _now.energy[ha::E_BATTERY] * (ha::config().batteryDischargePositive ? 1 : -1);
  }
  if (_now.energyOk >> ha::E_GRID & 1) {
    const float sign = ha::config().gridImportPositive ? 1.0f : -1.0f;
    _gridSum += _now.energy[ha::E_GRID] * sign;
    _grid[hour24] = _samples ? _gridSum / (_samples + 1) : _now.energy[ha::E_GRID] * sign;
  }
  _samples++;
}

int Clockface::pagesAvailable(uint8_t *pages) const {
  refreshEnergy();
  const ha::Config &config = ha::config();
  int count = 0;
  // The pages come in the order set under Pages (standard: Energy first, so it is what the clock starts on); a page without data is left out
  bool energy = false;
  for (int i = 0; i < ha::E_COUNT; i++) energy |= energyConfigured(config, i);
  for (const char *letter = ha::pageOrder(); *letter; letter++) {
    if (*letter == 'E' && (config.pages & ha::PAGE_ENERGY) && energy) pages[count++] = SCREEN_ENERGY;
    else if (*letter == 'H' && (config.pages & ha::PAGE_HOME)) pages[count++] = SCREEN_HOME;
    else if (*letter == 'M' && (config.pages & ha::PAGE_MEDIA) && _media.count) pages[count++] = SCREEN_MEDIA;  // only while something is playing
    else if (*letter == 'D' && (config.pages & ha::PAGE_DEVICES) && (_devices.offlineTotal || (!(config.pages & ha::PAGE_DEVICES_OFFLINE) && _devices.lowTotal))) pages[count++] = SCREEN_DEVICES;  // only while something is low or offline
  }
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

  // Up to four rows: the house, the solar panels, the battery and the grid, those that are set up
  const bool batteryOn = energyConfigured(config, ha::E_BATTERY);
  const float discharge = have(ha::E_BATTERY) ? value(ha::E_BATTERY) * (config.batteryDischargePositive ? 1 : -1) : 0;
  const bool solarOn = energyConfigured(config, ha::E_SOLAR);
  int n = 0;
  if (usageShown(config)) {
    frame.rowIcon[n] = SI_HOUSE; frame.rowTone[n] = T_BLUE;
    if (have(ha::E_USAGE)) {  // blue when quiet, towards pink-red at the highest load of the last month (5 kW until that is known)
      const float peak = _outlook.peakDay == HaClient::dayKey(time(nullptr)) && _outlook.peak > 0.5f ? _outlook.peak : 5.0f;
      frame.houseLoad = (uint8_t)fminf(255, fmaxf(1, lroundf(value(ha::E_USAGE) / peak * 255)));
    }
    kilowatts(frame.left[n], 8, have(ha::E_USAGE), value(ha::E_USAGE));
    if (have(ha::E_USAGE_KWH) || !_useTodayOk) kilowattHours(frame.right[n], 8, have(ha::E_USAGE_KWH), value(ha::E_USAGE_KWH));
    else kilowattHours(frame.right[n], 8, true, _useToday);  // no meter: worked out from the hourly means
    n++;
  }
  // Three rows (export graph): the middle one is the sun while it produces, the battery otherwise
  const bool threeRows = ha::exportGraph();
  const bool sunNow = have(ha::E_SOLAR) && value(ha::E_SOLAR) > 0.05f;
  const bool showSolar = solarOn && (!threeRows || sunNow || !batteryOn);
  const bool showBattery = batteryOn && (!threeRows || !showSolar);
  if (showSolar) {
    frame.rowIcon[n] = SI_SUN; frame.rowTone[n] = T_GREEN;
    kilowatts(frame.left[n], 8, have(ha::E_SOLAR), value(ha::E_SOLAR));
    kilowattHours(frame.right[n], 8, have(ha::E_SOLAR_KWH), value(ha::E_SOLAR_KWH));
    n++;
  }
  if (showBattery) {
    frame.rowIcon[n] = SI_BATTERY; frame.rowTone[n] = T_BATTERY;
    if (have(ha::E_BATTERY) && fabsf(discharge) >= 0.05f) {
      frame.batteryFlow = discharge < 0 ? 1 : -1;
      frame.batteryStep = (uint8_t)(millis() / 400 % 5);
    }
    frame.batterySoc = have(ha::E_SOC) ? (uint8_t)fminf(fmaxf(value(ha::E_SOC), 0), 100) : 0;
    kilowatts(frame.left[n], 8, have(ha::E_BATTERY), discharge);
    kilowattHours(frame.right[n], 8, have(ha::E_BATTERY_KWH), value(ha::E_BATTERY_KWH));
    n++;
  }
  int gridRow = -1;
  if (energyConfigured(config, ha::E_GRID)) {
    gridRow = n;
    const float grid = have(ha::E_GRID) ? value(ha::E_GRID) * (config.gridImportPositive ? 1 : -1) : 0;  // positive: importing
    frame.rowIcon[n] = SI_POLE;
    // The pole stays the grid's yellow; the numbers show the direction (amber importing, pastel blue sending out)
    const float out = fmaxf(0, -grid);
    frame.rowTone[n] = T_AMBER;  // the pole is always the grid's yellow, whatever flows
    // The numbers: amber for importing, pastel blue for what is sent out (the power now; the day's total only when that is negative too)
    frame.rowLeftTone[n] = !have(ha::E_GRID) ? T_MUTE : out > 0.05f ? T_PASTEL : T_AMBER;
    frame.rowRightTone[n] = !have(ha::E_GRID) ? T_MUTE : have(ha::E_GRID_KWH) && value(ha::E_GRID_KWH) * (config.gridImportPositive ? 1 : -1) < 0 ? T_PASTEL : T_AMBER;
    kilowatts(frame.left[n], 8, have(ha::E_GRID), grid);
    kilowattHours(frame.right[n], 8, have(ha::E_GRID_KWH), value(ha::E_GRID_KWH));
    n++;
  }
  for (int i = 0; i < n; i++) if (i != gridRow) frame.rowLeftTone[i] = frame.rowRightTone[i] = frame.rowTone[i];
  frame.rowCount = n;
  // 20 hours of the day: from midnight until the evening has to scroll (the hour now sits at the 12th column, at most)
  const int first = _hour24 - kBars / 2;  // the current hour always in the middle; hours before 0:00 and after 23:00 stay empty
  frame.nowCol = (int8_t)(kBars / 2);
  frame.narrowStart = _hour24 >= 21;
  frame.hourFrom = (int8_t)first;
  frame.hourTo = (int8_t)((first + kBars) % 24);
  const int today = HaClient::dayKey(time(nullptr));
  const bool expectOk = _outlook.useDay == today, forecastOk = solarOn && _outlook.solarDay == today, nextOk = _outlook.nextDay == today;
  // The prediction of hour h: today's, or (h > 23) tomorrow's usual weekday, which has no solar forecast
  auto usable = [&](int h) { return h < 24 ? expectOk && (_outlook.useMask >> h & 1) : nextOk && h < 48 && (_outlook.nextUseMask >> (h - 24) & 1); };
  auto useRaw = [&](int h) { return h < 24 ? _outlook.use[h] : _outlook.nextUse[h - 24]; };
  // The prediction follows today: when the hours so far used more or less than predicted, the coming hours are scaled by that, fading out over about 6 hours
  float level = 1.f;
  if (expectOk) {
    float actual = 0, predicted = 0;
    int hours = 0;
    for (int h = 0; h < _hour24; h++) if ((_outlook.useMask >> h & 1) && _use[h] > 0) { actual += _use[h]; predicted += _outlook.use[h]; hours++; }
    if (hours >= 3 && predicted > 0.1f) level = fminf(1.4f, fmaxf(0.7f, actual / predicted));
  }
  auto expectedUse = [&](int h) { return useRaw(h) * (1.f + (level - 1.f) * powf(0.5f, (h - _hour24) / 6.0f)); };
  // The tariff of each hour as cheap / normal / expensive: the lowest and highest third of the hours of today
  float sorted[24];
  int known = 0;
  for (int h = 0; h < 24; h++) if (_priceMask >> h & 1) sorted[known++] = _price[h];
  std::sort(sorted, sorted + known);
  float low = 0, high = 0;
  if (known >= 4) { low = sorted[known / 3]; high = sorted[known - 1 - known / 3]; }
  // With the export graph the power sent out hangs below the bars: what the grid takes, from the battery as far as it delivers, the rest from the solar panels
  const bool exportOn = (threeRows || n <= 3) && energyConfigured(config, ha::E_GRID);  // three rows or fewer always get the export graph
  // Hours before 0:00 (h < 0) are yesterday's late hours
  auto useAt = [&](int h) { return h < 0 ? _prevUse[h + 24] : _use[h]; };
  auto solarAt = [&](int h) { return h < 0 ? _prevSolar[h + 24] : _solar[h]; };
  auto batteryAt = [&](int h) { return h < 0 ? _prevBattery[h + 24] : _battery[h]; };
  auto gridAt = [&](int h) { return h < 0 ? _prevGrid[h + 24] : _grid[h]; };
  auto sentOut = [&](int h, float &fromSolar, float &fromBattery) {
    const float own = h < 0 ? _prevGridOut[h + 24] : _gridOut[h];
    const float out = own >= 0 ? own : fmaxf(0, -gridAt(h));  // the export meter when there is one, else the net power
    fromBattery = batteryOn ? fminf(out, fmaxf(0, batteryAt(h))) : 0;
    fromSolar = out - fromBattery;
  };
  auto expectedOut = [&](int h) {
    if (h > 23) return nextOk && h < 48 && (_outlook.nextOutMask >> (h - 24) & 1) ? _outlook.nextOut[h - 24] : 0.0f;
    if (expectOk && (_outlook.outMask >> h & 1)) return _outlook.out[h];  // the usual export of this weekday from the to-grid meters, as the Energy dashboard shows it
    return forecastOk && expectOk && (_outlook.solarMask >> h & 1) && (_outlook.useMask >> h & 1) ? fmaxf(0, _outlook.solar[h] - expectedUse(h)) : 0.0f;
  };
  // The scale covers the whole day but not the hour in progress, so the bars already drawn do not change size as the day goes on
  float top = 1.0f, maxOut = 0;
  for (int h = 0; h < 24; h++) {
    if (h != _hour24) top = fmaxf(top, fmaxf(_use[h], exportOn ? 0.0f : _solar[h]));
    if (h > _hour24) {
      if (usable(h)) top = fmaxf(top, expectedUse(h));
      if (forecastOk && _outlook.solarMask >> h & 1) top = fmaxf(top, _outlook.solar[h]);
    }
    if (exportOn) {
      float fromSolar, fromBattery;
      sentOut(h, fromSolar, fromBattery);
      maxOut = fmaxf(maxOut, h <= _hour24 ? fromSolar + fromBattery : expectedOut(h));
    }
  }
  for (int h = 24; h < first + kBars; h++) {  // tomorrow's hours at the right edge count for the scale too
    if (usable(h)) top = fmaxf(top, expectedUse(h));
    if (exportOn) maxOut = fmaxf(maxOut, expectedOut(h));
  }
  for (int h = first; h < 0; h++) {  // yesterday's hours on the left of the graph count for the scale too
    top = fmaxf(top, fmaxf(useAt(h), exportOn ? 0.0f : solarAt(h)));
    if (exportOn) {
      float fromSolar, fromBattery;
      sentOut(h, fromSolar, fromBattery);
      maxOut = fmaxf(maxOut, fromSolar + fromBattery);
    }
  }
  int total = barRoom(n);
  auto exact = [](float perKw) { return perKw > 0.f ? 1000.f / (5.f * ceilf(1000.f / perKw / 5.f)) : perKw; };  // a scale of a whole 5 Wh per pixel (rounded down in pixels, so everything still fits): the label is then exact
  float pixelsPerKw = exact(total / top);
  int hang = 0;  // rows under the baseline for the power sent out (and one clear row)
  if (exportOn && maxOut > 0.05f) {
    pixelsPerKw = exact((total - 1) / (top + maxOut));
    hang = std::min(total - 2, (int)ceilf(maxOut * pixelsPerKw));
  }
  auto scaleValue = [&](float perKw) { return perKw > 0.f ? (uint16_t)fminf(9995.f, fmaxf(5.f, 5.f * lroundf(1000.f / perKw / 5.f))) : (uint16_t)0; };  // rounded to 5, so it hardly ever changes while on screen
  frame.scaleSpot = SCALE_CENTRE;
  if (!hang) {  // nothing hangs below: the scale text goes in the top row of the graph, in the middle if no bar reaches it, else the right, else the left corner
    const int w = scaleWidth(scaleValue(pixelsPerKw)), topRow = 61 - total + 1;
    auto colKw = [&](int i) {  // the tallest thing in this column, in kW (the same hour's values the bars are drawn from)
      const int h = first + i;
      if (h > 23) return usable(h) ? expectedUse(h) : 0.f;
      return fmaxf(h <= _hour24 ? fmaxf(usageShown(config) ? useAt(h) : 0.f, solarOn ? solarAt(h) : 0.f) : 0.f,
                          h < _hour24 ? 0.f : fmaxf(usable(h) ? expectedUse(h) : 0.f, forecastOk && (_outlook.solarMask >> h & 1) ? _outlook.solar[h] : 0.f));
    };
    auto clear = [&](int x0) {  // no bar in [x0 - 1, x0 + w] comes within the text's 5 rows and the clear row under it
      for (int i = 0; i < kBars; i++) {
        const int bx = barX(i, !frame.narrowStart), bw = barW(i, !frame.narrowStart);
        if (bx + bw <= x0 - 1 || bx >= x0 + w + 1) continue;
        const float kw = colKw(i);
        if (kw > 0.f && 61 - (int)fmaxf(1, lroundf(kw * pixelsPerKw)) + 1 <= topRow + 5) return false;
      }
      return true;
    };
    if (clear((64 - w) / 2)) frame.scaleSpot = SCALE_CENTRE;
    else if (clear(64 - w)) frame.scaleSpot = SCALE_RIGHT;
    else if (clear(0)) frame.scaleSpot = SCALE_LEFT;
    else {  // none: the graph gives way, and the text takes the right corner
      frame.scaleSpot = SCALE_RIGHT;
      total -= kScaleBand;
      pixelsPerKw = exact(total / top);
    }
  }
  const int room = total - (hang ? hang + 1 : 0);
  frame.baseY = (uint8_t)(61 - (hang ? hang + 1 : 0));
  frame.whPerPx = scaleValue(pixelsPerKw);
  frame.negRows = (uint8_t)hang;
  auto bar = [&](float kw) { return (uint8_t)fminf(room, fmaxf(1, lroundf(kw * pixelsPerKw))); };
  auto hung = [&](float kw) { return (int)fminf(hang, fmaxf(1, lroundf(kw * pixelsPerKw))); };
  for (int i = 0; i < kBars; i++) {
    const int h = first + i;
    if (h > 33) continue;
    if (h >= 0 && known >= 4 && (h < 24 ? _priceMask >> h & 1 : h < 48 && (_priceNextMask >> (h - 24) & 1))) {
      const float price = h < 24 ? _price[h] : _priceNext[h - 24];
      frame.priceBand[i] = low >= high ? 2 : price <= low ? 1 : price >= high ? 3 : 2;
    } else if (h >= 0 && known >= 4) frame.priceBand[i] = 4;  // no price for this hour yet (tomorrow's, before they are published): grey
    if (h <= _hour24) {
      if (usageShown(config) && useAt(h) > 0) frame.useBars[i] = bar(useAt(h));
      float sentSolar = 0, sentBattery = 0;
      if (hang) sentOut(h, sentSolar, sentBattery);
      const float usedSolar = fmaxf(0, solarAt(h) - sentSolar);  // the solar power the house used itself: what was exported is not part of the use
      if (solarOn && solarAt(h) > 0) frame.solarBars[i] = bar(hang && useAt(h) > 0 ? fminf(usedSolar, useAt(h)) : solarAt(h));  // what the house does not use goes below
      if (batteryOn && batteryAt(h) > 0.02f && useAt(h) > 0) {  // the part of the use the battery delivered, between the solar part and the grid part
        const float solar = fminf(useAt(h), hang ? usedSolar : fmaxf(0, solarAt(h))), battery = fminf(useAt(h) - solar, batteryAt(h));
        const int below = solar > 0 ? bar(solar) : 0, upTo = bar(solar + battery);
        frame.batteryBars[i] = (uint8_t)std::max(1, upTo - below);
      }
      if (solarOn && !frame.batteryBars[i] && solarAt(h) > 0 && frame.useBars[i] > 0) {  // the row where solar turns into grid: half green, half amber when the boundary is in the middle of it, never in the top row
        const float px = (hang && useAt(h) > 0 ? fminf(usedSolar, useAt(h)) : solarAt(h)) * pixelsPerKw;
        const int whole = (int)floorf(px);
        if (px - whole >= 0.25f && px - whole < 0.75f && whole + 1 < frame.useBars[i]) {
          frame.solarBars[i] = (uint8_t)whole;
          frame.halfRow[i] = (uint8_t)(whole + 1);
        }
      }
      if (hang) {
        float fromSolar, fromBattery;
        sentOut(h, fromSolar, fromBattery);
        if (fromSolar > 0.02f) frame.exportSolar[i] = (uint8_t)hung(fromSolar);
        if (fromBattery > 0.02f) frame.exportBattery[i] = (uint8_t)std::max(1, hung(fromSolar + fromBattery) - (int)frame.exportSolar[i]);
      }
    }
    if (h >= _hour24) {  // the coming hours, and under the hour in progress: the prediction, dim, with what is measured bright over it
      if (usageShown(config) && usable(h) && useRaw(h) > 0) frame.expectBars[i] = bar(expectedUse(h));
      if (forecastOk && h < 24 && _outlook.solarMask >> h & 1 && _outlook.solar[h] > 0) frame.forecastBars[i] = bar(_outlook.solar[h]);
      if (hang && expectedOut(h) > 0.02f) frame.exportExpect[i] = (uint8_t)hung(expectedOut(h));
    }
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
  ha::media::poll();  // the station name of a radio stream, when Home Assistant has none
  if (!feedsHalted()) HaClient::instance().begin();  // restarts the connection task after a held picture or an update
  refreshEnergy();
  const uint32_t now = millis();
  followSnapshot(now);
  if (_alert != ALERT_NONE && _alertUntil && (int32_t)(now - _alertUntil) >= 0) _alert = ALERT_NONE;
  collectHours(_dateTime->getDay() * 24 + _dateTime->getHour24(), _dateTime->getHour24(), _dateTime->getMinute());
}

namespace {
// The picture for a source name (case ignored); SRC_TEXT when there is none
uint8_t sourceIconFor(const char *source) {
  char name[16];
  size_t n = 0;
  for (; source[n] && n + 1 < sizeof(name); n++) name[n] = tolower((unsigned char)source[n]);
  name[n] = 0;
  auto has = [&](const char *part) { return strstr(name, part) != nullptr; };
  if (has("hdmi")) return SRC_HDMI;
  if (has("optic") || has("toslink") || has("spdif")) return SRC_OPTICAL;
  if (has("line") || has("aux")) return ha::lineInAsPhono() ? SRC_PHONO : SRC_LINE;
  if (has("bluetooth")) return SRC_BLUETOOTH;
  if (has("airplay")) return SRC_AIRPLAY;
  if (has("radio") || has("tunein")) return SRC_RADIO;
  if (has("phono") || has("turntable") || has("vinyl")) return SRC_PHONO;
  if (!strcmp(name, "tv") || !strncmp(name, "tv ", 3)) return SRC_TV;
  return SRC_TEXT;
}
}  // namespace

void Clockface::fillMedia(Frame &frame, uint32_t now) {
  if (!_media.count) return;
  const ha::media::Player &p = _media.player[0];  // the first of the chosen players that is playing
  strlcpy(frame.title, p.title, sizeof(frame.title));
  strlcpy(frame.artist, p.artist, sizeof(frame.artist));
  if (p.station[0] && !p.artist[0] && (!p.title[0] || !strcmp(p.station, p.title))) {  // only the station is known
    frame.title[0] = 0;
    strlcpy(frame.station, p.station, sizeof(frame.station));
    frame.stationOnly = 1;
  }
  // The station of a radio stream: at the top right in the date's place when a title and an artist are both known, else on the
  // second line (alone when there is no artist, taking turns with the artist otherwise); never twice when it is the title
  if (!frame.stationOnly && p.station[0] && strcmp(p.station, p.title)) {
    strlcpy(frame.station, p.station, sizeof(frame.station));
    if (p.title[0] && p.artist[0]) frame.stationTop = 1;
    else frame.showStation = !p.artist[0] || (now / 4000) % 2;
  }
  // A line wider than the panel scrolls; every new track, room or line starts from the left
  char key[sizeof(_scrollKey)];
  snprintf(key, sizeof(key), "%s%u", frame.stationOnly ? p.station : p.title, (unsigned)frame.showStation);
  if (strcmp(key, _scrollKey)) { strlcpy(_scrollKey, key, sizeof(_scrollKey)); _scrollSince = now; }
  if (home::scrolls(frame)) frame.scrollTick = 1 + (now - _scrollSince) / 70 % 60000;
  if (p.duration) {
    const uint32_t played = std::min<uint32_t>(p.duration, p.position + (now - p.at) / 1000);
    frame.progress = 1 + played * kArt / p.duration;
  }
  const uint16_t *cover = nullptr;
  uint16_t id = 0;
  if (ha::media::art(p, (ha::config().pages & ha::PAGE_ART_ONLINE) != 0, (ha::config().pages & ha::PAGE_STATION_LOGO) != 0, &cover, &id) == ha::media::ART_READY) { frame.art = cover; frame.artVersion = id; }
  // Nothing to show but the player's source (a Sonos on its TV input, Line-in...): a picture for it, or its name, and no text bars
  if (!frame.art && p.source[0] && !p.artist[0] && !p.station[0] && (!p.title[0] || !strcasecmp(p.title, p.source))) {
    strlcpy(frame.source, p.source, sizeof(frame.source));
    frame.title[0] = frame.artist[0] = 0;
    frame.progress = 0;
    frame.sourceIcon = sourceIconFor(p.source);
  }
}

// The Device problems page lists every low or offline device, seven rows at a time: after a rest it scrolls a row at a time to the end of
// the list, and only then does the page's time (Seconds per page) start to run
namespace {
constexpr uint32_t kSlideIn = 450;  // the swipe into the page
constexpr uint32_t kDevicesRest = kSlideIn + 2000, kDevicesPixelMs = 150;  // ms after the page was chosen before the list moves, and per pixel (a row in about a second)
constexpr uint32_t kNamesRest = kSlideIn - 400;  // long names start from their beginning; they rest 1.4 s first, so they move a second after the swipe-in
}

int Clockface::deviceRows() const {
  return _devices.lowShown + _devices.offlineShown + (_devices.lowTotal + _devices.offlineTotal > _devices.lowShown + _devices.offlineShown ? 1 : 0);
}

uint32_t Clockface::devicesScrollMs() const {
  const int extra = deviceRows() - 8;
  return extra > 0 ? kDevicesRest + extra * 7 * kDevicesPixelMs : 0;
}

void Clockface::fillDevices(Frame &frame, uint32_t now) const {
  const int rows = deviceRows(), extra = std::max(0, rows - 8);
  int first = 0, dy = 0;
  if (extra && now - _pageSince >= kDevicesRest) {
    const uint32_t pixel = (now - _pageSince - kDevicesRest) / kDevicesPixelMs;  // steadily, a pixel at a time
    if (pixel >= (uint32_t)extra * 7) first = extra;
    else { first = pixel / 7; dy = pixel % 7; }
  }
  frame.deviceDy = dy;
  for (int n = 0; n < (dy ? 9 : 8) && first + n < rows; n++) {
    const int k = first + n;
    uint8_t kind;
    if (k < _devices.offlineShown) { ha::devices::nameAt(_devices.offline, k, frame.device[n], sizeof(frame.device[n])); kind = SI_OFFLINE; }  // offline first: the more serious
    else if (k < _devices.offlineShown + _devices.lowShown) { ha::devices::nameAt(_devices.low, k - _devices.offlineShown, frame.device[n], sizeof(frame.device[n])); kind = SI_LOW_BATTERY; }
    else {  // what did not fit in the lists the clock keeps
      snprintf(frame.device[n], sizeof(frame.device[n]), "+%u MORE", (unsigned)std::min(_devices.lowTotal + _devices.offlineTotal - _devices.lowShown - _devices.offlineShown, 99));
      kind = SI_NONE;
    }
    frame.deviceKind[n] = kind;
  }
  for (int i = 0; i < 9; i++)  // a name wider than the panel scrolls, from its beginning, once the page has rested
    if (strlen(frame.device[i]) > 12) { frame.scrollTick = 1 + (now - _pageSince < kNamesRest ? 0 : (now - _pageSince - kNamesRest) / 70) % 60000; break; }
}

void Clockface::update() {
  const uint32_t now = millis();
  // Stutter diagnostics, at most one line every 2 s: a long gap since the last frame (something else held the loop) or a long frame
  static uint32_t lastFrame = 0;
  struct Timing {
    uint32_t start, gap;
    ~Timing() {
      const uint32_t took = millis() - start;
      static uint32_t told = 0;
      if ((gap > 150 || took > 40) && millis() - told > 2000) { told = millis(); consoleLog("[Home] frame took %u ms, %u ms since the last one (heap free %u, largest block %u)\n", (unsigned)took, (unsigned)gap, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)); }
    }
  } timing{now, lastFrame ? now - lastFrame : 0};
  lastFrame = now;
  if (ha::media::version() != _mediaSeen || ha::hideBareMedia() != _hideBare) {
    _mediaSeen = ha::media::version(); _hideBare = ha::hideBareMedia();
    ha::media::snapshot(_media);
    if (_hideBare) {  // players with nothing but a source (no title, artist or station) are left out
      int kept = 0;
      for (int i = 0; i < _media.count; i++) {
        const ha::media::Player &p = _media.player[i];
        if (p.artist[0] || p.station[0] || (p.title[0] && strcasecmp(p.title, p.source))) _media.player[kept++] = p;
      }
      for (int i = kept; i < _media.count; i++) memset(&_media.player[i], 0, sizeof(ha::media::Player));
      _media.count = kept;
    }
  }
  if (!_media.count && !_sliding && !_drawn.art) ha::media::release();
  if (ha::devices::version() != _devicesSeen) { _devicesSince = now; _devicesSeen = ha::devices::version(); ha::devices::snapshot(_devices); }

  uint8_t pages[4];
  const int count = pagesAvailable(pages);
  if (_pageIndex >= count) _pageIndex = 0;
  const uint32_t pageMs = (uint32_t)ha::config().pageSecs * 1000 + (pages[_pageIndex] == SCREEN_DEVICES ? devicesScrollMs() : 0);  // the list scrolls first
  if (now - _pageSince >= pageMs) {  // with a single page the time starts over, so a long list scrolls again and again
    if (count > 1) _pageIndex = (_pageIndex + 1) % count;
    _pageSince = now;
  }

  Frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.colonLevel = colon::level(now);
  frame.page = pages[_pageIndex];
  frame.thinClock = ha::thinClock();
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
      if (ha::media::art(_media.player[0], (ha::config().pages & ha::PAGE_ART_ONLINE) != 0, (ha::config().pages & ha::PAGE_STATION_LOGO) != 0, &cover, &id) == ha::media::ART_READY) { frame.art = cover; frame.artVersion = id; frame.backdrop = 1; }
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
