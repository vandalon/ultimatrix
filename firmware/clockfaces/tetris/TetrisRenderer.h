// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
// The Tetris clock's picture. Frame is everything the picture depends on and nothing else (no pointers, no padding),
// so TetrisScreens fills it, Clockface compares it with the last one by memcmp, and draw() paints it from scratch.
#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

namespace tetrisview {

enum Phase : uint8_t { DateFalling, TimeFalling, Idle };

// One digit of the clock: which digit, which of its pieces is falling, and how many rows it has fallen
struct Slot {
  int8_t digit;    // tetris::kBlank for none
  uint8_t piece;   // pieces before this one have landed; the digit's piece count means all have
  uint8_t fall;
};

struct Frame {
  uint8_t phase;
  uint8_t colon;   // idle only: the colon is lit
  int8_t timeX, dateX, separatorX, colonX;
  Slot time[4], date[4];
};
static_assert(sizeof(Frame) == 6 + 24, "Frame must have no padding: it is compared with memcmp");

// Clear the panel and paint the frame; the caller flips the buffer
void draw(MatrixPanel_I2S_DMA &display, const Frame &frame);

}  // namespace tetrisview
