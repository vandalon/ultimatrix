// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once
#include <Arduino.h>

// Bluetooth proxy for Home Assistant (UM_BLE_PROXY): the clock listens for Bluetooth LE adverts and forwards them over the
// ESPHome native API (TCP 6053, plain text), the way an ESPHome device with `bluetooth_proxy:` does. Passive scanning only
// (no connections to devices). Off by default; the memory of the Bluetooth stack is kept or handed back at the start, so
// switching it on or off restarts the clock.
namespace btproxy {
bool enabled();                     // switched on in the settings (NVS key btProxy)
bool active();                      // enabled and not an update-only boot: the stack runs
bool setEnabled(bool on);           // false when the settings storage is too full to keep it
void boot();                        // first thing in setup(): gives unused Bluetooth memory back
void begin();                       // starts the stack and the server task (once the face runs)
void setFilter(bool (*allowed)(uint32_t ip));   // network scope check for the client address (lwIP byte order)
const char *state();                // off, starting, idle (no client), paused, scanning
uint32_t sent();                    // adverts forwarded since the start
uint32_t dropped();                 // adverts dropped (queue full, slow client, low memory)
bool clientConnected();
}
