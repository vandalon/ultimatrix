// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <stdint.h>

// What the NFL face shows, as plain fixed-size data: the feed fills it, the renderer draws it (also in the native previews).
namespace nfl {

constexpr int kMaxGames = 16, kMaxFixtures = 48, kPeriods = 5, kTeams = 16;  // a week has up to 16 games; four quarters and one overtime; a conference has 16 teams

enum State : uint8_t { Pre = 0, Live = 1, Final = 2 };

struct Game {
  char away[5] = "", home[5] = "";  // "KC", "GB"
  uint16_t awayColor = 0xFFFF, homeColor = 0xFFFF;  // RGB565, picked to be readable on black
  uint16_t awayScore = 0, homeScore = 0;
  State state = Pre;
  uint8_t quarter = 0;   // 1-4, 5 is overtime
  char clock[8] = "";    // "4:12"
  bool halftime = false, delayed = false;
  uint8_t possession = 0;  // 0 unknown, 1 the away team has the ball, 2 the home team
  uint8_t down = 0, distance = 0;  // 1-4 and the yards to go
  uint8_t fieldPos = 50;   // where the ball is, 0 (the away end zone, on the left) to 100 (the home end zone)
  bool redZone = false;
  uint8_t awayTimeouts = 3, homeTimeouts = 3;
  char spot[8] = "";       // "GB 25": where the ball lies, as ESPN says it
  char awayRecord[8] = "", homeRecord[8] = "";  // "5-2"
  uint8_t periods = 0;     // how many periods have a score
  uint8_t awayPoints[kPeriods] = {}, homePoints[kPeriods] = {};  // points per period
  int64_t start = 0;  // UTC
};

struct Fixture {  // a game on a coming day
  char away[5] = "", home[5] = "";
  uint32_t start = 0;  // UTC
};

struct Standing {
  char abbr[5] = "";
  uint8_t wins = 0, losses = 0, ties = 0, seed = 0;  // seed: the place in the conference as ESPN ranks it (1-7 are the playoff places)
  char streak[4] = "";  // "W3", "L1"
};
struct Table {
  Standing rows[kTeams];
  uint8_t count = 0;  // best first
};

struct Snapshot {
  Game games[kMaxGames];
  uint8_t count = 0;
  uint8_t live = 0;
  Fixture next[kMaxFixtures];
  uint8_t nextCount = 0;
  Table afc, nfc;
  uint32_t version = 0;
};

}  // namespace nfl
