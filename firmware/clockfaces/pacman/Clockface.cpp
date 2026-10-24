// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
// Grew out of the Pac-Man clockface of Clockwise (MIT, see THIRD_PARTY_NOTICES.md) as extended in clockwizer.
#include "Clockface.h"
#include <FaceKit.h>
#include <MiniFont.h>
#include <Settings.h>

using namespace facekit;

namespace {

enum Dir : int8_t { UP = 0, RIGHT = 1, DOWN = 2, LEFT = 3 };
enum Block : uint8_t { EMPTY = 0, FOOD = 1, SUPER_FOOD = 4, NO_NODE = 9 };

// The maze is a lattice of 8 x 7 lanes, 5 px wide and 8 px apart (walls of 3 px), with a wall of 1 px on the top and left edge of the
// panel and 2 px on the right, and 1 px on the bottom. The clock box takes the middle of the middle row. Corridors are always one lane wide, and
// the dots (junction, then every 4 px) sit on one grid in both directions.
const int NC = 8, NR = 7, NODES = NC * NR;
const int XS[NC] = {1, 9, 17, 25, 33, 41, 49, 57};        // the lanes, sideways
const int YS[NR] = {1, 9, 17, 29, 42, 50, 58};            // and down
const int MID = 3;                                         // the tunnel row, and the row of the clock box
const int BX0 = 8, BY0 = 23, BX1 = 54, BY1 = 40;           // the inside of the clock box
const int TUNNEL_LEN = 13;                                 // pixels from the last lane out of the panel and in on the other side
const int LEVEL_X = BX0 + 1, LEVEL_Y = BY0 + 1;            // the level number, top left
const int ROW_Y = BY1 - 5;                                 // the score (left) and the spare lives (right) sit on this row
const int SCORE_X = BX0 + 1, LIVES_X = BX1 - 17;
const int SLOT_X[4] = {9, 17, 25, 33}, SLOT_Y = BY1 - 5;   // the ghosts wait in a row at the bottom left of the box, over the score, each above a lane; the first one starts outside the box
const int OUT_R = 2, OUT_C = 4;                            // where the first ghost starts, on the lane above the box
const unsigned long FREEZE_MS = 1000, CLEAR_MS = 2000, FLASH_MS = 250;   // everything stops FREEZE_MS when a ghost is eaten; the walls flash for CLEAR_MS when the maze is cleared
const int START_R = 4, START_C = 3;                        // Pac-Man starts on the lane under the box, in the middle
const int SPRITE = 5, GHOST_COUNT = 4;
const int FLOOR_Y = BY1 + 1;                               // the wall under the box, where the ghosts leave
const int START_LIVES = 3, MAX_LIVES = 4, EXTRA_LIFE_EVERY = 6000;   // a life more every 6000 points, up to three spares

bool nodeExists(int r, int c) { return !(r == MID && c >= 1 && c <= NC - 2); }
bool hOk(int r, int c) { return nodeExists(r, c) && nodeExists(r, c + 1); }
bool vOk(int r, int c) { return nodeExists(r, c) && nodeExists(r + 1, c); }
const int NODE_COUNT = NODES - (NC - 2);

// A fixed maze, for the unlikely case that the generator finds nothing
const char *const FALLBACK_H[NR] = {"1101011", "1010101", "1111111", "0000000", "1011101", "1001001", "1110111"};
const char *const FALLBACK_V[NR - 1] = {"11111111", "10111101", "10000001", "10000001", "01111110", "11111111"};

struct Links {
  bool h[NR][NC - 1];   // lane to the one on its right
  bool v[NR - 1][NC];   // lane to the one below it
};
Links cur, prevLinks;
bool havePrev = false;

void setH(Links &l, int r, int c, bool on) { l.h[r][c] = on; l.h[r][NC - 2 - c] = on; }   // the maze is mirrored
void setV(Links &l, int r, int c, bool on) { l.v[r][c] = on; l.v[r][NC - 1 - c] = on; }

int degree(const Links &l, int r, int c) {
  int d = 0;
  if (c > 0 && l.h[r][c - 1]) d++;
  if (c < NC - 1 && l.h[r][c]) d++;
  if (r > 0 && l.v[r - 1][c]) d++;
  if (r < NR - 1 && l.v[r][c]) d++;
  if (r == MID && (c == 0 || c == NC - 1)) d++;   // the tunnel
  return d;
}

bool connected(const Links &l) {
  bool seen[NR][NC] = {};
  int stack[NODES], top = 0, reached = 0;
  stack[top++] = 0; seen[0][0] = true;
  while (top) {
    const int cur_ = stack[--top], r = cur_ / NC, c = cur_ % NC;
    reached++;
    int nr[5], nc[5], n = 0;
    if (c > 0 && l.h[r][c - 1]) { nr[n] = r; nc[n++] = c - 1; }
    if (c < NC - 1 && l.h[r][c]) { nr[n] = r; nc[n++] = c + 1; }
    if (r > 0 && l.v[r - 1][c]) { nr[n] = r - 1; nc[n++] = c; }
    if (r < NR - 1 && l.v[r][c]) { nr[n] = r + 1; nc[n++] = c; }
    if (r == MID && c == 0) { nr[n] = r; nc[n++] = NC - 1; }
    if (r == MID && c == NC - 1) { nr[n] = r; nc[n++] = 0; }
    for (int i = 0; i < n; i++) if (!seen[nr[i]][nc[i]]) { seen[nr[i]][nc[i]] = true; stack[top++] = nr[i] * NC + nc[i]; }
  }
  return reached == NODE_COUNT;
}

int openLinks(const Links &l) {
  int n = 0;
  for (int r = 0; r < NR; r++) for (int c = 0; c < NC - 1; c++) if (hOk(r, c) && l.h[r][c]) n++;
  for (int r = 0; r < NR - 1; r++) for (int c = 0; c < NC; c++) if (vOk(r, c) && l.v[r][c]) n++;
  return n;
}

// ---- the picture of a maze in bits: one bit per pixel, bit x of row y ----
void rect(uint64_t open[64], int x0, int y0, int x1, int y1) {
  if (x1 < x0 || y1 < y0) return;
  const uint64_t bits = (x1 - x0 >= 63 ? ~0ULL : ((1ULL << (x1 - x0 + 1)) - 1)) << x0;
  for (int y = max(y0, 0); y <= min(y1, 63); y++) open[y] |= bits;
}

void openPixels(const Links &l, uint64_t open[64]) {
  memset(open, 0, 64 * sizeof(uint64_t));
  for (int r = 0; r < NR; r++) {
    for (int c = 0; c < NC; c++) {
      if (!nodeExists(r, c)) continue;
      rect(open, XS[c], YS[r], XS[c] + 4, YS[r] + 4);
      if (c < NC - 1 && hOk(r, c) && l.h[r][c]) rect(open, XS[c] + 5, YS[r], XS[c + 1] - 1, YS[r] + 4);
      if (r < NR - 1 && vOk(r, c) && l.v[r][c]) rect(open, XS[c], YS[r] + 5, XS[c] + 4, YS[r + 1] - 1);
    }
  }
  rect(open, 0, YS[MID], XS[0], YS[MID] + 4);              // the tunnel runs out to both edges of the panel
  rect(open, XS[NC - 1] + 4, YS[MID], 63, YS[MID] + 4);
  rect(open, BX0, BY0, BX1, BY1);                          // the inside of the clock box
}

// the wall: a line one pixel wide round everything that is open
void dilate(const uint64_t open[64], uint64_t out[64]) {
  for (int y = 0; y < 64; y++) {
    uint64_t a = open[y];
    if (y > 0) a |= open[y - 1];
    if (y < 63) a |= open[y + 1];
    out[y] = a | (a << 1) | (a >> 1);
  }
}

// how many pixels the black areas of this maze are over the limit
int emptyExcess(const uint64_t open[64], int limit) {
  uint64_t halo[64], rem[64];
  dilate(open, halo);
  const int xlo = max(XS[0] - 1, 0), xhi = min(XS[NC - 1] + 5, 63), ylo = max(YS[0] - 1, 0), yhi = min(YS[NR - 1] + 5, 63);
  const uint64_t cols = ((1ULL << (xhi - xlo + 1)) - 1) << xlo;   // the strips along the panel edge do not count
  for (int y = 0; y < 64; y++) rem[y] = (y >= ylo && y <= yhi) ? ~halo[y] & cols : 0;
  int excess = 0;
  for (int y0 = 0; y0 < 64; y0++) {
    while (rem[y0]) {
      uint64_t reg[64] = {};
      reg[y0] = rem[y0] & -rem[y0];
      for (bool grown = true; grown;) {
        grown = false;
        for (int y = 0; y < 64; y++) {
          uint64_t n = reg[y] | (reg[y] << 1) | (reg[y] >> 1) | (y > 0 ? reg[y - 1] : 0) | (y < 63 ? reg[y + 1] : 0);
          n &= rem[y];
          if (n != reg[y]) { reg[y] = n; grown = true; }
        }
      }
      int count = 0;
      for (int y = 0; y < 64; y++) { count += __builtin_popcountll(reg[y]); rem[y] &= ~reg[y]; }
      if (count > limit) excess += count - limit;
    }
  }
  return excess;
}

// ---- making a maze, and making the next one look as little like the last as possible ----
const int TOTAL_LINKS = 78, MIN_CLOSED = 16, MIN_LOOPS = 10, MAX_EMPTY = 70;

void randomCandidate(Links &l) {
  memset(&l, 0, sizeof(l));
  const int pct = 45 + random(26);
  for (int r = 0; r < NR; r++) for (int c = 0; c <= 3; c++) if (hOk(r, c)) setH(l, r, c, random(100) < pct);
  for (int r = 0; r < NR - 1; r++) for (int c = 0; c <= 3; c++) if (vOk(r, c)) setV(l, r, c, random(100) < pct);
  // every junction needs two ways out: open a closed corridor next to the ones that have fewer
  for (int round = 0; round < 80; round++) {
    int bad[NODES], nbad = 0;
    for (int r = 0; r < NR; r++) for (int c = 0; c < NC; c++) if (nodeExists(r, c) && degree(l, r, c) < 2) bad[nbad++] = r * NC + c;
    if (!nbad) break;
    const int pick = bad[random(nbad)], r = pick / NC, c = pick % NC;
    int kind[4], kr[4], kc[4], n = 0;   // 0 sideways, 1 down
    if (c > 0 && hOk(r, c - 1) && !l.h[r][c - 1]) { kind[n] = 0; kr[n] = r; kc[n++] = c - 1; }
    if (c < NC - 1 && hOk(r, c) && !l.h[r][c]) { kind[n] = 0; kr[n] = r; kc[n++] = c; }
    if (r > 0 && vOk(r - 1, c) && !l.v[r - 1][c]) { kind[n] = 1; kr[n] = r - 1; kc[n++] = c; }
    if (r < NR - 1 && vOk(r, c) && !l.v[r][c]) { kind[n] = 1; kr[n] = r; kc[n++] = c; }
    if (!n) return;
    const int i = random(n);
    if (kind[i] == 0) setH(l, kr[i], kc[i], true); else setV(l, kr[i], kc[i], true);
  }
}

bool validMaze(const Links &l) {
  for (int r = 0; r < NR; r++) for (int c = 0; c < NC; c++) if (nodeExists(r, c) && degree(l, r, c) < 2) return false;
  if (!connected(l)) return false;
  const int open = openLinks(l);
  if (TOTAL_LINKS - open < MIN_CLOSED) return false;
  if (open + 1 - NODE_COUNT + 1 < MIN_LOOPS) return false;   // corridors (the tunnel counts) minus junctions plus one: the loops
  uint64_t bits[64];
  openPixels(l, bits);
  return emptyExcess(bits, MAX_EMPTY) == 0;
}

int distanceBetween(const Links &a, const Links &b) {
  int d = 0;
  for (int r = 0; r < NR; r++) for (int c = 0; c < NC - 1; c++) if (hOk(r, c) && a.h[r][c] != b.h[r][c]) d++;
  for (int r = 0; r < NR - 1; r++) for (int c = 0; c < NC; c++) if (vOk(r, c) && a.v[r][c] != b.v[r][c]) d++;
  return d;
}

// The next maze: a dozen good ones, and the one that differs from the last in the most corridors
void makeMaze() {
  Links best;
  int bestDistance = -1, found = 0;
  for (int attempt = 0; attempt < 700 && found < 12; attempt++) {
    Links c;
    randomCandidate(c);
    if (!validMaze(c)) continue;
    found++;
    const int d = havePrev ? distanceBetween(c, prevLinks) : 0;
    if (d > bestDistance) { best = c; bestDistance = d; }
    if (!havePrev) break;
  }
  if (!found) {
    memset(&best, 0, sizeof(best));
    for (int r = 0; r < NR; r++) for (int c = 0; c < NC - 1; c++) best.h[r][c] = hOk(r, c) && FALLBACK_H[r][c] == '1';
    for (int r = 0; r < NR - 1; r++) for (int c = 0; c < NC; c++) best.v[r][c] = vOk(r, c) && FALLBACK_V[r][c] == '1';
  }
  cur = best;
  prevLinks = best;
  havePrev = true;
}

// ---- the level in play ----
uint8_t MAP[NR][NC];
uint64_t wallBits[64];

// the dots in the corridors between the junctions, every 4 px along the corridor; the junctions hold their own dot in MAP
struct Pill {
  int8_t x, y;
  uint8_t row, col;
  bool vertical;
  bool alive;
};
const int MAX_PILLS = 120;
Pill pills[MAX_PILLS];
int pillCount = 0;

void buildLevel() {
  uint64_t open[64];
  openPixels(cur, open);
  for (int y = 0; y < 64; y++) wallBits[y] = ~open[y];   // everything that is not corridor or box is solid, in the border colour
  for (int r = 0; r < NR; r++) {
    for (int c = 0; c < NC; c++) {
      MAP[r][c] = !nodeExists(r, c) ? NO_NODE : FOOD;
    }
  }
  MAP[1][0] = MAP[1][NC - 1] = MAP[NR - 2][0] = MAP[NR - 2][NC - 1] = SUPER_FOOD;   // a power pellet near every corner, one lane in, as in the arcade
  MAP[START_R][START_C] = EMPTY;
  pillCount = 0;
  for (int r = 0; r < NR; r++) {
    for (int c = 0; c < NC; c++) {
      if (!nodeExists(r, c)) continue;
      for (int v = 0; v < 2; v++) {
        if (v ? (r >= NR - 1 || !vOk(r, c) || !cur.v[r][c]) : (c >= NC - 1 || !hOk(r, c) || !cur.h[r][c])) continue;
        const int len = v ? YS[r + 1] - YS[r] : XS[c + 1] - XS[c];
        const int k = len / 4 - 1;   // 8 px: one dot in the middle, 12 px: two, always 4 px apart
        for (int i = 1; i <= k && pillCount < MAX_PILLS; i++) {
          const int along = len * i / (k + 1);
          Pill &p = pills[pillCount++];
          p.x = v ? XS[c] + 2 : XS[c] + 2 + along;
          p.y = v ? YS[r] + 2 + along : YS[r] + 2;
          p.row = r; p.col = c; p.vertical = v; p.alive = true;
        }
      }
    }
  }
}

int colAt(int x) { for (int i = 0; i < NC; i++) if (XS[i] == x) return i; return -1; }
int rowAt(int y) { for (int i = 0; i < NR; i++) if (YS[i] == y) return i; return -1; }
int nodeAt(int x, int y) {   // the junction a sprite stands on, or -1
  const int c = colAt(x), r = rowAt(y);
  return (c >= 0 && r >= 0 && nodeExists(r, c)) ? r * NC + c : -1;
}

// ---- colours ----
const uint16_t kYellow = 0xFE40, kDefaultDots = 0xB58C, kWhite = 0xFFFF;
uint16_t wallColour = 0x0016, dotColour = kDefaultDots, hoursColour = kYellow, minutesColour = kYellow, ghostColours[4] = {};

int colourDistance(uint16_t a, uint16_t b) {
  const int dr = (int)((a >> 11) & 0x1F) * 8 - (int)((b >> 11) & 0x1F) * 8;
  const int dg = (int)((a >> 5) & 0x3F) * 4 - (int)((b >> 5) & 0x3F) * 4;
  const int db = (int)(a & 0x1F) * 8 - (int)(b & 0x1F) * 8;
  return dr * dr + dg * dg + db * db;
}

// the first pastel from 'first' on that is not close to the wall, or to the ghosts chosen before
uint16_t pastelGhost(int first, int chosen) {
  static const uint16_t PASTELS[] = {0x9FD3, 0xFDDF, 0x965F, 0xFFB2, 0xFDF2, 0xD4FF};   // green, pink, blue, lemon, peach, purple
  for (int i = 0; i < 6; i++) {
    const uint16_t colour = PASTELS[(first + i) % 6];
    bool distinct = colourDistance(colour, wallColour) > 70 * 70;
    for (int j = 0; j < chosen; j++) distinct &= colourDistance(colour, ghostColours[j]) > 70 * 70;
    if (distinct) return colour;
  }
  return PASTELS[first % 6];
}

// the colours can be changed on the settings page while the face runs
void refreshColours() {
  const auto &settings = *Settings::getInstance();
  wallColour = settings.wallColor();
  const uint16_t dots = settings.pacmanColor(settings.dotColor);
  dotColour = dots ? dots : kDefaultDots;
  const uint16_t hours = settings.pacmanColor(settings.pacmanClockColor), minutes = settings.pacmanColor(settings.clockMinutesColor);
  hoursColour = hours ? hours : kYellow;
  minutesColour = minutes ? minutes : kYellow;
  for (int i = 0; i < GHOST_COUNT; i++) {
    const uint16_t chosen = settings.pacmanColor(settings.ghostColors[i]);
    ghostColours[i] = chosen ? chosen : pastelGhost(i, i);
  }
}

// ---- the actors ----
enum GhostMode : uint8_t { IN_HOUSE, EXITING, ROAMING, EYES, ENTERING };
struct Ghost {
  int x = 0, y = 0;
  Dir dir = LEFT;
  GhostMode mode = IN_HOUSE;
  bool eaten = false;           // eaten during this power-up: it is no longer afraid
  bool reverse = false;         // turn round at the next junction
  unsigned long leaveAt = 0;    // in the box: when it may leave
  unsigned long graceUntil = 0; // a ghost that just left the box cannot catch (or be caught) until this time
  unsigned long nextStepAt = 0;
  int8_t slot = 0;              // the place in the row in the box (0 is the leftmost), and so the lane it leaves above
};
Ghost ghosts[GHOST_COUNT];
int8_t ghostTrail[GHOST_COUNT][12];   // the last junctions each ghost chose a way on: a ghost that keeps coming back to the same ones is steered somewhere else
int ghostTrailPos[GHOST_COUNT];
int ghostHomeX(int i) { return SLOT_X[ghosts[i].slot]; }

struct Pacman {
  int x = 0, y = 0;
  Dir dir = RIGHT;
  bool powered = false, mouth = true, stopped = false;
  uint8_t iteration = 0;
  uint16_t colour = kYellow;
  unsigned long poweredSince = 0;
};
Pacman pac;

const int FOURTH_GHOST_LEVEL = 3;
const unsigned long GRACE_MS = 1000, EXIT_GAP_MS = 2500, REENTER_MS = 1000, PAC_MS = 55;
unsigned long nextExitAt = 0, modeClockMs = 0, lastTickMs = 0, deathStart = 0, deathUntil = 0, lastPacMillis = 0, lastDimStep = 0;
bool wasPowered = false, wasChasing = false;
int dimLevel = 0, dimLives = 0;   // 0 = bright, 255 = dimmed while a ghost sits over the level or the lives
int level = 1, lives = START_LIVES;
long score = 0, nextExtraLife = EXTRA_LIFE_EVERY;   // 10 a dot, 50 a power pellet, 200 and doubling for the ghosts eaten on one pellet
int ghostsEaten = 0;
// Players differ: every game is played by one of four, from someone who just started to someone who knows the maze
struct Skill { const char *name; int margin; int mistake; int reflex; bool tactics; };   // ms to spare before a ghost, % of junctions where he takes a wrong turn, px a ghost head-on makes him turn round, pellets used with thought
const Skill SKILLS[4] = {{"novice", 150, 14, 0, false}, {"casual", 250, 8, 18, true}, {"good", 400, 4, 26, true}, {"expert", 750, 0, 30, true}};
int skill = 2;
unsigned long gameStartedMs = 0;
unsigned long freezeStart = 0, freezeUntil = 0;   // a ghost was eaten: everything stands still and its points show
int freezeGhost = -1, freezePoints = 0;
unsigned long clearStart = 0, clearUntil = 0;   // the maze is cleared: the walls flash, then the next level
void addScore(int points) {
  score += points;
  while (score >= nextExtraLife) { if (lives < MAX_LIVES) lives++; nextExtraLife += EXTRA_LIFE_EVERY; }
}
uint8_t recent[NODES];
int8_t trail[24];     // the last junctions he decided on: stepping into one he has been to a few times lately costs a lot
int trailPos = 0;   // how lately Pac-Man was on each junction: the more, the less he wants to go back, so he does not run the same circuit again and again

int ramp() { return (level < 10 ? level : 10) - 1; }   // harder with every level, levelling off at 10, starting over after 21
unsigned long ghostStepMs() { return 80 - 2 * ramp(); }
unsigned long powerMs() { return 7000 - 600 * ramp() < 1500 ? 1500 : 7000 - 600 * ramp(); }

Dir opposite(Dir d) { return d == LEFT ? RIGHT : d == RIGHT ? LEFT : d == UP ? DOWN : UP; }
void moveBy(int &x, int &y, Dir dir) { if (dir == RIGHT) x++; else if (dir == LEFT) x--; else if (dir == DOWN) y++; else y--; }

unsigned long poweredLeft() {
  if (!pac.powered) return 0;
  const unsigned long elapsed = millis() - pac.poweredSince;
  return elapsed >= powerMs() ? 0 : powerMs() - elapsed;
}
bool frightened(const Ghost &g) { return pac.powered && !g.eaten && g.mode == ROAMING; }
// is a ghost over this part of the box (x0..x1, y0..y1)
bool ghostOver(int x0, int y0, int x1, int y1) {
  for (int i = 0; i < GHOST_COUNT; i++) if (!(i == 3 && level < FOURTH_GHOST_LEVEL && ghosts[i].mode == IN_HOUSE) && ghosts[i].x <= x1 && ghosts[i].x + SPRITE - 1 >= x0 && ghosts[i].y <= y1 && ghosts[i].y + SPRITE - 1 >= y0) return true;
  return false;
}

// the hole in the floor of the clock box
void openHole(int i) { for (int row = 0; row < 1; row++) wallBits[FLOOR_Y + row] &= ~(0x1FULL << ghostHomeX(i)); }
void closeHole(int i) { for (int row = 0; row < 1; row++) wallBits[FLOOR_Y + row] |= 0x1FULL << ghostHomeX(i); }

void resetGhosts() {
  const unsigned long now = millis();
  memset(ghostTrail, -1, sizeof(ghostTrail));
  memset(ghostTrailPos, 0, sizeof(ghostTrailPos));
  for (int i = 0; i < GHOST_COUNT; i++) {
    Ghost &g = ghosts[i];
    if (g.mode == EXITING || g.mode == ENTERING) closeHole(i);
    g.eaten = false; g.reverse = false; g.nextStepAt = 0;
    g.graceUntil = now + GRACE_MS;
    if (i == 0) { g.mode = ROAMING; g.x = XS[OUT_C]; g.y = YS[OUT_R]; g.dir = LEFT; continue; }   // as in the arcade, one ghost is out already
    g.slot = 3 - i;   // the last ghost on the left, the first one on the right, and that one leaves first
    g.mode = IN_HOUSE; g.x = SLOT_X[g.slot]; g.y = SLOT_Y; g.dir = DOWN;
    g.leaveAt = now + 3000 + (i - 1) * 2500;
    if (i == 3 && level < FOURTH_GHOST_LEVEL) g.leaveAt = now + 3600000;   // the fourth ghost sleeps in the box on the first levels
  }
  nextExitAt = 0;
  modeClockMs = 0;
  wasChasing = false;
}

void placeActors() {
  memset(recent, 0, sizeof(recent));
  memset(trail, -1, sizeof(trail));
  trailPos = 0;
  pac = Pacman();
  pac.x = XS[START_C];
  pac.y = YS[START_R];
  wasPowered = false;
  resetGhosts();
}

void startLevel() {
  makeMaze();
  buildLevel();
  placeActors();
}

void newGame() {
  level = 1; lives = START_LIVES; score = 0; nextExtraLife = EXTRA_LIFE_EVERY;
  const int roll = random(100);
  skill = roll < 25 ? 0 : roll < 60 ? 1 : roll < 85 ? 2 : 3;
  gameStartedMs = millis();
  Serial.printf("[PacmanClassic] new game, played by a %s\n", SKILLS[skill].name);
}

// ---- the junctions as a graph: 50 places, corridors of 8 or 12 px, the tunnel counting as 13 ----
const int INF = 30000;
int gateMargin = 500;   // ms a junction must be reached before a ghost can be there; more on the later levels, where the ghosts are quicker
int nodeOf(int r, int c) { return r * NC + c; }

bool neighbour(int r, int c, Dir d, int &nr, int &nc, int &len) {
  nr = r; nc = c;
  switch (d) {
    case RIGHT: if (c < NC - 1 && hOk(r, c) && cur.h[r][c]) { nc = c + 1; len = XS[c + 1] - XS[c]; return true; } if (c == NC - 1 && r == MID) { nc = 0; len = TUNNEL_LEN; return true; } return false;
    case LEFT:  if (c > 0 && hOk(r, c - 1) && cur.h[r][c - 1]) { nc = c - 1; len = XS[c] - XS[c - 1]; return true; } if (c == 0 && r == MID) { nc = NC - 1; len = TUNNEL_LEN; return true; } return false;
    case DOWN:  if (r < NR - 1 && vOk(r, c) && cur.v[r][c]) { nr = r + 1; len = YS[r + 1] - YS[r]; return true; } return false;
    default:    if (r > 0 && vOk(r - 1, c) && cur.v[r - 1][c]) { nr = r - 1; len = YS[r] - YS[r - 1]; return true; } return false;
  }
}

// Shortest ways over the junctions. first[] holds the first step towards each junction. A junction is left out when the walk
// there would take longer than gateMs[junction] allows (time = pixels * msPerPx, with half a second to spare).
void dijkstra(int dist[NODES], int8_t first[NODES], int start, int startDist, int banNode, const int *gateMs, int msPerPx) {
  bool done[NODES] = {};
  for (int i = 0; i < NODES; i++) { dist[i] = INF; if (first) first[i] = -1; }
  dist[start] = startDist;
  for (int round = 0; round < NODES; round++) {
    int u = -1;
    for (int i = 0; i < NODES; i++) if (!done[i] && dist[i] < INF && (u < 0 || dist[i] < dist[u])) u = i;
    if (u < 0) break;
    done[u] = true;
    for (int d = 0; d < 4; d++) {
      int nr, nc, len;
      if (!neighbour(u / NC, u % NC, static_cast<Dir>(d), nr, nc, len)) continue;
      const int v = nodeOf(nr, nc);
      if (u == start && v == banNode) continue;
      const int nd = dist[u] + len;
      if (nd >= dist[v]) continue;
      if (gateMs && nd * msPerPx + gateMargin >= gateMs[v]) continue;
      dist[v] = nd;
      if (first) first[v] = u == start ? d : first[u];
    }
  }
}

// Where a ghost will next be on a junction: the junction, the pixels to go and the junction it may not turn back to
bool ghostStart(int i, int &node, int &px, int &ban) {
  const Ghost &g = ghosts[i];
  ban = -1;
  px = 0;
  if (g.mode == IN_HOUSE || g.mode == EXITING) {
    node = nodeOf(START_R, g.slot + 1);
    px = abs(g.x - ghostHomeX(i)) + max(0, YS[START_R] - g.y);
    if (g.mode == IN_HOUSE && g.leaveAt > millis()) px += (int)((g.leaveAt - millis()) / ghostStepMs());
    return true;
  }
  if (g.mode != ROAMING) return false;
  int r = rowAt(g.y), c = colAt(g.x);
  const int here = nodeAt(g.x, g.y);
  if (here >= 0) {
    r = here / NC; c = here % NC;
  } else if (r >= 0) {   // in a corridor sideways, or in the tunnel
    int found = -1;
    if (g.dir == RIGHT) { for (int k = 0; k < NC; k++) if (XS[k] > g.x) { found = k; break; } if (found < 0) { found = 0; px = TUNNEL_LEN; } else px = XS[found] - g.x; }
    else { for (int k = NC - 1; k >= 0; k--) if (XS[k] < g.x) { found = k; break; } if (found < 0) { found = NC - 1; px = TUNNEL_LEN; } else px = g.x - XS[found]; }
    c = found;
  } else if (c >= 0) {   // up or down
    int found = -1;
    if (g.dir == DOWN) { for (int k = 0; k < NR; k++) if (YS[k] > g.y) { found = k; break; } if (found < 0) { found = NR - 1; px = 0; } else px = YS[found] - g.y; }
    else { for (int k = NR - 1; k >= 0; k--) if (YS[k] < g.y) { found = k; break; } if (found < 0) { found = 0; px = 0; } else px = g.y - YS[found]; }
    r = found;
  } else {
    return false;
  }
  node = nodeOf(r, c);
  int nr, nc, len;
  if (neighbour(r, c, opposite(g.dir), nr, nc, len)) ban = nodeOf(nr, nc);
  return true;
}

// For every junction: the soonest a hunting ghost can be there, in milliseconds (a big number when none can)
void ghostTimes(int times[NODES]) {
  for (int i = 0; i < NODES; i++) times[i] = 1000000;
  const unsigned long now = millis();
  for (int i = 0; i < GHOST_COUNT; i++) {
    const Ghost &g = ghosts[i];
    bool hunts;
    switch (g.mode) {
      case IN_HOUSE: hunts = g.leaveAt <= now + 2500; break;
      case EXITING: hunts = true; break;
      case ROAMING: hunts = !frightened(g) || poweredLeft() < 1500; break;
      default: hunts = false;
    }
    int node, px, ban;
    if (!hunts || !ghostStart(i, node, px, ban)) continue;
    int dist[NODES];
    dijkstra(dist, nullptr, node, px, ban, nullptr, 0);
    for (int j = 0; j < NODES; j++) if (dist[j] < INF) times[j] = min(times[j], dist[j] * (int)ghostStepMs());
  }
}

// ---- Pac-Man ----
bool linkHasPills(int row, int col, Dir dir) {
  const bool vertical = dir == UP || dir == DOWN;
  const int r = dir == UP ? row - 1 : row, c = dir == LEFT ? col - 1 : col;
  for (int i = 0; i < pillCount; i++) if (pills[i].alive && pills[i].row == r && pills[i].col == c && pills[i].vertical == vertical) return true;
  return false;
}

int foodLeft() {
  int n = 0;
  for (int r = 0; r < NR; r++) for (int c = 0; c < NC; c++) if (MAP[r][c] == FOOD || MAP[r][c] == SUPER_FOOD) n++;
  for (int i = 0; i < pillCount; i++) if (pills[i].alive) n++;
  return n;
}

// At a junction: walk the maze to every junction he can reach before a hunting ghost can, head for the nearest food, keep the power
// pellet for when a ghost is close, chase frightened ghosts he can still catch, keep going straight and do not turn back for nothing.
// When every way is cut off he runs for where the ghosts are furthest away.
// Play yourself: the web page sends a direction, he keeps going that way and takes the turn at the next junction where it is open
const unsigned long MANUAL_MS = 20000;   // the computer plays again after this long without a command
unsigned long manualUntil = 0;
int wantedDir = -1;
bool manual() { return millis() < manualUntil; }

void manualDecision() {
  const int row = rowAt(pac.y), col = colAt(pac.x);
  int nr, nc, len;
  if (wantedDir >= 0 && neighbour(row, col, static_cast<Dir>(wantedDir), nr, nc, len)) pac.dir = static_cast<Dir>(wantedDir);
  else if (!neighbour(row, col, pac.dir, nr, nc, len)) pac.stopped = true;   // a wall ahead: wait for a direction
}

void directionDecision() {
  const int row = rowAt(pac.y), col = colAt(pac.x), start = nodeOf(row, col);
  for (int i = 0; i < NODES; i++) if (recent[i]) recent[i]--;
  recent[start] = min(recent[start] + 14, 60);
  gateMargin = SKILLS[skill].margin + 25 * ramp();
  trail[trailPos++ % 24] = start;
  int times[NODES], dist[NODES];
  int8_t first[NODES];
  ghostTimes(times);
  dijkstra(dist, first, start, 0, -1, times, PAC_MS);
  const bool threatened = times[start] < 6000;
  // how much of the maze each way leads to: a way into a small corner is a trap when a ghost is near
  int hunting = 0, near = 0;   // ghosts out in the maze that are not afraid, and how many of them are close to him
  for (const Ghost &g : ghosts) {
    if (g.mode != ROAMING || frightened(g)) continue;
    hunting++;
    if (abs(g.x - pac.x) + abs(g.y - pac.y) < 45) near++;
  }
  int reach[4] = {};
  int room[4] = {};   // how much safe room each way leads to: junctions he gets to first, the more the earlier
  for (int j = 0; j < NODES; j++) {
    if (j == start || dist[j] >= INF || first[j] < 0) continue;
    reach[first[j]]++;
    room[first[j]] += 1 + min(max(times[j] - dist[j] * (int)PAC_MS, 0), 3000) / 300;
  }

  int score[4] = {INF, INF, INF, INF};
  for (int j = 0; j < NODES; j++) {
    if (j == start || dist[j] >= INF || first[j] < 0) continue;
    const int r = j / NC, c = j % NC;
    if (!nodeExists(r, c)) continue;
    bool pill = MAP[r][c] == FOOD || MAP[r][c] == SUPER_FOOD;
    for (int d = 0; d < 4 && !pill; d++) pill = linkHasPills(r, c, static_cast<Dir>(d));
    if (!pill) continue;
    int cost = dist[j];
    if (!pac.powered && times[j] < 1000000) {   // food with room to spare before a ghost can get there is worth more than food just ahead of one
      const int slack = times[j] - dist[j] * (int)PAC_MS;
      if (slack < 3500) cost += (3500 - slack) / 80;
    }
    int around = 0;   // food next to that junction: a place with more left is worth the trip, so he clears an area instead of leaving single dots behind
    for (int d = 0; d < 4; d++) {
      int nr, nc, len;
      if (neighbour(r, c, static_cast<Dir>(d), nr, nc, len) && (linkHasPills(r, c, static_cast<Dir>(d)) || MAP[nr][nc] == FOOD)) around++;
    }
    cost -= 5 * around;
    if (MAP[r][c] == SUPER_FOOD) {   // the pellet is for catching ghosts: eat it with ghosts about, not when powered already or when there is nothing to catch
      if (!SKILLS[skill].tactics) cost += 0;
      else if (pac.powered || hunting == 0) cost += 90;
      else if (threatened) cost -= 40 + 18 * (near > 1 ? near - 1 : 0);
      else cost += 25;
    }
    score[first[j]] = min(score[first[j]], cost);
  }
  for (int d = 0; d < 4; d++) {   // food in the corridor right here
    int nr, nc, len;
    if (neighbour(row, col, static_cast<Dir>(d), nr, nc, len) && dist[nodeOf(nr, nc)] < INF && linkHasPills(row, col, static_cast<Dir>(d))) score[d] = min(score[d], len - 24);
  }
  if (pac.powered) {   // go for the closest frightened ghost he can still reach before the power ends, and for nothing else
    int way = -1, wayDist = INF;
    for (int i = 0; i < GHOST_COUNT; i++) {
      int node, px, ban;
      if (!frightened(ghosts[i]) || !ghostStart(i, node, px, ban) || node == start || dist[node] >= INF || first[node] < 0) continue;
      if ((long)dist[node] * (long)PAC_MS + 150 < (long)poweredLeft() && dist[node] < wayDist) { wayDist = dist[node]; way = first[node]; }
    }
    if (way >= 0) {
      for (int d = 0; d < 4; d++) if (d != way) score[d] = INF;
      score[way] = wayDist - 140 - 30 * ghostsEaten;
    }
  }

  int best = -1, bestScore = INF;
  for (int d = 0; d < 4; d++) {
    if (score[d] >= INF) continue;
    if (threatened && !pac.powered && reach[d] < 10) score[d] += (10 - reach[d]) * 14;
    int nr, nc, len;
    neighbour(row, col, static_cast<Dir>(d), nr, nc, len);
    // the step into a junction he visited lately costs extra, and a little luck keeps two equal ways from always going the same
    int seen = 0;   // how often the junction ahead is among the last 24 he decided on
    for (int k = 0; k < 24; k++) if (trail[k] == nodeOf(nr, nc)) seen++;
    const int s = score[d] - (d == pac.dir ? 4 : 0) + (d == opposite(pac.dir) ? 14 : 0) + recent[nodeOf(nr, nc)] / 2 + seen * 22 + (int)random(7);
    if (s < bestScore) { bestScore = s; best = d; }
  }
  if (best < 0) {   // no food he can safely reach: towards the most room to move, then away from the ghosts
    int bestRoom = 0;
    for (int d = 0; d < 4; d++) {
      const int v = room[d] + (d == pac.dir ? 3 : 0) - (d == opposite(pac.dir) ? 3 : 0);
      if (room[d] > 0 && v > bestRoom) { bestRoom = v; best = d; }
    }
  }
  for (int m = 0; best < 0 && m < 3; m++) {   // cornered: settle for less spare time, the least bad way out
    gateMargin = m == 0 ? SKILLS[skill].margin / 3 : m == 1 ? -150 : -400;
    dijkstra(dist, first, start, 0, -1, times, PAC_MS);
    int bestRoom = 0, roomNow[4] = {};
    for (int j = 0; j < NODES; j++) if (j != start && dist[j] < INF && first[j] >= 0) roomNow[first[j]] += 1 + min(max(times[j] - dist[j] * (int)PAC_MS, 0), 3000) / 300;
    for (int d = 0; d < 4; d++) if (roomNow[d] > bestRoom) { bestRoom = roomNow[d]; best = d; }
  }
  if (best < 0) {   // not even that: away from the ghosts
    int bestTime = -1;
    for (int d = 0; d < 4; d++) {
      int nr, nc, len;
      if (!neighbour(row, col, static_cast<Dir>(d), nr, nc, len)) continue;
      const int t = min(times[nodeOf(nr, nc)], 20000) + (d == pac.dir ? 1500 : 0) - (d == opposite(pac.dir) ? 1500 : 0);
      if (t > bestTime) { bestTime = t; best = d; }
    }
  }
  if (best >= 0 && random(100) < SKILLS[skill].mistake) {   // a slip: a wrong turn, never straight back
    int open[4], n = 0;
    for (int d = 0; d < 4; d++) {
      int nr, nc, len;
      if (d != opposite(pac.dir) && neighbour(row, col, static_cast<Dir>(d), nr, nc, len)) open[n++] = d;
    }
    if (n > 1) best = open[random(n)];
  }
  if (best >= 0) pac.dir = static_cast<Dir>(best);
}

// Pac-Man covers a pill, even partly: eaten
void eatPills() {
  for (int i = 0; i < pillCount; i++) {
    Pill &p = pills[i];
    if (p.alive && p.x >= pac.x && p.x < pac.x + SPRITE && p.y >= pac.y && p.y < pac.y + SPRITE) { p.alive = false; addScore(10); }
  }
}

// In a corridor, with a hunting ghost coming straight at him in the same corridor close ahead: turn round while there is room
unsigned long reverseLockUntil = 0;
bool ghostComingAtHim() {
  if (SKILLS[skill].reflex <= 0) return false;
  const bool sideways = pac.dir == LEFT || pac.dir == RIGHT;
  const int ahead = pac.dir == RIGHT || pac.dir == DOWN ? 1 : -1;
  for (const Ghost &g : ghosts) {
    if (g.mode != ROAMING || frightened(g) || millis() < g.graceUntil) continue;
    const int along = sideways ? (g.x - pac.x) * ahead : (g.y - pac.y) * ahead, across = sideways ? abs(g.y - pac.y) : abs(g.x - pac.x);
    const Dir facing = sideways ? (ahead > 0 ? LEFT : RIGHT) : (ahead > 0 ? UP : DOWN);
    if (across < 3 && along > 0 && along <= SKILLS[skill].reflex && g.dir == facing) return true;
  }
  return false;
}

void stepPacman() {
  if (millis() - lastPacMillis < PAC_MS) return;
  lastPacMillis = millis();
  const int here = nodeAt(pac.x, pac.y);
  if (here >= 0) {
    const int r = here / NC, c = here % NC;
    if (MAP[r][c] == SUPER_FOOD) { pac.powered = true; pac.poweredSince = millis(); ghostsEaten = 0; addScore(50); }   // power up only on the pellet
    else if (MAP[r][c] == FOOD) addScore(10);
    MAP[r][c] = EMPTY;
    pac.stopped = false;
    if (manual()) manualDecision(); else directionDecision();
    if (foodLeft() == 0) {   // cleared: the ghosts vanish and the walls flash before the next level
      clearStart = millis();
      clearUntil = clearStart + CLEAR_MS;
      Serial.printf("[PacmanClassic] level %d cleared, score %ld, lives %d, %s, game time %lus\n", level, score, lives, SKILLS[skill].name, (millis() - gameStartedMs) / 1000);
      return;
    }
  }
  if (here < 0 && manual()) { if (wantedDir == opposite(pac.dir)) pac.dir = static_cast<Dir>(wantedDir); }   // turning round is allowed anywhere
  else if (here < 0 && !pac.powered && millis() >= reverseLockUntil && ghostComingAtHim()) { pac.dir = opposite(pac.dir); reverseLockUntil = millis() + 700; }
  if (pac.stopped) return;
  moveBy(pac.x, pac.y, pac.dir);
  if (pac.iteration % 3 == 0) pac.mouth = !pac.mouth;
  if (pac.powered) {
    pac.colour = pac.iteration % 2 == 0 ? (uint16_t)random(0x10000) : kYellow;
    if (millis() - pac.poweredSince >= powerMs()) { pac.powered = false; pac.colour = kYellow; }
  }
  pac.iteration++;
  eatPills();
  if (pac.x <= -5) pac.x = 64; else if (pac.x >= 64) pac.x = -5;   // out through the tunnel, in on the other side
}

// ---- the ghosts ----
// scatter, chase, scatter, chase ... in seconds; the last chase never ends
bool chasing() {
  static const uint8_t SCHEDULE[7] = {7, 20, 7, 20, 5, 20, 5};
  unsigned long t = modeClockMs / 1000;
  const int chasePct = 45 + 6 * ramp() < 100 ? 45 + 6 * ramp() : 100, scatterPct = 160 - 6 * ramp() > 100 ? 160 - 6 * ramp() : 100;   // early levels: short hunts, long breaks
  for (int i = 0; i < 7; i++) {
    const unsigned long len = SCHEDULE[i] * (i % 2 ? chasePct : scatterPct) / 100;
    if (t < len) return i % 2 == 1;
    t -= len;
  }
  return true;
}

// where ghost i heads for while it chases (or scatters)
void ghostTarget(int i, int &tx, int &ty) {
  static const int CORNER_X[4] = {XS[NC - 1], XS[0], XS[NC - 1], XS[0]}, CORNER_Y[4] = {YS[0], YS[0], YS[NR - 1], YS[NR - 1]};
  const int ahead = pac.dir == RIGHT ? 1 : pac.dir == LEFT ? -1 : 0, below = pac.dir == DOWN ? 1 : pac.dir == UP ? -1 : 0;
  if (!chasing()) { tx = CORNER_X[i]; ty = CORNER_Y[i]; return; }
  switch (i) {
    case 0: tx = pac.x; ty = pac.y; break;                                                  // straight at him
    case 1: tx = pac.x + ahead * 24; ty = pac.y + below * 24; break;                        // where he is going
    case 2: {                                                                               // through the first ghost, twice as far
      const int px = pac.x + ahead * 12, py = pac.y + below * 12;
      tx = px * 2 - ghosts[0].x; ty = py * 2 - ghosts[0].y;
      break;
    }
    default:                                                                                // shy up close
      if (abs(ghosts[3].x - pac.x) + abs(ghosts[3].y - pac.y) > 40) { tx = pac.x; ty = pac.y; }
      else { tx = CORNER_X[3]; ty = CORNER_Y[3]; }
  }
}

// On a junction: where next. Frightened ghosts wander, eyes walk home, the others make for their target, never straight back.
void ghostJunction(int i) {
  Ghost &g = ghosts[i];
  const int row = rowAt(g.y), col = colAt(g.x);
  const Dir back = opposite(g.dir);

  if (g.mode == EYES) {   // the shortest way to the lane under the box
    int dist[NODES];
    dijkstra(dist, nullptr, nodeOf(START_R, 1), 0, -1, nullptr, 0);   // eyes always come back above the leftmost place
    int best = -1, bestDist = INF;
    for (int d = 0; d < 4; d++) {
      int nr, nc, len;
      if (!neighbour(row, col, static_cast<Dir>(d), nr, nc, len)) continue;
      if (dist[nodeOf(nr, nc)] + len < bestDist) { bestDist = dist[nodeOf(nr, nc)] + len; best = d; }
    }
    if (best >= 0) g.dir = static_cast<Dir>(best);
    return;
  }

  ghostTrail[i][ghostTrailPos[i]++ % 12] = nodeOf(row, col);
  Dir options[4];
  int count = 0;
  for (int d = 0; d < 4; d++) {
    int nr, nc, len;
    if (static_cast<Dir>(d) != back && neighbour(row, col, static_cast<Dir>(d), nr, nc, len)) options[count++] = static_cast<Dir>(d);
  }
  int nr, nc, len;
  const bool canBack = neighbour(row, col, back, nr, nc, len);
  if (g.reverse && canBack) { g.dir = back; g.reverse = false; return; }
  g.reverse = false;
  if (!count) { if (canBack) g.dir = back; return; }
  if (frightened(g)) { g.dir = options[random(count)]; return; }

  int tx, ty;
  ghostTarget(i, tx, ty);
  static const Dir ORDER[4] = {UP, LEFT, DOWN, RIGHT};   // this order breaks ties, as in the arcade
  long bestDistance = 1L << 30;
  Dir best = options[0];
  for (Dir d : ORDER) {
    bool ok = false;
    for (int k = 0; k < count; k++) if (options[k] == d) ok = true;
    if (!ok) continue;
    neighbour(row, col, d, nr, nc, len);
    int visits = 0;   // a junction visited lately counts as 30 px further away for every time
    for (int k = 0; k < 12; k++) if (ghostTrail[i][k] == nodeOf(nr, nc)) visits++;
    const long dx = XS[nc] - tx, dy = YS[nr] - ty, cost = dx * dx + dy * dy + visits * 900L;
    if (cost < bestDistance) { bestDistance = cost; best = d; }
  }
  g.dir = best;
}

void stepGhost(int i) {
  Ghost &g = ghosts[i];
  const unsigned long now = millis();
  switch (g.mode) {
    case EXITING:   // along the box to the hole, straight down through it onto the lane below, then the wall closes behind it
      if (g.x != ghostHomeX(i)) { g.dir = g.x < ghostHomeX(i) ? RIGHT : LEFT; g.x += g.x < ghostHomeX(i) ? 1 : -1; break; }
      g.dir = DOWN;
      g.y++;
      if (g.y >= YS[START_R]) {
        closeHole(i);
        g.y = YS[START_R];
        g.mode = ROAMING;
        g.dir = i < 2 ? LEFT : RIGHT;
        g.graceUntil = now + GRACE_MS;
        nextExitAt = now + EXIT_GAP_MS;
      }
      break;
    case ENTERING:   // eyes back up through the hole into the box and along it to their place
      if (g.y > SLOT_Y) {
        g.dir = UP;
        g.y--;
        if (g.y <= FLOOR_Y - 6) closeHole(i);   // the whole ghost is above the floor again
      } else if (g.x != SLOT_X[g.slot]) {
        g.dir = g.x < SLOT_X[g.slot] ? RIGHT : LEFT;
        g.x += g.x < SLOT_X[g.slot] ? 1 : -1;
      } else {
        g.mode = IN_HOUSE;
        g.dir = DOWN;
        g.leaveAt = now + REENTER_MS;   // a second in the box, then out again without waiting for the others
        nextExitAt = 0;
      }
      break;
    case ROAMING:
    case EYES: {
      if (nodeAt(g.x, g.y) >= 0) {
        if (g.mode == EYES && rowAt(g.y) == START_R && colAt(g.x) == 1) {   // above the leftmost place: in, once no other ghost is using a hole
          bool busy = false;
          for (const Ghost &o : ghosts) if (o.mode == EXITING || o.mode == ENTERING) busy = true;
          if (busy) return;
          int free = 0;   // the ghosts waiting there make room: the ones left of the first empty place move one place right
          while (free < 4) { bool taken = false; for (const Ghost &o : ghosts) if (o.mode == IN_HOUSE && o.slot == free) taken = true; if (!taken) break; free++; }
          for (Ghost &o : ghosts) if (o.mode == IN_HOUSE && o.slot < free) o.slot++;
          g.slot = 0;
          g.mode = ENTERING;
          openHole(i);
          return;
        }
        ghostJunction(i);
      }
      moveBy(g.x, g.y, g.dir);
      if (g.x <= -5) g.x = 64; else if (g.x >= 64) g.x = -5;   // the tunnel
      break;
    }
    default: break;
  }
}

void updateGhosts() {
  const unsigned long now = millis();
  unsigned long dt = now - lastTickMs;
  lastTickMs = now;
  if (dt > 200) dt = 200;
  modeClockMs += dt;

  // a power pellet scares them (they turn round at the next junction), and when it wears off they are themselves again
  if (pac.powered && !wasPowered) for (Ghost &g : ghosts) { g.eaten = false; if (g.mode == ROAMING) g.reverse = true; }
  if (!pac.powered && wasPowered) for (Ghost &g : ghosts) g.eaten = false;
  wasPowered = pac.powered;
  // a change between scatter and chase turns them round too
  const bool chase = chasing();
  if (chase != wasChasing) { wasChasing = chase; for (Ghost &g : ghosts) if (g.mode == ROAMING && !frightened(g)) g.reverse = true; }

  for (int i = 0; i < GHOST_COUNT; i++) {
    Ghost &g = ghosts[i];
    if (g.mode == IN_HOUSE) continue;
    unsigned long interval = ghostStepMs();
    if (g.mode == EYES) interval = 22;
    else if (g.mode == EXITING || g.mode == ENTERING) interval = 60;
    else if (frightened(g)) interval = 115;
    if (g.nextStepAt == 0 || g.nextStepAt + 500 < now) g.nextStepAt = now;
    for (int guard = 0; now >= g.nextStepAt && guard < 4; guard++) { stepGhost(i); g.nextStepAt += interval; }
  }

  // ghosts in the box slide to their place when one comes in on the left
  for (Ghost &g : ghosts) {
    if (g.mode != IN_HOUSE || g.x == SLOT_X[g.slot]) continue;
    if (now - g.nextStepAt < 35 && g.nextStepAt) continue;
    g.nextStepAt = now;
    g.x += g.x < SLOT_X[g.slot] ? 1 : -1;
  }

  // time is up: open the wall under the ghost and let it out straight down, one ghost at a time with a pause in between
  bool anyExiting = false;
  for (const Ghost &g : ghosts) if (g.mode == EXITING || g.mode == ENTERING) anyExiting = true;
  int next = -1;
  for (int i = 0; i < GHOST_COUNT && !anyExiting; i++) {
    const Ghost &g = ghosts[i];
    if (g.mode == IN_HOUSE && g.x == SLOT_X[g.slot] && now >= g.leaveAt && now >= nextExitAt && (next < 0 || g.leaveAt < ghosts[next].leaveAt)) next = i;
  }
  if (next >= 0) {
    ghosts[next].mode = EXITING;
    ghosts[next].nextStepAt = 0;
    openHole(next);
  }
}

// after the pause every clock that was running goes on from where it stopped
void endFreeze() {
  const unsigned long now = millis(), pause = now - freezeStart;
  pac.poweredSince += pause;
  nextExitAt += pause;
  for (Ghost &g : ghosts) { g.leaveAt += pause; g.graceUntil += pause; g.nextStepAt = 0; }
  lastTickMs = lastPacMillis = now;
  freezeUntil = 0;
  freezeGhost = -1;
}

void checkGhostCollisions() {
  const unsigned long now = millis();
  for (int i = 0; i < GHOST_COUNT; i++) {
    Ghost &g = ghosts[i];
    if (g.mode != ROAMING || now < g.graceUntil) continue;
    if (abs(g.x - pac.x) < 4 && abs(g.y - pac.y) < 4) {
      if (frightened(g)) {
        g.mode = EYES;   // eaten: the eyes go home to the box
        freezePoints = 200 << (ghostsEaten < 3 ? ghostsEaten : 3);   // 200, 400, 800, 1600
        ghostsEaten++;
        addScore(freezePoints);
        freezeStart = now; freezeUntil = now + FREEZE_MS; freezeGhost = i;
        return;
        g.eaten = true;
        g.nextStepAt = 0;
      } else {
        deathUntil = now + 1500;
        deathStart = now;
        Serial.printf("[PacmanClassic] caught on level %d by ghost %d at %d,%d, score %ld, lives left %d, %s, food left %d, dir %d, ghost dir %d\n", level, i, pac.x, pac.y, score, lives - 1, SKILLS[skill].name, foodLeft(), (int)pac.dir, (int)ghosts[i].dir);
        return;
      }
    }
  }
}

void updateDeath() {
  if (millis() < deathUntil) return;
  if (--lives <= 0) {   // out of lives: a new game on a new maze
    Serial.printf("[PacmanClassic] game over on level %d, score %ld, %s, game time %lus\n", level, score, SKILLS[skill].name, (millis() - gameStartedMs) / 1000);
    newGame();
    startLevel();
  } else {
    placeActors();      // the maze stays, as it was
  }
  deathUntil = 0;
}

// ---- drawing ----
// The sprites are 5 x 5, drawn facing right and turned the way Pac-Man goes
const uint8_t PAC_SHAPE[2][25] = {
  {0, 1, 1, 1, 0,
   1, 1, 0, 1, 1,
   1, 1, 1, 1, 1,
   1, 1, 1, 1, 1,
   0, 1, 1, 1, 0},
  {0, 1, 1, 1, 1,
   1, 1, 0, 1, 0,
   1, 1, 1, 0, 0,
   1, 1, 1, 1, 0,
   0, 1, 1, 1, 1}};

// dying: 0 while he moves, then how far he has dissolved (0..1)
void drawPacman(MatrixPanel_I2S_DMA &d, float dying) {
  for (int py = 0; py < SPRITE; py++) {
    for (int px = 0; px < SPRITE; px++) {
      int col, row;   // where in the facing-right picture this pixel comes from
      switch (pac.dir) {
        case LEFT: col = 4 - px; row = py; break;
        case UP: col = 4 - py; row = px; break;
        case DOWN: col = py; row = 4 - px; break;
        default: col = px; row = py;
      }
      if (!PAC_SHAPE[pac.mouth ? 1 : 0][row * 5 + col]) continue;
      if (dying > 0 && (int)(hash(py * 5 + px + 11) % 100) < (int)(dying * 100)) continue;
      put(d, pac.x + px, pac.y + py, pac.colour);
    }
  }
}

// 0 transparent, 1 body, 2 eye
const uint8_t GHOST_SHAPE[25] = {
  0, 1, 1, 1, 0,
  1, 1, 1, 1, 1,
  1, 2, 1, 2, 1,
  1, 1, 1, 1, 1,
  1, 0, 1, 0, 1};

void drawGhost(MatrixPanel_I2S_DMA &d, int i, bool afraid, bool flash) {
  const Ghost &g = ghosts[i];
  if (g.mode == EYES || g.mode == ENTERING) {   // only the eyes, looking where it goes
    const int dx = g.dir == RIGHT ? 1 : g.dir == LEFT ? -1 : 0, dy = g.dir == DOWN ? 1 : g.dir == UP ? -1 : 0;
    for (int eye = 0; eye < 2; eye++) {
      const int ex = 1 + eye * 2;
      put(d, g.x + ex, g.y + 2, 0xFFFF);
      put(d, g.x + ex + dx, g.y + 2 + dy, 0x001F);
    }
    return;
  }
  uint16_t body = ghostColours[i], eye = 0xFFFF;
  if (afraid) { body = flash ? 0xFFFF : 0x001F; eye = flash ? 0xF800 : 0xFFFF; }
  for (int k = 0; k < 25; k++) {
    if (!GHOST_SHAPE[k]) continue;
    put(d, g.x + k % 5, g.y + k / 5, GHOST_SHAPE[k] == 2 ? eye : body);
  }
}

// The level and the lives each fade to a quarter while a ghost sits over them, and are bright again when none does
int approach(int value, int target) { return value == target ? value : target > value ? min(target, value + 64) : max(target, value - 64); }
uint16_t shade(uint16_t colour, int level) { return mix(colour, dim(colour, 64), level); }

const uint8_t kSegments[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};   // segments a b c d e f g in bits 0-6

// a digit in segments, w x h with strokes of t
void drawDigit(MatrixPanel_I2S_DMA &d, int digit, int x, int y, uint16_t colour, int w, int h, int t) {
  const uint8_t mask = kSegments[digit];
  const int mid = (h - t) / 2, half = mid + t - 1;
  if (mask & 1) box(d, x, y, w, t, colour);                    // a
  if (mask & 8) box(d, x, y + h - t, w, t, colour);            // d
  if (mask & 64) box(d, x, y + mid, w, t, colour);             // g
  if (mask & 2) box(d, x + w - t, y, t, half + 1, colour);     // b
  if (mask & 4) box(d, x + w - t, y + mid, t, h - mid, colour);// c
  if (mask & 32) box(d, x, y, t, half + 1, colour);            // f
  if (mask & 16) box(d, x, y + mid, t, h - mid, colour);       // e
}

void drawText(MatrixPanel_I2S_DMA &d, const char *text, int x, int y, uint16_t colour) {
  for (; *text; text++, x += 4) {
    const uint8_t *glyph = miniGlyph(*text);
    for (int row = 0; row < 5; row++)
      for (int col = 0; col < 3; col++)
        if (glyph[row] >> (2 - col) & 1) put(d, x + col, y + row, colour);
  }
}

// the level top left, the score bottom left and the spare lives bottom right, the time in between and on the right
void drawBox(MatrixPanel_I2S_DMA &d, ClockTime *time) {
  char text[8];
  snprintf(text, sizeof(text), "%d", level);
  drawText(d, text, LEVEL_X, LEVEL_Y, shade(kWhite, dimLevel));
  snprintf(text, sizeof(text), "%05ld", score % 100000);
  if (!ghostOver(SCORE_X, ROW_Y, SCORE_X + 18, ROW_Y + 4)) drawText(d, text, SCORE_X, ROW_Y, kWhite);   // gone while a ghost is over it
  const int spare = lives > 1 ? min(lives - 1, 3) : 0;
  for (int i = 0; i < spare; i++)
    for (int py = 0; py < SPRITE; py++)
      for (int px = 0; px < SPRITE; px++)
        if (PAC_SHAPE[1][py * 5 + px]) put(d, LIVES_X + i * 6 + px, ROW_Y + py, shade(kYellow, dimLives));

  const int w = 7, h = 10, t = 2;   // 1 px clear above and at the right
  const int width = w + 1 + w + 1 + 2 + 1 + w + 1 + w;
  const int x0 = BX1 - 1 - width + 1, y0 = BY0 + 1;
  const int hour = time->getHour(), minute = time->getMinute();
  const uint16_t hc = hoursColour, mc = minutesColour;   // the ghosts sit on the left, so the clock never dims
  if (time->is24hFormat() || hour >= 10) drawDigit(d, hour / 10, x0, y0, hc, w, h, t);
  drawDigit(d, hour % 10, x0 + w + 1, y0, hc, w, h, t);
  const int colon = x0 + 2 * w + 2;   // after the hours (2 digits and the gap between them) and 1 px of air
  if ((millis() / 1000) % 2 == 0) {   // the colon blinks, in the colour of the dots
    box(d, colon, y0 + 2, 2, 2, dotColour);
    box(d, colon, y0 + 6, 2, 2, dotColour);
  }
  drawDigit(d, minute / 10, colon + 3, y0, mc, w, h, t);
  drawDigit(d, minute % 10, colon + 3 + w + 1, y0, mc, w, h, t);
}

}  // namespace

void Clockface::steer(int dir) {
  if (dir < 0 || dir > 3) { manualUntil = 0; wantedDir = -1; return; }
  if (!manual()) Serial.printf("[PacmanClassic] you took over\n");
  manualUntil = millis() + MANUAL_MS;
  wantedDir = dir;
}

Clockface::Clockface(MatrixPanel_I2S_DMA *display) { _display = display; }

void Clockface::setup(ClockTime *dateTime) {
  _dateTime = dateTime;
  randomSeed(esp_random());
  refreshColours();
  newGame();
  havePrev = false;
  lastTickMs = millis();
  startLevel();
  deathUntil = 0;
  dimLevel = dimLives = 0;
  lastDimStep = millis();
}

void Clockface::update() {
  MatrixPanel_I2S_DMA &d = *_display;
  refreshColours();

  // ease the level and the lives towards dim (a ghost over them) or bright
  if (millis() - lastDimStep >= 50) {
    lastDimStep = millis();
    const int levelTarget = ghostOver(LEVEL_X, LEVEL_Y, LEVEL_X + 7, LEVEL_Y + 4) ? 255 : 0;
    const int livesTarget = ghostOver(LIVES_X, ROW_Y, LIVES_X + 16, ROW_Y + 4) ? 255 : 0;
    dimLevel = approach(dimLevel, levelTarget);
    dimLives = approach(dimLives, livesTarget);
  }

  if (deathUntil) {
    updateDeath();
  } else if (freezeUntil) {
    if (millis() >= freezeUntil) endFreeze();
  } else if (clearUntil) {
    if (millis() >= clearUntil) {
      clearUntil = 0;
      level = level >= 21 ? 1 : level + 1;
      startLevel();   // a new maze, as different from this one as it can be
      lastTickMs = lastPacMillis = millis();
    }
  } else {
    stepPacman();
    updateGhosts();
    checkGhostCollisions();
  }

  // ---- the picture ----
  d.fillScreen(0);
  const uint16_t wallsNow = clearUntil && ((millis() - clearStart) / FLASH_MS) % 2 == 0 ? kWhite : wallColour;   // flashing white when the maze is cleared
  for (int y = 0; y < 64; y++) {   // the walls
    const uint64_t row = wallBits[y];
    for (int x = 0; x < 64; x++) {
      if (!((row >> x) & 1)) continue;
      int len = 1;
      while (x + len < 64 && ((row >> (x + len)) & 1)) len++;
      box(d, x, y, len, 1, wallsNow);
      x += len;
    }
  }
  for (int r = 0; r < NR; r++) {   // the dots on the junctions
    for (int c = 0; c < NC; c++) {
      if (MAP[r][c] == FOOD) put(d, XS[c] + 2, YS[r] + 2, dotColour);
      else if (MAP[r][c] == SUPER_FOOD && (millis() / 250) % 2 == 0) {   // a round power pellet in the colour of the dots, blinking
        const int cx = XS[c] + 2, cy = YS[r] + 2;
        const uint16_t soft = dim(dotColour, 140), faint = dim(dotColour, 60);   // the edge is dithered with dimmer pixels to look round
        box(d, cx - 1, cy - 1, 3, 3, dotColour);
        put(d, cx - 1, cy - 1, soft); put(d, cx + 1, cy - 1, soft); put(d, cx - 1, cy + 1, soft); put(d, cx + 1, cy + 1, soft);
        put(d, cx - 2, cy, faint); put(d, cx + 2, cy, faint); put(d, cx, cy - 2, faint); put(d, cx, cy + 2, faint);
      }
    }
  }
  for (int i = 0; i < pillCount; i++) if (pills[i].alive) put(d, pills[i].x, pills[i].y, dotColour);

  drawBox(d, _dateTime);

  const bool flash = pac.powered && poweredLeft() < 2000 && (millis() / 200) % 2 == 0;
  for (int i = 0; i < GHOST_COUNT; i++) if (!clearUntil && !(freezeUntil && i == freezeGhost) && !(i == 3 && level < FOURTH_GHOST_LEVEL && ghosts[i].mode == IN_HOUSE)) drawGhost(d, i, frightened(ghosts[i]), flash);
  if (freezeUntil) {   // Pac-Man is gone for a moment and the points show where the ghost was
    char points[8];
    snprintf(points, sizeof(points), "%d", freezePoints);
    const int width = (int)strlen(points) * 4 - 1;
    drawText(d, points, constrain(ghosts[freezeGhost].x + 2 - width / 2, 0, 64 - width), ghosts[freezeGhost].y, 0x07FF);
  } else if (deathUntil) {
    const float t = (float)(millis() - deathStart) / 1500.0f;
    if (t < 0.9f) drawPacman(d, t < 0.3f ? 0.0f : (t - 0.3f) / 0.6f);
  } else {
    drawPacman(d, 0);
  }
  d.flipDMABuffer();
}
