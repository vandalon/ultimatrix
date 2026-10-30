// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <stddef.h>
#include <stdint.h>

// The tariff of each hour of today, read from the attributes of the Energy dashboard's price sensor: price integrations list them
// as raw_today (Nord Pool, Energi Data Service: start/value), prices (Frank Energie: from/price; ENTSO-E: time/price), data (EPEX Spot:
// start_time/price_per_kwh), forecast (Zonneplan: start_date/price_tax_included.amount) or a plain list called today. Home Assistant
// renders a template of its own over the sensor (HaClient subscribes to it) whose result is "day|p0,p1,...,p23": the day of the
// month, then the mean price of each local hour (empty when unknown).
namespace ha {
namespace prices {

constexpr int kHours = 24;

struct Snapshot {
  float hour[kHours];
  uint32_t mask;  // bit h: hour has a price
  float next[kHours];  // tomorrow's, once the sensor has published them
  uint32_t nextMask;
  uint8_t day;    // day of the month the hours belong to
};

// The template for a sensor; returns its length, 0 when it does not fit
size_t buildTemplate(const char *entity, char *out, size_t capacity);
void parseResult(const char *text, Snapshot &out);

void publish(const Snapshot &snapshot);  // from the Home Assistant task
bool snapshot(Snapshot &out);            // false until a result has arrived
void clear();                            // no price sensor (or the connection is gone)
uint32_t version();

}  // namespace prices
}  // namespace ha
