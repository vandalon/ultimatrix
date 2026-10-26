// Renders the NBA face's pages with the real drawing code and made-up games.
// Usage: nba <output folder>; writes one PPM per scene and scenes.tsv (file, caption).
#include <Arduino.h>
#include <fstream>
#include <string>
#include "NbaRenderer.h"
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

static uint16_t rgb(int r, int g, int b) { return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); }

static nba::Game game(const char *away, uint16_t ac, const char *home, uint16_t hc, int as, int hs, nba::State state, int period, const char *clock, const char *ar, const char *hr) {
  nba::Game g;
  strlcpy(g.away, away, sizeof(g.away)); strlcpy(g.home, home, sizeof(g.home));
  g.awayColor = ac; g.homeColor = hc; g.awayScore = as; g.homeScore = hs; g.state = state; g.period = period;
  strlcpy(g.clock, clock, sizeof(g.clock)); strlcpy(g.awayRecord, ar, sizeof(g.awayRecord)); strlcpy(g.homeRecord, hr, sizeof(g.homeRecord));
  g.start = 1793058000 + 3600;
  return g;
}


// ---- the demo: the rotation of the face with its slides and the basket animation, as numbered frames ----
enum Kind { KGame, KQuarters, KToday, KNext, KEast, KWest, KClock, KBasket };
struct Item { Kind kind; int index, page, pages; };

static nbaview::Background backgroundOf(Kind k) {
  switch (k) {
    case KGame: return nbaview::BgGame;
    case KQuarters: return nbaview::BgQuarters;
    case KToday: return nbaview::BgToday;
    case KNext: return nbaview::BgNext;
    case KEast: return nbaview::BgEast;
    case KWest: return nbaview::BgWest;
    default: return nbaview::BgClock;
  }
}

static void renderItem(const nba::Snapshot &snap, const Item &item, uint32_t t, uint32_t local) {
  int order[nba::kMaxGames];
  const int count = nbaview::orderGames(snap, "", order);
  switch (item.kind) {
    case KGame: nbaview::drawGame(display, snap.games[item.index], &clockTime, t, local > 1100 && local < 2000 && item.index == 0, false, item.index == 0, item.page, item.pages); break;
    case KQuarters: nbaview::drawQuarters(display, snap.games[item.index], &clockTime, t); break;
    case KToday: nbaview::drawToday(display, snap, order, count, "", item.page, &clockTime, t); break;
    case KNext: nbaview::drawUpcoming(display, snap, "", item.page, &clockTime, t); break;
    case KEast: nbaview::drawStandings(display, snap.east, true, "BOS", item.page, &clockTime, t); break;
    case KWest: nbaview::drawStandings(display, snap.west, false, "LAL", item.page, &clockTime, t); break;
    default: nbaview::drawClock(display, &clockTime, 7000 + local, "LAL-BOS 7:30P");
  }
}

static void writeFrame(int number) {
  char name[48];
  snprintf(name, sizeof(name), "/frame-%04d.ppm", number);
  FILE *f = fopen((outDir + name).c_str(), "wb");
  fprintf(f, "P6\n64 64\n255\n");
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++) {
      uint16_t c = display.getPixel(x, y);
      unsigned char rgb[] = {(unsigned char)((c >> 11) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63), (unsigned char)((c & 31) * 255 / 31)};
      fwrite(rgb, 1, sizeof(rgb), f);
    }
  fclose(f);
}

static void demo(const nba::Snapshot &snap) {
  const Item items[] = {{KGame, 0, 0, 2}, {KBasket, 0, 0, 0}, {KGame, 1, 1, 2}, {KQuarters, 0, 0, 1}, {KToday, 0, 0, 1}, {KNext, 0, 0, 2}, {KNext, 0, 1, 2},
                        {KEast, 0, 0, 2}, {KEast, 0, 1, 2}, {KWest, 0, 0, 2}, {KClock, 0, 0, 1}};
  const uint32_t pageMs = 2400, clockMs = 5200, slideMs = 380, step = 50;
  uint32_t t = 0;
  int frame = 0;
  const Item *previous = nullptr;
  for (const Item &item : items) {
    const uint32_t duration = item.kind == KBasket ? nbaview::kBasketMs : item.kind == KClock ? clockMs : pageMs;
    for (uint32_t local = 0; local < duration; local += step, t += step) {
      display.fillScreen(0);
      if (item.kind == KBasket) {
        nbaview::drawBasket(display, snap.games[0], true, 3, local);
      } else if (previous && previous->kind != KBasket && local < slideMs) {
        const float u = local / (float)slideMs;
        const int dx = (int)(64.0f * (1 - (1 - u) * (1 - u)));
        if (backgroundOf(previous->kind) == backgroundOf(item.kind)) {  // the shared background stays put
          nbaview::drawBackground(display, backgroundOf(item.kind), t);
          nbaview::backgroundDrawnByFace(true);
        }
        facekit::view.dx = -dx;
        renderItem(snap, *previous, t, 0);
        facekit::view.dx = 64 - dx;
        renderItem(snap, item, t, local);
        facekit::view.dx = 0;
        nbaview::backgroundDrawnByFace(false);
      } else {
        renderItem(snap, item, t, local);
      }
      writeFrame(frame++);
    }
    previous = &item;
  }
  fprintf(stderr, "%d frames\n", frame);
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: nba <folder>\n"); return 2; }
  outDir = argv[1];
  scenes.open(outDir + "/scenes.tsv");
  clockTime.hour = 21; clockTime.minute = 34; clockTime.utcOffset = 3600;

  nba::Snapshot snap;
  const uint16_t lal = rgb(150, 90, 220), bos = rgb(30, 170, 80), gsw = rgb(255, 200, 40), mia = rgb(230, 50, 70), nyk = rgb(40, 110, 230), den = rgb(255, 200, 40), okc = rgb(40, 150, 235), utah = rgb(120, 70, 190);
  nba::Game live = game("LAL", lal, "BOS", bos, 78, 81, nba::Live, 3, "4:12", "3-1", "4-0");
  for (int i = 0; i < 3; i++) { live.awayQ[i] = (uint8_t[]){28, 22, 28}[i]; live.homeQ[i] = (uint8_t[]){24, 27, 30}[i]; }
  snap.games[0] = live;
  snap.games[1] = game("GSW", gsw, "MIA", mia, 52, 49, nba::Live, 2, "0:45", "2-2", "1-3");
  snap.games[2] = game("NYK", nyk, "DEN", den, 0, 0, nba::Pre, 0, "", "2-1", "3-1");
  snap.games[3] = game("OKC", okc, "UTAH", utah, 132, 123, nba::Final, 4, "0.0", "4-0", "1-3");
  for (int i = 0; i < 4; i++) { snap.games[3].awayQ[i] = (uint8_t[]){47, 24, 29, 32}[i]; snap.games[3].homeQ[i] = (uint8_t[]){24, 31, 33, 35}[i]; }
  snap.count = 4; snap.live = 2;
  const char *teams[][2] = {{"LAL", "GSW"}, {"BOS", "NYK"}, {"MIA", "PHI"}, {"DEN", "DAL"}, {"OKC", "HOU"}, {"CLE", "ORL"}, {"MIL", "CHI"}, {"UTAH", "SAC"}};
  for (int i = 0; i < 8; i++) {
    strlcpy(snap.next[i].away, teams[i][0], 5); strlcpy(snap.next[i].home, teams[i][1], 5);
    snap.next[i].start = 1793058000 + (i < 3 ? 86400 : i < 6 ? 2 * 86400 : 3 * 86400) + (i % 3) * 7200;
  }
  snap.nextCount = 8;
  const char *east[] = {"BOS", "CLE", "NYK", "MIL", "ORL", "MIA", "ATL", "DET", "CHI", "PHI", "IND", "TOR", "BKN", "CHA", "WAS"};
  const char *west[] = {"OKC", "DEN", "HOU", "LAL", "MEM", "GSW", "DAL", "SAC", "MIN", "PHX", "LAC", "POR", "UTAH", "NO", "SA"};
  for (int i = 0; i < 15; i++) {
    strlcpy(snap.east.rows[i].abbr, east[i], 5); snap.east.rows[i].wins = 9 - i / 2; snap.east.rows[i].losses = i / 2 + 1; strlcpy(snap.east.rows[i].streak, i % 3 ? "W2" : "L1", 4);
    strlcpy(snap.west.rows[i].abbr, west[i], 5); snap.west.rows[i].wins = 10 - i / 2; snap.west.rows[i].losses = i / 2; strlcpy(snap.west.rows[i].streak, i % 2 ? "W3" : "L2", 4);
  }
  snap.east.count = snap.west.count = 15;

  if (argc > 2) { demo(snap); return 0; }
  display.fillScreen(0);
  nbaview::drawGame(display, snap.games[0], &clockTime, 0, false, false, false, 0, 2);
  capture("Live game");
  nbaview::drawGame(display, snap.games[0], &clockTime, 100, false, true, true, 0, 2);
  capture("Live game, home just scored, favourite");
  nbaview::drawGame(display, snap.games[2], &clockTime, 0, false, false, false, 0, 1);
  capture("Game to come");
  nbaview::drawGame(display, snap.games[3], &clockTime, 0, false, false, false, 0, 1);
  capture("Final");
  nbaview::drawQuarters(display, snap.games[0], &clockTime, 0);
  capture("Quarters, live");
  nbaview::drawQuarters(display, snap.games[3], &clockTime, 0);
  capture("Quarters, final");
  int order[nba::kMaxGames];
  int n = nbaview::orderGames(snap, "GSW", order);
  nbaview::drawToday(display, snap, order, n, "GSW", 0, &clockTime, 0);
  capture("Today's games (GSW favourite)");
  nbaview::drawUpcoming(display, snap, "NYK", 0, &clockTime, 0);
  capture("Upcoming, page 1");
  nbaview::drawUpcoming(display, snap, "NYK", 1, &clockTime, 0);
  capture("Upcoming, page 2");
  nbaview::drawStandings(display, snap.east, true, "BOS", 0, &clockTime, 0);
  capture("East, page 1");
  nbaview::drawStandings(display, snap.west, false, "LAL", 0, &clockTime, 0);
  capture("West, page 1");
  nbaview::drawStandings(display, snap.west, false, "LAL", 1, &clockTime, 0);
  capture("West, page 2");
  const uint32_t moments[] = {2000, 8600, 9100, 9800, 10300, 12500};
  for (uint32_t m : moments) {
    nbaview::drawClock(display, &clockTime, m, "LAL-BOS 7:30P");
    char caption[40];
    snprintf(caption, sizeof(caption), "Clock at %u ms", (unsigned)m);
    capture(caption);
  }
  for (int i = 0; i < 8; i++) {
    nbaview::drawBasket(display, snap.games[0], true, 3, (uint32_t)(i * nbaview::kBasketMs / 7));
    char caption[40];
    snprintf(caption, sizeof(caption), "Basket %d of 8", i + 1);
    capture(caption);
  }
  return 0;
}
