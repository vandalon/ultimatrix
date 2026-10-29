// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "EspnParse.h"
#include <MemoryLimits.h>

#include <stdlib.h>
#include <string.h>

namespace espn {

namespace {

// The index in path[level] when it is a number
inline int indexAt(const char (*p)[JsonScan::KEY], int level) { return atoi(p[level]); }

struct NextMatchScan {
  NextMatch *out;
  static void leaf(void *sink, const char (*p)[JsonScan::KEY], int n, const char *v, bool) {
    NextMatchScan &s = *(NextMatchScan *)sink;
    if (jsonPathIs(p, n, "team.nextEvent.0.date")) s.out->date = v;
    else if (jsonPathIs(p, n, "team.nextEvent.0.league.slug")) s.out->league = v;
  }
};

// The header lists the team's matches of today, a live one too (teams/<id> has dropped it by then)
struct TodayMatchScan {
  NextMatch *out;
  static void leaf(void *sink, const char (*p)[JsonScan::KEY], int n, const char *v, bool) {
    TodayMatchScan &s = *(TodayMatchScan *)sink;
    if (jsonPathIs(p, n, "sports.0.leagues.0.events.0.date")) s.out->date = v;
    else if (jsonPathIs(p, n, "sports.0.leagues.0.slug")) s.out->league = v;
  }
};

struct StandingsScan {
  Groups *out;
  GroupPoints *points = nullptr;
  int group = -1;
  int entry = -1, stat = -1, value = INT16_MIN;
  bool isPoints = false;
  static void leaf(void *sink, const char (*p)[JsonScan::KEY], int n, const char *v, bool) {
    StandingsScan &s = *(StandingsScan *)sink;
    // Any value of an entry counts the entry, so a team without an abbreviation keeps its place in the table
    if (n < 6 || !jsonPathIs(p, 5, "children.*.standings.entries.*")) return;
    int g = indexAt(p, 1), k = indexAt(p, 5 - 1);
    if (g < 0 || g >= limits::standingsGroups || k < 0 || k >= limits::standingsTeams) return;
    if (g != s.group) {
      s.group = g;
      s.out->emplace_back();
      if (s.points) s.points->emplace_back();
      s.entry = -1;
    }
    std::vector<std::string> &teams = s.out->back();
    if ((int)teams.size() <= k) teams.resize(k + 1);
    if (jsonPathIs(p, n, "children.*.standings.entries.*.team.abbreviation")) teams[k] = v;
    if (!s.points) return;
    auto &points = s.points->back();
    if ((int)points.size() <= k) points.resize(k + 1, INT16_MIN);
    if (k != s.entry) {
      s.entry = k;
      s.stat = -1;
    }
    if (n != 8 || strcmp(p[5], "stats")) return;
    int stat = indexAt(p, 6);
    if (stat != s.stat) {
      s.stat = stat;
      s.isPoints = false;
      s.value = INT16_MIN;
    }
    if (!strcmp(p[7], "name")) s.isPoints = !strcmp(v, "points");
    else if (!strcmp(p[7], "value")) s.value = atoi(v);
    if (s.isPoints && s.value != INT16_MIN) points[k] = s.value;
  }
};

#if !defined(UM_FOOTBALL_ONLY)
struct RaceScan {
  Weekend *out;
  int session = -1, driver = -1;
  static void leaf(void *sink, const char (*p)[JsonScan::KEY], int n, const char *v, bool) {
    RaceScan &s = *(RaceScan *)sink;
    if (n < 3 || strcmp(p[0], "events") || strcmp(p[1], "0")) return;  // the first event only
    if (n == 3) {
      if (!strcmp(p[2], "id")) s.out->eventId = v;
      return;
    }
    if (n < 5 || strcmp(p[2], "competitions")) return;
    int c = indexAt(p, 3);
    if (c != s.session) {
      s.session = c;
      s.driver = -1;
      s.out->sessions.emplace_back();
    }
    RaceSession &session = s.out->sessions.back();
    if (jsonPathIs(p, n, "events.0.competitions.*.id")) session.id = v;
    else if (jsonPathIs(p, n, "events.0.competitions.*.type.abbreviation")) session.type = v;
    else if (jsonPathIs(p, n, "events.0.competitions.*.date")) session.date = v;
    else if (jsonPathIs(p, n, "events.0.competitions.*.status.type.state")) session.state = v;
    else if (n >= 7 && !strcmp(p[4], "competitors")) {
      int d = indexAt(p, 5);
      if ((int)session.drivers.size() <= d) session.drivers.resize(d + 1);
      if (jsonPathIs(p, n, "events.0.competitions.*.competitors.*.order")) session.drivers[d].order = atoi(v);
      else if (jsonPathIs(p, n, "events.0.competitions.*.competitors.*.athlete.shortName")) session.drivers[d].shortName = v;
    }
  }
};

struct FlagScan {
  std::string *flag;
  static void leaf(void *sink, const char (*p)[JsonScan::KEY], int n, const char *v, bool) {
    if (jsonPathIs(p, n, "flag")) *((FlagScan *)sink)->flag = v;
  }
};

#endif
struct ScoreboardScan {
  std::vector<ScoreEvent> *out;
  bool details;
  std::string *nextDate = nullptr;
  const char *afterDate = nullptr;
  int event = -1, detail = -1;  // the index of the event and of the detail being read

  static bool truth(const char *v) { return !strcmp(v, "true"); }

  // The play just read is dropped unless it is a goal or a card
  void endDetail(ScoreEvent *ev) {
    if (!ev || detail < 0 || ev->details.empty()) return;
    const Detail &d = ev->details.back();
    if (!d.scoringPlay && !d.yellowCard && !d.redCard && !d.shootout) ev->details.pop_back();
  }

  static void leaf(void *sink, const char (*p)[JsonScan::KEY], int n, const char *v, bool) {
    ScoreboardScan &s = *(ScoreboardScan *)sink;
    if (s.nextDate && s.afterDate && jsonPathIs(p, n, "leagues.*.calendar.*") &&
        strncmp(v, s.afterDate, 10) > 0 && (s.nextDate->empty() || strcmp(v, s.nextDate->c_str()) < 0))
      *s.nextDate = v;
    if (n < 3 || strcmp(p[0], "events")) return;
    int e = indexAt(p, 1);
    if (e < 0 || e >= limits::scoreboardMatches) return;
    if (e != s.event) {
      s.endDetail(s.out->empty() ? nullptr : &s.out->back());
      s.event = e;
      s.detail = -1;
      s.out->emplace_back();
    }
    ScoreEvent &ev = s.out->back();
    if (n == 3) {
      if (!strcmp(p[2], "id")) ev.id = v;
      else if (!strcmp(p[2], "date")) ev.date = v;
      return;
    }
    if (jsonPathIs(p, n, "events.*.status.type.state")) ev.state = v;
    else if (jsonPathIs(p, n, "events.*.status.type.name")) ev.name = v;
    else if (jsonPathIs(p, n, "events.*.status.type.shortDetail")) ev.shortDetail = v;
    else if (n >= 7 && !strcmp(p[2], "competitions") && !strcmp(p[3], "0")) {
      int k = indexAt(p, 5);
      if (!strcmp(p[4], "competitors")) {
        if (k < 0 || k >= limits::competitors) return;
        if ((int)ev.competitors.size() <= k) ev.competitors.resize(k + 1);
        Competitor &c = ev.competitors[k];
        if (jsonPathIs(p, n, "events.*.competitions.0.competitors.*.homeAway")) c.homeAway = v;
        else if (jsonPathIs(p, n, "events.*.competitions.0.competitors.*.score")) c.score = v;
        else if (jsonPathIs(p, n, "events.*.competitions.0.competitors.*.shootoutScore")) c.shootoutScore = atoi(v);
        else if (jsonPathIs(p, n, "events.*.competitions.0.competitors.*.team.abbreviation")) c.abbreviation = v, c.hasAbbreviation = true;
        else if (jsonPathIs(p, n, "events.*.competitions.0.competitors.*.team.color")) c.color = v, c.hasColor = true;
        else if (jsonPathIs(p, n, "events.*.competitions.0.competitors.*.team.alternateColor")) c.alternateColor = v, c.hasAlternateColor = true;
        else if (s.details && jsonPathIs(p, n, "events.*.competitions.0.competitors.*.id")) c.id = v;
      } else if (s.details && !strcmp(p[4], "details")) {
        if (k < 0 || k >= limits::matchDetails) return;
        if (k != s.detail) {  // the next play: only goals and cards are kept, a busy evening has dozens of plays
          s.endDetail(&ev);
          s.detail = k;
          ev.details.emplace_back();
        }
        Detail &d = ev.details.back();
        if (jsonPathIs(p, n, "events.*.competitions.0.details.*.clock.value")) d.clock = atof(v);
        else if (jsonPathIs(p, n, "events.*.competitions.0.details.*.team.id")) d.teamId = v;
        else if (jsonPathIs(p, n, "events.*.competitions.0.details.*.scoringPlay")) d.scoringPlay = truth(v);
        else if (jsonPathIs(p, n, "events.*.competitions.0.details.*.yellowCard")) d.yellowCard = truth(v);
        else if (jsonPathIs(p, n, "events.*.competitions.0.details.*.redCard")) d.redCard = truth(v);
        else if (jsonPathIs(p, n, "events.*.competitions.0.details.*.shootout")) d.shootout = truth(v);
        else if (jsonPathIs(p, n, "events.*.competitions.0.details.*.athletesInvolved.0.jersey")) d.jersey = v, d.hasJersey = true;
        else if (jsonPathIs(p, n, "events.*.competitions.0.details.*.athletesInvolved.0.displayName")) d.displayName = v, d.hasDisplayName = true;
      }
    }
  }
};

struct SummaryScan {
  Summary *out;
  int squad = -1, player = -1, key = -1;
  bool keyIsSubstitution = false;
  Substitution current;

  void endKey() {
    if (key >= 0 && keyIsSubstitution) out->substitutions.push_back(current);
    current = Substitution();
    keyIsSubstitution = false;
  }

  static void leaf(void *sink, const char (*p)[JsonScan::KEY], int n, const char *v, bool) {
    SummaryScan &s = *(SummaryScan *)sink;
    if (n < 3) return;
    if (!strcmp(p[0], "rosters")) {
      int q = indexAt(p, 1);
      if (q != s.squad) {
        s.squad = q;
        s.player = -1;
        s.out->squads.emplace_back();
      }
      Squad &squad = s.out->squads.back();
      if (jsonPathIs(p, n, "rosters.*.homeAway")) squad.homeAway = v;
      else if (jsonPathIs(p, n, "rosters.*.team.id")) squad.teamId = v;
      else if (!strcmp(p[2], "roster") && n >= 5) {
        int k = indexAt(p, 3);
        if (k != s.player) {
          s.player = k;
          squad.players.emplace_back();
        }
        if (jsonPathIs(p, n, "rosters.*.roster.*.athlete.id")) squad.players.back().athleteId = v;
        else if (jsonPathIs(p, n, "rosters.*.roster.*.jersey")) squad.players.back().jersey = v;
      }
    } else if (!strcmp(p[0], "keyEvents")) {
      int k = indexAt(p, 1);
      if (k != s.key) {
        s.endKey();
        s.key = k;
      }
      if (jsonPathIs(p, n, "keyEvents.*.type.type")) s.keyIsSubstitution = !strcmp(v, "substitution");
      else if (jsonPathIs(p, n, "keyEvents.*.clock.value")) s.current.clock = atof(v);
      else if (jsonPathIs(p, n, "keyEvents.*.team.id")) s.current.teamId = v;
      else if (jsonPathIs(p, n, "keyEvents.*.participants.0.athlete.id")) s.current.on = v;
      else if (jsonPathIs(p, n, "keyEvents.*.participants.1.athlete.id")) s.current.off = v;
    }
  }
};

}  // namespace

bool parseNextMatch(JsonScan::Next next, void *source, NextMatch &out) {
  NextMatchScan scan{&out};
  return JsonScan::read(next, source, NextMatchScan::leaf, &scan);
}

bool parseTodayMatch(JsonScan::Next next, void *source, NextMatch &out) {
  TodayMatchScan scan{&out};
  return JsonScan::read(next, source, TodayMatchScan::leaf, &scan);
}

bool parseStandings(JsonScan::Next next, void *source, Groups &out, GroupPoints *points) {
  StandingsScan scan;
  scan.out = &out;
  scan.points = points;
  return JsonScan::read(next, source, StandingsScan::leaf, &scan);
}

#if !defined(UM_FOOTBALL_ONLY)
bool parseRace(JsonScan::Next next, void *source, Weekend &out) {
  RaceScan scan;
  scan.out = &out;
  return JsonScan::read(next, source, RaceScan::leaf, &scan);
}

bool parseFlag(JsonScan::Next next, void *source, std::string &flag) {
  FlagScan scan{&flag};
  return JsonScan::read(next, source, FlagScan::leaf, &scan);
}

#endif
bool parseScoreboard(JsonScan::Next next, void *source, std::vector<ScoreEvent> &out, bool withDetails,
                     std::string *nextDate, const char *afterDate) {
  ScoreboardScan scan;
  scan.out = &out;
  scan.details = withDetails;
  scan.nextDate = nextDate;
  scan.afterDate = afterDate;
  bool ok = JsonScan::read(next, source, ScoreboardScan::leaf, &scan);
  scan.endDetail(out.empty() ? nullptr : &out.back());
  return ok;
}

bool parseSummary(JsonScan::Next next, void *source, Summary &out) {
  SummaryScan scan;
  scan.out = &out;
  bool ok = JsonScan::read(next, source, SummaryScan::leaf, &scan);
  scan.endKey();
  return ok;
}

}  // namespace espn
