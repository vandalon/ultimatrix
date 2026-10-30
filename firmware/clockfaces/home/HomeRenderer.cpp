// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "HomeRenderer.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <utility>
#include "FaceKit.h"
#include "MiniFont.h"
#include "NumericFont.h"
#include "SourceArt.h"

using namespace facekit;

namespace home {

uint16_t tone(uint8_t t) {
  switch (t) {
    case T_BLUE: return rgb(24, 188, 242);  // Home Assistant's own blue, not the brighter Material cyan
    case T_AMBER: return rgb(255, 193, 7);
    case T_ORANGE: return rgb(255, 152, 0);
    case T_GREEN: return rgb(76, 175, 80);
    case T_RED: return rgb(244, 67, 54);
    case T_DIM: return rgb(10, 50, 66);  // HA blue, dimmed: the same hue as T_BLUE, not a plain grey
    case T_MUTE: return rgb(127, 168, 191);
    case T_SUN: return rgb(255, 213, 79);
    case T_BATTERY: return rgb(77, 182, 172);
    case T_FADED: return rgb(138, 106, 16);
    case T_PASTEL: return rgb(144, 202, 249);  // pastel blue: the radio station
    default: return rgb(232, 238, 242);
  }
}

namespace {

const uint16_t kDivider = rgb(12, 20, 27);

// ---- Letters of the labels and popups: 4x5 (I 3 wide, M and W 5 wide) instead of the shared 3x5 font, whose M, N and W look alike ----
// Five bits per row, left pixel = bit 4. Marks stay in the 3x5 font.
const uint8_t kLetters[26][5] = {{12,18,30,18,18},{28,18,28,18,28},{14,16,16,16,14},{28,18,18,18,28},{30,16,28,16,30},{30,16,28,16,16},{14,16,22,18,14},{18,18,30,18,18},{28,8,8,8,28},{6,2,2,18,12},{18,20,24,20,18},{16,16,16,16,30},{17,27,21,17,17},{18,26,22,18,18},{12,18,18,18,12},{28,18,28,16,16},{12,18,18,22,14},{28,18,28,20,18},{14,16,12,2,28},{28,8,8,8,8},{18,18,18,18,12},{18,18,18,12,4},{17,17,21,21,10},{18,18,12,18,18},{18,18,12,8,8},{30,2,12,16,30}};
const uint8_t kDigits[10][5] = {{12,18,18,18,12},{8,24,8,8,28},{12,2,12,16,30},{28,2,12,2,28},{18,18,30,2,2},{30,16,28,2,28},{12,16,28,18,12},{30,2,4,8,8},{12,18,12,18,12},{12,18,14,2,12}};
const uint8_t kDigitWidth[10] = {4,3,4,4,4,4,4,4,4,4};
const uint8_t kLetterWidth[26] = {4,4,4,4,4,4,4,4,3,4,4,4,5,4,4,4,4,4,4,3,4,4,5,4,4,4};

int letterWidth(char c) {
  c = toupper(c);
  if (c >= '0' && c <= '9') return kDigitWidth[c - '0'];
  return c >= 'A' && c <= 'Z' ? kLetterWidth[c - 'A'] : 3;
}

int homeWidth(const char *s) {
  int n = 0;
  for (; *s; ++s) n += letterWidth(*s) + 1;
  return n ? n - 1 : 0;
}

void homeGlyph(MatrixPanel_I2S_DMA &d, char c, int x, int y, uint16_t color) {
  c = toupper(c);
  if (c >= 'A' && c <= 'Z') {
    for (int j = 0; j < 5; j++)
      for (int i = 0; i < 5; i++)
        if (kLetters[c - 'A'][j] >> (4 - i) & 1) put(d, x + i, y + j, color);
  } else if (c >= '0' && c <= '9') {
    for (int j = 0; j < 5; j++)
      for (int i = 0; i < 5; i++)
        if (kDigits[c - '0'][j] >> (4 - i) & 1) put(d, x + i, y + j, color);
  } else {
    const uint8_t *g = miniGlyph(c);
    for (int j = 0; j < 5; j++)
      for (int i = 0; i < 3; i++)
        if (g[j] >> (2 - i) & 1) put(d, x + i, y + j, color);
  }
}

// A line that is too wide for the panel goes back to the narrow 3x5 font
void homeText(MatrixPanel_I2S_DMA &d, const char *s, int x, int y, uint16_t color) {
  if (x >= 0 && homeWidth(s) + x <= W) {
    for (; *s; ++s) { homeGlyph(d, *s, x, y, color); x += letterWidth(*s) + 1; }
  } else miniText(d, s, x, y, color);
}

void homeCentered(MatrixPanel_I2S_DMA &d, const char *s, int y, uint16_t color) {
  const int width = homeWidth(s);
  if (width <= W - 2) homeText(d, s, (W - width) / 2, y, color);
  else miniCentered(d, s, y, color);
}

// ---- Bitmaps: '#' is lit ----
const char *const kThermometer[] = {"....###....", "...#...#...", "...#.#.#...", "...#.#.#...", "...#.#.#...", "...#.#.#...", "...#####...",
                                    "..#######..", "..#######..", "..#######..", "...#####..."};
const char *const kDrop[] = {".....#.....", ".....#.....", "....###....", "....###....", "...#####...", "..#######..", "..#######..",
                             "..#######..", "...#####..."};
const char *const kPerson[] = {"....###....", "...#...#...", "...#...#...", "....###....", "...........", "...#####...", "..#.....#..",
                               ".#.......#.", ".#.......#.", ".#########."};
const char *const kBolt[] = {"......###..", ".....###...", "....###....", "...###.....", "..#######..", ".....###...", "....###....",
                             "...###.....", "..###......", "..##......."};
const char *const kRing[] = {"...#####...", "..#.....#..", ".#.......#.", ".#.......#.", ".#...#...#.", ".#.......#.", ".#.......#.",
                             "..#.....#..", "...#####..."};

// The car of the EV tile, and the badges in its top right corner while it is charging or plugged in
const char *const kSmallCar[] = {"..####..", ".#....#.", "########", "#.#..#.#", ".##..##."};
const char *const kBoltBadge[] = {"..#", ".#.", "###", ".#.", "#.."};
const char *const kPlugBadge[] = {"#.#", "###", "###", ".#.", ".#."};

const char *const kGarage[] = {"#########", "#.......#", "#.#####.#", "#.......#", "#.#####.#", "#.......#", "#.#####.#", "#.......#", "#########"};
const char *const kBulb[] = {"..###..", ".#####.", ".#####.", ".#####.", "..###..", "..###..", "...#..."};
const char *const kHouse[] = {"...#...", "..#.#..", ".#...#.", "#.....#", "#.###.#", "#.#.#.#", "#######"};
const char *const kSun[] = {"...#...", ".#...#.", "..###..", "#.###.#", "..###..", ".#...#.", "...#..."};
const char *const kPole[] = {".#####.", "...#...", ".#####.", "...#...", "...#...", "...#...", "..###.."};

// The Energy page's row icons, all 5 px wide like the grid's
const char *const kRowHouse[] = {"..#..", ".###.", "#####", "#####", "##.##", "##.##"};  // 6 rows: sits on the digits' baseline
const char *const kRowSun[] = {"..#..", "#.#.#", ".###.", "#####", ".....", "#####"};  // a sun over the horizon
const char *const kRowPole[] = {"...#.", "..#..", ".##..", "#####", "..##.", ".#...", "#...."};  // a lightning bolt for the grid

const char *const kBell[] = {"......###......", ".....#...#.....", "....#.....#....", "....#.....#....", "...#.......#...", "...#.......#...", "...#.......#...",
                             "..#.........#..", "..#.........#..", ".#...........#.", ".#############.", "......###......", ".......#......."};
const char *const kWarning[] = {".......#.......", "......#.#......", "......#.#......", ".....#...#.....", ".....#...#.....", "....#..#..#....",
                                "....#..#..#....", "...#...#...#...", "...#...#...#...", "..#....#....#..", "..#.........#..", ".#.....#.....#.",
                                ".#...........#.", "###############"};
const char *const kBigWasher[] = {".#############.", ".#...........#.", ".#.#.#.....#.#.", ".#...........#.", ".#############.", ".#...#####...#.",
                                  ".#..#.....#..#.", ".#.#..###..#.#.", ".#.#.#...#.#.#.", ".#.#.#...#.#.#.", ".#.#.#...#.#.#.", ".#.#..###..#.#.",
                                  ".#..#.....#..#.", ".#...#####...#.", ".#############."};
const char *const kBigGarage[] = {".......#.......", "......#.#......", ".....#...#.....", "...##.....##...", "..#.........#..", ".#...........#.",
                                  "##...........##", ".#...........#.", ".#.#########.#.", ".#.#.......#.#.", ".#.#########.#.", ".#.#.......#.#.",
                                  ".#.#########.#.", ".#.#.......#.#.", ".#############."};

template <int N> void bitmap(MatrixPanel_I2S_DMA &d, const char *const (&rows)[N], int x, int y, uint16_t color) {
  for (int row = 0; row < N; row++)
    for (int col = 0; rows[row][col]; col++)
      if (rows[row][col] == '#') put(d, x + col, y + row, color);
}

// The 7x7 icon of an index
void smallIcon(MatrixPanel_I2S_DMA &d, uint8_t icon, int x, int y, uint16_t color) {
  switch (icon) {
    case SI_DOOR: {
      const char *const rows[] = {".#######.", ".#.....#.", ".#.....#.", ".#.....#.", ".#....##.", ".#.....#.", ".#.....#.", ".#.....#.", "#########"};
      bitmap(d, rows, x - 1, y - 1, color); break;
    }
    case SI_WINDOW: {
      const char *const rows[] = {"#########", "#...#...#", "#...#...#", "#...#...#", "#########", "#...#...#", "#...#...#", "#...#...#", "#########"};
      bitmap(d, rows, x - 1, y - 1, color); break;
    }
    case SI_GARAGE: bitmap(d, kGarage, x - 1, y - 1, color); break;
    case SI_WASHER: {
      const char *const rows[] = {"#########", "#.#.....#", "#########", "#..###..#", "#.#...#.#", "#.#...#.#", "#..###..#", "#.......#", "#########"};
      bitmap(d, rows, x - 1, y - 1, color); break;
    }
    case SI_DRYER: {
      const char *const rows[] = {"#########", "#.#.#...#", "#########", "#..###..#", "#.#...#.#", "#.#.#.#.#", "#..###..#", "#.......#", "#########"};
      bitmap(d, rows, x - 1, y - 1, color); break;
    }
    case SI_BULB: bitmap(d, kBulb, x, y, color); break;
    case SI_LOCK: {
      const char *const rows[] = {"..#####..", ".#.....#.", ".#.......", ".#.......", "#########", "####.####", "####.####", "####.####", "#########"};  // open: the icon is for what is unlocked
      bitmap(d, rows, x - 1, y - 1, color); break;
    }
    case SI_PLUG: {
      const char *const rows[] = {"..#...#..", "..#...#..", "..#...#..", "#########", "#########", ".#######.", "..#####..", "...###...", "....#...."};
      bitmap(d, rows, x - 1, y - 1, color); break;
    }
    case SI_HOUSE: bitmap(d, kHouse, x, y, color); break;
    case SI_SUN: bitmap(d, kSun, x, y, color); break;
    case SI_POLE: bitmap(d, kPole, x, y, color); break;
    case SI_LOW_BATTERY: {
      const char *const rows[] = {"...###...", ".#######.", ".#.....#.", ".#.....#.", ".#.....#.", ".#.....#.", ".#.###.#.", ".#.....#.", ".#######."};
      bitmap(d, rows, x - 1, y - 1, color); break;
    }
    case SI_OFFLINE: {
      const char *const rows[] = {"..#####..", ".#.....#.", "#.......#", "...###...", "..#...#..", ".........", "...#.#...", "....#....", "...#.#..."};
      bitmap(d, rows, x - 1, y - 1, color); break;
    }
    case SI_LEAK: {
      const char *const rows[] = {"....#....", "...###...", "...###...", "..#####..", ".#######.", ".#######.", "..#####..", ".........", "##.###.##"};
      bitmap(d, rows, x - 1, y - 1, color); break;
    }
    case SI_BLINDS: {
      const char *const rows[] = {"#########", "#.....#.#", "#######.#", "#.....#.#", "#######.#", "#.....#.#", "#######.#", "#.....###", "#######.."};
      bitmap(d, rows, x - 1, y - 1, color); break;
    }
    case SI_MEDIA: {
      const char *const rows[] = {"#########", "#.......#", "#..#....#", "#..##...#", "#..###..#", "#..##...#", "#..#....#", "#.......#", "#########"};
      bitmap(d, rows, x - 1, y - 1, color); break;
    }
    case SI_HEATING: {
      const char *const rows[] = {"..#.#.#..", ".#.#.#...", "..#.#.#..", ".........", ".#.#.#.#.", ".#######.", "##.#.#.##", ".#######.", ".#.#.#.#."};
      bitmap(d, rows, x - 1, y - 1, color); break;
    }
    case SI_PRESENCE: {
      const char *const rows[] = {"...##....", "...##....", "....#....", "..#####..", ".#..#..#.", "....#....", "...#.#...", "..#...#..", ".#.....#."};
      bitmap(d, rows, x - 1, y - 1, color); break;
    }
    default: break;
  }
}

// A battery, filled from the bottom by its charge: a terminal row, an outline and four rows inside
void battery(MatrixPanel_I2S_DMA &d, int x, int y, int percent, uint16_t color) {
  const int filled = (percent * 4 + 50) / 100;
  box(d, x + 2, y, 3, 1, color);
  for (int row = 1; row < 7; row++) {
    const bool edge = row == 1 || row == 6;
    const bool full = row >= 2 && row <= 5 && row - 2 >= 4 - filled;
    for (int col = 0; col < 7; col++)
      if (edge || full || col == 0 || col == 6) put(d, x + col, y + row, color);
  }
}

// A battery 5 px wide, filled from the bottom by its charge
// The battery: walls, and the charge as the fill. While it charges or discharges a sweep runs through the whole body (up when charging, down when
// discharging): in the battery's teal as far as the real charge reaches, in the same teal dimmed above it; the last step is the plain charge.
void rowBattery(MatrixPanel_I2S_DMA &d, int x, int y, int percent, uint16_t color, int flow, int step) {
  const int filled = (percent * 4 + 50) / 100;
  put(d, x + 2, y, color);
  const uint16_t level = flow ? dim(color, 140) : color, above = dim(color, 140);
  for (int row = 1; row < 7; row++) {
    const bool edge = row == 1 || row == 6;
    const bool full = row >= 2 && row <= 5 && row - 2 >= 4 - filled;
    const bool swept = row >= 2 && row <= 5 && (flow > 0 ? row >= 6 - step : flow < 0 && row >= 2 + step);
    for (int col = 0; col < 5; col++) {
      if (edge || col == 0 || col == 4) put(d, x + col, y + row, color);
      else if (swept) put(d, x + col, y + row, full ? color : above);
      else if (full) put(d, x + col, y + row, level);
    }
  }
}

constexpr int kRowIconWidth = 5;
void rowIcon(MatrixPanel_I2S_DMA &d, uint8_t icon, int x, int y, int percent, uint16_t color, int flow = 0, int step = 0) {
  switch (icon) {
    case SI_HOUSE: bitmap(d, kRowHouse, x, y + 1, color); break;
    case SI_SUN: bitmap(d, kRowSun, x, y + 1, color); break;
    case SI_POLE: bitmap(d, kRowPole, x, y, color); break;
    case SI_BATTERY: rowBattery(d, x, y, percent, color, flow, step); break;
    default: break;
  }
}

// Every icon ends in the same column (x + 10), so the gap to the number is the same for all of them
void tileIcon(MatrixPanel_I2S_DMA &d, uint8_t icon, int x, int y, uint16_t color) {
  switch (icon) {
    case TI_THERMOMETER: bitmap(d, kThermometer, x + 2, y, color); break;
    case TI_DROP: bitmap(d, kDrop, x + 2, y + 1, color); break;
    case TI_PERSON: bitmap(d, kPerson, x + 1, y, color); break;
    case TI_BOLT: bitmap(d, kBolt, x + 2, y, color); break;
    case TI_RING: bitmap(d, kRing, x + 1, y + 1, color); break;
    case TI_CAR: bitmap(d, kSmallCar, x + 3, y + 2, color); break;
    case TI_CAR_CHARGING: bitmap(d, kSmallCar, x, y + 4, color); bitmap(d, kBoltBadge, x + 8, y, color); break;
    case TI_CAR_PLUGGED: bitmap(d, kSmallCar, x, y + 4, color); bitmap(d, kPlugBadge, x + 8, y, color); break;
    case TI_CO2: miniText(d, "CO2", x, y + 3, color); break;
    case TI_PRICE: {  // a ring with a lowercase e (4x5) inside
      const char *const e[] = {".##.", "#..#", "####", "#...", ".###"};
      bitmap(d, kRing, x + 1, y + 1, color); bitmap(d, e, x + 4, y + 3, color); break;
    }
    case TI_RAIN: {
      const char *const rows[] = {"....###....", "..#######..", ".#########.", "###########", "...........", "..#..#..#..", ".#..#..#...", "...........", "..#..#..#.."};
      bitmap(d, rows, x, y, color); break;
    }
    case TI_OUTDOOR_DROP: bitmap(d, kDrop, x + 1, y + 1, color); put(d, x + 10, y, tone(T_SUN)); break;
    default: break;
  }
}

// ---- Text in the block digits of the boot screen (4x7), letters from the 3x5 font ----
int glyphWidth(char c) {
  if (c >= '0' && c <= '9') return 4;
  if (c == '.' || c == ':') return 2;
  if (c == '`') return 3;
  if (c == '%') return 5;
  return letterWidth(c);
}

int textWidth(const char *s) {
  int width = 0;
  for (; *s; ++s) width += glyphWidth(*s) + 1;
  return width ? width - 1 : 0;
}

void glyph(MatrixPanel_I2S_DMA &d, char c, int x, int y, uint16_t color) {
  if (c >= '0' && c <= '9') {
    for (int j = 0; j < 7; j++)
      for (int i = 0; i < 4; i++)
        if (numericfont::smallPixel(c - '0', i, j)) put(d, x + i, y + j, color);
  } else if (c == '.') {
    box(d, x, y + 5, 2, 2, color);
  } else if (c == ':') {
    box(d, x, y + 1, 2, 2, color);
    box(d, x, y + 4, 2, 2, color);
  } else if (c == '`') {  // the degree sign
    box(d, x, y, 3, 1, color);
    box(d, x, y + 2, 3, 1, color);
    put(d, x, y + 1, color);
    put(d, x + 2, y + 1, color);
  } else if (c == '%') {
    static const char *const kPercent[] = {"##..#", "##.#.", "..#..", "..#..", ".#.##", "#..##"};
    for (int j = 0; j < 6; j++)
      for (int i = 0; i < 5; i++)
        if (kPercent[j][i] == '#') put(d, x + i, y + j, color);
  } else {
    homeGlyph(d, c, x, y + 2, color);
  }
}

void block(MatrixPanel_I2S_DMA &d, const char *text, int x, int y, uint16_t color) {
  for (; *text; ++text) {
    glyph(d, *text, x, y, color);
    x += glyphWidth(*text) + 1;
  }
}

// Numbers line up on their decimal point: the point is at dotX, whole numbers end before it, the rest follows
void blockOnPoint(MatrixPanel_I2S_DMA &d, const char *text, int dotX, int y, uint16_t color) {
  const char *dot = strchr(text, '.');
  char whole[8] = "";
  if (!dot) {
    // A whole number ("11"): the digits end where the first decimal of the other values ends
    size_t digits = 0;
    while (text[digits] >= '0' && text[digits] <= '9' && digits < sizeof(whole) - 1) digits++;
    if (!digits) { block(d, text, dotX - 1 - textWidth(text), y, color); return; }  // "-": nothing to show
    memcpy(whole, text, digits);
    whole[digits] = 0;
    block(d, whole, dotX + 3 + glyphWidth('0') - textWidth(whole), y, color);
    return;
  }
  size_t n = dot - text < 7 ? dot - text : 7;
  memcpy(whole, text, n);
  whole[n] = 0;
  block(d, whole, dotX - 1 - textWidth(whole), y, color);
  glyph(d, '.', dotX, y, color);
  block(d, dot + 1, dotX + 3, y, color);
}

// Small numbers (status counts, the date): the clock's digits at their 3x5 size, other characters from the label font
int countWidth(const char *s) {
  int width = 0;
  for (; *s; ++s) width += (*s >= '0' && *s <= '9' ? 3 : letterWidth(*s)) + 1;
  return width ? width - 1 : 0;
}

void countText(MatrixPanel_I2S_DMA &d, const char *s, int x, int y, uint16_t color) {
  for (; *s; ++s) {
    if (*s >= '0' && *s <= '9') {
      for (int j = 0; j < 5; j++)
        for (int i = 0; i < 3; i++)
          if (numericfont::tinyPixel(*s - '0', i, j)) put(d, x + i, y + j, color);
      x += 4;
    } else {
      homeGlyph(d, *s, x, y, color);
      x += letterWidth(*s) + 1;
    }
  }
}

// A line wider than its window scrolls slowly (one pixel per 140 ms) from its start to its end and back, resting at both ends
int pingPong(uint16_t tick, int overflow) {
  constexpr int kRest = 10;
  const int cycle = 2 * (kRest + overflow), at = (tick / 2) % cycle;
  if (at < kRest) return 0;
  if (at < kRest + overflow) return at - kRest;
  if (at < 2 * kRest + overflow) return overflow;
  return cycle - at;
}

constexpr int kStationRoom = 35;  // top right: right of the time

// The station name at the top right: bold capitals and digits 7 rows tall with 2 px strokes (the arcade faces' look), as high as the clock
struct BoldRows { char c; const char *row[7]; };
const BoldRows kBold[] = {
  {'A', {".XXX.", "XXXXX", "XX.XX", "XXXXX", "XXXXX", "XX.XX", "XX.XX"}}, {'B', {"XXXX.", "XXXXX", "XX.XX", "XXXX.", "XX.XX", "XXXXX", "XXXX."}},
  {'C', {".XXX.", "XXXXX", "XX...", "XX...", "XX...", "XXXXX", ".XXX."}}, {'D', {"XXXX.", "XXXXX", "XX.XX", "XX.XX", "XX.XX", "XXXXX", "XXXX."}},
  {'E', {"XXXXX", "XXXXX", "XX...", "XXXX.", "XX...", "XXXXX", "XXXXX"}}, {'F', {"XXXXX", "XXXXX", "XX...", "XXXX.", "XX...", "XX...", "XX..."}},
  {'G', {".XXX.", "XXXXX", "XX...", "XX.XX", "XX.XX", "XXXXX", ".XXX."}}, {'H', {"XX.XX", "XX.XX", "XX.XX", "XXXXX", "XXXXX", "XX.XX", "XX.XX"}},
  {'I', {"XX", "XX", "XX", "XX", "XX", "XX", "XX"}}, {'J', {"...XX", "...XX", "...XX", "...XX", "XX.XX", "XXXXX", ".XXX."}},
  {'K', {"XX..XX", "XX.XX.", "XXXX..", "XXX...", "XXXX..", "XX.XX.", "XX..XX"}}, {'L', {"XX...", "XX...", "XX...", "XX...", "XX...", "XXXXX", "XXXXX"}},
  {'M', {"XX.XX", "XXXXX", "XXXXX", "XX.XX", "XX.XX", "XX.XX", "XX.XX"}}, {'N', {"XX..XX", "XXX.XX", "XXXXXX", "XX.XXX", "XX..XX", "XX..XX", "XX..XX"}},
  {'O', {".XXX.", "XXXXX", "XX.XX", "XX.XX", "XX.XX", "XXXXX", ".XXX."}}, {'P', {"XXXX.", "XXXXX", "XX.XX", "XXXXX", "XXXX.", "XX...", "XX..."}},
  {'Q', {".XXX.", "XXXXX", "XX.XX", "XX.XX", "XX.XX", "XXXXX", ".XXXX"}}, {'R', {"XXXX.", "XXXXX", "XX.XX", "XXXX.", "XXXX.", "XX.XX", "XX.XX"}},
  {'S', {".XXXX", "XXXXX", "XX...", ".XXX.", "...XX", "XXXXX", "XXXX."}}, {'T', {"XXXXXX", "XXXXXX", "..XX..", "..XX..", "..XX..", "..XX..", "..XX.."}},
  {'U', {"XX.XX", "XX.XX", "XX.XX", "XX.XX", "XX.XX", "XXXXX", ".XXX."}}, {'V', {"XX.XX", "XX.XX", "XX.XX", "XX.XX", "XX.XX", ".XXX.", "..X.."}},
  {'W', {"XX.XX", "XX.XX", "XX.XX", "XX.XX", "XXXXX", "XXXXX", "XX.XX"}}, {'X', {"XX.XX", "XX.XX", ".XXX.", "..X..", ".XXX.", "XX.XX", "XX.XX"}},
  {'Y', {"XX..XX", "XX..XX", ".XXXX.", "..XX..", "..XX..", "..XX..", "..XX.."}}, {'Z', {"XXXXX", "XXXXX", "...XX", "..XX.", ".XX..", "XXXXX", "XXXXX"}},
  {'0', {".XXX.", "XXXXX", "XX.XX", "XX.XX", "XX.XX", "XXXXX", ".XXX."}}, {'1', {"..XX.", ".XXX.", "XXXX.", "..XX.", "..XX.", "..XX.", "XXXXX"}},
  {'2', {".XXX.", "XXXXX", "...XX", "..XX.", ".XX..", "XXXXX", "XXXXX"}}, {'3', {"XXXX.", "XXXXX", "...XX", ".XXX.", "...XX", "XXXXX", "XXXX."}},
  {'4', {"XX.XX", "XX.XX", "XX.XX", "XXXXX", "XXXXX", "...XX", "...XX"}}, {'5', {"XXXXX", "XXXXX", "XXXX.", "...XX", "...XX", "XXXXX", "XXXX."}},
  {'6', {".XXX.", "XXXXX", "XX...", "XXXX.", "XX.XX", "XXXXX", ".XXX."}}, {'7', {"XXXXX", "XXXXX", "...XX", "..XX.", ".XX..", ".XX..", ".XX.."}},
  {'8', {".XXX.", "XXXXX", "XX.XX", ".XXX.", "XX.XX", "XXXXX", ".XXX."}}, {'9', {".XXX.", "XXXXX", "XX.XX", "XXXXX", "...XX", "XXXXX", ".XXX."}},
  {'-', {"....", "....", "....", "XXXX", "XXXX", "....", "...."}}, {'.', {"..", "..", "..", "..", "..", "XX", "XX"}},
  {'\'', {"XX", "XX", "..", "..", "..", "..", ".."}},
};

const BoldRows *boldRows(char c) {
  for (const BoldRows &g : kBold) if (g.c == c) return &g;
  return nullptr;
}

int boldTextWidth(const char *s) {  // a character without a shape is a gap of 3
  int width = 0;
  for (; *s; ++s) {
    const BoldRows *g = boldRows(toupper(*s));
    width += (g ? (int)strlen(g->row[0]) : 3) + 1;
  }
  return width ? width - 1 : 0;
}

// Only the columns from clipLeft to clipRight are drawn, so a name wider than its window can scroll inside it
void boldText(MatrixPanel_I2S_DMA &d, const char *s, int x, int y, uint16_t color, int clipLeft = 0, int clipRight = 64) {
  for (; *s; ++s) {
    const BoldRows *g = boldRows(toupper(*s));
    if (!g) { x += 4; continue; }
    for (int j = 0; j < 7; j++)
      for (int i = 0; g->row[j][i]; i++)
        if (g->row[j][i] == 'X' && x + i >= clipLeft && x + i < clipRight) put(d, x + i, y + j, color);
    x += (int)strlen(g->row[0]) + 1;
  }
}

bool thinHeader = false;  // set per frame by header(): 1 px strokes (4x7 digits and letters) instead of 2 px ones (5x8)

// The clock's digits with 2 px strokes: the shapes of the 4x7 digits, every pixel drawn also to its right and below, 5 x 8 (rows y to y + 7)
void clockGlyph(MatrixPanel_I2S_DMA &d, char c, int x, int y, uint16_t color) {
  if (c == ':') {
    const int w = thinHeader ? 1 : 2;
    box(d, x, y + (thinHeader ? 2 : 1), w, 2, color);  // thin: the top dot 1 px lower
    box(d, x, y + 5, w, 2, color);
    return;
  }
  if (c < '0' || c > '9') return;
  if (thinHeader) {
    for (int j = 0; j < 7; j++) for (int i = 0; i < 4; i++) if (numericfont::smallPixel(c - '0', i, j)) put(d, x + i, y + 1 + j, color);
    return;
  }
  for (int j = 0; j < 8; j++)
    for (int i = 0; i < 5; i++)
      if (numericfont::smallPixel(c - '0', i, j) || numericfont::smallPixel(c - '0', i - 1, j) || numericfont::smallPixel(c - '0', i, j - 1) || numericfont::smallPixel(c - '0', i - 1, j - 1)) put(d, x + i, y + j, color);
}

int clockWidth(const char *s) {
  int width = 0;
  for (; *s; ++s) width += (*s == ':' ? (thinHeader ? 1 : 2) : (thinHeader ? 4 : 5)) + 1;
  return width ? width - 1 : 0;
}

// The month in letters built like the digits: 3x5 shapes stretched to 4x7, then drawn with 2 px strokes (5 x 8). Only the letters of the month names
const uint8_t kMonthLetters[26][5] = {
  {2,5,7,5,5}, {6,5,6,5,6}, {3,4,4,4,3}, {6,5,5,5,6}, {7,4,6,4,7}, {7,4,6,4,4}, {3,4,5,5,3}, {0,0,0,0,0}, {0,0,0,0,0}, {1,1,1,5,2}, {0,0,0,0,0}, {4,4,4,4,7}, {5,7,7,5,5},
  {6,5,5,5,5}, {7,5,5,5,7}, {6,5,6,4,4}, {0,0,0,0,0}, {6,5,6,5,5}, {3,4,2,1,6}, {7,2,2,2,2}, {5,5,5,5,7}, {5,5,5,5,2}, {0,0,0,0,0}, {0,0,0,0,0}, {5,5,2,2,2}, {0,0,0,0,0}};

bool monthPixel(char c, int x, int y) {
  static const uint8_t kRowOf[7] = {0, 1, 1, 2, 3, 3, 4}, kColOf[4] = {0, 1, 1, 2};
  if (x < 0 || x >= 4 || y < 0 || y >= 7) return false;
  return kMonthLetters[c - 'A'][kRowOf[y]] >> (2 - kColOf[x]) & 1;
}

void monthGlyph(MatrixPanel_I2S_DMA &d, char c, int x, int y, uint16_t color) {
  if (thinHeader) {
    static const char *const kM[7] = {"#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#", "#...#"};  // 5 wide, the V in the middle
    static const char *const kY[7] = {"#.#", "#.#", "#.#", ".#.", ".#.", ".#.", ".#."};
    for (int j = 0; j < 7; j++) for (int i = 0; i < 5; i++) {
      const bool lit = c == 'M' ? kM[j][i] == '#' : c == 'Y' ? i < 3 && kY[j][i] == '#' : c == 'T' ? (j == 0 && i < 3) || (i == 1 && i < 3) : i < 4 && monthPixel(c, i, j);  // the T and Y are 3 px wide with a 1 px stem in the middle
      if (lit) put(d, x + i, y + 1 + j, color);
    }
    return;
  }
  for (int j = 0; j < 8; j++)
    for (int i = 0; i < 5; i++)
      if (monthPixel(c, i, j) || monthPixel(c, i - 1, j) || monthPixel(c, i, j - 1) || monthPixel(c, i - 1, j - 1)) put(d, x + i, y + j, color);
}

// How far the next letter is: the fat ones are all 6; the normal ones 5, the 3 wide T and Y 4 and the 5 wide M 6
int monthAdvance(char c) { return !thinHeader ? 6 : c == 'M' ? 6 : c == 'T' || c == 'Y' ? 4 : 5; }
int monthWidth(const char *s) {
  int width = 0;
  for (; *s; ++s) width += monthAdvance(toupper(*s));
  return width ? width - 1 : 0;
}

// ---- Screens ----
bool slideStation = false;  // a slide involves a page with the station name at the top: the date or station of each page slides with it
bool sliding = false;  // a slide between two pages is being drawn: the album art stays put underneath instead of sliding with its page
// mono >= 0 draws everything in that one colour (black on a flashing panel)
void header(MatrixPanel_I2S_DMA &d, const Frame &f, int mono = -1, int dy = 1) {
  const View slide = view;  // the clock and date stay put while a page slides
  view = View();
  thinHeader = f.thinClock;
  int timeX = 13 - clockWidth(f.time) / 2;
  for (const char *c = f.time; *c; ++c) {
    const uint16_t color = *c == ':' ? rgb(f.colonLevel, f.colonLevel, f.colonLevel) : tone(T_BLUE);
    clockGlyph(d, *c, timeX, dy - 1, mono >= 0 ? mono : color);
    timeX += (*c == ':' ? (thinHeader ? 1 : 2) : (thinHeader ? 4 : 5)) + 1;
  }
  if (sliding && slideStation) { view = slide; view.x0 = 26; }  // the date or station goes with its page and passes behind the clock
  if (f.stationTop && f.station[0]) {  // the radio station in the date's place, cut to what fits right of the time
    char upper[sizeof(f.station)];
    size_t u = 0;
    for (const char *c = f.station; *c && u + 1 < sizeof(upper); ++c) upper[u++] = toupper(*c);
    upper[u] = 0;
    const int width = boldTextWidth(upper), color = mono >= 0 ? mono : tone(T_PASTEL);
    if (width <= kStationRoom) boldText(d, upper, 62 - width, dy, color);
    else boldText(d, upper, 62 - kStationRoom - pingPong(f.scrollTick, width - kStationRoom), dy, color, 62 - kStationRoom, 63);
  } else {  // "OCT 15", quieter than the time: the month in letters built like the digits, the day in the clock's digits, 2 px apart
    const char *space = strchr(f.date, ' ');
    const uint16_t color = mono >= 0 ? mono : tone(T_MUTE);
    if (space) {
      char month[8];
      strlcpy(month, f.date, std::min<size_t>(sizeof(month), space - f.date + 1));
      const int dayX = 63 - clockWidth(space + 1);
      int x = dayX - 2 - monthWidth(month);
      for (const char *c = month; *c; ++c) { monthGlyph(d, toupper(*c), x, dy - 1, color); x += monthAdvance(toupper(*c)); }
      x = dayX;
      for (const char *c = space + 1; *c; ++c, x += thinHeader ? 5 : 6) clockGlyph(d, *c, x, dy - 1, color);
    }
  }
  view = slide;
}

// The weather tile follows HA's standard condition names; unknown data gets no misleading sunny icon.
void weatherIcon(MatrixPanel_I2S_DMA &d, const char *condition, int x, int y) {
  const bool clear = !strcmp(condition, "sunny"), moon = !strcmp(condition, "clear-night");
  const bool partly = !strcmp(condition, "partlycloudy");
  const bool rain = !strcmp(condition, "rainy") || !strcmp(condition, "pouring") || !strcmp(condition, "lightning-rainy");
  const bool snow = !strcmp(condition, "snowy") || !strcmp(condition, "snowy-rainy") || !strcmp(condition, "hail");
  const bool storm = !strcmp(condition, "lightning") || !strcmp(condition, "lightning-rainy") || !strcmp(condition, "exceptional");
  if (!clear && !moon && !partly && strcmp(condition, "windy") && strcmp(condition, "windy-variant")) x += 2;  // the 9 px icons end in the same column as the rest
  if (clear) { smallIcon(d, SI_SUN, x + 4, y + 3, tone(T_SUN)); return; }  // alone, the sun is centred on the number's height and sits next to it
  if (partly) smallIcon(d, SI_SUN, x, y + 1, tone(T_SUN));
  if (moon) {
    const char *const rows[] = {"...###...", "..###....", ".###.....", ".###.....", ".####....", "..#####..", "...###..."};
    bitmap(d, rows, x + 2, y + 3, tone(T_MUTE)); return;
  }
  if (!strcmp(condition, "fog") || !strcmp(condition, "windy") || !strcmp(condition, "windy-variant")) {
    const bool fog = !strcmp(condition, "fog");
    box(d, x + 1, y + 2, 8, 1, tone(T_MUTE));
    box(d, x + (fog ? 0 : 3), y + 5, 8, 1, tone(T_MUTE));
    box(d, x + 1, y + 8, 8, 1, tone(T_MUTE)); return;
  }
  if (partly || rain || snow || storm || !strcmp(condition, "cloudy")) {
    const char *const cloud[] = {"..####...", ".######..", "#########", ".#######."};
    bitmap(d, cloud, x + (partly ? 2 : 0), y + (partly ? 7 : 1), tone(T_WHITE));
    if (rain) for (int i = 0; i < 3; i++) { put(d, x + i * 3 + 1, y + 7, tone(T_BLUE)); put(d, x + i * 3, y + 8, tone(T_BLUE)); }
    if (snow) { put(d, x + 2, y + 7, tone(T_WHITE)); put(d, x + 6, y + 9, tone(T_WHITE)); put(d, x + 8, y + 6, tone(T_WHITE)); }
    if (storm) { put(d, x + 5, y + 5, tone(T_SUN)); box(d, x + 4, y + 6, 2, 1, tone(T_SUN)); put(d, x + 4, y + 7, tone(T_SUN)); put(d, x + 3, y + 8, tone(T_SUN)); }
  } else homeText(d, "-", x + 3, y + 3, tone(T_MUTE));
}

void tileValue(MatrixPanel_I2S_DMA &d, const char *text, int x, int y, uint16_t color) {
  if (textWidth(text) <= 18) block(d, text, x, y, color);
  else if (homeWidth(text) <= 18) homeText(d, text, x, y + 2, color);
  else homeText(d, "-", x, y + 2, color);  // never spill into the adjacent tile or silently truncate a unit
}

void homeScreen(MatrixPanel_I2S_DMA &d, const Frame &f) {
  header(d, f);
  box(d, 0, 9, 64, 1, kDivider);
  // The tiles that are on, spread over the middle: one or two per row, rows of 11 px spread evenly (a lone tile in a row is centred)
  int shown[6], n = 0;
  for (int i = 0; i < 6; i++)
    if (f.tileIcon[i] != TI_NONE || f.tileText[i][0]) shown[n++] = i;
  const int perRow = n <= 2 ? 1 : 2, rows = (n + perRow - 1) / perRow;
  const int gap = rows == 3 ? 1 : rows == 2 ? 6 : 0;
  const int top = 10 + (37 - (11 * rows + gap * (rows - 1))) / 2;
  for (int k = 0; k < n; k++) {
    const int i = shown[k], row = k / perRow, inRow = k % perRow, y = top + row * (11 + gap);
    const bool alone = perRow == 1 || (row == rows - 1 && n % 2 == 1);
    const int x = alone ? (64 - (13 + textWidth(f.tileText[i]))) / 2 : (inRow ? 33 : 0);
    if (f.tileIcon[i] == TI_WEATHER) weatherIcon(d, f.weather[i], x, y - 1);
    else tileIcon(d, f.tileIcon[i], x, y, tone(T_MUTE));  // one muted colour for the icons, white values: the colour is kept for what needs attention
    tileValue(d, f.tileText[i], x + 13, y + 2, tone(T_WHITE));
  }
  box(d, 0, 46, 64, 1, kDivider);
  // The state icons are all 9 px wide: equal gaps between them and to the edges, however many are shown
  int lit[6], m = 0, sum = 0;
  for (int i = 0; i < 6; i++)
    if (f.icon[i] != SI_NONE) { lit[m++] = i; sum += 9; }
  int used = 0;
  for (int k = 0; k < m; k++) {
    const int i = lit[k], w = 9;  // all state icons are 9x9
    const int x = ((64 - sum) * (k + 1) + (m + 1) / 2) / (m + 1) + used;  // k+1 gaps (rounded) plus the icons before this one
    used += w;
    if (f.icon[i] == SI_BULB) {
      const char *const bulb[] = {"...###...", "..#...#..", ".#.....#.", ".#.....#.", "..#...#..", "...#.#...", "...###...", ".........", "...###..."};
      bitmap(d, bulb, x, 48, tone(f.iconTone[i]));
    } else smallIcon(d, f.icon[i], x + 1, 49, tone(f.iconTone[i]));
    char count[4];
    if (!(f.iconOk >> i & 1)) strlcpy(count, "-", sizeof(count));
    else if (f.iconCount[i] > 99) strlcpy(count, "99", sizeof(count));
    else snprintf(count, sizeof(count), "%u", (unsigned)f.iconCount[i]);
    countText(d, count, x + w / 2 - countWidth(count) / 2, 59, tone(f.iconTone[i]));
  }
}

// Where blockOnPoint puts a value relative to its decimal point (dotX 0): the first and the last column it covers
void pointExtent(const char *text, int &first, int &last) {
  const char *dot = strchr(text, '.');
  if (dot) {
    char whole[8] = "";
    const size_t n = dot - text < 7 ? dot - text : 7;
    memcpy(whole, text, n);
    first = -1 - textWidth(whole);
    last = 3 + textWidth(dot + 1) - 1;
  } else {
    last = 3 + glyphWidth('0') - 1;
    first = last + 1 - textWidth(text);
  }
}

// "kWh" after each value, small: lower-case k and h with a capital W, 5 rows high on the digits' baseline
constexpr int kUnitWidth = 13;
void kwhUnit(MatrixPanel_I2S_DMA &d, int x, int y, uint16_t color) {
  static const char *const k[] = {"#..", "#.#", "##.", "#.#", "#.#"};
  static const char *const w[] = {"#...#", "#...#", "#.#.#", "##.##", "#...#"};
  static const char *const h[] = {"#..", "#..", "###", "#.#", "#.#"};
  bitmap(d, k, x, y + 2, color);
  bitmap(d, w, x + 4, y + 2, color);
  bitmap(d, h, x + 10, y + 2, color);
}

// The house row: blue when quiet, then periwinkle, violet and a soft pink-red at the highest load (255); none of the source colours
uint16_t loadColor(int load) {
  static const uint8_t stops[4][3] = {{24, 188, 242}, {130, 150, 240}, {180, 120, 230}, {235, 110, 150}};
  const int at = load * 3 * 256 / 255;  // 0..768
  const int k = at >= 768 ? 2 : at / 256, f = at >= 768 ? 256 : at % 256;
  int c[3];
  for (int j = 0; j < 3; j++) c[j] = stops[k][j] + (stops[k + 1][j] - stops[k][j]) * f / 256;
  return rgb(c[0], c[1], c[2]);
}

// ---- 3x5 digits (the scale text) ----
const char *const kSmallDigit[10][5] = {{"###", "#.#", "#.#", "#.#", "###"}, {".#.", "##.", ".#.", ".#.", "###"}, {"###", "..#", "###", "#..", "###"}, {"###", "..#", "###", "..#", "###"}, {"#.#", "#.#", "###", "..#", "..#"},
                                        {"###", "#..", "###", "..#", "###"}, {"###", "#..", "###", "#.#", "###"}, {"###", "..#", "..#", ".#.", ".#."}, {"###", "#.#", "###", "#.#", "###"}, {"###", "#.#", "###", "..#", "###"}};

// Is there graph on this pixel (screen row yy)? The bars above the baseline, and what hangs below it when power is sent out
bool barPixel(const Frame &f, int x, int yy) {
  const int base = f.baseY ? f.baseY : 61;
  for (int i = 0; i < kBars; i++) {
    if (x < barX(i, !f.narrowStart) || x >= barX(i, !f.narrowStart) + barW(i, !f.narrowStart)) continue;
    if (yy <= base) {
      const int row = base - yy + 1;  // pixels of height needed to reach this row
      return (i <= f.nowCol && (f.useBars[i] >= row || f.solarBars[i] == row)) || (i >= f.nowCol && (f.expectBars[i] >= row || f.forecastBars[i] == row));
    }
    if (f.negRows && yy >= base + 4) {
      const int depth = yy - base - 3;
      return (i <= f.nowCol && depth <= f.exportSolar[i] + f.exportBattery[i]) || (i >= f.nowCol && depth <= f.exportExpect[i]);
    }
    return false;
  }
  return false;
}

// ---- The scale: "75/px" (watt-hours per pixel of height), the readable 3x5 digits, very dim, in a corner of the graph where nothing is: the bottom corners of what is sent out first, then the top ones ----
const char *const kTinySlash[5] = {"..#", "..#", ".#.", "#..", "#.."};
const char *const kTinyP[5] = {"##.", "#.#", "##.", "#..", "#.."};
const char *const kTinyX[5] = {"#.#", "#.#", ".#.", "#.#", "#.#"};

// The glyphs of the scale text, 5 rows each; returns how many and the total width (a gap of 1 between glyphs)
int scaleGlyphs(int wh, const char *const *out[16], int &width) {
  char num[8];
  const int digits = snprintf(num, sizeof(num), "%d", wh);
  int n = 0;
  width = -1;
  auto add = [&](const char *const *g) { out[n++] = g; width += (int)strlen(g[0]) + 1; };
  for (int i = 0; i < digits; i++) add(kSmallDigit[num[i] - '0']);
  add(kTinySlash); add(kTinyP); add(kTinyX);
  return n;
}

struct Box { int x0, y0, x1, y1; };

// Draws the scale (or only checks that it has room, with dry set); false when there is no room
bool scaleLabel(MatrixPanel_I2S_DMA &d, const Frame &f, int top, int strip, const Box *taken, int takenCount, bool dry) {
  if (!f.whPerPx) return true;
  const char *const *glyph[16];
  int width;
  const int n = scaleGlyphs(f.whPerPx, glyph, width), h = 5, base = f.baseY ? f.baseY : 61;
  auto busy = [&](int x, int y) {
    if (y < 0 || y > 63 || x < 0 || x > 63) return false;
    if (y == strip || barPixel(f, x, y)) return true;
    for (int i = 0; i < takenCount; i++) if (x >= taken[i].x0 && x <= taken[i].x1 && y >= taken[i].y0 && y <= taken[i].y1) return true;
    return false;
  };
  auto fits = [&](int x0, int y0) {  // the text and a clear pixel all round
    for (int y = y0 - 1; y <= y0 + h; y++) for (int x = x0 - 1; x <= x0 + width; x++) if (busy(x, y)) return false;
    return true;
  };
  if (!f.negRows) {  // the top row of the graph, where Clockface found room for it
    if (dry) return true;
    const uint16_t color = rgb(46, 54, 70);
    int x = f.scaleSpot == SCALE_RIGHT ? 64 - width : f.scaleSpot == SCALE_LEFT ? 0 : (64 - width) / 2;
    for (int g = 0; g < n; g++) {
      const int gw = (int)strlen(glyph[g][0]);
      for (int y = 0; y < h; y++) for (int xx = 0; xx < gw; xx++) if (glyph[g][y][xx] == '#') box(d, x + xx, top + y, 1, 1, color);
      x += gw + 1;
    }
    return true;
  }
  struct Zone { int y0, y1; bool bottom; } zones[2];
  int count = 0;
  if (f.negRows) zones[count++] = {strip + 2, 63, true};
  zones[count++] = {top, f.negRows ? base : 61, false};
  int px = -1, py = -1;
  auto room = [&](int x0, int y) {  // free columns beside the text, away from its corner
    int free = 0;
    for (int x = x0 == 0 ? width + 1 : x0 - 2; x >= 0 && x < 64; x += x0 == 0 ? 1 : -1, free++)
      for (int yy = y - 1; yy <= y + h; yy++) if (busy(x, yy)) return free;
    return free;
  };
  auto tryRow = [&](int y) {  // a corner of this row: the side with more room beside it, the right one in a tie
    const bool left = fits(0, y), right = fits(64 - width, y);
    if (!left && !right) return;
    const bool useRight = right && (!left || room(64 - width, y) >= room(0, y));
    px = useRight ? 64 - width : 0;
    py = y;
  };
  for (int z = 0; z < count && px < 0; z++) tryRow(zones[z].bottom ? zones[z].y1 - h + 1 : zones[z].y0);  // a real corner first, at the edge the area is aligned to
  for (int z = 0; z < count && px < 0; z++) tryRow(zones[z].bottom ? zones[z].y0 : zones[z].y1 - h + 1);  // then the corners at the other end of the area
  for (int z = 0; z < count && px < 0; z++) {  // then along the same edge, away from what is in the way
    for (int s = 0; s <= zones[z].y1 - zones[z].y0 - h + 1 && px < 0; s++) tryRow(zones[z].bottom ? zones[z].y1 - h + 1 - s : zones[z].y0 + s);
  }
  if (px < 0) return false;
  if (dry) return true;
  const uint16_t color = rgb(46, 54, 70);
  int x = px;
  for (int g = 0; g < n; g++) {
    const int gw = (int)strlen(glyph[g][0]);
    for (int y = 0; y < h; y++) for (int xx = 0; xx < gw; xx++) if (glyph[g][y][xx] == '#') box(d, x + xx, py + y, 1, 1, color);
    x += gw + 1;
  }
  return true;
}

void energyScreen(MatrixPanel_I2S_DMA &d, const Frame &f) {
  header(d, f);
  box(d, 0, 9, 64, 1, kDivider);
  const int rows = f.rowCount;
  // Three blocks side by side: the power (lined up on the end of its unit), the icons and today's energy (lined up on the decimal point,
  // the unit of the total after it). The room left over goes to the gaps around the icon
  int widest = 0, first = 99, last = -99;
  for (int i = 0; i < rows; i++) {
    widest = textWidth(f.left[i]) > widest ? textWidth(f.left[i]) : widest;
    int a, b;
    pointExtent(f.right[i], a, b);
    b += 1 + kUnitWidth;  // the unit stands after every value
    first = a < first ? a : first;
    last = b > last ? b : last;
  }
  const int rightWidth = rows ? last - first + 1 : 0;
  // The numbers sit 1 px from the border; what is left over goes to the gaps either side of the icon
  const int free = 64 - widest - kRowIconWidth - rightWidth, border = free >= 4 ? 1 : 0;
  const int gaps = free - 2 * border, gapLeft = gaps / 2, gapRight = gaps - gapLeft;
  const int leftEnd = border + widest, iconX = leftEnd + gapLeft, dotX = iconX + kRowIconWidth + gapRight - first;
  for (int i = 0; i < rows; i++) {
    const int y = 11 + i * 9;  // 2 clear rows between the lines
    const bool house = f.rowIcon[i] == SI_HOUSE && f.houseLoad;
    const uint16_t color = house ? loadColor(f.houseLoad) : tone(f.rowTone[i]);
    const uint16_t left = house ? color : tone(f.rowLeftTone[i]), right = house ? color : tone(f.rowRightTone[i]);
    block(d, f.left[i], leftEnd - textWidth(f.left[i]), y, left);
    rowIcon(d, f.rowIcon[i], iconX, y, f.batterySoc, color, f.batteryFlow, f.batteryStep);
    blockOnPoint(d, f.right[i], dotX, y, right);
    int a, b;
    pointExtent(f.right[i], a, b);
    kwhUnit(d, dotX + b + 2, y, right);
  }
  const int divider = 9 * rows + 11;  // no line any more: the first bar row, 2 clear rows under the numbers
  const int base = f.baseY ? f.baseY : 61;
  const int strip = f.negRows ? base + 2 : 63;  // the tariff strip: along the bottom, or the divider between the bars and what was sent out, one clear row on each side
  for (int i = 0; i < kBars; i++) {  // the bars fill the room under the numbers (barRoom), starting 2 clear rows below them
    const int x = barX(i, !f.narrowStart), w = barW(i, !f.narrowStart);
    const bool now = i == f.nowCol;
    if (i >= f.nowCol) {  // the expected usage (the hour in progress too, with the measured bars drawn over it) as a dim bar, the expected solar as a green dot above it
      const int e = f.expectBars[i], s = f.forecastBars[i];
      if (e) { box(d, x, base - e + 1, w, e, dim(tone(T_AMBER), 70)); }
      if (s) box(d, x, base - s + 1, w, 1, dim(tone(T_GREEN), 200));
    }
    const int use = f.useBars[i], solar = f.solarBars[i];
    const int green = solar < use ? solar : use;  // stacked: the solar part at the bottom, the rest (grid, battery) on top
    if (green) box(d, x, base - green + 1, w, green, dim(tone(T_GREEN), now ? 255 : 170));
    const int cell = use - green < f.batteryBars[i] ? use - green : f.batteryBars[i];  // the battery's part, teal, above the solar part
    if (cell > 0) box(d, x, base - green - cell + 1, w, cell, dim(tone(T_BATTERY), now ? 255 : 170));
    if (use > green + cell) box(d, x, base - use + 1, w, use - green - cell, tone(now ? T_AMBER : T_FADED));
    if (f.halfRow[i]) {  // the boundary in the middle of a row: the left pixel solar, the right one grid
      const int y = base - f.halfRow[i] + 1;
      box(d, x, y, 1, 1, dim(tone(T_GREEN), now ? 255 : 170));
      box(d, x + 1, y, w - 1, 1, tone(now ? T_AMBER : T_FADED));
    }
    if (solar > use) box(d, x, base - solar + 1, w, 1, tone(T_GREEN));  // solar beyond the use: a green cap
    if (f.negRows) {  // sent out: the solar panels in green, the battery in teal; the expected export dim
      const int es = f.exportSolar[i], eb = f.exportBattery[i], ee = f.exportExpect[i];
      if (i >= f.nowCol && ee) box(d, x, strip + 2, w, ee, dim(tone(T_GREEN), 90));
      if (es) box(d, x, strip + 2, w, es, dim(tone(T_GREEN), now ? 255 : 170));
      if (eb) box(d, x, strip + 2 + es, w, eb, dim(tone(T_BATTERY), now ? 255 : 170));
    }
    if (f.priceBand[i]) {  // the hour's tariff under it: bright from now on, dim for the past
      const uint16_t band = tone(f.priceBand[i] == 1 ? T_GREEN : f.priceBand[i] == 2 ? T_AMBER : f.priceBand[i] == 4 ? T_MUTE : T_RED);
      box(d, x, strip, w, 1, i >= f.nowCol ? band : dim(band, 115));
    }
  }
  scaleLabel(d, f, divider, strip, nullptr, 0, false);
}

void statusScreen(MatrixPanel_I2S_DMA &d, const Frame &f) {
  header(d, f);
  smallIcon(d, SI_HOUSE, 28, 20, tone(T_BLUE));
  homeCentered(d, f.line1, 33, tone(T_WHITE));
  homeCentered(d, f.line2, 41, tone(T_MUTE));
}

const char *const kNote[] = {"...########", "...#......#", "...#......#", "...#......#", "...#......#", ".###....###", "####...####", ".##.....##."};

// One line of the Now playing page: centred when it fits, else scrolling from its start to its end and back
int lineWidth(const char *s) { return homeWidth(s); }

void playingLine(MatrixPanel_I2S_DMA &d, const char *s, int y, uint16_t color, uint16_t tick) {
  const int width = lineWidth(s);
  if (width <= W - 2) {
    int x = (W - width) / 2;
    for (const char *c = s; *c; ++c) { homeGlyph(d, *c, x, y, color); x += letterWidth(*c) + 1; }
    return;
  }
  int x = 1 - pingPong(tick, width - (W - 2));
  for (const char *c = s; *c; ++c) {
    const int w = letterWidth(*c);
    if (x >= 0 && x + w <= W) homeGlyph(d, *c, x, y, color);
    x += w + 1;
  }
}

// Whether a line of the frame is too wide for the panel and so needs scrollTick to run
bool needsScroll(const Frame &f) {
  if (f.page != SCREEN_MEDIA && !f.stationTop) return false;
  if (f.sourceIcon) return false;
  if (f.stationOnly) return lineWidth(f.station) > W - 2;
  if (lineWidth(f.title) > W - 2 || lineWidth(f.showStation ? f.station : f.artist) > W - 2) return true;
  char upper[sizeof(f.station)];
  size_t u = 0;
  for (const char *c = f.station; *c && u + 1 < sizeof(upper); ++c) upper[u++] = toupper(*c);
  upper[u] = 0;
  return f.stationTop && boldTextWidth(upper) > kStationRoom;
}

// A player with nothing to show but its source ("TV", "Line-in"): a picture for the usual ones, else the name in big Impact letters;
// no text bars, no progress line
int sourceIndex(char c) {
  const char *at = c ? strchr(home::sourceart::kChars, toupper((unsigned char)c)) : nullptr;
  return at ? (int)(at - home::sourceart::kChars) : 0;  // anything unknown is a space
}
int sourceGap(const home::sourceart::Font &font) { return font.height >= 22 ? 2 : 1; }
int sourceWidth(const char *s, size_t n, const home::sourceart::Font &font) {
  int width = 0;
  for (size_t i = 0; i < n; i++) width += font.width[sourceIndex(s[i])] + sourceGap(font);
  return width ? width - sourceGap(font) : 0;
}
void sourceLine(MatrixPanel_I2S_DMA &d, const char *s, size_t n, const home::sourceart::Font &font, int y, uint16_t color) {
  int x = (W - sourceWidth(s, n, font)) / 2;
  for (size_t i = 0; i < n; i++) {
    const int index = sourceIndex(s[i]), w = font.width[index];
    const uint32_t *rows = font.rows + index * font.height;
    for (int r = 0; r < font.height; r++)
      for (int c = 0; c < w; c++)
        if (rows[r] >> (w - 1 - c) & 1 && x + c >= 0 && x + c < W) put(d, x + c, y + r, color);
    x += w + sourceGap(font);
  }
}
void sourceScreen(MatrixPanel_I2S_DMA &d, const Frame &f) {
  header(d, f);  // the rest of the panel stays black
  const int top = 9, room = 55;
  if (f.sourceIcon != SRC_TEXT) {
    const home::sourceart::Icon &icon = home::sourceart::kIcons[f.sourceIcon - 1];
    const int x0 = (W - icon.w) / 2, y0 = top + (room - icon.h) / 2;
    for (int y = 0; y < icon.h; y++)
      for (int x = 0; x < icon.w; x++)
        if (const uint8_t k = icon.pixels[y * icon.w + x]) put(d, x0 + x, y0 + y, icon.palette[k]);
    return;
  }
  const size_t n = strlen(f.source);
  const uint16_t ink = rgb(235, 240, 255);
  for (const home::sourceart::Font &font : home::sourceart::kFonts)  // one line, as big as it goes
    if (sourceWidth(f.source, n, font) <= W - 2) { sourceLine(d, f.source, n, font, top + (room - font.height) / 2, ink); return; }
  // Too long: two lines, split at the space or hyphen nearest the middle
  size_t cut = 0;
  for (size_t i = 1; i + 1 < n; i++)
    if ((f.source[i] == ' ' || f.source[i] == '-') && (!cut || labs((long)i - (long)n / 2) < labs((long)cut - (long)n / 2))) cut = i;
  const home::sourceart::Font &small = home::sourceart::kFonts[3];
  if (!cut) { sourceLine(d, f.source, n, small, top + (room - small.height) / 2, ink); return; }  // one long word: as much as fits
  const size_t second = f.source[cut] == ' ' ? cut + 1 : cut + (f.source[cut] == '-' ? 1 : 0);
  const size_t firstLen = f.source[cut] == '-' ? cut + 1 : cut;
  const home::sourceart::Font *use = &small;
  for (int i = 2; i >= 0; i--) {
    const home::sourceart::Font &font = home::sourceart::kFonts[i];
    if (sourceWidth(f.source, firstLen, font) <= W - 2 && sourceWidth(f.source + second, n - second, font) <= W - 2) { use = &font; break; }
  }
  const int gap = 3, y0 = top + (room - 2 * use->height - gap) / 2;
  sourceLine(d, f.source, firstLen, *use, y0, ink);
  sourceLine(d, f.source + second, n - second, *use, y0 + use->height + gap, ink);
}

void mediaScreen(MatrixPanel_I2S_DMA &d, const Frame &f) {
  if (f.sourceIcon) { sourceScreen(d, f); return; }
  if (f.art && sliding && f.backdrop) {
    // the dimmed cover is already behind the slide; only the dimmed bands (as dark as the art under them) are drawn here
  } else if (f.art) {
    // The cover fills the panel; the clock row and the three text lines sit on dimmed bands
    for (int y = 0; y < kArt; y++) {
      const int bandTop = f.stationOnly ? 57 : 51;  // 5-row text with 1 row above and below (two lines: rows 52-56 and 58-62)
      const bool dim = y < 9 || y >= bandTop;
      for (int x = 0; x < kArt; x++) {
        uint16_t c = f.art[y * kArt + x];
        if (dim) c = (c >> 2) & 0x39E7;
        put(d, x, y, c);
      }
    }
  } else {
    bitmap(d, kNote, (W - 11) / 2, 18, tone(T_DIM));
  }
  header(d, f);
  if (f.stationOnly) playingLine(d, f.station, 58, tone(T_PASTEL), f.scrollTick);
  else playingLine(d, f.title, 52, tone(T_WHITE), f.scrollTick);
  if (!f.stationOnly) playingLine(d, f.showStation ? f.station : f.artist, 58, tone(T_MUTE), f.scrollTick);
  if (f.progress) {  // thin line along the bottom edge
    for (int x = 0; x < W; x++) put(d, x, 63, x < f.progress - 1 ? tone(T_BLUE) : tone(T_DIM));
  }
}

// Rows of "dot name": low batteries orange, offline devices in red; names wider than the panel are cut
void devicesScreen(MatrixPanel_I2S_DMA &d, const Frame &f) {
  header(d, f);
  box(d, 0, 9, 64, 1, kDivider);
  view.y0 = 10;  // the first row sits right under the divider  // rows gliding up under the header are cut off at the divider
  for (int i = 0; i < 9 && f.device[i][0]; i++) {
    const int y = 10 + i * 7 - f.deviceDy;
    const bool more = f.deviceKind[i] == SI_NONE;
    if (!more)
      for (int j = 1; j < 4; j++)
        for (int k = 1; k < 4; k++) put(d, k, y + j, tone(f.deviceKind[i] == SI_OFFLINE ? T_RED : T_ORANGE));
    const int left = more ? 1 : 6, room = W - left;
    const int width = homeWidth(f.device[i]);
    int x = left - (width <= room ? 0 : pingPong(f.scrollTick, width - room));  // a long name goes to its end and back, like the music lines
    for (const char *c = f.device[i]; *c; ++c) {
      const int w = letterWidth(*c);
      if (x >= left && x + w <= W) homeGlyph(d, *c, x, y, tone(more ? T_MUTE : T_WHITE));
      x += w + 1;
    }
  }
  view.y0 = 0;
}

// The colour a popup flashes in
uint16_t alertColour(uint8_t alert) {
  switch (alert) {
    case ALERT_BELL: return tone(T_AMBER);
    case ALERT_WASHER: return tone(T_BLUE);
    case ALERT_ALARM: return tone(T_RED);
    default: return tone(T_ORANGE);
  }
}

void alertScreen(MatrixPanel_I2S_DMA &d, const Frame &f) {
  const bool phase = f.alertPhase;
  const bool lit = f.alertFlash && phase;  // flashing: the panel is a block of the alert's colour and the writing turns black
  if (lit) box(d, 0, 0, 64, 64, alertColour(f.alert));
  auto ink = [&](uint16_t colour) -> uint16_t { return lit ? 0 : colour; };
  header(d, f, lit ? 0 : -1);
  int y = 15;
  switch (f.alert) {
    case ALERT_BELL: bitmap(d, kBell, 24 + (phase ? 1 : -1), y, ink(tone(T_AMBER))); break;
    case ALERT_WASHER: bitmap(d, kBigWasher, 24, y, ink(phase ? tone(T_BLUE) : rgb(79, 195, 247))); break;
    case ALERT_GARAGE: bitmap(d, kBigGarage, 24, y, ink(phase ? tone(T_ORANGE) : rgb(122, 74, 0))); break;
    case ALERT_ALARM: y = 11; bitmap(d, kWarning, 24, y, ink(phase ? tone(T_WHITE) : tone(T_RED))); break;
    default: bitmap(d, kWarning, 24, y, ink(phase ? tone(T_ORANGE) : rgb(122, 74, 0))); break;
  }
  const int text = f.alert == ALERT_ALARM ? 31 : 35;
  const uint16_t second = f.alert == ALERT_WASHER ? tone(T_GREEN) : f.alert == ALERT_BELL ? tone(T_MUTE)
                          : f.alert == ALERT_ALARM ? (phase ? tone(T_WHITE) : tone(T_RED)) : tone(T_ORANGE);
  homeCentered(d, f.line1, text, ink(tone(T_WHITE)));
  homeCentered(d, f.line2, text + 8, ink(second));
}

}  // namespace

// The divider under the header doubles as the page indicator: a bright stretch of it marks the page, sliding along with the pages.
// position is in 1/1000 of a page; the stretch wraps round from the last page to the first
void pageBar(MatrixPanel_I2S_DMA &d, int count, int position, int y) {
  if (count < 2) return;
  const int left = position * 64 / (count * 1000), width = (position + 1000) * 64 / (count * 1000) - left, on = dim(tone(T_BLUE), 170);
  for (int i = 0; i < width; i++) put(d, (left + i) % 64, y, on);
}

// The Now playing page with its cover filling the panel: the page bar would only sit on the picture
bool artView(const Frame &f) { return f.page == SCREEN_MEDIA && f.art && !f.backdrop; }

bool slides(const Frame &f) { return f.alert == ALERT_NONE && f.page != SCREEN_STATUS && f.pageCount > 1; }

void drawPage(MatrixPanel_I2S_DMA &d, const Frame &f) {
  if (f.alert != ALERT_NONE) alertScreen(d, f);
  else if (f.page == SCREEN_ENERGY) energyScreen(d, f);
  else if (f.page == SCREEN_STATUS) statusScreen(d, f);
  else if (f.page == SCREEN_MEDIA) mediaScreen(d, f);
  else if (f.page == SCREEN_DEVICES) devicesScreen(d, f);
  else homeScreen(d, f);
}

// The cover of what plays, at a quarter of its brightness, behind a page
void backdrop(MatrixPanel_I2S_DMA &d, const Frame &f) {
  if (!f.backdrop || !f.art) return;
  for (int y = 0; y < kArt; y++)
    for (int x = 0; x < kArt; x++) put(d, x, y, (f.art[y * kArt + x] >> 2) & 0x39E7);
}

bool scrolls(const Frame &f) { return needsScroll(f); }

void draw(MatrixPanel_I2S_DMA &d, const Frame &f) {
  d.fillScreen(0);
  if (f.page != SCREEN_MEDIA) backdrop(d, f);
  drawPage(d, f);
  if (slides(f) && !artView(f)) pageBar(d, f.pageCount, f.pageIndex * 1000, 9);
}

bool canSlide(const Frame &from, const Frame &to) { return slides(from) && slides(to) && from.page != to.page; }

void drawSlide(MatrixPanel_I2S_DMA &d, const Frame &from, const Frame &to, int progress) {
  const int shift = progress * 64 / 1000;
  d.fillScreen(0);
  backdrop(d, to.backdrop ? to : from);
  sliding = true;
  slideStation = from.stationTop || to.stationTop || artView(from) || artView(to);
  view.dx = -shift;
  drawPage(d, from);
  view.dx = 64 - shift;
  drawPage(d, to);
  sliding = false;
  slideStation = false;
  view = View();
  if (artView(from) || artView(to)) return;  // no page bar while the cover is on the panel, or comes or goes
  const int count = to.pageCount, steps = (to.pageIndex + count - from.pageIndex % count) % count;
  pageBar(d, count, (from.pageIndex * 1000 + steps * progress) % (count * 1000), 9);
}

}  // namespace home
