// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Clockface.h"
#include <string.h>
#include "TetrisRenderer.h"
#include "TetrisScreens.h"

static TetrisScreens screens;
static tetrisview::Frame lastFrame;
static bool drawn = false;

Clockface::Clockface(MatrixPanel_I2S_DMA* display) {
  _display = display;
}

void Clockface::invalidate() { drawn = false; }

void Clockface::setup(ClockTime *dateTime) {
  _dateTime = dateTime;
  screens.reset();
  drawn = false;
}

// Redraws only when the frame differs from the last one: while idle that is once a second, for the colon
void Clockface::update() {
  TetrisScreens::Input in = {_dateTime->getHour(), _dateTime->getMinute(), _dateTime->getSecond(),
                             _dateTime->getDay(), _dateTime->getMonth(), (uint32_t)millis()};
  const tetrisview::Frame &frame = screens.advance(in);
  if (drawn && memcmp(&frame, &lastFrame, sizeof(frame)) == 0) return;
  tetrisview::draw(*_display, frame);
  _display->flipDMABuffer();
  lastFrame = frame;
  drawn = true;
}
