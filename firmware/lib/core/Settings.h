#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <algorithm>
#include <atomic>
#include "PacmanColors.h"

#define UM_DEFAULT_FOOTBALL_LEAGUES "ned.1,ned.cup"
#define UM_DEFAULT_FOOTBALL_TEAMS "449:NED:n"

struct Settings {
  uint8_t accessScope = 1;  // who may reach the web interface: 0 everything, 1 private (RFC 1918) networks (default), 2 the clock's own subnet
  bool anyHost = false;  // allow requests under public host names (off: refused, see main.cpp HostGuard)
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
  uint8_t pacmanClockColor = 0;  // Pac-Man hours colour; 0: the face's own. With clockMinutesColor and clockShade it is packed into one byte of the record
  uint8_t clockMinutesColor = 0;  // Pac-Man minutes colour; 0: the face's own
  uint8_t clockShade = 0;  // Pac-Man background clock: 0 subtle, 1 strong
  uint8_t arcHours = 0, arcMinutes = 0, arcScore = 0;  // Invaders and Pong: clock hours, clock minutes and score colours (pacmancolors list, 0 = the face's own); own NVS keys
  uint8_t pongDate = 1, pongDay = 1, pongLevel = 0;     // Pong: how well the computer plays against you, 0 easiest to 8 hardest; own NVS key
  uint8_t pongSpeed = 2;     // Pong: game speed while the computer plays, 0 slowest to 4 fastest (50 75 100 125 150 %); own NVS key
  uint8_t pongBackdrop = 3;  // Pong: brightness of the clock behind the court, 0 off to 5 brightest; own NVS key, outside the record
  bool nbaExtra = false;  // NBA: the extra pages in the round (today's games, quarter scores, favourites, tables); off: only the next-up page / big clock, and the live games; own NVS key "nbaExtra"
  bool nbaLiveOnly = true;  // NBA: while a game is live only the live games (and their quarters) are shown; own NVS key "nbaLive"
  uint8_t nbaIdle = 0;  // NBA: what to show while no game is live: 0 the next-up page (the big clock when nothing is coming), 1 the big clock, 2 both in turn; own NVS key "nbaIdle"
  uint8_t nbaTables = 0;  // NBA: the East and West tables in the round: 0 off, 1 when no game is live, 2 always; own NVS key "nbaTab"
  bool mlbExtra = false;  // MLB: the extra pages in the round (today's games, line scores, favourites, tables); off: only the next-up page / big clock, and the live games; own NVS key "mlbExtra"
  bool mlbLiveOnly = true;  // MLB: while a game is live only the live games (and their line scores) are shown; own NVS key "mlbLive"
  uint8_t mlbIdle = 0;  // MLB: what to show while no game is live: 0 the next-up page (the big clock when nothing is coming), 1 the big clock, 2 both in turn; own NVS key "mlbIdle"
  uint8_t mlbTables = 0;  // MLB: the AL and NL tables in the round: 0 off, 1 when no game is live, 2 always; own NVS key "mlbTab"
  uint8_t mlbHours = 10, mlbMinutes = 5;  // MLB: colours of the big clock hours and minutes (the pacmancolors list: 10 white, 5 peach); own NVS keys "mlbHrs", "mlbMin"
  String mlbTeams;  // MLB: favourite teams as abbreviations, comma separated ("NYY,LAD"); own NVS key "mlbTeams"
  bool nflExtra = false;  // NFL: the extra pages in the round (today's games, line scores, favourites, tables); off: only the next-up page / big clock, and the live games; own NVS key "nflExtra"
  bool nflLiveOnly = true;  // NFL: while a game is live only the live games (and their line scores) are shown; own NVS key "nflLive"
  uint8_t nflIdle = 0;  // NFL: what to show while no game is live: 0 the next-up page (the big clock when nothing is coming), 1 the big clock, 2 both in turn; own NVS key "nflIdle"
  uint8_t nflTables = 0;  // NFL: the AFC and NFC tables in the round: 0 off, 1 when no game is live, 2 always; own NVS key "nflTab"
  uint8_t nflHours = 10, nflMinutes = 3;  // NFL: colours of the big clock hours and minutes (the pacmancolors list: 10 white, 3 pastel blue); own NVS keys "nflHrs", "nflMin"
  String nflTeams;  // NFL: favourite teams as abbreviations, comma separated ("NYY,LAD"); own NVS key "nflTeams"
  String nbaTeams;  // NBA: favourite teams as abbreviations, comma separated ("LAL,BOS"); own NVS key "nbaTeams"
  uint8_t ghostColors[4] = {};  // Pac-Man: 0 lets the face pick a contrasting pastel
  bool ballRoll = true;
  bool showRankings = true;
  bool f1Flags = false;
  bool autoUpdate = true;  // check once a day and install a newer signed image by itself
  bool nightClock = true;  // off: the dim red night clock never replaces the face (not by schedule, not in a dark room); own NVS key
  bool dither = false;  // smoother dark colours at low brightness (HUB75 ordered dithering); own NVS key
  bool telnet = false;  // LAN console on port 23, unauthenticated: off unless switched on
  String f1Drivers;
  String sportsOrder;  // sports that may take over the panel when they are on, best first: "f1,motogp,football" (SportsPlan.h); empty: off. One own NVS key "sports" with the fields below
  String sportsHome;   // mode to go back to when nothing is on (empty: stay on the sports face)
  uint8_t sportsLead = 10;  // minutes before the start the switch happens
  uint8_t sportsRacing = 0;  // 0 qualifying and races, 1 races only, 2 every session
  uint8_t sportsMax = 4;  // automatic switches into a sport per day
  uint8_t sportsBridge = 60;  // minutes: between two sessions of the same sport this close together the clock stays (0 = off)
  uint8_t sportsHold = 15;  // minutes the clock stays on a sport after it finished (end of the session, full time) before it goes home
  bool sportsFav = true;  // Football: only matches of the favourite teams
  bool sportsOthers = true;  // switching: also the favourites' matches in competitions that are not followed (looked up per team)
  bool footballOthers = true;  // Football face: also show the favourites' matches in competitions that are not followed
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
