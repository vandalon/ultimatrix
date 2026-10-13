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
#include "TetrisShapes.h"

namespace tetris {

// Cells of every shape and rotation, in whole blocks right (dx) and up (dy) from the shape's anchor.
const Shape kShapes[kShapeCount][4] = {
    {
        {4, {{0, 0}, {1, 0}, {0, 1}, {1, 1}}},
        {4, {{0, 0}, {1, 0}, {0, 1}, {1, 1}}},
        {4, {{0, 0}, {1, 0}, {0, 1}, {1, 1}}},
        {4, {{0, 0}, {1, 0}, {0, 1}, {1, 1}}},
    },
    {
        {4, {{0, 0}, {1, 0}, {0, 1}, {0, 2}}},
        {4, {{0, 0}, {0, 1}, {1, 1}, {2, 1}}},
        {4, {{1, 0}, {1, 1}, {1, 2}, {0, 2}}},
        {4, {{0, 0}, {1, 0}, {2, 0}, {2, 1}}},
    },
    {
        {4, {{0, 0}, {1, 0}, {1, 1}, {1, 2}}},
        {4, {{0, 0}, {1, 0}, {2, 0}, {0, 1}}},
        {4, {{0, 0}, {0, 1}, {0, 2}, {1, 2}}},
        {4, {{0, 1}, {1, 1}, {2, 1}, {2, 0}}},
    },
    {
        {4, {{0, 0}, {1, 0}, {2, 0}, {3, 0}}},
        {4, {{0, 0}, {0, 1}, {0, 2}, {0, 3}}},
        {4, {{0, 0}, {1, 0}, {2, 0}, {3, 0}}},
        {4, {{0, 0}, {0, 1}, {0, 2}, {0, 3}}},
    },
    {
        {4, {{1, 0}, {0, 1}, {1, 1}, {0, 2}}},
        {4, {{0, 0}, {1, 0}, {1, 1}, {2, 1}}},
        {4, {{1, 0}, {0, 1}, {1, 1}, {0, 2}}},
        {4, {{0, 0}, {1, 0}, {1, 1}, {2, 1}}},
    },
    {
        {4, {{0, 0}, {0, 1}, {1, 1}, {1, 2}}},
        {4, {{1, 0}, {2, 0}, {0, 1}, {1, 1}}},
        {4, {{0, 0}, {0, 1}, {1, 1}, {1, 2}}},
        {4, {{1, 0}, {2, 0}, {0, 1}, {1, 1}}},
    },
    {
        {4, {{0, 0}, {1, 0}, {2, 0}, {1, 1}}},
        {4, {{0, 0}, {0, 1}, {0, 2}, {1, 1}}},
        {4, {{1, 0}, {0, 1}, {1, 1}, {2, 1}}},
        {4, {{1, 0}, {0, 1}, {1, 1}, {1, 2}}},
    },
    {
        {3, {{0, 0}, {1, 0}, {0, 1}}},
        {3, {{0, 0}, {0, 1}, {1, 1}}},
        {3, {{1, 0}, {1, 1}, {0, 1}}},
        {3, {{0, 0}, {1, 0}, {1, 1}}},
    },
};

}  // namespace tetris
