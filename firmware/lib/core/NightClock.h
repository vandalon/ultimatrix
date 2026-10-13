// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// The plain red clock shown at night: hours and minutes in the block digits, nothing else.
class NightClock {
public:
  // Draws only when the minute changed since the last call (or after reset()).
  void draw(MatrixPanel_I2S_DMA &gfx, int hour, int minute, bool use24h);
  void reset() { _shown = -1; }
private:
  int _shown = -1;
};
