// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <string>
#include <vector>
#include <stdint.h>

#include <JsonScan.h>

// What the football ticker reads from ESPN's feeds, one function per feed. Each reads the text through
// JsonScan (a few hundred bytes of memory whatever the size of the feed) and returns just the fields the ticker
// uses. No Arduino types in here, so the host tests can run it on saved feeds.
//
// Every function takes the reader JsonScan wants: next(source) gives the next character, -1 at the end.
// They return false when the text ended early or isn't JSON; what was read before that is in the result.
namespace espn {

// teams/<id>: the team's next match
struct NextMatch {
  std::string league;  // the league's slug, "" when not there
  std::string date;    // "2026-10-09T08:30Z", "" when not there
};
bool parseNextMatch(JsonScan::Next next, void *source, NextMatch &out);
// scoreboard/header?team=<id>: the team's match of today, "" when there is none
bool parseTodayMatch(JsonScan::Next next, void *source, NextMatch &out);

// <league>/standings: the teams' abbreviations, group by group in table order
typedef std::vector<std::vector<std::string>> Groups;
typedef std::vector<std::vector<int16_t>> GroupPoints;
bool parseStandings(JsonScan::Next next, void *source, Groups &out, GroupPoints *points = nullptr);

#if !defined(UM_FOOTBALL_ONLY)
// racing/f1/scoreboard: the Grand Prix weekend, its sessions in the order ESPN lists them
struct RaceDriver {
  int order = 0;          // the place in the session, 0 when not there
  std::string shortName;  // "M. Verstappen"
};
struct RaceSession {
  std::string id, type, date, state;  // type: "FP1", "Qual", "SR"...; state: "pre", "in" or "post"
  std::vector<RaceDriver> drivers;
};
struct Weekend {
  std::string eventId;
  std::vector<RaceSession> sessions;
};
bool parseRace(JsonScan::Next next, void *source, Weekend &out);

// racing/f1/events/<event>/competitions/<session>/status: the flag being shown ("Yellow"), "" when there is none
bool parseFlag(JsonScan::Next next, void *source, std::string &flag);

#endif
// <league>/scoreboard: the matches of a day. Only the first competition of an event is read, like the ticker always did.
struct Competitor {
  std::string homeAway, score, id;  // "home" or "away"; the score is text ("2")
  std::string abbreviation, color, alternateColor;
  bool hasAbbreviation = false, hasColor = false, hasAlternateColor = false;
  int shootoutScore = -1;  // the penalty shootout, -1 when there was none
};
struct Detail {  // a goal, a card or another play of the match
  double clock = 0;  // seconds
  std::string teamId, jersey, displayName;  // of the first player involved
  bool scoringPlay = false, yellowCard = false, redCard = false;
  bool shootout = false;  // a kick of the penalty shootout (scoringPlay: it went in)
  bool hasJersey = false, hasDisplayName = false;
};
struct ScoreEvent {
  std::string id, date, state, name, shortDetail;  // state: "pre", "in" or "post"; name: "STATUS_FULL_TIME"...
  std::vector<Competitor> competitors;
  std::vector<Detail> details;
};
// withDetails: also the team ids and the goals and cards (a busy evening has many)
bool parseScoreboard(JsonScan::Next next, void *source, std::vector<ScoreEvent> &out, bool withDetails,
                     std::string *nextDate = nullptr, const char *afterDate = nullptr);

// <league>/summary?event=<id>: the squads' shirt numbers and the substitutions of a match
struct Player {
  std::string athleteId;
  std::string jersey;  // "" when not there
};
struct Squad {
  std::string homeAway, teamId;
  std::vector<Player> players;
};
struct Substitution {
  double clock = 0;  // seconds
  std::string teamId;
  std::string on, off;  // the athletes' ids: ESPN lists the one coming on first, then the one going off
};
struct Summary {
  std::vector<Squad> squads;
  std::vector<Substitution> substitutions;  // the other key events (goals, cards, kick-off...) are left out
};
bool parseSummary(JsonScan::Next next, void *source, Summary &out);

}  // namespace espn
