// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// The plain night clock: block digits, red by default; Pac-Man uses the chosen border palette.
class NightClock {
public:
  // Draws when the minute or Pac-Man colour changes (or after reset()).
  void draw(MatrixPanel_I2S_DMA &gfx, int hour, int minute, bool use24h);
  void reset() { _shown = -1; }
private:
  int _shown = -1;
};
