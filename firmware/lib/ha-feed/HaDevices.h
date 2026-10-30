// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "HaConfig.h"

// The Devices page: the names of the devices with a low battery (20% or less, or a battery sensor that says low) and of the
// devices that are offline. Home Assistant renders a small template of its own (HaClient subscribes to it) whose result
// is "name;name~name;name": low batteries, then offline devices.
namespace ha {
namespace devices {

constexpr int kName = 25, kLowPool = 300, kOfflinePool = 1100;  // characters per name; bytes for the names of each list, packed as "name;name"

struct Snapshot {
  char low[kLowPool], offline[kOfflinePool];
  uint8_t lowShown, offlineShown;     // names kept (the ones that fit the pool)
  uint8_t lowTotal, offlineTotal;     // devices found
};

// The k-th name (from 0) of a packed list
bool nameAt(const char *pool, int k, char *out, size_t size);

// The template; returns its length, 0 when it does not fit
size_t buildTemplate(char *out, size_t capacity);
void parseResult(const char *text, Snapshot &out);

void publish(const Snapshot &snapshot);   // from the Home Assistant task
bool snapshot(Snapshot &out);             // false until a result has arrived
uint32_t version();

}  // namespace devices
}  // namespace ha
