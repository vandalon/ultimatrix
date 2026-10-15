// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "HomeRenderer.h"

#include <string.h>
#include "FaceKit.h"
#include "MiniFont.h"
#include "NumericFont.h"

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
    default: return rgb(232, 238, 242);
  }
}

namespace {

// A starry night behind everything else: most stars a single dim pixel, a few brighter points for a bit of
// twinkle, scattered across the whole screen so the clock's own content just paints over them.
struct Star { uint8_t x, y; bool bright; };
constexpr Star kStars[] = {{3, 3, false}, {9, 8, true}, {16, 4, false}, {22, 2, false}, {28, 7, false}, {36, 3, false},
                           {42, 9, false}, {48, 5, false}, {54, 3, true}, {60, 8, false}, {62, 16, false}, {2, 20, true},
                           {1, 30, false}, {8, 25, false}, {14, 33, false}, {20, 28, false}, {26, 36, false}, {33, 22, true},
                           {40, 30, false}, {46, 26, false}, {52, 33, false}, {61, 24, false}, {60, 34, true}, {2, 38, false},
                           {61, 44, false}, {3, 50, false}, {10, 55, false}, {17, 58, false}, {24, 54, false}, {31, 59, true},
                           {38, 55, false}, {45, 58, false}, {51, 50, false}, {58, 52, false}, {44, 8, true}, {18, 10, false}};

void backdrop(MatrixPanel_I2S_DMA &d) {
  for (const Star &s : kStars) put(d, s.x, s.y, s.bright ? rgb(28, 54, 66) : rgb(14, 30, 38));
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

const char *const kDoor[] = {".#####.", ".#...#.", ".#...#.", ".#.#.#.", ".#...#.", ".#...#.", "#######"};
const char *const kWindow[] = {"#######", "#..#..#", "#..#..#", "#######", "#..#..#", "#..#..#", "#######"};
const char *const kGarage[] = {"...#...", "..#.#..", ".#...#.", ".#####.", ".#...#.", ".#####.", ".#...#."};
const char *const kWasher[] = {"#######", "#.....#", "#.###.#", "#.#.#.#", "#.###.#", "#.....#", "#######"};
const char *const kDryer[] = {"#######", "#.....#", "#.#.#.#", "#.....#", "#.#.#.#", "#.....#", "#######"};
const char *const kBulb[] = {"..###..", ".#####.", ".#####.", ".#####.", "..###..", "..###..", "...#..."};
const char *const kLock[] = {"..###..", ".#...#.", ".#...#.", "#######", "###.###", "###.###", "#######"};
const char *const kPlug[] = {".#...#.", ".#...#.", "#######", "#######", ".#####.", "..###..", "...#..."};
const char *const kHouse[] = {"...#...", "..#.#..", ".#...#.", "#.....#", "#.###.#", "#.#.#.#", "#######"};
const char *const kSun[] = {"...#...", ".#...#.", "..###..", "#.###.#", "..###..", ".#...#.", "...#..."};
const char *const kPole[] = {".#####.", "...#...", ".#####.", "...#...", "...#...", "...#...", "..###.."};

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
    case SI_DOOR: bitmap(d, kDoor, x, y, color); break;
    case SI_WINDOW: bitmap(d, kWindow, x, y, color); break;
    case SI_GARAGE: bitmap(d, kGarage, x, y, color); break;
    case SI_WASHER: bitmap(d, kWasher, x, y, color); break;
    case SI_DRYER: bitmap(d, kDryer, x, y, color); break;
    case SI_BULB: bitmap(d, kBulb, x, y, color); break;
    case SI_LOCK: bitmap(d, kLock, x, y, color); break;
    case SI_PLUG: bitmap(d, kPlug, x, y, color); break;
    case SI_HOUSE: bitmap(d, kHouse, x, y, color); break;
    case SI_SUN: bitmap(d, kSun, x, y, color); break;
    case SI_POLE: bitmap(d, kPole, x, y, color); break;
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

void tileIcon(MatrixPanel_I2S_DMA &d, uint8_t icon, int x, int y, uint16_t color) {
  switch (icon) {
    case TI_THERMOMETER: bitmap(d, kThermometer, x, y, color); break;
    case TI_DROP: bitmap(d, kDrop, x, y + 1, color); break;
    case TI_PERSON: bitmap(d, kPerson, x, y, color); break;
    case TI_BOLT: bitmap(d, kBolt, x, y, color); break;
    case TI_RING: bitmap(d, kRing, x, y + 1, color); break;
    default: break;
  }
}

// ---- Text in the block digits of the boot screen (4x7), letters from the 3x5 font ----
int glyphWidth(char c) {
  if (c >= '0' && c <= '9') return 4;
  if (c == '.' || c == ':') return 2;
  if (c == '`') return 3;
  if (c == '%') return 5;
  return 3;
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
    const uint8_t *g = miniGlyph(c);
    for (int j = 0; j < 5; j++)
      for (int i = 0; i < 3; i++)
        if (g[j] >> (2 - i) & 1) put(d, x + i, y + 2 + j, color);
  }
}

void block(MatrixPanel_I2S_DMA &d, const char *text, int x, int y, uint16_t color) {
  for (; *text; ++text) {
    glyph(d, *text, x, y, color);
    x += glyphWidth(*text) + 1;
  }
}

void blockCentered(MatrixPanel_I2S_DMA &d, const char *text, int cx, int y, uint16_t color) {
  block(d, text, cx - textWidth(text) / 2, y, color);
}

// Numbers line up on their decimal point: the point is at dotX, whole numbers end before it, the rest follows
void blockOnPoint(MatrixPanel_I2S_DMA &d, const char *text, int dotX, int y, uint16_t color) {
  const char *dot = strchr(text, '.');
  char whole[8] = "";
  if (!dot) {
    // Watts ("850W"): the digits end where the first decimal of a kilowatt value ends, so the W stands under the K
    size_t digits = 0;
    while (text[digits] >= '0' && text[digits] <= '9' && digits < sizeof(whole) - 1) digits++;
    if (!digits) { block(d, text, dotX - 1 - textWidth(text), y, color); return; }  // "-": nothing to show
    memcpy(whole, text, digits);
    whole[digits] = 0;
    const int unitX = dotX + 3 + glyphWidth('0') + 1;
    block(d, whole, unitX - 1 - textWidth(whole), y, color);
    block(d, text + digits, unitX, y, color);
    return;
  }
  size_t n = dot - text < 7 ? dot - text : 7;
  memcpy(whole, text, n);
  whole[n] = 0;
  block(d, whole, dotX - 1 - textWidth(whole), y, color);
  glyph(d, '.', dotX, y, color);
  block(d, dot + 1, dotX + 3, y, color);
}

// ---- Screens ----
// mono >= 0 draws everything in that one colour (black on a flashing panel)
void header(MatrixPanel_I2S_DMA &d, const Frame &f, bool dots, int mono = -1) {
  blockCentered(d, f.time, 13, 2, mono >= 0 ? mono : tone(T_BLUE));
  blockCentered(d, f.date, 51, 2, mono >= 0 ? mono : tone(T_BLUE));  // the same HA blue as the time
  if (dots && f.pageCount > 1) {
    const int x0 = 32 - (f.pageCount * 4 - 2) / 2;
    for (int i = 0; i < f.pageCount; i++) box(d, x0 + i * 4, 4, 2, 2, mono >= 0 ? mono : tone(i == f.pageIndex ? T_BLUE : T_DIM));
  }
}

void homeScreen(MatrixPanel_I2S_DMA &d, const Frame &f) {
  backdrop(d);
  header(d, f, true);
  box(d, 4, 10, 56, 1, rgb(20, 32, 42));
  for (int i = 0; i < 3; i++) {
    if (f.tileIcon[i] == TI_NONE && !f.tileText[i][0]) continue;
    const int cx = 11 + i * 21;
    tileIcon(d, f.tileIcon[i], cx - 5, 17, tone(f.tileTone[i]));
    blockCentered(d, f.tileText[i], cx, 32, tone(f.tileTone[i]));
  }
  box(d, 4, 45, 56, 1, rgb(20, 32, 42));
  for (int i = 0; i < 6; i++) smallIcon(d, f.icon[i], 3 + i * 10, 52, tone(f.iconTone[i]));
}

void energyScreen(MatrixPanel_I2S_DMA &d, const Frame &f) {
  backdrop(d);
  header(d, f, true);
  box(d, 4, 10, 56, 1, rgb(20, 32, 42));
  miniText(d, "NOW", 10, 12, tone(T_MUTE));
  miniText(d, "KWH", 45, 12, tone(T_MUTE));
  for (int i = 0; i < 3; i++) {
    const int y = 19 + i * 9;
    const uint16_t color = tone(f.rowTone[i]);
    blockOnPoint(d, f.left[i], 13, y, color);
    if (f.rowIcon[i] == SI_BATTERY) battery(d, 31, y, f.batterySoc, color);
    else smallIcon(d, f.rowIcon[i], 31, y, color);
    blockOnPoint(d, f.right[i], 50, y, color);
  }
  box(d, 4, 46, 56, 1, rgb(20, 32, 42));
  for (int i = 0; i < kBars; i++) {
    const int x = 5 + i * 3;
    const int use = f.useBars[i], solar = f.solarBars[i];
    if (use) box(d, x, 61 - use + 1, 2, use, tone(i == kBars - 1 ? T_AMBER : T_FADED));
    if (solar) box(d, x, 61 - solar + 1, 2, 1, tone(T_GREEN));
  }
}

void statusScreen(MatrixPanel_I2S_DMA &d, const Frame &f) {
  header(d, f, false);
  smallIcon(d, SI_HOUSE, 28, 20, tone(T_BLUE));
  miniCentered(d, f.line1, 33, tone(T_WHITE));
  miniCentered(d, f.line2, 41, tone(T_MUTE));
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
  header(d, f, false, lit ? 0 : -1);
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
  miniCentered(d, f.line1, text, ink(tone(T_WHITE)));
  miniCentered(d, f.line2, text + 8, ink(second));
}

}  // namespace

void draw(MatrixPanel_I2S_DMA &d, const Frame &f) {
  d.fillScreen(0);
  if (f.alert != ALERT_NONE) alertScreen(d, f);
  else if (f.page == SCREEN_ENERGY) energyScreen(d, f);
  else if (f.page == SCREEN_STATUS) statusScreen(d, f);
  else homeScreen(d, f);
}

}  // namespace home
