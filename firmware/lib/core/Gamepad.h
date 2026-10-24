// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once
#include <Arduino.h>

// A Bluetooth LE game controller (HID over GATT) that steers Pac-Man Classic. Built into that face only (UM_GAMEPAD).
// The clock is the Bluetooth central: Pair listens for a controller in pairing mode for a minute and bonds with it; a bonded
// controller connects again when it is switched on. Classic Bluetooth controllers (PS4, PS5, Switch Pro) are not supported.
namespace gamepad {
bool enabled();                     // switched on in the settings (takes effect at the next start)
void setEnabled(bool on);
void boot();                        // first thing in setup(): gives unused Bluetooth memory back
void begin();                       // starts the Bluetooth stack (once the face runs)
void poll();                        // call from loop(): starts and stops scans, reconnects
bool take(int &dir);                // true when the controller asked for a new direction (0 up, 1 right, 2 down, 3 left)
void pair();                        // look for a controller in pairing mode for 60 seconds
void forget();                      // drop the controller and everything bonded
const char *state();                // starting, ready, pairing, searching, connecting, connected
const char *name();                 // the connected (or last seen) controller, "" when unknown
int bonded();                       // number of bonded controllers
}
