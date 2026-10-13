// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "BootScreen.h"
#include <Fonts/Picopixel.h>
#include "NumericFont.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
extern "C" {
#include "qrcodegen.h"
}

namespace boot {
namespace {

// 0..255 raised cosine, 64 steps per period
const uint8_t kPulse[64] = {
    0,   1,   2,   5,   10,  15,  21,  29,  37,  47,  57,  67,  79,  90,  103, 115,
    127, 140, 152, 165, 176, 188, 198, 208, 218, 226, 234, 240, 245, 250, 253, 254,
    255, 254, 253, 250, 245, 240, 234, 226, 218, 208, 198, 188, 176, 165, 152, 140,
    128, 115, 103, 90,  79,  67,  57,  47,  37,  29,  21,  15,  10,  5,   2,   1};

constexpr int kSize = 64;
constexpr uint32_t kFadeMs = 1400;       // the logo's fade-in
constexpr uint32_t kDimMs = 900;         // the U dimming down once the Wi-Fi symbol comes in
constexpr uint32_t kLabelPeriodMs = 1800;
constexpr uint32_t kPulsePeriodHalf(uint32_t period) { return period / 2; }  // the peak
constexpr uint32_t kInvertMs = 5000;     // the access point code flips between normal and inverted

struct Rgb { uint8_t r, g, b; };
constexpr Rgb kCyan{70, 210, 255}, kBlue{40, 110, 255}, kGreen{80, 220, 120}, kOrange{255, 130, 30}, kViolet{190, 120, 255}, kWhite{255, 255, 255};
constexpr Rgb kAddress{165, 180, 215};  // the address digits: one flat, slightly dimmed and blue-tinted white, apart from the blue/cyan logo and icons

const uint8_t *levelMap = nullptr;
unsigned mapped(unsigned v) { return levelMap ? levelMap[v > 255 ? 255 : v] : v; }

uint16_t pack(unsigned r, unsigned g, unsigned b) { return (uint16_t)(((mapped(r) >> 3) << 11) | ((mapped(g) >> 2) << 5) | (mapped(b) >> 3)); }

uint16_t shade(Rgb c, unsigned level) {  // level 0..255
  return pack(c.r * level / 255, c.g * level / 255, c.b * level / 255);
}

// The icons' gradient: t 0 (top) to 1 (bottom) runs from a lighter to a deeper shade of the colour
Rgb tint(Rgb c, float t) {
  float lift = 0.3f * (1 - t), deepen = 1 - 0.22f * t;
  auto channel = [&](uint8_t v) { return (uint8_t)fminf(255, (v + (255 - v) * lift) * deepen); };
  return {channel(c.r), channel(c.g), channel(c.b)};
}

// Picopixel's cursor is the baseline and its advance is 4 px
void centredSmall(Adafruit_GFX &gfx, const char *text, int baseline, uint16_t color) {
  int x = (kSize - 4 * (int)strlen(text)) / 2;
  gfx.setFont(&Picopixel);
  gfx.setCursor(x < 0 ? 0 : x, baseline);
  gfx.setTextColor(color);
  gfx.print(text);
  gfx.setFont(nullptr);
}

// Full-screen backdrop: a dim lattice of single LEDs (every other pixel) with a U lit in it, fading toward the
// bottom. The icons are drawn over it, so it stays well below their brightness.
constexpr float kCentreY = 40, kOuterX = 28, kOuterY = 22, kInnerX = 16, kInnerY = 14;

// Brightness 0..255 of the backdrop at a pixel (0 between the lattice LEDs). glow 0 is the dim backdrop, 255 the U
// shown at full brightness as the logo.
unsigned backdropLevel(int x, int y, unsigned glow = 0) {
  if (!(x & 1) || !(y & 1)) return 0;
  float dx = x - 32.0f, dy = y - kCentreY;
  float ox = dx < 0 ? kOuterX + 1 : kOuterX;  // the U's outer edge is one pixel wider on the left
  bool outer = y <= kCentreY ? fabsf(dx) <= ox : (dx * dx) / (ox * ox) + (dy * dy) / (kOuterY * kOuterY) <= 1;
  bool inner = y <= kCentreY ? fabsf(dx) <= kInnerX : (dx * dx) / (kInnerX * kInnerX) + (dy * dy) / (kInnerY * kInnerY) <= 1;
  if (!(outer && !inner)) return 10;
  unsigned dim = 70, bright = 255 - 190 * y / kSize;  // the dimmed U is one flat level, only the lit logo has the gradient
  return dim + (bright - dim) * glow / 255;
}

Plot888 plot888 = nullptr;

void backdrop(Adafruit_GFX &gfx, unsigned glow = 0, unsigned fade = 255) {  // fade 0..255: the logo's fade-in, a wipe down from the top
  for (int y = 1; y < kSize; y += 2) {
    int edge = (int)(fade * 13 / 10) - y * 255 / kSize;  // the wipe's soft edge is 76 (0.3 of the height) wide
    unsigned rowFade = edge <= 0 ? 0 : edge >= 76 ? 255 : edge * 255 / 76;
    for (int x = 1; x < kSize; x += 2) {
      int t = 255 * y / kSize * (int)glow / 255;  // cyan at the top to blue at the bottom when lit; the dimmed U is plain cyan
      Rgb c{(uint8_t)(kCyan.r + (kBlue.r - kCyan.r) * t / 255), (uint8_t)(kCyan.g + (kBlue.g - kCyan.g) * t / 255),
            (uint8_t)(kCyan.b + (kBlue.b - kCyan.b) * t / 255)};
      unsigned level = backdropLevel(x, y, glow) * rowFade / 255;
      if (plot888) plot888(gfx, x, y, c.r * level, c.g * level, c.b * level);
      else gfx.drawPixel(x, y, shade(c, level));
    }
  }
}

// The icons are 32-wide pixel art at their original size: the Wi-Fi symbol
// and a round clock with an alert badge for a missing time. Each is centred above the address and drawn as a blend
// between the backdrop and the icon colour, so a pulse fades the icon into the U instead of to black.
enum Icon : int8_t { kWifi, kNoTime, kUpdate };

const char *const kWifiArt[] = {
    "............########............",
    "........################........",
    "......####################......",
    "....########################....",
    "...##########################...",
    ".############......############.",
    "##########............##########",
    ".#######................#######.",
    "..####.......######.......####..",
    "...##.....############.....##...",
    "........################........",
    ".......##################.......",
    "......####################......",
    ".......##################.......",
    "........####........####........",
    ".........##..........##.........",
    "................................",
    ".............######.............",
    "............########............",
    ".............######.............",
    "..............####..............",
    "...............##...............",
};

const char *const kNoTimeArt[] = {
    ".......##########...............",
    ".....##############.............",
    "....####........####............",
    "...###............###...........",
    "..###......##......###..........",
    ".###.......##.......###.........",
    ".##........##........##.........",
    "###........##........###........",
    "##.........##.........##........",
    "##.........##.........##........",
    "##.........##.........##........",
    "##.........########...##........",
    "##.........########...##........",
    "##....................##........",
    "##....................##........",
    "##....................##........",
    "###..................#..........",
    ".##.............................",
    ".###..................######....",
    "..###................########...",
    "...###..............##########..",
    "....####........#..#####..#####.",
    ".....###########..######..######",
    ".......#########..######..######",
    "..................######..######",
    "..................######..######",
    "..................######..######",
    "..................##############",
    "...................#####..#####.",
    "....................####..####..",
    ".....................########...",
    "......................######....",
};

// A download arrow dropping into a tray: new firmware is waiting
const char *const kUpdateArt[] = {
    ".............######.............",
    ".............######.............",
    ".............######.............",
    ".............######.............",
    ".............######.............",
    ".............######.............",
    "......####################......",
    ".......##################.......",
    "........################........",
    ".........##############.........",
    "..........############..........",
    "...........##########...........",
    "............########............",
    ".............######.............",
    "..............####..............",
    "................................",
    "..##........................##..",
    "..##........................##..",
    "..##........................##..",
    "..############################..",
    "..############################..",
};

// Works out which panel pixels the icon covers (once per icon) and the rows it spans
void buildIcon(IconCache &cache, Icon icon) {
  const char *const *art = icon == kWifi ? kWifiArt : icon == kNoTime ? kNoTimeArt : kUpdateArt;
  const int rows = icon == kWifi ? sizeof(kWifiArt) / sizeof(*kWifiArt)
      : icon == kNoTime ? sizeof(kNoTimeArt) / sizeof(*kNoTimeArt) : sizeof(kUpdateArt) / sizeof(*kUpdateArt);
  const int left = (kSize - 32) / 2, top = (icon == kUpdate ? 23 : 26) - rows / 2;  // centred on (32, 26); the update icon sits higher so icon and two-line label centre together
  memset(cache.cover, 0, sizeof(cache.cover));
  for (int y = 0; y < rows; ++y)
    for (int x = 0; x < 32; ++x)
      if (art[y][x] == '#') cache.cover[(top + y) * kSize + left + x] = 255;
  cache.top = top;
  cache.bottom = top + rows - 1;
  cache.icon = icon;
}

// Draws an icon in colour c over the backdrop; level 0..255 is how far it has faded in. The colour runs from
// lighter at the top to deeper at the bottom.
void drawIcon(Adafruit_GFX &gfx, IconCache &cache, Icon icon, Rgb c, unsigned level, unsigned glow) {
  if (cache.icon != icon) buildIcon(cache, icon);
  for (int y = cache.top; y <= cache.bottom; ++y) {
    Rgb g = tint(c, (float)(y - cache.top) / (cache.bottom - cache.top));
    for (int x = 0; x < kSize; ++x) {
      unsigned w = cache.cover[y * kSize + x] * level / 255;  // 0..255
      if (!w) continue;
      unsigned back = backdropLevel(x, y, glow);
      unsigned r = ((kCyan.r * back / 255) * (255 - w) + g.r * w) / 255;
      unsigned gr = ((kCyan.g * back / 255) * (255 - w) + g.g * w) / 255;
      unsigned b = ((kCyan.b * back / 255) * (255 - w) + g.b * w) / 255;
      gfx.drawPixel(x, y, pack(r, gr, b));
    }
  }
}

// A tick in a ring: someone has joined the access point
void joinedIcon(Adafruit_GFX &gfx, uint16_t color) {
  gfx.fillCircle(32, 22, 17, color);
  gfx.fillCircle(32, 22, 14, 0);
  for (int d = 0; d < 3; ++d) {
    gfx.drawLine(23, 22 + d, 29, 28 + d, color);
    gfx.drawLine(29, 28 + d, 42, 15 + d, color);
  }
}

// An address in the one-pixel-stroke block digits (4 x 7, as in the football score), centred. The dots are 2 x 2;
// an address too wide for that (255.255.255.255) gets 1 x 2 dots, and one still too wide falls back to the
// small font
void blockText(Adafruit_GFX &gfx, const char *text, int top, Rgb color) {
  auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
  int digits = 0, dots = 0;
  for (const char *c = text; *c; ++c) isDigit(*c) ? ++digits : ++dots;
  int dotWidth = 2;
  int width = digits * 5 + dots * (dotWidth + 1) - 1;
  if (width > kSize) { dotWidth = 1; width = digits * 5 + dots * dotWidth - 1; }
  if (width > kSize) { centredSmall(gfx, text, top + 6, shade(color, 255)); return; }
  int x = (kSize - width) / 2;
  for (const char *c = text; *c; ++c) {
    if (isDigit(*c)) {
      for (int j = 0; j < 7; ++j)
        for (int i = 0; i < 4; ++i)
          if (numericfont::smallPixel(*c - '0', i, j)) gfx.drawPixel(x + i, top + j, shade(color, 255));
      x += 5;
    } else {
      for (int j = 5; j < 7; ++j) gfx.fillRect(x, top + j, dotWidth, 1, shade(color, 255));
      x += dotWidth + (dotWidth > 1 ? 1 : 0);
    }
  }
}

// Escape the characters that are special in the WIFI: payload
void append(char *out, size_t size, const char *text) {
  size_t n = strlen(out);
  for (; *text && n + 2 < size; ++text) {
    if (strchr("\\;,:\"", *text)) out[n++] = '\\';
    out[n++] = *text;
  }
  out[n] = 0;
}

}  // namespace

void setPlot888(Plot888 plot) { plot888 = plot; }
void setLevelMap(const uint8_t *map) { levelMap = map; }

uint8_t pulse(uint32_t ms, uint32_t period, uint32_t offset) {
  if (!period) return 255;
  return kPulse[((ms + offset) % period) * 64 / period];
}

void Screen::setAccessPoint(const char *ssid, const char *password, const char *address) {
  snprintf(ssid_, sizeof(ssid_), "%s", ssid ? ssid : "");
  snprintf(address_, sizeof(address_), "%s", address ? address : "");
  // An open network leaves T:nopass out (an omitted type means open): with an access point name of 8 characters
  // or fewer the text is 17 bytes or less and fits the smallest code (21 modules)
  bool open = !password || !*password;
  char payload[160];
  snprintf(payload, sizeof(payload), open ? "WIFI:S:" : "WIFI:T:WPA;S:");
  append(payload, sizeof(payload), ssid_);
  if (!open) {
    strncat(payload, ";P:", sizeof(payload) - strlen(payload) - 1);
    append(payload, sizeof(payload), password);
  }
  strncat(payload, ";;", sizeof(payload) - strlen(payload) - 1);
  uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
  static_assert(sizeof(qr_) >= qrcodegen_BUFFER_LEN_FOR_VERSION(6), "qr_ too small");
  qrReady_ = qrcodegen_encodeText(payload, temp, qr_, qrcodegen_Ecc_LOW, 1, 6, qrcodegen_Mask_AUTO, true);
  drawn_ = false;
}

bool Screen::update(Adafruit_GFX &gfx, Kind kind, uint32_t since, const char *text) {
  // One pulse clock runs across Connecting/Syncing/Error, so the next screen carries on from the phase
  bool pulsing = kind == Kind::Connecting || kind == Kind::Syncing || kind == Kind::Error;
  bool wasPulsing = lastKind_ == Kind::Connecting || lastKind_ == Kind::Syncing || lastKind_ == Kind::Error;
  if (kind != lastKind_) {
    if (!(pulsing && wasPulsing)) clock_ = since;
    // Entering Connected from a pulse: keep pulsing until the next peak, then hold it
    settling_ = kind == Kind::Connected && wasPulsing;
    if (settling_) settleAt_ = clock_ + (kPulsePeriodHalf(kLabelPeriodMs) + kLabelPeriodMs - clock_ % kLabelPeriodMs) % kLabelPeriodMs;
    lastKind_ = kind;
  } else if (since >= lastSince_) {
    clock_ += since - lastSince_;
  }
  lastSince_ = since;

  Frame now;
  memset(&now, 0, sizeof(now));
  now.kind = (uint8_t)kind;
  snprintf(now.text, sizeof(now.text), "%s", text ? text : "");
  switch (kind) {
    case Kind::Logo:
      now.level = pulse(since < kFadeMs ? since : kFadeMs, 2 * kFadeMs) >> 3;  // 0..31, rising once
      now.glow = 31;
      break;
    case Kind::Joined:
      now.level = 31;
      break;
    case Kind::Setup:
      now.page = (since / kInvertMs) & 1;  // 0: white modules on black (the first), 1: black on white
      break;
    case Kind::Connected:
      if (settling_ && clock_ < settleAt_) now.level = pulse(clock_, kLabelPeriodMs) >> 3;
      else { settling_ = false; now.level = 31; }  // steady green
      break;
    default:
      now.level = pulse(pulsing ? clock_ : since, kLabelPeriodMs) >> 3;
      if (kind == Kind::Connecting && since < kDimMs) now.glow = 31 - 31 * since / kDimMs;  // the logo's U dims as the symbol comes in
      break;
  }
  if (drawn_ && !memcmp(&now, &last_, sizeof(now))) return false;
  last_ = now;
  drawn_ = true;

  unsigned level = now.level * 255 / 31;
  gfx.fillScreen(0);
  gfx.setTextWrap(false);
  gfx.setTextSize(1);
  gfx.setFont(nullptr);
  unsigned glow = now.glow * 255 / 31;
  if (kind != Kind::Setup) backdrop(gfx, glow, kind == Kind::Logo ? level : 255);
  switch (kind) {
    case Kind::Logo:  // just the U, faded in
      break;
    case Kind::Connecting:  // blue and pulsing until the network is up
      drawIcon(gfx, icon_, kWifi, kBlue, level, glow);
      break;
    case Kind::Syncing:  // linked, waiting for the time: green and pulsing
      drawIcon(gfx, icon_, kWifi, kGreen, level, glow);
      blockText(gfx, now.text, 54, kAddress);
      break;
    case Kind::Connected:  // linked and time synced: green
      drawIcon(gfx, icon_, kWifi, kGreen, level, glow);
      blockText(gfx, now.text, 54, kAddress);
      break;
    case Kind::Error:  // linked, but no time: the settings page is reachable at the address
      drawIcon(gfx, icon_, kNoTime, kOrange, level, glow);
      blockText(gfx, now.text, 54, kAddress);
      break;
    case Kind::Update:  // a newer signed firmware is on the server: violet and pulsing
      drawIcon(gfx, icon_, kUpdate, kViolet, level, glow);
      centredSmall(gfx, "UPDATE", 43, shade(kViolet, 255));
      centredSmall(gfx, "AVAILABLE", 50, shade(kViolet, 255));
      break;
    case Kind::Joined:
      joinedIcon(gfx, shade(kGreen, 255));
      blockText(gfx, address_, 54, kAddress);
      break;
    case Kind::Setup: {
      int size = qrReady_ ? qrcodegen_getSize(qr_) : 0;
      if (size) {
        // The largest whole-pixel module size that fits (3 px for the smallest code, 63 px); the rest is the border
        int scale = kSize / size;
        if (scale < 1) scale = 1;
        int margin = (kSize - size * scale) / 2;
        uint16_t white = shade(kWhite, 255), background = now.page ? white : 0, module = now.page ? 0 : white;  // page 0: white on black
        gfx.fillScreen(background);
        for (int y = 0; y < size; ++y)
          for (int x = 0; x < size; ++x)
            if (qrcodegen_getModule(qr_, x, y))
              gfx.fillRect(margin + x * scale, margin + y * scale, scale, scale, module);
      } else {
        backdrop(gfx, 255);  // no code to show: the logo
      }
      break;
    }
  }
  return true;
}

}  // namespace boot
