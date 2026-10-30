#pragma once
#include <stdint.h>

// The colon of the time blinks, 1 s on and 1 s off: a fade has only a few whole light steps at a low panel brightness, and its dim end loses its hue.
namespace colon {

inline bool on(uint32_t now) { return now % 2000 < 1000; }
inline uint8_t level(uint32_t now) { return on(now) ? 255 : 0; }  // 0..255

}  // namespace colon
