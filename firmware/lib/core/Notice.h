// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once
#include <Arduino.h>

class MatrixPanel_I2S_DMA;

// A message with an optional title that takes the whole panel for a while (sent from Home Assistant over MQTT), on any
// face. Text that does not fit scrolls. A normal message waits for the morning while night mode is on (the night hours, sunset to sunrise or a dark room, whether or not the red night clock is shown); a high
// priority one shows at full brightness at any hour, blinks like the Home face's doorbell and carries a warning sign.
namespace notice {
constexpr size_t kTitleMax = 41;    // 40 characters and the end; the first two lines of ten are shown
constexpr size_t kTextMax = 201;    // 200 characters and the end
constexpr size_t kPayloadMax = 400; // the largest message in JSON or as plain text

enum Priority : uint8_t { PRIORITY_NORMAL, PRIORITY_HIGH };

struct Message {
  char title[kTitleMax] = "";
  char text[kTextMax] = "";
  uint16_t color = 0;    // RGB565 of the title and the blink; 0 picks white (normal) or red (high)
  uint16_t seconds = 0;  // at least this long; 0 picks 10 (normal) or 30 (high). Scrolling text takes the time it needs.
  Priority priority = PRIORITY_NORMAL;
};

void show(const Message &message, uint32_t now);  // an empty message dismisses what is on show
// True while a message is on the panel; night is whether night mode is on. A normal message held back by the night is not shown (and not yet counted down);
// it is dropped when it has waited an hour.
bool active(uint32_t now, bool night);
bool fullBrightness(uint32_t now);  // a high priority message is showing: the panel goes to full brightness
void draw(MatrixPanel_I2S_DMA &gfx, uint32_t now);  // paints only when the picture changes
}
