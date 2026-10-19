// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Notice.h"
#include "NoticeImage.h"
#include <new>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "PanelLayout.h"

namespace notice {
namespace {
constexpr int kCols = (layout::width - 4) / 6;  // 6 pixels a character, 2 pixels of margin on each side
constexpr int kTitleLines = 2, kBodyLines = 40;
constexpr uint32_t kHoldMs = 3600000;                    // a normal message held back by the night is dropped after this
constexpr uint32_t kPauseMs = 2000, kMsPerPixel = 80;    // the scroll rests at both ends and moves about 12 pixels a second
constexpr uint32_t kBlinkMs = 250;                       // as fast as the doorbell
constexpr int kIconTop = 2, kIconHeight = 14;
constexpr uint16_t kRed = 0xF800, kWhite = 0xFFFF;

// The warning sign of the Home face's alerts
const char *const kWarning[] = {".......#.......", "......#.#......", "......#.#......", ".....#...#.....", ".....#...#.....", "....#..#..#....",
                                "....#..#..#....", "...#...#...#...", "...#...#...#...", "..#....#....#..", "..#.........#..", ".#.....#.....#.",
                                ".#...........#.", "###############"};

// On the heap at the first message: the faces have little static RAM to spare, and most clocks never show one
struct State {
  Message current, next;
  std::shared_ptr<Image> image;
  bool imageDone = false;
  bool hasNext = false, on = false, started = false;
  char title[kTitleLines][kCols + 1];
  char body[kBodyLines][kCols + 1];
  int titleCount = 0, bodyCount = 0, headerHeight = 0, viewHeight = 0, scrollMax = 0, shown = -1;
  uint32_t receivedAt = 0, nextAt = 0, startedAt = 0, endsAt = 0, cycleMs = 0, imageAt = 0;
};
State *state = nullptr;

int wrap(const char *p, char (*lines)[kCols + 1], int maxLines) {
  int count = 0;
  while (*p && count < maxLines) {
    while (*p == ' ') p++;
    if (!*p) break;
    int len = 0, lastSpace = 0;
    while (p[len] && len < kCols) { if (p[len] == ' ') lastSpace = len; len++; }
    int take = len;
    if (p[len] && p[len] != ' ' && lastSpace) take = lastSpace;  // break at the last space; a word longer than a line breaks hard
    int keep = take;
    while (keep > 0 && p[keep - 1] == ' ') keep--;
    memcpy(lines[count], p, keep);
    lines[count++][keep] = 0;
    p += take;
  }
  return count;
}

void flatten(char *text) {
  for (; *text; text++) if (*text == '\n' || *text == '\r' || *text == '\t') *text = ' ';
}

int titleTop(const State &n) { return n.current.priority == PRIORITY_HIGH ? kIconTop + kIconHeight + 2 : 2; }

// Wraps the current message and works out how much of the panel the header (warning sign and title) takes
void prepare(State &n) {
  n.image.reset();
  n.imageAt = millis();
  n.imageDone = !n.current.image[0];
  flatten(n.current.title);
  flatten(n.current.text);
  n.titleCount = wrap(n.current.title, n.title, kTitleLines);
  n.bodyCount = wrap(n.current.text, n.body, kBodyLines);
  const int top = titleTop(n);
  n.headerHeight = n.titleCount ? top + n.titleCount * 8 + 3 : (n.current.priority == PRIORITY_HIGH ? top : 0);
  n.viewHeight = layout::height - n.headerHeight;
  n.scrollMax = max(0, n.bodyCount * 8 - n.viewHeight);
  n.started = false;
  n.on = true;
}

void begin(State &n, uint32_t now) {
  uint32_t total = (uint32_t)(n.current.seconds ? n.current.seconds : (n.current.priority == PRIORITY_HIGH ? 30 : 10)) * 1000;
  const bool highImage = n.current.priority == PRIORITY_HIGH && n.image && n.image->status == Image::READY;
  if (!highImage && n.image && n.image->status == Image::READY && (n.current.title[0] || n.current.text[0])) total = max(total, (uint32_t)6000);
  n.cycleMs = 0;
  if (!highImage && n.scrollMax > 0) {  // long text scrolls once at least: rest, move, rest
    n.cycleMs = 2 * kPauseMs + (uint32_t)n.scrollMax * kMsPerPixel;
    const uint32_t scrollTime = n.image && n.image->status == Image::READY ? 2 * n.cycleMs : n.cycleMs;
    if (total < scrollTime) total = scrollTime;
  }
  n.startedAt = now;
  n.endsAt = now + total;
  n.started = true;
  n.shown = -1;
}

bool highShowing(const State &n, uint32_t now) {
  return n.on && n.started && n.current.priority == PRIORITY_HIGH && (int32_t)(now - n.endsAt) < 0;
}
}

void show(const Message &message, uint32_t now) {
  if (!message.title[0] && !message.text[0] && !message.image[0]) {
    if (state) { state->on = state->hasNext = false; state->image.reset(); }
    return;
  }
  if (!state) state = new (std::nothrow) State;
  if (!state) return;
  State &n = *state;
  if (message.priority == PRIORITY_NORMAL && n.on && n.current.priority == PRIORITY_HIGH && (!n.started || highShowing(n, now))) {  // it waits behind the urgent one
    n.next = message;
    n.nextAt = now;
    n.hasNext = true;
    return;
  }
  if (message.priority == PRIORITY_HIGH && n.on && !n.started && n.current.priority == PRIORITY_NORMAL) {  // a held message goes next
    n.next = n.current;
    n.nextAt = n.receivedAt;
    n.hasNext = true;
  }
  n.current = message;
  n.receivedAt = now;
  prepare(n);
}

void poll(uint32_t now) {
  if (!state) return;
  State &n = *state;
  if (!n.on) { n.image.reset(); return; }
  if (n.imageDone) return;
  if (n.image && n.image->status != Image::LOADING) n.imageDone = true;
  else if (!n.image && now - n.imageAt < 30000) n.image = loadImage(n.current.image, n.current.imageFill);
  else if (now - n.imageAt >= 30000) { n.image.reset(); n.imageDone = true; }
  if (n.imageDone && (!n.image || n.image->status != Image::READY) && !n.current.title[0] && !n.current.text[0]) n.on = false;
}

bool active(uint32_t now, bool night) {
  if (!state) return false;
  State &n = *state;
  if (n.on && n.started && (int32_t)(now - n.endsAt) >= 0) n.on = false;
  if (!n.on && n.hasNext) {
    n.current = n.next;
    n.receivedAt = n.nextAt;
    n.hasNext = false;
    prepare(n);
  }
  if (!n.on) return false;
  if (!n.imageDone) return false;  // keep the face visible until the image is ready or has failed
  if (!n.started) {
    if (n.current.priority == PRIORITY_NORMAL && night) {  // held for the morning
      if (now - n.receivedAt > kHoldMs) n.on = false;
      return false;
    }
    begin(n, now);
  }
  return true;
}

bool fullBrightness(uint32_t now) { return state && highShowing(*state, now); }

void draw(MatrixPanel_I2S_DMA &gfx, uint32_t now) {
  if (!state) return;
  State &n = *state;
  const bool high = n.current.priority == PRIORITY_HIGH;
  const bool imagePage = n.image && n.image->status == Image::READY &&
      (high || (!n.current.title[0] && !n.current.text[0]) || ((now - n.startedAt) / 3000) % 2 == 0);
  int offset = 0;
  if (!imagePage && n.scrollMax > 0) {
    uint32_t elapsed = now - n.startedAt;
    if (n.image && n.image->status == Image::READY)
      elapsed = (elapsed / 6000) * 3000 + (elapsed % 6000 > 3000 ? elapsed % 6000 - 3000 : 0);
    const uint32_t t = elapsed % n.cycleMs;  // starts over while the display time lasts
    if (t >= kPauseMs) offset = min((int)((t - kPauseMs) / kMsPerPixel), n.scrollMax);
  }
  const int phase = high && !imagePage ? (int)(((now - n.startedAt) / kBlinkMs) & 1) : 0;
  const int key = (offset * 2 + phase) * 2 + (imagePage ? 1 : 0);
  if (key == n.shown) return;
  n.shown = key;
  const uint16_t color = n.current.color ? n.current.color : (high ? kRed : kWhite);
  const bool lit = phase == 1;  // blinking: the panel is a block of the colour and the writing turns black
  const uint16_t background = lit ? color : 0, ink = lit ? 0 : color, text = lit ? 0 : kWhite;
  gfx.fillScreen(background);
  if (imagePage) {
    for (int y = 0; y < 64; y++)
      for (int x = 0; x < 64; x++) gfx.drawPixel(x, y, n.image->pixels[y * 64 + x]);
    gfx.flipDMABuffer();
    return;
  }
  gfx.setFont(nullptr);
  gfx.setTextSize(1);
  gfx.setTextWrap(false);
  gfx.setTextColor(text);
  const int top = n.headerHeight + (n.scrollMax ? 0 : (n.viewHeight - n.bodyCount * 8) / 2);
  for (int i = 0; i < n.bodyCount; i++) {
    const int y = top + i * 8 - offset;
    if (y + 8 <= n.headerHeight || y >= layout::height) continue;
    gfx.setCursor(2, y);
    gfx.print(n.body[i]);
  }
  if (n.headerHeight) gfx.fillRect(0, 0, layout::width, n.headerHeight, background);  // the text scrolls away under the header
  if (high) {
    for (int row = 0; row < (int)(sizeof(kWarning) / sizeof(kWarning[0])); row++)
      for (int col = 0; kWarning[row][col]; col++)
        if (kWarning[row][col] == '#') gfx.drawPixel(24 + col, kIconTop + row, ink);
  }
  gfx.setTextColor(ink);
  for (int i = 0; i < n.titleCount; i++) {
    gfx.setCursor((layout::width - (int)strlen(n.title[i]) * 6) / 2, titleTop(n) + i * 8);
    gfx.print(n.title[i]);
  }
  if (n.titleCount) gfx.drawFastHLine(8, n.headerHeight - 2, layout::width - 16, ink);
  gfx.flipDMABuffer();
}
}
