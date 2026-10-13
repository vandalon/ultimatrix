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
#include "TetrisDigits.h"

namespace tetris {

static const Piece kDigit0[] = {
    {2, 5, 4, 16, 0},
    {4, 7, 2, 16, 1},
    {3, 4, 0, 16, 1},
    {6, 6, 1, 16, 1},
    {5, 1, 4, 14, 0},
    {6, 6, 0, 13, 3},
    {5, 1, 4, 12, 0},
    {5, 1, 0, 11, 0},
    {6, 6, 4, 10, 1},
    {6, 6, 0, 9, 1},
    {5, 1, 1, 8, 1},
    {2, 5, 3, 8, 3},
};

static const Piece kDigit1[] = {
    {2, 5, 4, 16, 0},
    {3, 4, 4, 15, 1},
    {3, 4, 5, 13, 3},
    {2, 5, 4, 11, 2},
    {0, 0, 4, 8, 0},
};

static const Piece kDigit2[] = {
    {0, 0, 4, 16, 0},
    {3, 4, 0, 16, 1},
    {1, 2, 1, 16, 3},
    {1, 2, 1, 15, 0},
    {3, 4, 1, 12, 2},
    {1, 2, 0, 12, 1},
    {2, 5, 3, 12, 3},
    {0, 0, 4, 10, 0},
    {3, 4, 1, 8, 0},
    {2, 5, 3, 8, 3},
    {1, 2, 0, 8, 1},
};

static const Piece kDigit3[] = {
    {1, 2, 3, 16, 3},
    {2, 5, 0, 16, 1},
    {3, 4, 1, 15, 2},
    {0, 0, 4, 14, 0},
    {3, 4, 1, 12, 2},
    {1, 2, 0, 12, 1},
    {3, 4, 5, 12, 3},
    {2, 5, 3, 11, 0},
    {3, 4, 1, 8, 0},
    {1, 2, 0, 8, 1},
    {2, 5, 3, 8, 3},
};

static const Piece kDigit4[] = {
    {0, 0, 4, 16, 0},
    {0, 0, 4, 14, 0},
    {3, 4, 1, 12, 0},
    {1, 2, 0, 12, 1},
    {2, 5, 0, 10, 0},
    {2, 5, 3, 12, 3},
    {3, 4, 4, 10, 3},
    {2, 5, 0, 9, 2},
    {3, 4, 5, 10, 1},
};

static const Piece kDigit5[] = {
    {0, 0, 0, 16, 0},
    {2, 5, 2, 16, 1},
    {2, 5, 3, 15, 0},
    {3, 4, 5, 16, 1},
    {3, 4, 1, 12, 0},
    {1, 2, 0, 12, 1},
    {2, 5, 3, 12, 3},
    {0, 0, 0, 10, 0},
    {3, 4, 1, 8, 2},
    {1, 2, 0, 8, 1},
    {2, 5, 3, 8, 3},
};

static const Piece kDigit6[] = {
    {2, 5, 0, 16, 1},
    {5, 1, 2, 16, 1},
    {6, 6, 0, 15, 3},
    {6, 6, 4, 16, 3},
    {5, 1, 4, 14, 0},
    {3, 4, 1, 12, 2},
    {2, 5, 0, 13, 2},
    {3, 4, 2, 11, 0},
    {0, 0, 0, 10, 0},
    {3, 4, 1, 8, 0},
    {1, 2, 0, 8, 1},
    {2, 5, 3, 8, 3},
};

static const Piece kDigit7[] = {
    {0, 0, 4, 16, 0},
    {1, 2, 4, 14, 0},
    {3, 4, 5, 13, 1},
    {2, 5, 4, 11, 2},
    {3, 4, 1, 8, 2},
    {2, 5, 3, 8, 3},
    {1, 2, 0, 8, 1},
};

static const Piece kDigit8[] = {
    {3, 4, 1, 16, 0},
    {6, 6, 0, 16, 1},
    {3, 4, 5, 16, 1},
    {1, 2, 2, 15, 3},
    {4, 7, 0, 14, 0},
    {1, 2, 1, 12, 3},
    {6, 6, 4, 13, 1},
    {2, 5, 0, 11, 1},
    {4, 7, 0, 10, 0},
    {4, 7, 4, 11, 0},
    {5, 1, 0, 8, 1},
    {5, 1, 2, 8, 1},
    {1, 2, 4, 9, 2},
};

static const Piece kDigit9[] = {
    {0, 0, 0, 16, 0},
    {3, 4, 2, 16, 0},
    {1, 2, 2, 15, 3},
    {1, 2, 4, 15, 2},
    {3, 4, 1, 12, 2},
    {3, 4, 5, 12, 3},
    {5, 1, 0, 12, 0},
    {1, 2, 2, 11, 3},
    {5, 1, 4, 9, 0},
    {6, 6, 0, 10, 1},
    {5, 1, 0, 8, 1},
    {6, 6, 2, 8, 2},
};

const Digit kDigits[10] = {
    {kDigit0, 12},
    {kDigit1, 5},
    {kDigit2, 11},
    {kDigit3, 11},
    {kDigit4, 9},
    {kDigit5, 11},
    {kDigit6, 12},
    {kDigit7, 7},
    {kDigit8, 13},
    {kDigit9, 12},
};

}  // namespace tetris
