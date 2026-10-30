// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <stdint.h>

// What the MLB face shows, as plain fixed-size data: the feed fills it, the renderer draws it (also in the native previews).
namespace mlb {

constexpr int kMaxGames = 15, kMaxFixtures = 48, kInnings = 12, kTeams = 15;  // up to 15 games a day; 9 innings and up to 3 extra ones; a league has 15 teams

enum State : uint8_t { Pre = 0, Live = 1, Final = 2 };

struct Game {
  char away[5] = "", home[5] = "";  // "NYY", "LAD"
  uint16_t awayColor = 0xFFFF, homeColor = 0xFFFF;  // RGB565, picked to be readable on black
  uint16_t awayScore = 0, homeScore = 0;
  State state = Pre;
  uint8_t inning = 0;    // 1-9, 10 and up is extra innings
  bool bottom = false;   // the bottom half (the home team bats)
  bool between = false;  // between halves: the half that `bottom` names has just ended
  bool delayed = false;  // rain delay, suspended
  uint8_t outs = 0, balls = 0, strikes = 0;
  uint8_t bases = 0;     // runners: bit 0 first, 1 second, 2 third
  uint8_t awayHits = 0, homeHits = 0, awayErrors = 0, homeErrors = 0;
  char awayRecord[8] = "", homeRecord[8] = "";  // "98-64"
  uint8_t awayInnings = 0, homeInnings = 0;  // how many innings have a score so far (the home team has one fewer while the top half is on)
  uint8_t awayRuns[kInnings] = {}, homeRuns[kInnings] = {};  // runs per inning
  int64_t start = 0;  // UTC
};

struct Fixture {  // a game on a coming day
  char away[5] = "", home[5] = "";
  uint32_t start = 0;  // UTC
};

struct Standing {
  char abbr[5] = "";
  uint8_t wins = 0, losses = 0, seed = 0;  // seed: the place in the league as ESPN ranks it (1-6 are the playoff places)
  char streak[4] = "";  // "W3", "L1"
};
struct Table {
  Standing rows[kTeams];
  uint8_t count = 0;  // best first
};

struct Series {  // a playoff series, the two teams in alphabetical order
  char round[6] = "";  // "WORLD", "ALCS", "NLDS", "WILD"
  char first[5] = "", second[5] = "";
  uint16_t firstColor = 0xFFFF, secondColor = 0xFFFF;
  uint8_t firstWins = 0, secondWins = 0, bestOf = 0, game = 0;  // game: the number of the next (or live) game, 0 when the series is over
};
constexpr int kMaxSeries = 4;

struct Snapshot {
  Game games[kMaxGames];
  uint8_t count = 0;
  uint8_t live = 0;
  Fixture next[kMaxFixtures];
  uint8_t nextCount = 0;
  Table american, national;
  Series series[kMaxSeries];  // the playoff series under way (the card of the next-up page); none in the regular season
  uint8_t seriesCount = 0;
  uint32_t version = 0;
};

}  // namespace mlb
