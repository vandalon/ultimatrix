// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Clockface.h"
#include <MiniFont.h>
#include <NumericFont.h>
#include "Maze.h"

using namespace facekit;
using namespace pacmaze;

namespace {

// ---- Where things are: a cell is a 5 pixel corridor with a wall of 2 between. The maze is bigger than the
// panel; a camera follows Pac-Man and (OX, OY) is where the maze's top left corner is on the panel right now ----
const int PITCH = 7, VIEW_TOP = 16, VIEW_H = 64 - VIEW_TOP;
const int MAZE_W = COLS * PITCH + 2, MAZE_H = ROWS * PITCH + 2;
int OX = 0, OY = VIEW_TOP;
float camX = 0, camY = 0;
const uint16_t kBackground = rgb(4, 4, 18);
const uint16_t kWall = rgb(36, 64, 255), kWallFlash = rgb(230, 230, 255), kDoor = rgb(255, 184, 222);
const uint16_t kPill = rgb(255, 184, 174), kPacman = rgb(255, 224, 0);
const uint16_t kHours = rgb(238, 246, 240), kMinutes = rgb(190, 235, 150), kSpark = rgb(255, 255, 255);

// Speeds in pixels per second: calm, rather than arcade
const float kPacSpeed = 12, kGhostSpeed = 10.5f, kScaredSpeed = 6.5f, kEyesSpeed = 26;

// ---- The time, in the solid block digits of football and F1: strokes 2 px wide, 6 x 10 px, the smallest that stays unsquashed ----
const int kDigitW = 6, kDigitH = 10, kDigitTop = 3, kChangeMs = 1100;
const int kDigitX[4] = {12, 20, 38, 46};

struct Slot { int8_t digit; int8_t from; uint32_t since; };
Slot slots[4];

bool solid(int digit, int x, int y) { return digit >= 0 && numericfont::pixel(digit, x, y, kDigitW, kDigitH); }

void drawDigit(MatrixPanel_I2S_DMA &d, int index, uint16_t color, uint32_t now) {
  const Slot &slot = slots[index];
  const int age = slot.since ? (int)(now - slot.since) : kChangeMs;
  for (int row = 0; row < kDigitH; row++)
    for (int col = 0; col < kDigitW; col++) {
      const uint32_t noise = hash((row * kDigitW + col) * 7 + index * 131 + slot.digit * 17);
      uint16_t c = 0;
      if (age >= kChangeMs) {
        if (solid(slot.digit, col, row)) c = color;
      } else {
        const int leaveAt = noise % 450, arriveAt = 550 + (noise >> 8) % 450;
        if (solid(slot.from, col, row) && age < leaveAt + 120) c = age < leaveAt ? color : kSpark;
        else if (solid(slot.digit, col, row) && age >= arriveAt) c = age < arriveAt + 120 ? kSpark : color;
      }
      if (c) put(d, kDigitX[index] + col, kDigitTop + row, c);
    }
}

// ---- The game ----

uint32_t rng = 1;
uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

enum class Mode : uint8_t { InHouse, Leaving, Scatter, Chase, Scared, Eyes };
enum class State : uint8_t { Ready, Playing, Dying, Cleared };

struct Actor {
  int8_t c, r, dir;
  float prog;           // pixels travelled from the centre of (c, r) towards `dir`
};
struct Ghost : Actor {
  Mode mode;
  uint32_t since;       // when the mode began, for the house and the release
  uint32_t releaseAt;   // when it leaves the house
  bool reverse;
  bool eaten;           // already eaten during this fright
};

Maze maze;
Actor pac;
Ghost ghosts[4];
State state = State::Ready;
uint32_t stateSince = 0, lastNow = 0, scaredUntil = 0, modeClock = 0, popupAt = 0, fruitAt = 0, eatenCount = 0;
int level = 1, lives = 3, popupX = 0, popupY = 0, popupScore = 0, pillsEaten = 0, fruitsShown = 0;
int8_t pacLastDir = Right;

// scatter, chase, scatter, chase ... in seconds; the last chase never ends
const uint8_t kSchedule[7] = {7, 20, 7, 20, 5, 20, 5};

bool passable(int c, int r) { return c >= 0 && c < COLS && r >= 0 && r < ROWS && !isHouse(c, r); }

void moveCamera(float dt, bool snap);

void newMaze(uint32_t seed) {
  MazeBuilder builder(seed);
  builder.build(maze);
}

int pixelX(const Actor &a) { return OX + PITCH * a.c + 2 + DX[a.dir] * (int)(a.prog + 0.5f); }
int pixelY(const Actor &a) { return OY + PITCH * a.r + 2 + DY[a.dir] * (int)(a.prog + 0.5f); }

void resetActors(uint32_t now) {
  pac = {HOUSE_LEFT, START_ROW, Left, 0};
  pacLastDir = Left;
  const int doorRow = HOUSE_ROW - 1;
  ghosts[0] = {{HOUSE_LEFT, (int8_t)doorRow, Left, 0}, Mode::Scatter, now, 0, false, false};          // Blinky waits outside
  ghosts[1] = {{HOUSE_LEFT, HOUSE_ROW, Up, 0}, Mode::InHouse, now, now + 3500, false, false};
  ghosts[2] = {{HOUSE_RIGHT, HOUSE_ROW, Up, 0}, Mode::InHouse, now, now + 8000, false, false};
  ghosts[3] = {{HOUSE_RIGHT, HOUSE_ROW, Up, 0}, Mode::InHouse, now, now + 13000, false, false};
  modeClock = 0;
  scaredUntil = 0;
}

void newLevel(uint32_t now, uint32_t seed) {
  newMaze(seed);
  pillsEaten = 0;
  fruitsShown = 0;
  fruitAt = 0;
  resetActors(now);
  state = State::Ready;
  stateSince = now;
  moveCamera(0, true);
}

// ---- Pac-Man plays by himself ----

struct Search {
  int cost[ROWS][COLS];
  int8_t first[ROWS][COLS];   // the first step of the way there
};

// How far a cell is from the ghosts that hunt him (cells, as the crow flies): near them is expensive
int fear(int c, int r, uint32_t now) {
  int worst = 0;
  for (const Ghost &g : ghosts) {
    if (g.mode == Mode::Scared || g.mode == Mode::Eyes || g.mode == Mode::InHouse) continue;
    const int distance = abs(g.c - c) + abs(g.r - r);
    if (distance <= 1) worst = max(worst, 60);
    else if (distance == 2) worst = max(worst, 18);
    else if (distance == 3) worst = max(worst, 5);
  }
  (void)now;
  return worst;
}

void search(Search &s, uint32_t now) {
  bool done[ROWS][COLS] = {};
  for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) { s.cost[r][c] = 1 << 20; s.first[r][c] = -1; }
  s.cost[pac.r][pac.c] = 0;
  for (int round = 0; round < ROWS * COLS; round++) {
    int bc = -1, br = -1, best = 1 << 20;
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++)
      if (!done[r][c] && s.cost[r][c] < best) { best = s.cost[r][c]; bc = c; br = r; }
    if (bc < 0) break;
    done[br][bc] = true;
    for (int d = 0; d < 4; d++) {
      if (!maze.open(bc, br, d)) continue;
      const int nc = (bc + DX[d] + COLS) % COLS, nr = br + DY[d];
      if (!passable(nc, nr)) continue;
      const int cost = best + 1 + fear(nc, nr, now);
      if (cost < s.cost[nr][nc]) { s.cost[nr][nc] = cost; s.first[nr][nc] = bc == pac.c && br == pac.r ? d : s.first[br][bc]; }
    }
  }
}

int choosePacDir(uint32_t now) {
  static Search s;
  search(s, now);
  const bool powered = now < scaredUntil;
  bool threatened = false;
  for (const Ghost &g : ghosts)
    if (g.mode != Mode::Scared && g.mode != Mode::Eyes && g.mode != Mode::InHouse && abs(g.c - pac.c) + abs(g.r - pac.r) <= 6) threatened = true;
  int bestScore = 1 << 20, bestDir = -1;
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) {
      if (!passable(c, r) || s.first[r][c] < 0) continue;
      int score = s.cost[r][c];
      if (maze.hasPill(c, r)) score += 0; else score += 1 << 12;
      if (maze.hasPower(c, r)) score += threatened && !powered ? -40 : 25;   // saved for when he needs it
      if (fruitAt && c == HOUSE_LEFT && r == HOUSE_ROW + 1) score -= 15;
      if (powered)
        for (const Ghost &g : ghosts)
          if (g.mode == Mode::Scared && g.c == c && g.r == r) score = s.cost[r][c] - 30;
      if (score < bestScore) { bestScore = score; bestDir = s.first[r][c]; }
    }
  if (bestDir < 0) {
    // Nothing left to go for, or boxed in: any open way
    for (int d = 0; d < 4; d++) if (maze.open(pac.c, pac.r, d) && passable((pac.c + DX[d] + COLS) % COLS, pac.r + DY[d])) return d;
  }
  return bestDir < 0 ? pac.dir : bestDir;
}

// ---- The ghosts ----

void scatterTarget(int index, int &tc, int &tr) {
  static const int8_t CORNER[4][2] = {{COLS - 1, 0}, {0, 0}, {COLS - 1, ROWS - 1}, {0, ROWS - 1}};
  tc = CORNER[index][0]; tr = CORNER[index][1];
}

void chaseTarget(int index, int &tc, int &tr) {
  const int ahead = pacLastDir;
  switch (index) {
    case 0: tc = pac.c; tr = pac.r; break;                                                        // Blinky: straight at him
    case 1: tc = pac.c + DX[ahead] * 2; tr = pac.r + DY[ahead] * 2; break;                        // Pinky: where he is going
    case 2: {                                                                                    // Inky: through Blinky, twice as far
      const int pc = pac.c + DX[ahead], pr = pac.r + DY[ahead];
      tc = pc * 2 - ghosts[0].c; tr = pr * 2 - ghosts[0].r;
      break;
    }
    default:                                                                                     // Clyde: shy up close
      if (abs(ghosts[3].c - pac.c) + abs(ghosts[3].r - pac.r) > 4) { tc = pac.c; tr = pac.r; }
      else scatterTarget(3, tc, tr);
  }
}

void chooseGhostDir(int index, uint32_t now) {
  Ghost &g = ghosts[index];
  int options[4], n = 0;
  const int back = (g.dir + 2) % 4;
  for (int d = 0; d < 4; d++) {
    if (d == back && !g.reverse) continue;
    if (!maze.open(g.c, g.r, d)) continue;
    const int nc = (g.c + DX[d] + COLS) % COLS, nr = g.r + DY[d];
    if (nr < 0 || nr >= ROWS) continue;
    if (isHouse(nc, nr)) continue;   // the way in is only for eyes, handled apart
    options[n++] = d;
  }
  if (g.reverse && n) {
    g.reverse = false;
    for (int i = 0; i < n; i++) if (options[i] == back) { g.dir = back; return; }
  }
  g.reverse = false;
  if (!n) { g.dir = back; return; }
  if (g.mode == Mode::Scared) { g.dir = options[rnd() % n]; return; }
  int tc, tr;
  if (g.mode == Mode::Scatter) scatterTarget(index, tc, tr);
  else chaseTarget(index, tc, tr);
  int best = -1, bestDistance = 1 << 20;
  for (int i = 0; i < n; i++) {   // the order up, left, down, right breaks ties, as in the arcade
    static const int8_t ORDER[4] = {Up, Left, Down, Right};
    (void)ORDER;
  }
  static const int8_t ORDER[4] = {Up, Left, Down, Right};
  for (int k = 0; k < 4; k++)
    for (int i = 0; i < n; i++) {
      if (options[i] != ORDER[k]) continue;
      const int nc = (g.c + DX[options[i]] + COLS) % COLS, nr = g.r + DY[options[i]];
      const int distance = (nc - tc) * (nc - tc) + (nr - tr) * (nr - tr);
      if (distance < bestDistance) { bestDistance = distance; best = options[i]; }
    }
  g.dir = best;
  (void)now;
}

void startScare(uint32_t now) {
  scaredUntil = now + 7000;
  eatenCount = 0;
  for (Ghost &g : ghosts) {
    g.eaten = false;
    if (g.mode == Mode::Scatter || g.mode == Mode::Chase) { g.mode = Mode::Scared; g.reverse = true; }
  }
}

void endScare() {
  for (Ghost &g : ghosts) if (g.mode == Mode::Scared) g.mode = (modeClock / 1000) % 2 == 0 ? Mode::Scatter : Mode::Chase;
}

int scheduleMode(uint32_t clockMs) {   // 0 scatter, 1 chase
  uint32_t t = clockMs / 1000;
  for (int i = 0; i < 7; i++) {
    if (t < kSchedule[i]) return i % 2;
    t -= kSchedule[i];
  }
  return 1;
}

float ghostSpeed(const Ghost &g) {
  if (g.mode == Mode::Eyes) return kEyesSpeed;
  if (g.mode == Mode::Scared) return kScaredSpeed;
  return kGhostSpeed;
}

void moveGhost(int index, float dt, uint32_t now) {
  Ghost &g = ghosts[index];
  if (g.mode == Mode::InHouse) {
    if (now >= g.releaseAt && now >= g.since + 1200) { g.mode = Mode::Leaving; g.dir = Up; g.prog = 0; }
    return;
  }
  g.prog += ghostSpeed(g) * dt;
  while (g.prog >= PITCH) {
    g.prog -= PITCH;
    g.c = (g.c + DX[g.dir] + COLS) % COLS;
    g.r += DY[g.dir];
    if (g.mode == Mode::Leaving) {   // out of the door: hunting
      g.mode = (modeClock / 1000) % 2 == 0 && scheduleMode(modeClock) == 0 ? Mode::Scatter : Mode::Chase;
      if (now < scaredUntil && !g.eaten) g.mode = Mode::Scared;
      g.dir = g.c <= COLS / 2 - 1 ? Left : Right;
      chooseGhostDir(index, now);
      continue;
    }
    if (g.mode == Mode::Eyes) {
      if (isHouse(g.c, g.r)) { g.mode = Mode::InHouse; g.since = now; g.prog = 0; g.dir = Up; g.eaten = true; break; }
      const int doorCol = g.c <= COLS / 2 - 1 ? HOUSE_LEFT : HOUSE_RIGHT;
      if (g.r == HOUSE_ROW - 1 && g.c == doorCol) { g.dir = Down; continue; }
      // Home by the shortest way: towards the door
      int best = -1, bestDistance = 1 << 20;
      for (int d = 0; d < 4; d++) {
        if (!maze.open(g.c, g.r, d)) continue;
        const int nc = (g.c + DX[d] + COLS) % COLS, nr = g.r + DY[d];
        if (!passable(nc, nr)) continue;
        const int distance = (nc - doorCol) * (nc - doorCol) * 2 + (nr - (HOUSE_ROW - 1)) * (nr - (HOUSE_ROW - 1)) * 2 + (d == (g.dir + 2) % 4 ? 3 : 0);
        if (distance < bestDistance) { bestDistance = distance; best = d; }
      }
      g.dir = best < 0 ? g.dir : best;
      continue;
    }
    // A mode change turns the ghost round at the next junction, as in the arcade
    if (g.mode == Mode::Scatter && scheduleMode(modeClock) == 1) { g.mode = Mode::Chase; g.reverse = true; }
    else if (g.mode == Mode::Chase && scheduleMode(modeClock) == 0) { g.mode = Mode::Scatter; g.reverse = true; }
    chooseGhostDir(index, now);
  }
}

void movePac(float dt, uint32_t now) {
  pac.prog += kPacSpeed * dt;
  while (pac.prog >= PITCH) {
    pac.prog -= PITCH;
    pac.c = (pac.c + DX[pac.dir] + COLS) % COLS;
    pac.r += DY[pac.dir];
    pacLastDir = pac.dir;
    if (maze.hasPill(pac.c, pac.r) || maze.hasPower(pac.c, pac.r)) {
      const bool power = maze.hasPower(pac.c, pac.r);
      maze.eat(pac.c, pac.r);
      pillsEaten++;
      if (power) startScare(now);
    }
    if (fruitAt && pac.c == HOUSE_LEFT && pac.r == HOUSE_ROW + 1) {
      fruitAt = 0;
      popupAt = now; popupScore = 100 * (level > 5 ? 5 : level); popupX = PITCH * pac.c; popupY = PITCH * pac.r - 4;
    }
    pac.dir = choosePacDir(now);
  }
}

bool touches(const Actor &a, const Actor &b) {
  return abs(pixelX(a) - pixelX(b)) < 5 && abs(pixelY(a) - pixelY(b)) < 5;
}

// ---- Drawing ----

// The camera follows Pac-Man, eased, and stops at the edges of the maze; it jumps when he goes through the tunnel
void moveCamera(float dt, bool snap) {
  const float px = PITCH * pac.c + 4.5f + DX[pac.dir] * pac.prog, py = PITCH * pac.r + 4.5f + DY[pac.dir] * pac.prog;
  float tx = px - W / 2.0f, ty = py - VIEW_H / 2.0f;
  tx = tx < 0 ? 0 : tx > MAZE_W - W ? MAZE_W - W : tx;
  ty = ty < 0 ? 0 : ty > MAZE_H - VIEW_H ? MAZE_H - VIEW_H : ty;
  if (snap || fabsf(tx - camX) > 50) { camX = tx; camY = ty; }
  else { const float k = dt * 4 > 1 ? 1 : dt * 4; camX += (tx - camX) * k; camY += (ty - camY) * k; }
  OX = -(int)(camX + 0.5f);
  OY = VIEW_TOP - (int)(camY + 0.5f);
}

const uint16_t kGhostColor[4] = {rgb(255, 20, 20), rgb(255, 184, 255), rgb(0, 240, 255), rgb(255, 184, 82)};

void drawMaze(MatrixPanel_I2S_DMA &d, bool flash) {
  const uint16_t wall = flash ? kWallFlash : kWall;
  for (int r = 0; r <= ROWS; r++)            // horizontal lines at the top of row r
    for (int c = 0; c < COLS; c++) {
      if (r == HOUSE_ROW && (c == HOUSE_LEFT || c == HOUSE_RIGHT)) { box(d, OX + PITCH * c, OY + PITCH * r, PITCH + 2, 2, kDoor); continue; }
      const bool wallHere = r == 0 || r == ROWS || !(maze.down[r - 1] >> c & 1);
      if (wallHere) box(d, OX + PITCH * c, OY + PITCH * r, PITCH + 2, 2, wall);
    }
  for (int r = 0; r < ROWS; r++)             // vertical lines at the left of column c
    for (int c = 0; c <= COLS; c++) {
      bool wallHere;
      if (c == 0 || c == COLS) wallHere = r != maze.tunnelRow;
      else wallHere = !(maze.right[r] >> (c - 1) & 1);
      if (r == HOUSE_ROW && (c == HOUSE_LEFT || c == HOUSE_RIGHT + 1)) wallHere = true;   // the walls of the house
      if (r == HOUSE_ROW && c == HOUSE_RIGHT) wallHere = false;
      if (wallHere) box(d, OX + PITCH * c, OY + PITCH * r, 2, PITCH + 2, wall);
    }
  box(d, OX + PITCH * HOUSE_LEFT, OY + PITCH * (HOUSE_ROW + 1), PITCH * 2 + 2, 2, wall);   // the floor of the house
}

void drawPills(MatrixPanel_I2S_DMA &d, uint32_t now) {
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) {
      const int x = OX + PITCH * c + 2, y = OY + PITCH * r + 2;
      if (maze.hasPower(c, r)) { if ((now / 260) % 2 == 0) box(d, x, y + 1, 5, 3, kPill), box(d, x + 1, y, 3, 5, kPill); }
      else if (maze.hasPill(c, r)) put(d, x + 2, y + 2, kPill);
    }
}

void drawPac(MatrixPanel_I2S_DMA &d, int x, int y, int dir, int open) {
  // open: 0 closed, 1 a little, 2 wide, 3+ the death animation
  const float dx = DX[dir], dy = DY[dir];
  for (int py = 0; py < 5; py++)
    for (int px = 0; px < 5; px++) {
      const float ux = px - 2.0f, uy = py - 2.0f;
      if (ux * ux + uy * uy > 5.5f) continue;
      const float forward = ux * dx + uy * dy, side = fabsf(ux * dy - uy * dx);
      if (open >= 3) { if (forward > 0 || (forward > -open * 0.3f + 0.6f && side < open * 0.45f)) continue; }
      else if (open == 1 && forward > 0 && side < forward * 0.5f + 0.1f) continue;
      else if (open == 2 && forward > 0 && side < forward * 1.2f + 0.1f) continue;
      put(d, x + px, y + py, kPacman);
    }
}

void drawGhost(MatrixPanel_I2S_DMA &d, int x, int y, int index, const Ghost &g, uint32_t now) {
  const int foot = (now / 180) % 2;
  const bool eyes = g.mode == Mode::Eyes;
  const bool scared = g.mode == Mode::Scared;
  const bool flashing = scared && scaredUntil > now && scaredUntil - now < 2000 && (now / 200) % 2;
  const uint16_t body = scared ? (flashing ? rgb(240, 240, 255) : rgb(33, 33, 255)) : kGhostColor[index];
  if (!eyes) {
    for (int i = 1; i < 4; i++) put(d, x + i, y, body);                    // the dome
    for (int row = 1; row < 4; row++) for (int i = 0; i < 5; i++) put(d, x + i, y + row, body);
    for (int i = 0; i < 5; i++) if ((i + foot) % 2 == 0) put(d, x + i, y + 4, body);   // the feet
  }
  if (scared) {
    const uint16_t face = flashing ? rgb(255, 40, 40) : rgb(255, 184, 174);
    put(d, x + 1, y + 1, face); put(d, x + 3, y + 1, face);
    for (int i = 0; i < 5; i++) if (i % 2 == 0) put(d, x + i, y + 3, face);   // the zigzag mouth
    return;
  }
  // Two eyes, one pixel wide and two high; the pupil says where he is going
  const uint16_t white = rgb(255, 255, 255), pupil = rgb(30, 60, 255);
  const int shift = g.dir == Left ? -1 : g.dir == Right ? 1 : 0;
  for (int eye = 0; eye < 2; eye++) {
    const int ex = x + 1 + eye * 2 + shift;
    put(d, ex, y + 1, white); put(d, ex, y + 2, white);
    put(d, ex, g.dir == Up ? y + 1 : y + 2, pupil);
  }
}

void drawFruit(MatrixPanel_I2S_DMA &d, int c, int r, int kind) {
  const int x = OX + PITCH * c + 2, y = OY + PITCH * r + 2;
  static const uint16_t FRUIT[4] = {rgb(240, 30, 40), rgb(255, 120, 30), rgb(240, 60, 120), rgb(200, 40, 40)};
  box(d, x, y + 1, 4, 4, FRUIT[kind & 3]);
  put(d, x + 2, y, rgb(60, 220, 60)); put(d, x + 3, y, rgb(60, 220, 60));
  put(d, x, y + 1, rgb(255, 255, 255));
}

}  // namespace

Clockface::Clockface(MatrixPanel_I2S_DMA* display) { _display = display; }

void Clockface::setup(ClockTime *dateTime) {
  _dateTime = dateTime;
  const int digits[4] = {dateTime->getHour() / 10, dateTime->getHour() % 10, dateTime->getMinute() / 10, dateTime->getMinute() % 10};
  for (int i = 0; i < 4; i++) slots[i] = {(int8_t)digits[i], (int8_t)digits[i], 0};
  rng = hash(millis() * 2654435761u + dateTime->getMinute() * 977 + dateTime->getHour() * 61 + 1) | 1;
  level = 1;
  lives = 3;
  popupAt = 0;
  newLevel(millis(), rnd());
  lastNow = millis();
}

void Clockface::update() {
  MatrixPanel_I2S_DMA &d = *_display;
  const uint32_t now = millis();
  float dt = (now - lastNow) / 1000.0f;
  if (dt > 0.2f) dt = 0.2f;
  lastNow = now;
  const int hour = _dateTime->getHour(), minute = _dateTime->getMinute();
  const int digits[4] = {hour / 10, hour % 10, minute / 10, minute % 10};
  for (int i = 0; i < 4; i++)
    if (digits[i] != slots[i].digit) slots[i] = {(int8_t)digits[i], slots[i].digit, now};

  // ---- The game moves on ----
  bool flash = false;
  switch (state) {
    case State::Ready:
      if (now - stateSince > 1800) { state = State::Playing; stateSince = now; pac.dir = choosePacDir(now); }
      break;
    case State::Playing: {
      modeClock += (uint32_t)(dt * 1000);
      if (scaredUntil && now >= scaredUntil) { scaredUntil = 0; endScare(); }
      movePac(dt, now);
      for (int i = 0; i < 4; i++) moveGhost(i, dt, now);
      if (!fruitAt && ((fruitsShown == 0 && pillsEaten >= 35) || (fruitsShown == 1 && pillsEaten >= 80))) { fruitAt = now; fruitsShown++; }
      if (fruitAt && now - fruitAt > 12000) fruitAt = 0;
      for (int i = 0; i < 4; i++) {
        Ghost &g = ghosts[i];
        if (g.mode == Mode::InHouse || g.mode == Mode::Leaving || g.mode == Mode::Eyes || !touches(pac, g)) continue;
        if (g.mode == Mode::Scared) {
          g.mode = Mode::Eyes;
          eatenCount++;
          popupAt = now; popupScore = 100 << eatenCount; popupX = pixelX(g) - 2 - OX; popupY = pixelY(g) - 6 - OY;
        } else {
          state = State::Dying; stateSince = now;
        }
      }
      if (maze.pills <= 0) { state = State::Cleared; stateSince = now; }
      break;
    }
    case State::Dying:
      if (now - stateSince > 1700) {
        if (--lives <= 0) { lives = 3; level = 1; newLevel(now, rnd()); }
        else { resetActors(now); state = State::Ready; stateSince = now; }
      }
      break;
    case State::Cleared:
      flash = ((now - stateSince) / 220) % 2 == 0;
      if (now - stateSince > 2000) { level++; newLevel(now, rnd()); }
      break;
  }

  // ---- Picture ----
  moveCamera(dt, false);
  d.fillScreen(kBackground);
  drawMaze(d, flash);
  if (state != State::Cleared) drawPills(d, now);
  if (fruitAt) drawFruit(d, HOUSE_LEFT, HOUSE_ROW + 1, level - 1);

  const bool showActors = state != State::Cleared;
  if (showActors) {
    for (int i = 0; i < 4; i++) {
      const Ghost &g = ghosts[i];
      int x = pixelX(g), y = pixelY(g);
      if (g.mode == Mode::InHouse) { x = OX + PITCH * g.c + 2; y = OY + PITCH * g.r + 2 + (int)(((now / 400) + i) % 2); }
      if (state == State::Dying && now - stateSince > 700) continue;
      drawGhost(d, x, y, i, g, now);
    }
    if (state == State::Dying) {
      const int t = (int)(now - stateSince);
      if (t > 500 && t < 1600) drawPac(d, pixelX(pac), pixelY(pac), Up, 3 + (t - 500) / 140);
      else if (t <= 500) drawPac(d, pixelX(pac), pixelY(pac), pac.dir, 0);
    } else {
      static const uint8_t MOUTH[4] = {2, 1, 0, 1};
      drawPac(d, pixelX(pac), pixelY(pac), pac.dir, state == State::Ready ? 0 : MOUTH[(now / 110) % 4]);
    }
  }
  if (popupAt && now - popupAt < 900) {
    char text[8];
    snprintf(text, sizeof(text), "%d", popupScore);
    int x = popupX + OX;
    if (x + miniWidth(text) > W - 1) x = W - 1 - miniWidth(text);
    if (x < 0) x = 0;
    miniText(d, text, x, popupY + OY, rgb(60, 230, 255));
  }
  // The time sits on top of the maze's window
  box(d, 0, 0, W, VIEW_TOP, kBackground);
  for (int i = 0; i < 4; i++) {
    if (i == 0 && !_dateTime->is24hFormat() && hour < 10 && slots[0].digit == 0 && now - slots[0].since >= kChangeMs) continue;
    drawDigit(d, i, i < 2 ? kHours : kMinutes, now);
  }
  {  // the colon: two power pellets
    const uint16_t c = dim(kHours, 150 + triangle(now / 4) * 105 / 255);
    for (int i = 0; i < 2; i++) box(d, 31, kDigitTop + (i ? 6 : 2), 2, 2, c);
  }
  // The lives, as Pac-Man in the corner of the maze's bottom wall
  d.flipDMABuffer();
}
