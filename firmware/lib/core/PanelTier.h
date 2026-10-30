#pragma once
#include <stdint.h>

// The panel's colour quality: 8 bit per channel, 20 MHz I2S clock, at least 100 Hz refresh (the driver merges the lowest
// bit planes until it is reached). There is one level; the clock no longer lowers it when memory runs short.
// The update-only boot has no face and every KB goes to the download, so it takes the light level.
namespace paneltier {

struct Tier {
  uint8_t bits;         // colour depth per channel
  uint8_t mhz;          // I2S clock
  uint8_t minRefresh;   // minimum refresh in Hz
  uint8_t iconPercent;  // boot screens: peak brightness of the icons, higher where the levels are coarser
  uint8_t uFloor;       // boot screens: input level at the bottom of the lit U
  uint8_t gradient;     // boot screens: how much of the icons' light-to-deep colour gradient is kept (percent)
};

constexpr Tier kNormal = {8, 20, 100, 75, 65, 100};
constexpr Tier kUpdateOnly = {7, 8, 100, 100, 140, 25};

}  // namespace paneltier
