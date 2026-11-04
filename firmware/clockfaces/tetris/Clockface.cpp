// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Clockface.h"
#include <string.h>
#include <esp_random.h>
#include <Settings.h>
#include "TetrisGame.h"
#include "TetrisGameView.h"
#include "TetrisRenderer.h"
#include "TetrisScreens.h"

static TetrisScreens screens;
static tetrisview::Frame lastFrame;
static bool drawn = false;

// The playable board: shown for the Game style, and for a while whenever somebody steers
static tetrisgame::Game game;
static bool gameOn = false;      // the board is on the panel
static bool human = false;       // somebody is steering
static uint32_t lastInput = 0, lastTick = 0;
static const uint32_t kHandBackMs = 20000;  // the computer plays on again after this long without a move

Clockface::Clockface(MatrixPanel_I2S_DMA* display) {
  _display = display;
}

void Clockface::invalidate() { drawn = false; }

void Clockface::setup(ClockTime *dateTime) {
  _dateTime = dateTime;
  screens.reset();
  drawn = false;
  gameOn = human = false;
}

static void startGame() {
  game.reset(esp_random());
  gameOn = true;
  lastTick = millis();
}

void Clockface::steer(int dir) {
  if (dir < 0 || dir > 4) {
    human = false;
    return;
  }
  if (!gameOn) startGame();
  human = true;
  lastInput = millis();
  game.act((tetrisgame::Action)dir);
}

// Classic: redraws only when the frame differs from the last one, which while idle is once a second for the colon.
// Game: the board moves, so every frame is painted.
void Clockface::update() {
  const uint32_t now = millis();
  if (human && now - lastInput > kHandBackMs) human = false;
  const Settings &settings = *Settings::getInstance();
  if (settings.tetrisStyle != 1 && !human) {
    if (gameOn) {
      gameOn = false;
      drawn = false;
    }
    TetrisScreens::Input in = {_dateTime->getHour(), _dateTime->getMinute(), _dateTime->getSecond(),
                               _dateTime->getDay(), _dateTime->getMonth(), now};
    const tetrisview::Frame &frame = screens.advance(in);
    if (drawn && memcmp(&frame, &lastFrame, sizeof(frame)) == 0) return;
    tetrisview::draw(*_display, frame);
    _display->flipDMABuffer();
    lastFrame = frame;
    drawn = true;
    return;
  }

  if (!gameOn) startGame();
  uint32_t dt = now - lastTick;
  lastTick = now;
  if (dt > 100) dt = 100;
  game.tick(dt, human, settings.tetrisLevel > 8 ? 5 : settings.tetrisLevel);
  if (game.phase == tetrisgame::Over && game.phaseMs >= tetrisgame::kOverMs) {
    game.reset(esp_random());
    human = false;  // a lost game ends the turn
  }
  tetrisgame::draw(*_display, game, {_dateTime->getHour(), _dateTime->getMinute(), _dateTime->getDay(), _dateTime->getMonth()});
  _display->flipDMABuffer();
}
