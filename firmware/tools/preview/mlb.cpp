// Renders the MLB face's pages with the real drawing code and made-up games.
// Usage: mlb <output folder>; writes one PPM per scene and scenes.tsv (file, caption).
//        mlb <output folder> ball <ms...>: only the clock page at those moments (PREVIEW_TIME="YYYY-MM-DD HH:MM" sets the clock).
#include <Arduino.h>
#include <algorithm>
#include <fstream>
#include <string>
#include "MlbRenderer.h"
#include <FaceKit.h>

unsigned long millis() { return 0; }
namespace ezt { time_t fakeNow = 0; }
void (*onFlip)(MatrixPanel_I2S_DMA *) = nullptr;

static MatrixPanel_I2S_DMA display;
static ClockTime clockTime;
static std::string outDir;
static std::ofstream scenes;
static int sceneNumber = 0;

static void capture(const char *caption) {
  char name[32];
  snprintf(name, sizeof(name), "scene-%02d.ppm", sceneNumber++);
  FILE *f = fopen((outDir + "/" + name).c_str(), "wb");
  if (!f) { perror(name); exit(1); }
  fprintf(f, "P6\n64 64\n255\n");
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++) {
      uint16_t c = display.getPixel(x, y);
      unsigned char rgb[] = {(unsigned char)((c >> 11) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63), (unsigned char)((c & 31) * 255 / 31)};
      fwrite(rgb, 1, sizeof(rgb), f);
    }
  fclose(f);
  scenes << name << '\t' << caption << '\n';
  display.fillScreen(0);
}

static mlb::Game game(const char *away, const char *home, int as, int hs, mlb::State state, int inning, bool bottom, const char *ar, const char *hr) {
  mlb::Game g;
  strlcpy(g.away, away, sizeof(g.away)); strlcpy(g.home, home, sizeof(g.home));
  g.awayScore = as; g.homeScore = hs; g.state = state; g.inning = inning; g.bottom = bottom;
  strlcpy(g.awayRecord, ar, sizeof(g.awayRecord)); strlcpy(g.homeRecord, hr, sizeof(g.homeRecord));
  g.start = 1793058000 + 3600;
  return g;
}

static void runs(mlb::Game &g, const std::initializer_list<int> &away, const std::initializer_list<int> &home) {
  int i = 0;
  for (int r : away) g.awayRuns[i++] = r;
  g.awayInnings = i;
  i = 0;
  for (int r : home) g.homeRuns[i++] = r;
  g.homeInnings = i;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: mlb <folder> [ball ms...]\n"); return 2; }
  outDir = argv[1];
  scenes.open(outDir + "/scenes.tsv");
  clockTime.hour = 21; clockTime.minute = 34; clockTime.utcOffset = 3600;

  mlb::Snapshot snap;
  // Milwaukee at Tampa Bay in the top of the 7th: runners on first and third, one out, two balls and a strike
  mlb::Game live = game("MIL", "TB", 4, 2, mlb::Live, 7, false, "103-59", "98-64");
  runs(live, {0, 1, 0, 2, 0, 1}, {0, 0, 1, 0, 1, 0});
  live.outs = 1; live.balls = 2; live.strikes = 1; live.bases = 5; live.awayHits = 8; live.homeHits = 5; live.awayErrors = 0; live.homeErrors = 1;
  mlb::Game second = game("LAD", "NYY", 3, 3, mlb::Live, 9, true, "100-62", "94-68");
  runs(second, {0, 0, 2, 0, 0, 1, 0, 0, 0}, {1, 0, 0, 0, 0, 2, 0, 0});
  second.outs = 2; second.balls = 3; second.strikes = 2; second.bases = 0; second.awayHits = 6; second.homeHits = 7; second.homeErrors = 2;
  mlb::Game between = live;
  between.between = true; between.bottom = false; between.outs = 0; between.bases = 0;
  mlb::Game coming = game("SD", "SF", 0, 0, mlb::Pre, 0, false, "91-71", "65-97");
  mlb::Game final = game("BOS", "HOU", 6, 3, mlb::Final, 9, false, "87-75", "81-81");
  runs(final, {0, 2, 0, 0, 1, 0, 3, 0, 0}, {1, 0, 0, 2, 0, 0, 0, 0});
  final.awayHits = 11; final.homeHits = 7; final.homeErrors = 1;
  mlb::Game extra = game("PHI", "ATL", 5, 4, mlb::Final, 11, false, "88-74", "94-68");
  runs(extra, {0, 0, 1, 0, 0, 2, 0, 0, 1, 0, 1}, {0, 1, 0, 0, 0, 0, 3, 0, 0, 0});
  extra.awayHits = 10; extra.homeHits = 9; extra.awayErrors = 1;
  snap.games[0] = live; snap.games[1] = second; snap.games[2] = coming; snap.games[3] = final;
  snap.count = 4; snap.live = 2;
  mlb::Snapshot day = snap;  // the games of a day with a game in every state
  const char *teams[][2] = {{"MIL", "TB"}, {"BOS", "NYY"}, {"SD", "LAD"}, {"CHC", "STL"}, {"TOR", "BAL"}, {"SEA", "TEX"}, {"CHW", "CLE"}, {"ARI", "COL"}, {"ATH", "KC"}, {"PIT", "CIN"}, {"MIN", "DET"}, {"WSH", "MIA"}};
  for (int i = 0; i < 12; i++) {
    strlcpy(snap.next[i].away, teams[i][0], 5); strlcpy(snap.next[i].home, teams[i][1], 5);
    snap.next[i].start = 1793058000 + (i < 5 ? 0 : i < 9 ? 86400 : 2 * 86400) + (i % 5) * 3000;
  }
  snap.nextCount = 12;
  if (getenv("QUIET")) { for (int i = 0; i < 6; i++) snap.next[i].start = 1793058000 + i * 86400; snap.nextCount = 6; }
  const char *al[] = {"TB", "CLE", "HOU", "NYY", "BOS", "CHW", "TEX", "BAL", "TOR", "MIN", "DET", "SEA", "KC", "ATH", "LAA"};
  const char *nl[] = {"MIL", "LAD", "ATL", "SD", "CHC", "PHI", "ARI", "PIT", "MIA", "STL", "WSH", "CIN", "NYM", "SF", "COL"};
  const int alWins[] = {98, 85, 81, 93, 87, 84, 80, 79, 79, 77, 76, 76, 69, 64, 62}, nlWins[] = {103, 100, 94, 91, 89, 88, 86, 82, 80, 77, 77, 75, 74, 65, 58};
  for (int i = 0; i < 15; i++) {
    strlcpy(snap.american.rows[i].abbr, al[i], 5); snap.american.rows[i].wins = alWins[i]; snap.american.rows[i].losses = 162 - alWins[i]; strlcpy(snap.american.rows[i].streak, i % 3 ? "W2" : "L1", 4);
    strlcpy(snap.national.rows[i].abbr, nl[i], 5); snap.national.rows[i].wins = nlWins[i]; snap.national.rows[i].losses = 162 - nlWins[i]; strlcpy(snap.national.rows[i].streak, i % 2 ? "W3" : "L2", 4);
  }
  snap.american.count = snap.national.count = 15;

  if (argc > 3 && std::string(argv[2]) == "ball") {
    if (const char *when = getenv("PREVIEW_TIME")) {
      struct tm tmv = {};
      if (sscanf(when, "%d-%d-%d %d:%d", &tmv.tm_year, &tmv.tm_mon, &tmv.tm_mday, &tmv.tm_hour, &tmv.tm_min) == 5) {
        tmv.tm_year -= 1900;
        tmv.tm_mon -= 1;
        ezt::fakeNow = timegm(&tmv);
        clockTime.hour = tmv.tm_hour;
        clockTime.minute = tmv.tm_min;
      }
    }
    for (int i = 3; i < argc; i++) {
      mlbview::drawClock(display, &clockTime, (uint32_t)atol(argv[i]), &snap.next[0]);
      capture(argv[i]);
    }
    return 0;
  }
  display.fillScreen(0);
  mlbview::drawGame(display, live, &clockTime, 0, false, false, false, 0, 2);
  capture("Live, top 7, runners on 1st and 3rd");
  mlbview::drawGame(display, live, &clockTime, 100, false, true, true, 0, 2);
  capture("Live, home just scored, favourite");
  mlbview::drawGame(display, second, &clockTime, 0, false, false, false, 1, 2);
  capture("Live, bottom 9, tied, bases empty");
  mlbview::drawGame(display, between, &clockTime, 0, false, false, false, 0, 1);
  capture("Between halves");
  mlbview::drawGame(display, coming, &clockTime, 0, false, false, false, 0, 1);
  capture("Game to come");
  mlbview::drawGame(display, final, &clockTime, 0, false, false, false, 0, 1);
  capture("Final");
  mlbview::drawGame(display, extra, &clockTime, 0, false, false, false, 0, 1);
  capture("Final, 11 innings");
  mlbview::drawLineScore(display, live, &clockTime, 0);
  capture("Line score, live");
  mlbview::drawLineScore(display, second, &clockTime, 0);
  capture("Line score, bottom 9");
  mlbview::drawLineScore(display, final, &clockTime, 0);
  capture("Line score, final");
  mlbview::drawLineScore(display, extra, &clockTime, 0);
  capture("Line score, 11 innings");
  int order[mlb::kMaxGames];
  int n = mlbview::orderGames(snap, "SD", order);
  mlbview::drawToday(display, snap, order, n, "SD", 0, &clockTime, 0);
  capture("Today's games (SD favourite)");
  const uint32_t slideMoments[] = {0, 5200, 5400, 5700, 8000, 10200, 15200, 20400};
  for (uint32_t e : slideMoments) {
    mlbview::drawUpcoming(display, snap, "NYY", 0, &clockTime, 0, e, 8000);
    char caption[40];
    snprintf(caption, sizeof(caption), "Next up, list at %u ms", (unsigned)e);
    capture(caption);
  }
  {  // the playoffs: the series card on the next-up page
    mlb::Series one;
    strcpy(one.round, "WORLD"); strcpy(one.first, "MIL"); strcpy(one.second, "TB"); one.firstColor = 0xFFE0; one.secondColor = 0x7DFE; one.firstWins = 3; one.secondWins = 0; one.bestOf = 7; one.game = 4;
    mlb::Series two = one;
    strcpy(two.first, "LAD"); strcpy(two.second, "NYY"); two.firstWins = 3; two.secondWins = 3; two.game = 7;
    mlb::Snapshot playoffs = snap;
    playoffs.series[0] = one; playoffs.series[1] = two; playoffs.seriesCount = 2;
    mlbview::drawUpcoming(display, playoffs, "", 0, &clockTime, 0, 0, 8000);
    capture("Next up in the playoffs, series 1");
    mlbview::drawUpcoming(display, playoffs, "", 0, &clockTime, 0, 8000, 8000);
    capture("Next up in the playoffs, series 2");
  }
  for (int games = 3; games <= 3; games++) {  // the finals: only these two teams playing, once a day
    mlb::Snapshot finals = snap;
    for (int i = 0; i < games; i++) {
      strlcpy(finals.next[i].away, i % 2 ? "TB" : "MIL", 5); strlcpy(finals.next[i].home, i % 2 ? "MIL" : "TB", 5);
      finals.next[i].start = 1793058000 + i * 86400;
    }
    finals.nextCount = games;
    mlbview::drawUpcoming(display, finals, "", 0, &clockTime, 0, 0, 8000);
    capture(games == 3 ? "Next up, same two teams, 3 games" : "Next up, same two teams, 4 games");
  }
  {  // the same two teams, two games on the first day
    mlb::Snapshot finals = snap;
    const int64_t starts[3] = {1793058000, 1793058000 + 10800, 1793058000 + 86400};
    for (int i = 0; i < 3; i++) {
      strlcpy(finals.next[i].away, "MIL", 5); strlcpy(finals.next[i].home, "TB", 5);
      finals.next[i].start = starts[i];
    }
    finals.nextCount = 3;
    mlbview::drawUpcoming(display, finals, "", 0, &clockTime, 0, 0, 8000);
    capture("Next up, same two teams, two on one day");
    mlb::Series ws; strcpy(ws.round, "WORLD"); strcpy(ws.first, "MIL"); strcpy(ws.second, "TB"); ws.firstColor = 0xFFE0; ws.secondColor = 0x7DFE; ws.firstWins = 1; ws.secondWins = 1; ws.bestOf = 7; ws.game = 3;
    finals.series[0] = ws; finals.seriesCount = 1;
    for (int i = 0; i < 4; i++) {
      strlcpy(finals.next[i].away, i % 2 ? "TB" : "MIL", 5); strlcpy(finals.next[i].home, i % 2 ? "MIL" : "TB", 5);
      finals.next[i].start = 1793058000 + i * 86400;
    }
    finals.nextCount = 4;
    mlbview::drawUpcoming(display, finals, "", 0, &clockTime, 0, 0, 8000);
    capture("World Series: series card and dates only");
  }
  {  // every kind of week the next-up page can meet
    auto fix = [](mlb::Snapshot &sn, int i, const char *away, const char *home, int64_t start) {
      strlcpy(sn.next[i].away, away, 5); strlcpy(sn.next[i].home, home, 5); sn.next[i].start = start;
    };
    const int64_t day = 86400, t0 = 1793058000;
    mlb::Snapshot a = snap;  // regular season, a busy day
    const char *aw7[7] = {"NYY", "LAD", "SF", "HOU", "ATL", "SEA", "CHC"}, *ho7[7] = {"BOS", "SD", "COL", "TEX", "NYM", "OAK", "STL"};
    for (int i = 0; i < 7; i++) fix(a, i, aw7[i], ho7[i], t0 + i * 600);
    a.nextCount = 7;
    mlbview::drawUpcoming(display, a, "", 0, &clockTime, 0, 700, 8000);
    capture("1 Regular season: busy day (7 games)");
    mlb::Snapshot b = snap;  // regular season, one game a day
    const char *aw[4] = {"NYY", "LAD", "SF", "HOU"}, *ho[4] = {"BOS", "SD", "COL", "TEX"};
    for (int i = 0; i < 4; i++) fix(b, i, aw[i], ho[i], t0 + i * day);
    b.nextCount = 4;
    mlbview::drawUpcoming(display, b, "", 0, &clockTime, 0, 700, 8000);
    capture("2 Regular season: one game a day");
    mlb::Snapshot c = snap;  // a week, a few games a day
    for (int i = 0; i < 9; i++) fix(c, i, aw[i % 4], ho[i % 4], t0 + (i < 3 ? 0 : i < 5 ? day : 2 * day) + i * 600);
    c.nextCount = 9;
    mlbview::drawUpcoming(display, c, "", 0, &clockTime, 0, 700, 8000);
    capture("3 Regular season: a few games a day");
    mlb::Snapshot d = snap;  // division series: several series at once
    mlb::Series s1, s2;
    strcpy(s1.round, "NLDS"); strcpy(s1.first, "LAD"); strcpy(s1.second, "SD"); s1.firstColor = 0x39FF; s1.secondColor = 0xFFE0; s1.firstWins = 1; s1.secondWins = 1; s1.bestOf = 5; s1.game = 3;
    strcpy(s2.round, "ALDS"); strcpy(s2.first, "NYY"); strcpy(s2.second, "BOS"); s2.firstColor = 0x9CF3; s2.secondColor = 0xF800; s2.firstWins = 2; s2.secondWins = 0; s2.bestOf = 5; s2.game = 3;
    d.series[0] = s1; d.series[1] = s2; d.seriesCount = 2;
    fix(d, 0, "SD", "LAD", t0); fix(d, 1, "BOS", "NYY", t0 + 3000); fix(d, 2, "LAD", "SD", t0 + day); fix(d, 3, "NYY", "BOS", t0 + day + 3000); fix(d, 4, "SD", "LAD", t0 + 2 * day);
    d.nextCount = 5;
    mlbview::drawUpcoming(display, d, "", 0, &clockTime, 0, 700, 8000);
    capture("4 Division series: several series at once");
    mlb::Snapshot e = snap;  // World Series
    e.series[0] = s1; strcpy(e.series[0].round, "WORLD"); strcpy(e.series[0].first, "LAD"); strcpy(e.series[0].second, "NYY"); e.series[0].bestOf = 7; e.series[0].game = 4; e.series[0].firstWins = 2; e.series[0].secondWins = 1; e.seriesCount = 1;
    for (int i = 0; i < 4; i++) fix(e, i, i % 2 ? "LAD" : "NYY", i % 2 ? "NYY" : "LAD", t0 + i * day);
    e.nextCount = 4;
    mlbview::drawUpcoming(display, e, "", 0, &clockTime, 0, 700, 8000);
    capture("5 World Series: dates only");
    mlb::Snapshot f = e;  // the same two teams but no series known
    f.seriesCount = 0;
    mlbview::drawUpcoming(display, f, "", 0, &clockTime, 0, 700, 8000);
    capture("6 Same two teams, no series known");
    mlb::Snapshot g = snap;  // nothing coming
    g.nextCount = 0;
    mlbview::drawUpcoming(display, g, "", 0, &clockTime, 0, 700, 8000);
    capture("7 No games coming");
  }
  mlbview::drawStandings(display, snap.american, true, "BOS", 0, &clockTime, 0);
  capture("AL, page 1");
  mlbview::drawStandings(display, snap.american, true, "BOS", 1, &clockTime, 0);
  capture("AL, page 2");
  mlbview::drawStandings(display, snap.national, false, "LAD", 0, &clockTime, 0);
  capture("NL, page 1");
  const uint32_t moments[] = {2000, 5900, 6300, 6900, 7400, 8500, 9300, 11500, 12 * 14000 + 5900, 12 * 14000 + 6400, 12 * 14000 + 6600, 12 * 14000 + 7200, 12 * 14000 + 7900, 13 * 14000 + 3600, 13 * 14000 + 4300, 13 * 14000 + 5000, 13 * 14000 + 5300, 13 * 14000 + 6200, 14 * 14000 + 6400, 14 * 14000 + 6700, 14 * 14000 + 7200, 15 * 14000 + 4000, 15 * 14000 + 5000, 15 * 14000 + 5500, 15 * 14000 + 6500};
  for (uint32_t m : moments) {
    mlbview::drawClock(display, &clockTime, m, &snap.next[0]);
    char caption[40];
    snprintf(caption, sizeof(caption), "Clock at %u ms", (unsigned)m);
    capture(caption);
  }
  for (int i = 0; i < 8; i++) {
    mlbview::drawRun(display, live, true, 2, (uint32_t)(i * mlbview::kRunMs / 7));
    char caption[40];
    snprintf(caption, sizeof(caption), "Run %d of 8", i + 1);
    capture(caption);
  }
  return 0;
}
