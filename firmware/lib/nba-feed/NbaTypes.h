// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <stdint.h>

// What the NBA face shows, as plain fixed-size data: the feed fills it, the renderer draws it (also in the native previews).
namespace nba {

constexpr int kMaxGames = 15, kMaxFixtures = 48, kPeriods = 7, kTeams = 15;  // 4 quarters and up to 3 overtimes; a conference has 15 teams

enum State : uint8_t { Pre = 0, Live = 1, Final = 2 };

struct Game {
  char away[5] = "", home[5] = "";  // "LAL", "BOS", "UTAH"
  uint16_t awayColor = 0xFFFF, homeColor = 0xFFFF;  // RGB565, picked to be readable on black
  uint16_t awayScore = 0, homeScore = 0;
  State state = Pre;
  uint8_t period = 0;  // 1-4, 5 and up is overtime
  bool halftime = false, endOfPeriod = false;
  char clock[8] = "";  // "7:42", "45.3"
  char awayRecord[8] = "", homeRecord[8] = "";  // "3-1"
  uint8_t awayQ[kPeriods] = {}, homeQ[kPeriods] = {};  // points per period
  int64_t start = 0;  // UTC
};

struct Fixture {  // a game on a coming day
  char away[5] = "", home[5] = "";
  uint32_t start = 0;  // UTC
};

struct Standing {
  char abbr[5] = "";
  uint8_t wins = 0, losses = 0;
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
  Table east, west;
  uint32_t version = 0;
};

}  // namespace nba
