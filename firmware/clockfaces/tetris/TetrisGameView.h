// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

// The picture of the playable Tetris: the board on the left, the time, the date, the next piece, the score and the
// level in the column on the right. Painted from scratch on every frame; the caller flips the buffer.

#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "TetrisGame.h"

namespace tetrisgame {

struct Clock {
  int hour, minute, day, month;  // hour as the clock shows it; month 1..12
};

void draw(MatrixPanel_I2S_DMA &display, const Game &game, const Clock &clock);

}  // namespace tetrisgame
