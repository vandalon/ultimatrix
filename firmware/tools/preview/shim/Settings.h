#pragma once
#include <Arduino.h>
#include <PacmanColors.h>
struct Settings {
  uint8_t matchSecs = 8;
  uint16_t resultMins = 120;
  bool ballRoll = true;
  uint8_t timeStyle = 2;
  uint8_t numberFont = 1;
  bool showRankings = true;
  uint8_t displayHeight = 64;
  bool showF1 = false;
  bool f1Flags = false;
  String f1Drivers;
  uint8_t color = 0;
  uint8_t pacmanClockColor = 0;
  uint8_t ghost1Color = 0, ghost2Color = 0, dotColor = 0;  // 0 = the face picks
  uint8_t ghostColors[4] = {};
  uint8_t wordLang = 0;  // Time in Words: position in WORD_LANGUAGES
  uint16_t wallColor() const { return pacmancolors::wall(color); }
  uint16_t pacmanColor(uint8_t choice) const { return pacmancolors::sprite(choice); }
  uint16_t nightColor() const { return pacmancolors::night(color); }
  uint32_t pacmanColorKey() const { return pacmancolors::key(color, ghostColors, dotColor); }
  static Settings *getInstance() { static Settings p; return &p; }
};
