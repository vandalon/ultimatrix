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
#include "TetrisDigits.h"

namespace tetris {

constexpr int kShapeCount = 8;

struct Cell {
  uint8_t dx, dy;  // whole blocks to the right and up from the anchor
};

struct Shape {
  uint8_t count;  // 3 for the corner, 4 for the rest
  Cell cells[4];
};

extern const Shape kShapes[kShapeCount][4];  // by shape, then rotation

// The rotation a falling piece shows after it fell "fall" rows. Pieces turn on the way down. The tests run in
// sequence and the later ones win, as in the original, so a piece turning 2 or 3 times spends its first rows at
// the second-to-last rotation.
inline uint8_t rotationAt(const Piece &piece, int fall) {
  uint8_t rotation = piece.rotations;
  const int stop = piece.stop;
  if (rotation == 1) {
    if (fall < stop / 2) rotation = 0;
  }
  if (rotation == 2) {
    if (fall < stop / 3) rotation = 0;
    if (fall < stop / 3 * 2) rotation = 1;
  }
  if (rotation == 3) {
    if (fall < stop / 4) rotation = 0;
    if (fall < stop / 4 * 2) rotation = 1;
    if (fall < stop / 4 * 3) rotation = 2;
  }
  return rotation;
}

}  // namespace tetris
