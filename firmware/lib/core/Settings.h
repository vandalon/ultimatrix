#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <algorithm>
#include <atomic>
#include "PacmanColors.h"

#define UM_DEFAULT_FOOTBALL_LEAGUES "ned.1,ned.cup"
#define UM_DEFAULT_FOOTBALL_TEAMS "449:NED:n"

struct Settings {
  bool mdnsUseName = false;  // use a hostname-safe clock name instead of ultimatrix
  String clockName = "Ultimatrix";  // own NVS key, shared by all faces
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
  uint8_t timeStyle = 2;  // Sports clock effect. Words: 0 none, 1 shuffle, 2 rotate all, 3 lift, 4 dissolve, 5 wipe (low 4 bits); the high bits hold the date: 0 numbers, 1 none, 2 in words.
  uint8_t faceOptions[5] = {1, 1, 1, 2, 2};  // each face's own numberFont/timeStyle value (see Settings.cpp); only the running face's pair is in numberFont and timeStyle
  uint8_t numberFont = 1;  // Football: alternative score digits. Time in Words: the language, an index into WORD_LANGUAGES (1 is Dutch)
  uint8_t color = 0, dotColor = 0;
  uint8_t pacmanClockColor = 0;  // default keeps off-white hours and green minutes
  uint8_t ghostColors[4] = {};  // Pac-Man: 0 lets the face pick a contrasting pastel
  bool ballRoll = true;
  bool showRankings = true;
  bool f1Flags = false;
  bool autoUpdate = true;  // check once a day and install a newer signed image by itself
  bool nightClock = true;  // off: the dim red night clock never replaces the face (not by schedule, not in a dark room); own NVS key
  bool telnet = false;  // LAN console on port 23, unauthenticated: off unless switched on
  String f1Drivers;
  String birthdays;  // "MMDD:NAME:YYYY" entries, comma separated (Birthdays.h); own NVS key, outside the settings record
  String footballLeagues = UM_DEFAULT_FOOTBALL_LEAGUES;
  String footballTeams = UM_DEFAULT_FOOTBALL_TEAMS;
  String timezone = "Europe/Amsterdam";  // the detected zone while manualTimezone is off
  bool manualTimezone = false;  // on: timezone was typed in and the geo-IP lookup leaves it alone
  const char *PREF_SHOW_RANKINGS = "showRankings";

  uint16_t wallColor() const { return pacmancolors::wall(color); }
  uint16_t nightColor() const { return pacmancolors::night(color); }
  uint16_t pacmanColor(uint8_t choice) const { return pacmancolors::sprite(choice); }
  uint32_t pacmanColorKey() const { return pacmancolors::key(color, ghostColors, dotColor); }

  static std::atomic<uint32_t> &counter() { static std::atomic<uint32_t> n{0}; return n; }
  static Settings *getInstance() { static Settings value; return &value; }
  static bool validClockName(const String &name) {
    if (name.isEmpty() || name.length() > 48) return false;
    for (size_t i = 0; i < name.length(); ++i) if ((uint8_t)name[i] < 0x20 || (uint8_t)name[i] == 0x7F) return false;
    return true;
  }
  void load();
  bool save();
  // Counts the settings saved by this firmware (web page, MQTT): lets a reader that polls skip the flash read when nothing was saved
  static uint32_t saveCount() { return counter().load(); }
};
