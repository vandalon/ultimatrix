// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
// The pixel typeface of the Time in Words face: lowercase letters five pixels high with ascenders and descenders, and the
// capitals of the date line. Accented letters are built from a base letter and a mark. The words are drawn at one or
// two times the size; the double size is smoothed (Scale2x) so that its corners are not blocky.
#pragma once

#include <stdint.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

namespace wordfont {

// Rows -2 to 6 around the x-height rows 0 to 4, as bits (bit 0 is the left column)
struct Bitmap {
  uint8_t width;
  uint8_t rows[9];
  bool at(int x, int row) const { return x >= 0 && x < width && row >= -2 && row <= 6 && (rows[row + 2] >> x & 1); }
};

// The next code point of a UTF-8 string, which is advanced; 0 at the end
uint32_t next(const char *&s);
// False for a letter the font does not have (it is then drawn as a '?')
bool glyph(uint32_t codepoint, bool capital, Bitmap &out);

// Width of a string in pixels at a scale of 1 or 2, and the rows it occupies (-2..6)
int width(const char *s, bool capital, int scale);
void extent(const char *s, bool capital, int &topRow, int &bottomRow);

// y is the screen row of the top of `topRow`. Returns the x after the last letter
int draw(MatrixPanel_I2S_DMA &d, const char *s, bool capital, int x, int y, int scale, int topRow, uint16_t color);

}  // namespace wordfont
