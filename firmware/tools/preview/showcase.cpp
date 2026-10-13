// Replays recorded feed data through the real faces for the GitHub GIFs.
#include <Arduino.h>
#include <Settings.h>
#include <Console.h>
#include <fstream>
#include <string>
#include <vector>
#include "Clockface.h"
#if defined(SHOWCASE_F1)
#include "F1Ticker.h"
#include <F1Drivers.h>
#ifdef UM_MOTOGP
#include <MotoGpRiders.h>
#endif
#elif defined(SHOWCASE_TETRIS)
#else
#include "FootballTicker.h"
#include "TeamColors.h"
#endif

static unsigned long virtualMs = 0;
unsigned long millis() { return virtualMs; }
void delay(unsigned long ms) { virtualMs += ms; }
namespace ezt { time_t fakeNow = 0; }
ConsoleStub Console;
void (*onFlip)(MatrixPanel_I2S_DMA *) = nullptr;
static MatrixPanel_I2S_DMA display;
static ClockTime clockTime;
static uint32_t revision = 1;
static std::string outDir;
static std::ofstream timing;
static unsigned int frameNumber = 0;

template <size_t N> static void copyText(char (&out)[N], const char *text) { strlcpy(out, text, N); }
static uint16_t color(const char *hex) {
  unsigned long c = strtoul(hex, nullptr, 16);
  return (((c >> 16) & 255) >> 3) << 11 | (((c >> 8) & 255) >> 2) << 5 | (c & 255) >> 3;
}

#ifdef SHOWCASE_F1
static F1Ticker::Snapshot source, feed;
static F1Ticker::Session practice, qualifying, race;
static time_t practiceStart, qualifyingStart, raceStart, raceEnd;
void F1Ticker::begin(ClockTime *) {}
void F1Ticker::snapshot(Snapshot &out, bool) { out = feed; }
uint32_t F1Ticker::version() { return revision; }
#ifdef UM_MOTOGP
uint16_t F1Ticker::driverColor(const char *code) { const MotoGpRider *r = motoGpRiderByCode(code); return r ? motoGpColor(r->maker) : 0xFFFF; }
#else
uint16_t F1Ticker::driverColor(const char *code) { return f1DriverColor(code); }
#endif
#elif !defined(SHOWCASE_TETRIS)
struct Table { String name; std::vector<String> teams; std::vector<int16_t> points; };
static std::vector<Table> tables;
// The recorded matches as the data task parsed them: a live row, result or fixture is made from each
struct Game {
  String home, away, score;
  TeamKit homeKit, awayKit;
  uint16_t scoreColor, barColor;
  float progress;
  bool live;
  String detail, competition;
  std::vector<FootballTicker::Event> events;
  uint8_t homePos, awayPos;
  bool favourite;
};
struct Coming {
  String home, away;
  TeamKit homeKit, awayKit;
  time_t kickoff;
  uint16_t color;
  uint8_t homePos, awayPos;
};
static std::vector<Game> games;
static std::vector<Coming> coming;
static std::vector<time_t> kickoffs;
static football::Snapshot feed;
static FootballTicker::Goal goal;
static FootballTicker::Incident incident, substitution;
static bool goalDue = false, incidentDue = false;
void FootballTicker::begin(ClockTime *, bool, bool, uint32_t) {}
void FootballTicker::setResultWindow(uint32_t) {}
void FootballTicker::snapshot(football::Snapshot &out) { out = feed; out.version = revision; }
uint32_t FootballTicker::version() { return revision; }
FootballTicker::Status FootballTicker::status() { return FootballTicker::OK; }
bool FootballTicker::nextGoal(Goal &out) { if (!goalDue) return false; out = goal; goalDue = false; return true; }
bool FootballTicker::nextIncident(Incident &out) { if (!incidentDue) return false; out = incident; incidentDue = false; return true; }

// The recorded data as the rows the data task would publish
static football::Side sideOf(const String &code, const TeamKit &kit, uint8_t position) {
  football::Side side = {};
  copyText(side.code, code.c_str());
  side.kit = kit;
  side.position = position;
  return side;
}
template <typename Row> static void marksOf(Row &row, const std::vector<FootballTicker::Event> &events) {
  for (const auto &e : events) row.marks[row.markCount++] = {e.minute, e.kind, e.home};
}
static football::LiveRow liveRow(const Game &e) {
  football::LiveRow row = {};
  row.home = sideOf(e.home, e.homeKit, e.homePos);
  row.away = sideOf(e.away, e.awayKit, e.awayPos);
  row.homeScore = e.score.toInt();
  row.awayScore = e.score.substring(e.score.indexOf('-') + 1).toInt();
  row.progress = e.progress;
  copyText(row.clock, e.detail.c_str());
  copyText(row.competition, e.competition.c_str());
  row.color = e.scoreColor;
  row.favourite = e.favourite;
  marksOf(row, e.events);
  return row;
}
static football::ResultRow resultRow(const Game &e) {
  football::ResultRow row = {};
  row.home = sideOf(e.home, e.homeKit, e.homePos);
  row.away = sideOf(e.away, e.awayKit, e.awayPos);
  row.homeScore = e.score.toInt();
  row.awayScore = e.score.substring(e.score.indexOf('-') + 1).toInt();
  copyText(row.competition, e.competition.c_str());
  row.color = e.scoreColor;
  row.favourite = e.favourite;
  marksOf(row, e.events);
  return row;
}
static football::FixtureRow fixtureRow(const Coming &u) {
  football::FixtureRow row = {};
  row.home = sideOf(u.home, u.homeKit, u.homePos);
  row.away = sideOf(u.away, u.awayKit, u.awayPos);
  row.kickoff = u.kickoff;
  row.color = u.color;
  return row;
}
// A snapshot with nothing but the league tables
static football::Snapshot blank() {
  football::Snapshot out = {};
  for (const Table &t : tables) {
    football::Group &group = out.groups[out.groupCount++];
    copyText(group.name, t.name.c_str());
    group.first = out.teamCount;
    group.rows = t.teams.size();
    for (size_t i = 0; i < t.teams.size(); i++) {
      copyText(out.teams[out.teamCount].code, t.teams[i].c_str());
      out.teams[out.teamCount++].points = t.points[i];
    }
  }
  return out;
}
static TeamKit kit(const char *code, const char *league, const char *shirt, const char *second) {
  TeamKit result = {color(shirt), 0xFFFF, color(second)};
  lookupTeamKit(code, league, result);
  return result;
}
#endif

#include "showcase-data.h"

static void setTime(time_t utc) {
  ezt::fakeNow = utc;
  clockTime.utcOffset = 7200;
  time_t local = utc + clockTime.utcOffset;
  struct tm date;
  gmtime_r(&local, &date);
  clockTime.year = date.tm_year + 1900; clockTime.month = date.tm_mon + 1; clockTime.day = date.tm_mday;
  clockTime.hour = date.tm_hour; clockTime.minute = date.tm_min; clockTime.second = date.tm_sec; clockTime.weekday = date.tm_wday;
}

static void capture() {
  char name[32];
  snprintf(name, sizeof(name), "frame-%05u.ppm", frameNumber++);
  FILE *f = fopen((outDir + "/" + name).c_str(), "wb");
  if (!f) { perror(name); exit(1); }
  fprintf(f, "P6\n64 64\n255\n");
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++) {
      uint16_t c = display.getPixel(x, y);
      unsigned char rgb[] = {(unsigned char)((c >> 11) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63),
                             (unsigned char)((c & 31) * 255 / 31)};
      fwrite(rgb, 1, sizeof(rgb), f);
    }
  fclose(f);
  timing << name << '\t' << virtualMs << '\n';
}

#ifdef SHOWCASE_TETRIS
static const unsigned long kFrameMs = 50;  // the face is called at layout::frameMs
#else
static const unsigned long kFrameMs = 40;
#endif

// restart = false carries on with the running face: a clock that keeps going, not one that has just booted
static void show(Clockface &face, const char *name, unsigned long duration, time_t utc, bool restart = true) {
  fprintf(stdout, "%5lu ms: %s\n", virtualMs, name);
  revision++;
  setTime(utc);
  if (restart) face.setup(&clockTime);
  unsigned long start = virtualMs;
  while (virtualMs - start < duration) {
    setTime(utc + (virtualMs - start) / 1000);
    face.update();
    capture();
    delay(kFrameMs);
  }
}

#if !defined(SHOWCASE_F1) && !defined(SHOWCASE_TETRIS)
static void liveMatch(int index, int minute, bool otherGames) {
  feed = blank();
  Game e = games[index];
  e.live = true; e.detail = String(minute > 90 ? 90 : minute) + "'";
  if (minute > 90) e.detail = e.detail + "+" + String(minute - 90);
  e.progress = std::min(minute / 90.0f, 1.0f);
  e.events.clear();
  int home = 0, away = 0;
  for (const auto &event : games[index].events) {
    if (event.minute > minute) break;
    e.events.push_back(event);
    if (event.kind == 'g') { if (event.home) home++; else away++; }
  }
  e.score = String(home) + "-" + String(away);
  e.favourite = true;
  feed.live[feed.liveCount++] = liveRow(e);
  if (otherGames)
    for (size_t i = 0; i < games.size(); i++) {
      if ((int)i == index || kickoffs[i] < kickoffs[index]) continue;
      Coming u = {};
      u.home = games[i].home; u.away = games[i].away; u.homeKit = games[i].homeKit; u.awayKit = games[i].awayKit;
      u.kickoff = kickoffs[i]; u.color = games[i].scoreColor;
      feed.fixtures[feed.fixtureCount++] = fixtureRow(u);
    }
}
static void animate(Clockface &face, const char *name) {
  fprintf(stdout, "%5lu ms: %s\n", virtualMs, name);
  revision++;
  onFlip = [](MatrixPanel_I2S_DMA *) { capture(); };
  unsigned before = frameNumber;
  face.update();
  // The animation plays one frame per update, 40 ms apart
  for (bool played = frameNumber != before; played;) {
    virtualMs += 40;
    if (!face.animating()) break;
    face.update();
  }
  onFlip = nullptr;
}
#endif

int main(int argc, char **argv) {
  if (argc != 2) return 1;
  outDir = argv[1];
  timing.open(outDir + "/frames.tsv");
  initData();
  randomSeed(7);
  Settings *params = Settings::getInstance();
  params->matchSecs = 4;
  params->timeStyle = 2;
  Clockface face(&display);
#if defined(SHOWCASE_TETRIS)
  // Local times (the preview clock is UTC+2); the face boots, rolls over a minute, then into the new year
  auto at = [](int year, int month, int day, int hour, int minute, int second) {
    struct tm date = {};
    date.tm_year = year - 1900; date.tm_mon = month - 1; date.tm_mday = day;
    date.tm_hour = hour; date.tm_min = minute; date.tm_sec = second;
    return timegm(&date) - 7200;
  };
  show(face, "start: the date falls, then the time", 12000, at(2026, 12, 31, 23, 58, 30));
  show(face, "a new minute: only the last digit falls", 8000, at(2026, 12, 31, 23, 58, 57), false);
  show(face, "midnight into the new year", 15000, at(2026, 12, 31, 23, 59, 57), false);
  show(face, "a one-digit hour and a two-digit month", 10000, at(2026, 10, 4, 9, 5, 20));
#elif defined(SHOWCASE_F1)
  params->f1Flags = true;
  feed = source;
  show(face, "championship and next race", 4000, 1791808498);
  feed = source; feed.weekend = true; feed.last = practice;
  params->resultMins = 1;
  show(face, "between practice and sprint qualifying", 6400, practiceStart + 5400);
  params->resultMins = 120;
  feed = source; feed.weekend = true; feed.live = practice; feed.live.live = true;
  copyText(feed.live.clock, "0:00");
  show(face, "practice closing order", 5200, practiceStart + 3600);
  feed = source; feed.weekend = true; feed.live = qualifying; feed.live.live = true;
  copyText(feed.live.clock, "0:00");
  show(face, "qualifying closing order", 6400, qualifyingStart + 3600);
  feed = source; feed.weekend = true; feed.live = race; feed.live.live = true; feed.live.finished = true;
  feed.live.finishedAt = raceEnd;
  show(face, "race finish", 7200, raceEnd);
  feed = source; feed.weekend = true; feed.last = qualifying;
  copyText(feed.last.name, "QUALI");
  show(face, "full qualifying results", 17200, qualifyingStart + 3605);
  feed = source; feed.weekend = true; feed.last = race;
  show(face, "race results", 13200, raceEnd + 10);
  feed = source;
  show(face, "championship and next race", 3200, 1791808560);
#else
  params->numberFont = 1;
  show(face, "stadium clock", 4000, 1791808498);
  feed = blank();
  for (const auto &u : coming) feed.fixtures[feed.fixtureCount++] = fixtureRow(u);
  show(face, "upcoming fixtures and both full league tables", 28000, 1791808502);
  params->showRankings = false;
  show(face, "upcoming fixtures without standings", 8000, 1791808530);
  liveMatch(0, 44, false);
  show(face, "single live match", 4000, kickoffs[0] + 44 * 60);
  const auto &yellow = *std::find_if(games[0].events.begin(), games[0].events.end(), [](const FootballTicker::Event &e) { return e.kind == 'y'; });
  incident = {}; incident.kind = 'y'; incident.home = games[0].home; incident.away = games[0].away;
  incident.homeKit = games[0].homeKit; incident.awayKit = games[0].awayKit; incident.homeTeam = yellow.home;
  incident.minute = yellow.minute; incident.number = yellow.number; copyText(incident.name, yellow.name);
  incidentDue = true;
  animate(face, "recorded yellow card");
  liveMatch(0, 74, true);
  show(face, "live match and today's fixtures", 8000, kickoffs[0] + 89 * 60);
  liveMatch(0, 75, true);
  goal = {games[0].home, games[0].away, 2, 1, true, games[0].homeKit, games[0].awayKit, millis()};
  goalDue = true;
  animate(face, "recorded Utrecht goal: 2-1");
  incident = substitution; incidentDue = true;
  animate(face, "recorded substitution");
  liveMatch(0, 92, true);
  show(face, "stoppage time", 4000, kickoffs[0] + 107 * 60);
  liveMatch(2, 13, false);
  const auto &red = *std::find_if(games[2].events.begin(), games[2].events.end(), [](const FootballTicker::Event &e) { return e.kind == 'r'; });
  incident = {}; incident.kind = 'r'; incident.home = games[2].home; incident.away = games[2].away;
  incident.homeKit = games[2].homeKit; incident.awayKit = games[2].awayKit; incident.homeTeam = red.home;
  incident.minute = red.minute; incident.number = red.number; copyText(incident.name, red.name);
  incidentDue = true;
  animate(face, "recorded Telstar / ADO red card");
  feed = blank();
  for (auto game : games) { game.favourite = false; feed.results[feed.resultCount++] = resultRow(game); }
  show(face, "full-time results rotating through the main scoreboard", 12000, kickoffs.back() + 7200);
#endif
  timing << "end\t" << virtualMs << '\n';
  return 0;
}
