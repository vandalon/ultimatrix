// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "WordFont.h"
#include <string.h>
#include <MiniFont.h>

namespace wordfont {

namespace {

// A letter as rows separated by '/', the first one on row `top` (-2 for a letter with an ascender, 0 otherwise)
struct Letter { char key; int8_t top; const char *rows; };
const Letter kLetters[] = {
  {'a', 0, ".##./...#/.###/#..#/.###"},
  {'b', -2, "#.../#.../###./#..#/#..#/#..#/###."},
  {'c', 0, ".##./#..#/#.../#..#/.##."},
  {'d', -2, "...#/...#/.###/#..#/#..#/#..#/.###"},
  {'e', 0, ".##./#..#/####/#.../.###"},
  {'f', -2, ".##/#../###/#../#../#../#.."},
  {'g', 0, ".###/#..#/#..#/.###/...#/...#/###."},
  {'h', -2, "#.../#.../###./#..#/#..#/#..#/#..#"},
  {'i', -2, "#/./#/#/#/#/#"},
  {'j', -2, ".#/../.#/.#/.#/.#/.#/.#/#."},
  {'k', -2, "#.../#.../#..#/#.#./##../#.#./#..#"},
  {'l', -2, "#/#/#/#/#/#/#"},
  {'m', 0, "####./#.#.#/#.#.#/#.#.#/#.#.#"},
  {'n', 0, "###./#..#/#..#/#..#/#..#"},
  {'o', 0, ".##./#..#/#..#/#..#/.##."},
  {'p', 0, "###./#..#/#..#/#..#/###./#.../#..."},
  {'q', 0, ".###/#..#/#..#/#..#/.###/...#/...#"},
  {'r', 0, "##./#.#/#../#../#.."},
  {'s', 0, ".###/#.../.##./...#/###."},
  {'t', -1, "#../###/#../#../#../.##"},
  {'u', 0, "#..#/#..#/#..#/#..#/.###"},
  {'v', 0, "#.#/#.#/#.#/#.#/.#."},
  {'w', 0, "#...#/#...#/#.#.#/#.#.#/.#.#."},
  {'x', 0, "#.#/#.#/.#./#.#/#.#"},
  {'y', 0, "#..#/#..#/#..#/.###/...#/...#/###."},
  {'z', 0, "###/..#/.#./#../###"},
  {'-', 2, "###"},
  {'\'', -2, "#/#"},
  {'.', 4, "#"},
  {'?', 0, "###./...#/.##./..../.#.."},
  {'\x01', 0, "#/#/#/#/#"},                 // a letter i without its dot
  {'\x02', -2, "#../#../#.#/##./#../#../#.."},  // l with a stroke
  {'\x03', -2, "##/#./#./#./#./#./#."},         // l with a hook
};

enum Mark : uint8_t { Acute, Grave, Circumflex, Caron, Diaeresis, Ring, Breve, DoubleAcute, Tilde, Ogonek, Cedilla, ApostropheRight, Slash };
struct Accent { uint32_t codepoint; char base; Mark mark; };
const Accent kAccents[] = {
  {0xE1, 'a', Acute}, {0xE2, 'a', Circumflex}, {0xE4, 'a', Diaeresis}, {0xE5, 'a', Ring}, {0xE7, 'c', Cedilla}, {0xE9, 'e', Acute},
  {0xEA, 'e', Circumflex}, {0xED, '\x01', Acute}, {0xF1, 'n', Tilde}, {0xF3, 'o', Acute}, {0xF6, 'o', Diaeresis}, {0xFA, 'u', Acute},
  {0xFC, 'u', Diaeresis}, {0x103, 'a', Breve}, {0x105, 'a', Ogonek}, {0x107, 'c', Acute}, {0x10D, 'c', Caron}, {0x119, 'e', Ogonek},
  {0x11B, 'e', Caron}, {0x142, '\x02', Slash}, {0x151, 'o', DoubleAcute}, {0x159, 'r', Caron}, {0x15B, 's', Acute}, {0x161, 's', Caron},
  {0x165, 't', ApostropheRight}, {0x16F, 'u', Ring}, {0x219, 's', Cedilla}, {0x21B, 't', Cedilla},
  // The date in words
  {0xE8, 'e', Grave}, {0xEB, 'e', Diaeresis}, {0xEC, '\x01', Grave}, {0xEE, '\x01', Circumflex}, {0xF4, 'o', Circumflex},
  {0xF8, 'o', Slash}, {0xF9, 'u', Grave}, {0xFB, 'u', Circumflex}, {0xFD, 'y', Acute}, {0x13E, '\x03', Slash},
  {0x17A, 'z', Acute}, {0x17E, 'z', Caron},
  // The capitals of the date line
  {0xC1, 'A', Acute}, {0xC2, 'A', Circumflex}, {0xC5, 'A', Ring}, {0xC9, 'E', Acute}, {0xD6, 'O', Diaeresis}, {0xD8, 'O', Slash},
  {0xDA, 'U', Acute}, {0xDC, 'U', Diaeresis}, {0x104, 'A', Ogonek}, {0x10C, 'C', Caron}, {0x15A, 'S', Acute}, {0x160, 'S', Caron},
};

const Letter *letter(char key) {
  for (const Letter &l : kLetters) if (l.key == key) return &l;
  return nullptr;
}

bool plain(char key, Bitmap &out) {
  const Letter *l = letter(key);
  if (!l) return false;
  memset(&out, 0, sizeof(out));
  int row = l->top, x = 0;
  out.width = 0;
  for (const char *p = l->rows;; p++) {
    if (*p == '/' || *p == 0) {
      if (x > out.width) out.width = x;
      x = 0;
      row++;
      if (*p == 0) break;
      continue;
    }
    if (*p == '#' && row >= -2 && row <= 6) out.rows[row + 2] |= 1 << x;
    x++;
  }
  return true;
}

// A capital from the small print font: three columns, rows 0 to 4
bool capital(char key, Bitmap &out) {
  memset(&out, 0, sizeof(out));
  out.width = 3;
  const uint8_t *g = facekit::miniGlyph(key);
  for (int row = 0; row < 5; row++)
    for (int col = 0; col < 3; col++)
      if (g[row] >> (2 - col) & 1) out.rows[row + 2] |= 1 << col;
  return true;
}

void set(Bitmap &b, int x, int row) {
  if (x >= 0 && x < 8 && row >= -2 && row <= 6) b.rows[row + 2] |= 1 << x;
}

void addMark(Bitmap &b, Mark mark) {
  const int w = b.width, mid = (w - 1) / 2;
  switch (mark) {
    case Acute: set(b, mid + 1, -2); set(b, mid, -1); break;
    case Grave: set(b, mid, -2); set(b, mid + 1, -1); break;
    case Circumflex: for (int x = 1; x < w - 1; x++) set(b, x, -2); set(b, 0, -1); set(b, w - 1, -1); break;
    case Caron: set(b, 0, -2); set(b, w - 1, -2); for (int x = 1; x < w - 1; x++) set(b, x, -1); break;
    case Diaeresis: set(b, 0, -1); set(b, w - 1, -1); break;
    case Ring: for (int x = 1; x < w - 1 || x < 2; x++) { set(b, x, -2); set(b, x, -1); } break;
    case Breve: for (int x = 1; x < w - 1; x++) set(b, x, -1); break;
    case DoubleAcute: set(b, 1, -2); set(b, 0, -1); set(b, 3, -2); set(b, 2, -1); break;
    case Tilde: set(b, 0, -1); set(b, 1, -2); set(b, 2, -2); set(b, 3, -1); break;
    case Ogonek: set(b, w - 1, 5); break;
    case Cedilla: set(b, 1, 5); set(b, 0, 6); break;
    case ApostropheRight: set(b, w - 1, -2); set(b, w - 1, -1); break;
    case Slash: break;  // the letter has the stroke itself
  }
}

}  // namespace

uint32_t next(const char *&s) {
  const uint8_t c = (uint8_t)*s;
  if (!c) return 0;
  s++;
  if (c < 0x80) return c;
  uint32_t cp;
  int more;
  if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; more = 1; }
  else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; more = 2; }
  else { cp = c & 0x07; more = 3; }
  while (more-- && ((uint8_t)*s & 0xC0) == 0x80) cp = (cp << 6) | ((uint8_t)*s++ & 0x3F);
  return cp;
}

bool glyph(uint32_t cp, bool caps, Bitmap &out) {
  if (cp < 0x80) {
    char c = (char)cp;
    if (caps) {
      if (c == ' ') { memset(&out, 0, sizeof(out)); out.width = 2; return true; }
      return capital(c, out);
    }
    if (c == ' ') { memset(&out, 0, sizeof(out)); out.width = 2; return true; }
    if (c >= 'A' && c <= 'Z') c += 32;
    if (plain(c, out)) return true;
    plain('?', out);
    return false;
  }
  for (const Accent &a : kAccents) {
    if (a.codepoint != cp) continue;
    if ((a.base >= 'A' && a.base <= 'Z') != caps) continue;  // capitals are for the date line, lowercase for the words
    const bool ok = a.base >= 'A' && a.base <= 'Z' ? capital(a.base, out) : plain(a.base, out);
    if (!ok) return false;
    if (a.base == 'O' && a.mark == Slash) { set(out, 2, 1); set(out, 1, 2); set(out, 0, 3); }
    else addMark(out, a.mark);
    return true;
  }
  plain('?', out);
  return false;
}

int width(const char *s, bool caps, int scale) {
  int total = 0;
  bool first = true;
  for (uint32_t cp; (cp = next(s));) {
    Bitmap b;
    glyph(cp, caps, b);
    if (!first) total += scale;  // one pixel between letters
    total += b.width * scale;
    first = false;
  }
  return total;
}

void extent(const char *s, bool caps, int &topRow, int &bottomRow) {
  topRow = 5; bottomRow = -3;
  for (uint32_t cp; (cp = next(s));) {
    Bitmap b;
    glyph(cp, caps, b);
    for (int row = -2; row <= 6; row++)
      if (b.rows[row + 2]) { if (row < topRow) topRow = row; if (row > bottomRow) bottomRow = row; }
  }
  if (topRow > bottomRow) { topRow = 0; bottomRow = 4; }
}

namespace {

void paint(MatrixPanel_I2S_DMA &d, int x, int y, uint16_t color, Plot plot, void *context) {
  if (plot) plot(d, x, y, color, context);
  else if ((unsigned)x < 64 && (unsigned)y < 64) d.drawPixel(x, y, color);
}

// Scale2x: each pixel becomes four, and diagonal steps are rounded off
void blit2(MatrixPanel_I2S_DMA &d, const Bitmap &b, int x, int y, int topRow, uint16_t color, Plot plot, void *context) {
  for (int row = -2; row <= 6; row++)
    for (int col = 0; col < b.width; col++) {
      const bool p = b.at(col, row), up = b.at(col, row - 1), down = b.at(col, row + 1), left = b.at(col - 1, row), right = b.at(col + 1, row);
      bool e[4];
      e[0] = (left == up && left != down && up != right) ? up : p;
      e[1] = (up == right && up != left && right != down) ? right : p;
      e[2] = (down == left && down != right && left != up) ? left : p;
      e[3] = (right == down && right != up && down != left) ? down : p;
      for (int k = 0; k < 4; k++) {
        const int px = x + col * 2 + (k & 1), py = y + (row - topRow) * 2 + (k >> 1);
        if (e[k]) paint(d, px, py, color, plot, context);
      }
    }
}

}  // namespace

int draw(MatrixPanel_I2S_DMA &d, const char *s, bool caps, int x, int y, int scale, int topRow, uint16_t color,
         Plot plot, void *context) {
  bool first = true;
  for (uint32_t cp; (cp = next(s));) {
    Bitmap b;
    glyph(cp, caps, b);
    if (!first) x += scale;
    first = false;
    if (scale == 2) {
      blit2(d, b, x, y, topRow, color, plot, context);
    } else {
      for (int row = -2; row <= 6; row++)
        for (int col = 0; col < b.width; col++)
          if (b.at(col, row)) {
            const int px = x + col, py = y + (row - topRow);
            paint(d, px, py, color, plot, context);
          }
    }
    x += b.width * scale;
  }
  return x;
}

}  // namespace wordfont
