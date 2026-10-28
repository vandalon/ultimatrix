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
    // Watts ("850"): the digits end where the first decimal of a kilowatt value ends
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

// Capitals and digits as tall as the clock's (7 rows): the label letters stretched the way the clock's digits are, rows 2 and 4 doubled
int tallWidth(const char *s) {
  int width = 0;
  for (; *s; ++s) width += (*s >= '0' && *s <= '9' ? 4 : letterWidth(*s)) + 1;
  return width ? width - 1 : 0;
}

void tallText(MatrixPanel_I2S_DMA &d, const char *s, int x, int y, uint16_t color) {
  static const uint8_t kRowOf[7] = {0, 1, 1, 2, 3, 3, 4};
  for (; *s; ++s) {
    const char c = toupper(*s);
    if (c >= '0' && c <= '9') {
      for (int j = 0; j < 7; j++)
        for (int i = 0; i < 4; i++)
          if (numericfont::smallPixel(c - '0', i, j)) put(d, x + i, y + j, color);
      x += 5;
    } else {
      if (c >= 'A' && c <= 'Z')
        for (int j = 0; j < 7; j++)
          for (int i = 0; i < 5; i++)
            if (kLetters[c - 'A'][kRowOf[j]] >> (4 - i) & 1) put(d, x + i, y + j, color);
      x += letterWidth(c) + 1;
    }
  }
}

// ---- Screens ----
bool slideStation = false;  // a slide involves a page with the station name at the top: the date or station of each page slides with it
bool sliding = false;  // a slide between two pages is being drawn: the album art stays put underneath instead of sliding with its page
// mono >= 0 draws everything in that one colour (black on a flashing panel)
void header(MatrixPanel_I2S_DMA &d, const Frame &f, int mono = -1, int dy = 1) {
  const View slide = view;  // the clock and date stay put while a page slides
  view = View();
  int timeX = 13 - textWidth(f.time) / 2;
  for (const char *c = f.time; *c; ++c) {
    const uint16_t color = *c == ':' ? rgb(24 * f.colonLevel / 255, 188 * f.colonLevel / 255, 242 * f.colonLevel / 255) : tone(T_BLUE);
    glyph(d, *c, timeX, dy, mono >= 0 ? mono : color);
    timeX += glyphWidth(*c) + 1;
  }
  if (sliding && slideStation) view = slide;  // the date or station goes with its page, so the two never overlap mid-slide
  if (f.stationTop && f.station[0]) {  // the radio station in the date's place, cut to what fits right of the time
    char name[24];
    size_t n = 0;
    for (const char *c = f.station; *c && n + 1 < sizeof(name); ++c) {
      name[n] = (*c >= 'a' && *c <= 'z') ? *c - 32 : *c;
      name[n + 1] = 0;
      if (countWidth(name) > 35) { name[n] = 0; break; }
      n++;
    }
    char upper[sizeof(f.station)];
    size_t u = 0;
    for (const char *c = f.station; *c && u + 1 < sizeof(upper); ++c) upper[u++] = toupper(*c);
    upper[u] = 0;
    if (tallWidth(upper) <= 35) tallText(d, upper, 62 - tallWidth(upper), dy, mono >= 0 ? mono : tone(T_PASTEL));  // as tall as the clock when it fits
    else countText(d, name, 62 - countWidth(name), 1 + dy, mono >= 0 ? mono : tone(T_PASTEL));
  } else countText(d, f.date, 62 - countWidth(f.date), 1 + dy, mono >= 0 ? mono : tone(T_MUTE));  // "OCT 15", quieter than the time
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

void energyScreen(MatrixPanel_I2S_DMA &d, const Frame &f) {
  header(d, f);
  box(d, 0, 9, 64, 1, kDivider);
  homeText(d, "NOW", 8, 13, tone(T_MUTE));
  homeText(d, "KWH", 43, 13, tone(T_MUTE));
  for (int i = 0; i < 3; i++) {
    const int y = 20 + i * 9;  // the three rows sit 2 px lower
    const uint16_t color = tone(f.rowTone[i]);
    blockOnPoint(d, f.left[i], 10, y, color);
    if (f.rowIcon[i] == SI_BATTERY) battery(d, 31, y, f.batterySoc, color);
    else smallIcon(d, f.rowIcon[i], 31, y, color);
    blockOnPoint(d, f.right[i], 50, y, color);
  }
  box(d, 0, 46, 64, 1, kDivider);
  for (int i = 0; i < kBars; i++) {
    const int x = (64 - (kBars * 3 - 1)) / 2 + i * 3;
    const int use = f.useBars[i], solar = f.solarBars[i];
    if (use) box(d, x, 61 - use + 1, 2, use, tone(i == kBars - 1 ? T_AMBER : T_FADED));
    if (solar) box(d, x, 61 - solar + 1, 2, 1, tone(T_GREEN));
  }
}

void statusScreen(MatrixPanel_I2S_DMA &d, const Frame &f) {
  header(d, f);
  smallIcon(d, SI_HOUSE, 28, 20, tone(T_BLUE));
  homeCentered(d, f.line1, 33, tone(T_WHITE));
  homeCentered(d, f.line2, 41, tone(T_MUTE));
}

const char *const kNote[] = {"...########", "...#......#", "...#......#", "...#......#", "...#......#", ".###....###", "####...####", ".##.....##."};

// One line of the Now playing page: centred when it fits, else scrolling to the left with a gap before it comes round again
void playingLine(MatrixPanel_I2S_DMA &d, const char *s, int y, uint16_t color, uint16_t tick) {
  const int width = homeWidth(s);
  if (width <= W - 2) { homeText(d, s, (W - width) / 2, y, color); return; }
  const int period = width + 8, shift = tick < 20 ? 0 : (tick - 20) % period;
  for (int copy = 0; copy < 2; copy++) {
    int x = 1 - shift + copy * period;
    for (const char *c = s; *c; ++c) {
      const int w = letterWidth(*c);
      if (x >= 0 && x + w <= W) homeGlyph(d, *c, x, y, color);
      x += w + 1;
    }
  }
}

void mediaScreen(MatrixPanel_I2S_DMA &d, const Frame &f) {
  if (f.art && sliding && f.backdrop) {
    // the dimmed cover is already behind the slide; only the dimmed bands (as dark as the art under them) are drawn here
  } else if (f.art) {
    // The cover fills the panel; the clock row and the three text lines sit on dimmed bands
    for (int y = 0; y < kArt; y++) {
      const bool dim = y < 9 || y >= 49;
      for (int x = 0; x < kArt; x++) {
        uint16_t c = f.art[y * kArt + x];
        if (dim) c = (y >= 49 && y < 51) ? (c >> 1) & 0x7BEF : (c >> 2) & 0x39E7;
        put(d, x, y, c);
      }
    }
  } else {
    bitmap(d, kNote, (W - 11) / 2, 18, tone(T_DIM));
  }
  header(d, f);
  playingLine(d, f.title, 51, tone(T_WHITE), f.scrollTick);
  playingLine(d, f.showStation ? f.station : f.artist, 57, tone(T_MUTE), f.scrollTick);
  if (f.progress) {  // thin line along the bottom edge
    for (int x = 0; x < W; x++) put(d, x, 63, x < f.progress - 1 ? tone(T_BLUE) : tone(T_DIM));
  }
}

// Rows of "dot name": low batteries orange, offline devices in red; names wider than the panel are cut
void devicesScreen(MatrixPanel_I2S_DMA &d, const Frame &f) {
  header(d, f);
  box(d, 0, 9, 64, 1, kDivider);
  for (int i = 0; i < 7 && f.device[i][0]; i++) {
    const int y = 11 + i * 7;
    const bool more = f.deviceKind[i] == SI_NONE;
    if (!more) box(d, 1, y + 1, 3, 3, tone(f.deviceKind[i] == SI_OFFLINE ? T_RED : T_ORANGE));
    const int left = more ? 1 : 6, room = W - left;
    const int width = homeWidth(f.device[i]);
    const int period = width + 8, shift = width <= room || f.scrollTick < 20 ? 0 : (f.scrollTick - 20) % period;
    for (int copy = 0; copy < (width <= room ? 1 : 2); copy++) {
      int x = left - shift + copy * period;
      for (const char *c = f.device[i]; *c; ++c) {
        const int w = letterWidth(*c);
        if (x >= left && x + w <= W) homeGlyph(d, *c, x, y, tone(more ? T_MUTE : T_WHITE));
        x += w + 1;
      }
    }
  }
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
  slideStation = from.stationTop || to.stationTop;
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
