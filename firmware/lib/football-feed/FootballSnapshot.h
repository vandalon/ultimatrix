// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once
#pragma once

#include <stdint.h>
#include <time.h>

#include "TeamKit.h"

// What the Football display draws: fixed-size plain data with no Strings or vectors, filled by
// FootballTicker's data task and copied out whole (FootballTicker::snapshot). The display never
// reads a file or waits for a download.
namespace football {

constexpr uint8_t kLive = 8;        // live matches kept, favourites first; the rest are left out
constexpr uint8_t kResults = 12;    // finished matches, most recent first
constexpr uint8_t kFixtures = 16;   // coming matches, soonest first
constexpr uint8_t kMarks = 24;      // goals and cards on the timeline of one live match (the latest)
constexpr uint8_t kGroups = 12;     // league tables (a league with groups has one per group)
constexpr uint16_t kTeams = 160;    // table rows over all groups

// A goal or card, for the timeline
struct Mark {
  uint8_t minute;  // 1-based playing minute, stoppage time counts on
  char kind;       // 'g' goal, 'y' yellow card, 'r' red card
  bool home;       // the home team's
};

struct Side {
  char code[4];     // three letters: all the display uses
  TeamKit kit;
  uint8_t position;  // place in the table or group, 0 = none
};

struct LiveRow {
  Side home, away;
  int8_t homeScore, awayScore;
  float progress;        // playing time 0..1
  char clock[8];         // "67'", "45'+2'", "HT"
  char competition[6];   // "ERE", "KNVB", ...
  uint16_t color;        // the competition's colour
  bool favourite;
  uint8_t markCount;
  Mark marks[kMarks];
};

struct ResultRow {
  Side home, away;
  int8_t homeScore, awayScore;
  char competition[6];
  uint16_t color;
  bool favourite;
  uint8_t markCount;     // a result takes turns on top like a live match: with its timeline
  Mark marks[kMarks];
};

struct FixtureRow {
  Side home, away;
  time_t kickoff;  // UTC
  uint16_t color;
};

// One table: its rows are teams[first .. first + rows)
struct Group {
  char name[32];  // "Eredivisie", "Champions League A"
  uint8_t rows;
  uint16_t first;
};

struct TableRow {
  char code[5];
  int16_t points;  // INT16_MIN = unknown
};

struct Snapshot {
  uint32_t version;
  uint8_t liveCount, resultCount, fixtureCount;
  LiveRow live[kLive];            // favourites first
  ResultRow results[kResults];
  FixtureRow fixtures[kFixtures];
  uint8_t groupCount;
  uint16_t teamCount;
  Group groups[kGroups];          // in the order of the followed competitions
  TableRow teams[kTeams];
};

static_assert(sizeof(Snapshot) <= 6144, "football::Snapshot grew: it exists three times in RAM");

}  // namespace football
