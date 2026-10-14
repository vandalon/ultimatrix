#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <algorithm>
#include "PacmanColors.h"

#define UM_DEFAULT_FOOTBALL_LEAGUES "ned.1,ned.cup"
#define UM_DEFAULT_FOOTBALL_TEAMS "449:NED:n"

struct Settings {
  uint8_t displayHeight = 64;
  uint8_t brightness = 32;
  uint8_t rotation = 0;
  bool use24h = true;
  String ntpServer = "time.cloudflare.com";
  bool autoBrightness = false;
  uint8_t ldrPin = 35;
  uint16_t ldrDark = 50, ldrLight = 2500;
  uint8_t minimumBrightness = 3;
  bool nightEnabled = false;
  uint16_t nightStart = 22 * 60, nightEnd = 7 * 60;
  uint8_t nightBrightness = 3;
  bool nightSun = false;  // night from sunset to sunrise at the detected location instead of the fixed hours
  float latitude = 0, longitude = 0;
  bool hasLocation = false;  // set once the network's location has been looked up
  bool swapBlueGreen = false;
  uint8_t matchSecs = 8;
  uint16_t resultMins = 120;
  uint8_t timeStyle = 2;  // Sports clock effect. Words: 0 none, 1 shuffle, 2 rotate all, 3 lift, 4 dissolve, 5 wipe.
  uint8_t numberFont = 1;  // Football: alternative score digits. Time in Words: the language, an index into WORD_LANGUAGES (1 is Dutch)
  uint8_t color = 0, dotColor = 0;
  uint8_t ghostColors[4] = {};  // Pac-Man: 0 lets the face pick a contrasting pastel
  bool ballRoll = true;
  bool showRankings = true;
  bool f1Flags = false;
  bool telnet = false;  // LAN console on port 23, unauthenticated: off unless switched on
  String f1Drivers;
  String footballLeagues = UM_DEFAULT_FOOTBALL_LEAGUES;
  String footballTeams = UM_DEFAULT_FOOTBALL_TEAMS;
  String timezone = "Europe/Amsterdam";  // the detected zone while manualTimezone is off
  bool manualTimezone = false;  // on: timezone was typed in and the geo-IP lookup leaves it alone
  const char *PREF_SHOW_RANKINGS = "showRankings";

  uint16_t wallColor() const { return pacmancolors::wall(color); }
  uint16_t nightColor() const { return pacmancolors::night(color); }
  uint16_t pacmanColor(uint8_t choice) const { return pacmancolors::sprite(choice); }
  uint32_t pacmanColorKey() const { return pacmancolors::key(color, ghostColors, dotColor); }

  static Settings *getInstance() { static Settings value; return &value; }
  void load();
  bool save();
};
