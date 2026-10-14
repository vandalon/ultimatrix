// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once
#include <stdint.h>

namespace pacmancolors {

constexpr uint8_t WALL_COUNT = 10, COLOR_COUNT = 11;
// The same RGB565 palettes and ordering as the original Pac-Man settings.
constexpr uint16_t WALLS[WALL_COUNT][2] = {
  {0x0016, 0x3800},  // default: blue walls, dark red night clock
  {0xD4FF, 0xD4FF},  // pastel purple
  {0x9FD3, 0x2A45},  // pastel green
  {0xFDBA, 0x49A8},  // pastel pink
  {0x965F, 0x29E9},  // pastel blue
  {0xFDF2, 0x49C5},  // peach
  {0xFFB2, 0x4A45},  // lemon
  {0xFC60, 0x4940},  // orange
  {0xD800, 0x4000},  // red
  {0x065B, 0x01E8},  // cyan
};
constexpr uint16_t COLORS[COLOR_COUNT] = {
  0, 0x9FD3, 0xFDDF, 0x965F, 0xFFB2, 0xFDF2, 0xD4FF, 0xF800, 0xFC60, 0x07FF, 0xFFFF
};  // default, green, pink, blue, lemon, peach, purple, red, orange, cyan, white

inline uint16_t wall(uint8_t choice) { return WALLS[choice < WALL_COUNT ? choice : 0][0]; }
inline uint16_t night(uint8_t choice) { return WALLS[choice < WALL_COUNT ? choice : 0][1]; }
inline uint16_t sprite(uint8_t choice) { return choice < COLOR_COUNT ? COLORS[choice] : 0; }

// Six choices fit in 21 bits when each ghost/dot is a base-11 digit. This leaves the
// existing settings record intact: flags 0-9 keep their meaning, colours use bits 10-30.
constexpr uint32_t COMBINATIONS = WALL_COUNT * COLOR_COUNT * COLOR_COUNT * COLOR_COUNT * COLOR_COUNT * COLOR_COUNT;
inline uint32_t key(uint8_t border, const uint8_t ghosts[4], uint8_t dots) {
  uint32_t result = border;
  for (int i = 0; i < 4; i++) result = result * COLOR_COUNT + ghosts[i];
  return result * COLOR_COUNT + dots;
}

}  // namespace pacmancolors
