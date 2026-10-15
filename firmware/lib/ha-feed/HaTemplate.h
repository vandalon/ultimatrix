// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "HaConfig.h"

// What the clock asks Home Assistant for: one template, rendered by Home Assistant itself, whose result is a
// short line "H;v;v;..." with one field per slot. The clock never sees the full state list.
namespace ha {

constexpr int kFields = kTiles + kIcons + E_COUNT + 2 + 1;  // the last: local midnight, for the statistics request

// The values behind the face, as of the last result. A missing value (entity unknown or unavailable, slot off) has its bit clear.
struct Snapshot {
  float tile[kTiles];
  float energy[E_COUNT];
  uint16_t iconAge[kIcons];   // minutes since the oldest lit entity changed
  uint8_t iconCount[kIcons];  // entities lit
  uint8_t tileOk, iconOk;
  uint16_t energyOk;
  uint8_t alarm, doorbell;    // entities triggered / on
  char midnight[32];          // the start of today in Home Assistant's time zone, ISO 8601
};

// Writes the template for the saved settings; returns its length, or 0 when it does not fit
size_t buildTemplate(const Config &config, char *out, size_t capacity);
// Reads a rendered result; false when it is not one of ours
bool parseResult(const char *text, size_t length, Snapshot &out);
// The sensors of Home Assistant's own Energy dashboard (the answer to energy/get_prefs) into energy[] slots; true when any was found
// gridTo gets the meters of what the grid receives (production tariffs), comma separated
bool parseEnergyPrefs(const char *json, char (&ids)[E_COUNT][kEntity], char (&gridTo)[kEntity]);
// Today's change of each statistic in a recorder/statistics_during_period answer: values[i] for ids[i]; the bit is set when it has one
uint8_t parseStatistics(const char *json, const char (*ids)[kEntity], int count, float *values);

constexpr int kHistoryBars = 24;
// Hourly means (kW) of one statistic in a recorder answer: out[kHistoryBars - 1] is the hour that starts at lastHourStart (seconds),
// the one before it comes first. Bit i of the result is set when out[i] has a value.
uint32_t parseHourly(const char *json, const char *id, uint32_t lastHourStart, float *out);

}  // namespace ha
