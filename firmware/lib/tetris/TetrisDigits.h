// Derived from TetrisMatrixDraw and TetrisNumbers of the TetrisAnimation library,
// https://github.com/toblum/TetrisAnimation
// Copyright (c) 2018 Tobias Blum. All right reserved.
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
// General Public License as published by the Free Software Foundation; either version 2.1 of the License, or
// (at your option) any later version. It is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
// LICENSES/LGPL-2.1.txt. Changes: tables converted to const bytes, split from the drawing code.
#pragma once

#include <stdint.h>

namespace tetris {

// One falling piece of a digit. The digit's rows run 1..16 from the top of its drop; x is in blocks from the digit's left.
struct Piece {
  uint8_t shape;      // 0 square, 1 L, 2 reverse L, 3 I, 4 S, 5 reverse S, 6 T, 7 corner
  uint8_t color;      // index into the renderer's palette
  uint8_t x;          // blocks from the left of the digit
  uint8_t stop;       // row where it comes to rest
  uint8_t rotations;  // quarter turns it has turned by the time it lands
};

struct Digit {
  const Piece *pieces;
  uint8_t count;
};

extern const Digit kDigits[10];
constexpr int kDropRows = 16;  // a digit starts its drop this many rows above where it lands

}  // namespace tetris
