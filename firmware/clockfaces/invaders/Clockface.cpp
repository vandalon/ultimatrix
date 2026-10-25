// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
// A self-playing Space Invaders squeezed onto 64 x 64 pixels. The time is in a band along the top; the mystery ship flies
// by every minute. Play it yourself from the settings page (or a game controller): the computer takes over again after a while.
#include "Clockface.h"
#include <FaceKit.h>
#include <MiniFont.h>
#include <BoldFont.h>
#include <Settings.h>
#include <PacmanColors.h>

using namespace facekit;

namespace {

const int COLS = 5, ROWS = 3, PITCH_X = 10, PITCH_Y = 7, INV_W = 7, INV_H = 5;
const int HUD_LINE = 14, UFO_Y = 16, UFO_W = 7, TOP_Y = 22;   // the top 14 rows are the clock and the score
const int SHIELDS = 4, SHIELD_W = 10, SHIELD_H = 6, SHIELD_Y = 49;
const int CANNON_Y = 60, CANNON_W = 7, CANNON_H = 4, CANNON_MAX = 57;
const int INVADED_Y = 55;                                   // an invader on this row has reached the cannon
const int START_LIVES = 3, EXTRA_LIFE_EVERY = 1500;
const unsigned long MANUAL_MS = 20000, DEATH_MS = 1400, CLEAR_MS = 1800, OVER_MS = 4000, READY_MS = 1200;
const unsigned long PLAYER_MS = 28, BULLET_MS = 6, BOMB_MS = 30, UFO_MS = 40;
const int MAX_BOMBS = 3, MAX_FX = 8;

const char *const SQUID_A[INV_H] = {"..###..", ".#####.", "##.#.##", ".#####.", ".#.#.#."};
const char *const SQUID_B[INV_H] = {"..###..", ".#####.", "##.#.##", ".#####.", "#.....#"};
const char *const CRAB_A[INV_H] = {"..#.#..", ".#####.", "##.#.##", "#######", "#.#.#.#"};
const char *const CRAB_B[INV_H] = {"..#.#..", "#.###.#", "#######", "##.#.##", ".#...#."};
const char *const OCTO_A[INV_H] = {".#####.", "#######", "##.#.##", "#######", ".#.#.#."};
const char *const OCTO_B[INV_H] = {".#####.", "#######", "##.#.##", "#######", "#.....#"};
const char *const BURST[INV_H] = {"#..#..#", ".#.#.#.", "..###..", ".#.#.#.", "#..#..#"};
const char *const UFO_ROWS[3] = {"..###..", ".#####.", "o.o.o.o"};
const char *const CANNON[CANNON_H] = {"...#...", "..###..", ".#####.", "#######"};
const char *const SHIELD_SHAPE[SHIELD_H] = {"..######..", ".########.", "##########", "##########", "###....###", "###....###"};

const uint16_t kSquid = rgb(255, 80, 200), kCrab = rgb(60, 220, 255), kOcto = rgb(80, 255, 90), kUfo = rgb(255, 40, 40), kUfoLights = rgb(255, 230, 60);
const uint16_t kShield = rgb(40, 200, 60), kCannon = rgb(150, 255, 150), kBomb = rgb(255, 230, 90), kBullet = rgb(255, 255, 255);
const uint16_t kWhite = rgb(255, 255, 255), kRed = rgb(255, 60, 40), kHud = rgb(120, 130, 150);
const uint16_t kLevel = rgb(66, 80, 140), kClock = rgb(120, 185, 255), kClockFlash = rgb(255, 255, 255);

struct Skill { const char *name; unsigned long react; int dodge; int aim; unsigned long fireGap; };
const Skill SKILLS[4] = {{"novice", 420, 3, 3, 700}, {"casual", 300, 4, 2, 450}, {"good", 180, 4, 2, 300}, {"expert", 90, 5, 1, 220}};

enum Phase : uint8_t { READY, PLAYING, DYING, CLEARED, OVER };
struct Bomb { bool on; int x, y; uint8_t kind; };
struct Burst { int x, y; unsigned long until; };

Phase phase = READY;
unsigned long phaseUntil = 0;
bool alive[ROWS][COLS];
int aliveCount, formX, formY, formDir, level, lives, px;
bool frameB;
long score, hiScore;
unsigned long nextMarch, nextPlayer, nextBullet, nextBomb, nextDrop, nextUfo, nextFire, manualUntil, threatSince, flashUntil, lastNow;
bool bulletOn, ufoOn, ufoPending, ufoBig, autoFire, fireOnce, wasManual, slideOn;
int slideTarget, bulletX, bulletY, ufoX, ufoDir, ufoPoints, moveDir, lastMinute, lastHour, aiMove, skill, extraLifeAt;
Bomb bombs[MAX_BOMBS];
Burst bursts[MAX_FX];
uint8_t shield[SHIELDS][SHIELD_H][SHIELD_W];
long popup;                  // points shown where the mystery ship was
int popupX;
unsigned long popupUntil;

bool manual() { return millis() < manualUntil; }
int shieldX(int s) { return 4 + s * 15; }
const char *const *invaderSprite(int row) {
  if (row == 0) return frameB ? SQUID_B : SQUID_A;
  if (row == ROWS - 1) return frameB ? OCTO_B : OCTO_A;
  return frameB ? CRAB_B : CRAB_A;
}
uint16_t invaderColour(int row) { return row == 0 ? kSquid : row == ROWS - 1 ? kOcto : kCrab; }
int rowPoints(int row) { return row == 0 ? 30 : row == ROWS - 1 ? 10 : 20; }

void addBurst(int x, int y, unsigned long ms) {
  for (int i = 0; i < MAX_FX; i++)
    if (bursts[i].until <= millis()) { bursts[i] = {x, y, millis() + ms}; return; }
}

void addScore(long points) {
  score += points;
  if (score > hiScore) hiScore = score;
  if (score >= extraLifeAt) { extraLifeAt += EXTRA_LIFE_EVERY; if (lives < 4) lives++; }
}

void buildShields() {
  for (int s = 0; s < SHIELDS; s++)
    for (int y = 0; y < SHIELD_H; y++)
      for (int x = 0; x < SHIELD_W; x++) shield[s][y][x] = SHIELD_SHAPE[y][x] == '#';
}

// A shield pixel at panel position (x, y), if one is standing there
uint8_t *shieldAt(int x, int y) {
  if (y < SHIELD_Y || y >= SHIELD_Y + SHIELD_H) return nullptr;
  for (int s = 0; s < SHIELDS; s++) {
    const int sx = x - shieldX(s);
    if (sx >= 0 && sx < SHIELD_W && shield[s][y - SHIELD_Y][sx]) return &shield[s][y - SHIELD_Y][sx];
  }
  return nullptr;
}

// A hit eats a ragged hole: the pixel itself and a random part of its neighbours
void erode(int x, int y) {
  for (int dy = -1; dy <= 1; dy++)
    for (int dx = -1; dx <= 1; dx++) {
      if ((dx || dy) && random(100) < (dx && dy ? 65 : 35)) continue;
      uint8_t *cell = shieldAt(x + dx, y + dy);
      if (cell) *cell = 0;
    }
}

int lowestRow() {
  for (int r = ROWS - 1; r >= 0; r--)
    for (int c = 0; c < COLS; c++)
      if (alive[r][c]) return r;
  return -1;
}
int leftCol() { for (int c = 0; c < COLS; c++) for (int r = 0; r < ROWS; r++) if (alive[r][c]) return c; return 0; }
int rightCol() { for (int c = COLS - 1; c >= 0; c--) for (int r = 0; r < ROWS; r++) if (alive[r][c]) return c; return 0; }
int invaderX(int c) { return formX + c * PITCH_X; }
int invaderY(int r) { return formY + r * PITCH_Y; }

unsigned long marchMs() {
  unsigned long ms = 30 + aliveCount * 12;
  ms = ms * 100 / (100 + (level - 1) * 12);
  return ms < 16 ? 16 : ms;
}
unsigned long dropMs() {
  long ms = 1100 - level * 110 - (COLS * ROWS - aliveCount) * 14;
  if (ms < 260) ms = 260;
  return ms / 2 + random(ms);
}

void startWave() {
  for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) alive[r][c] = true;
  aliveCount = ROWS * COLS;
  formX = 8; formDir = 1; frameB = false;
  formY = TOP_Y + (level - 1 > 4 ? 4 : level - 1) * 2;
  for (int i = 0; i < MAX_BOMBS; i++) bombs[i].on = false;
  bulletOn = ufoOn = false;
  const unsigned long now = millis();
  nextMarch = now + 600; nextBomb = now; nextDrop = now + 1500; nextBullet = now; nextPlayer = now; nextUfo = now; nextFire = now;
  buildShields();
  px = 28; moveDir = 0; threatSince = 0; aiMove = 0;
  phase = READY; phaseUntil = now + READY_MS;
}

void newGame() {
  level = 1; lives = START_LIVES; score = 0; extraLifeAt = EXTRA_LIFE_EVERY; autoFire = false;
  skill = random(4);
  ufoPending = false; popupUntil = 0;
  for (int i = 0; i < MAX_FX; i++) bursts[i].until = 0;
  startWave();
  Serial.printf("[Invaders] new game, %s\n", SKILLS[skill].name);
}

void loseLife() {
  phase = DYING; phaseUntil = millis() + DEATH_MS;
  bulletOn = false;
  for (int i = 0; i < MAX_BOMBS; i++) bombs[i].on = false;
  lives--;
  Serial.printf("[Invaders] hit on level %d, score %ld, %d lives left\n", level, score, lives);
}

// ---- the game ----

void killInvader(int r, int c) {
  alive[r][c] = false; aliveCount--;
  addBurst(invaderX(c), invaderY(r), 160);
  addScore(rowPoints(r));
}

void spawnUfo() {
  ufoOn = true; ufoPending = false;
  ufoDir = random(2) ? 1 : -1;
  ufoX = ufoDir > 0 ? -UFO_W : 64;
  static const int PAID[4] = {50, 100, 150, 300};
  ufoPoints = ufoBig ? 300 : PAID[random(4)];
  nextUfo = millis();
}

void marchFormation() {
  const int left = invaderX(leftCol()), right = invaderX(rightCol()) + INV_W;
  if ((formDir > 0 && right >= 63) || (formDir < 0 && left <= 1)) { formDir = -formDir; formY += 2; }
  else formX += formDir;
  frameB = !frameB;
  // the invaders eat the shields they march over
  const int lowY = invaderY(lowestRow()) + INV_H;
  if (lowY > SHIELD_Y)
    for (int s = 0; s < SHIELDS; s++)
      for (int y = 0; y < SHIELD_H; y++)
        for (int x = 0; x < SHIELD_W; x++)
          if (shield[s][y][x]) {
            const int sx = shieldX(s) + x, sy = SHIELD_Y + y;
            for (int r = 0; r < ROWS; r++)
              for (int c = 0; c < COLS; c++)
                if (alive[r][c] && sx >= invaderX(c) && sx < invaderX(c) + INV_W && sy >= invaderY(r) && sy < invaderY(r) + INV_H) shield[s][y][x] = 0;
          }
  if (invaderY(lowestRow()) + INV_H - 1 >= INVADED_Y) { addBurst(px, CANNON_Y, 600); loseLife(); }
}

void dropBomb() {
  if (aliveCount == 0) return;
  for (int i = 0; i < MAX_BOMBS; i++) {
    if (bombs[i].on) continue;
    // a column: often the one over the cannon, otherwise any
    int col = random(COLS);
    if (random(100) < 35) {
      int best = 99;
      for (int c = 0; c < COLS; c++) {
        const int d = abs(invaderX(c) + 3 - (px + 3));
        bool any = false;
        for (int r = 0; r < ROWS; r++) any |= alive[r][c];
        if (any && d < best) { best = d; col = c; }
      }
    }
    for (int r = ROWS - 1; r >= 0; r--)
      if (alive[r][col]) { bombs[i] = {true, invaderX(col) + 3, invaderY(r) + INV_H, (uint8_t)random(2)}; return; }
    return;
  }
}

void stepBullet() {
  bulletY--;
  if (bulletY < HUD_LINE + 1) { bulletOn = false; return; }
  // the mystery ship
  if (ufoOn && bulletY <= UFO_Y + 2 && bulletX >= ufoX && bulletX < ufoX + UFO_W) {
    addBurst(ufoX, UFO_Y - 1, 300);
    addScore(ufoPoints); popup = ufoPoints; popupX = ufoX; popupUntil = millis() + 1200;
    flashUntil = millis() + 700; ufoOn = false; bulletOn = false;
    return;
  }
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++)
      if (alive[r][c] && bulletX >= invaderX(c) && bulletX < invaderX(c) + INV_W && bulletY >= invaderY(r) && bulletY < invaderY(r) + INV_H) {
        killInvader(r, c); bulletOn = false; return;
      }
  if (shieldAt(bulletX, bulletY)) { erode(bulletX, bulletY); bulletOn = false; return; }
  for (int i = 0; i < MAX_BOMBS; i++)   // a shot and a bomb cancel each other out
    if (bombs[i].on && bulletX >= bombs[i].x && bulletX < bombs[i].x + 3 && bulletY >= bombs[i].y && bulletY < bombs[i].y + 3) {
      bombs[i].on = false; bulletOn = false; addBurst(bombs[i].x - 2, bombs[i].y - 1, 120); return;
    }
}

void stepBombs() {
  for (int i = 0; i < MAX_BOMBS; i++) {
    Bomb &b = bombs[i];
    if (!b.on) continue;
    b.y++;
    if (b.y >= 62) { b.on = false; continue; }
    if (shieldAt(b.x + 1, b.y + 2)) { erode(b.x + 1, b.y + 2); b.on = false; continue; }
    if (b.y + 2 >= CANNON_Y && b.x + 2 >= px && b.x <= px + CANNON_W - 1) { b.on = false; addBurst(px, CANNON_Y, 600); loseLife(); return; }
  }
}

// ---- the computer's turn ----

bool threatened(int x, int dodge) {
  for (int i = 0; i < MAX_BOMBS; i++)
    if (bombs[i].on && bombs[i].y >= 30 && abs(bombs[i].x + 1 - (x + 3)) <= dodge) return true;
  return false;
}

// Returns -1, 0 or 1 to move and sets fire
int think(bool &fire) {
  const Skill &sk = SKILLS[skill];
  const unsigned long now = millis();
  fire = false;
  if (threatened(px, sk.dodge)) {
    if (!threatSince) threatSince = now;
    if (now - threatSince >= sk.react) {
      for (int d = 1; d <= 20; d++) {
        const int first = px < 29 ? 1 : -1;   // towards the middle first
        for (int k = 0; k < 2; k++) {
          const int x = px + (k ? -first : first) * d;
          if (x < 0 || x > CANNON_MAX) continue;
          if (!threatened(x, sk.dodge)) return x > px ? 1 : -1;
        }
      }
    }
  } else threatSince = 0;

  // aim: the mystery ship if it is flying, else the invader column nearest to the cannon (the lowest ones first when they are close)
  int target = -1;
  if (ufoOn && ufoX > 0 && ufoX < 57) target = ufoX + 3 + ufoDir * 8;   // lead the ship
  else {
    int best = 999;
    for (int c = 0; c < COLS; c++) {
      int low = -1;
      for (int r = ROWS - 1; r >= 0; r--) if (alive[r][c]) { low = r; break; }
      if (low < 0) continue;
      const int score = abs(invaderX(c) + 3 - (px + 3)) - low * 3 * (invaderY(low) > 34 ? 3 : 1);
      if (score < best) { best = score; target = invaderX(c) + 3 + formDir; }
    }
  }
  if (target < 0) return 0;
  const int off = target - (px + 3);
  if (abs(off) <= sk.aim) { fire = !bulletOn && now >= nextFire; return 0; }
  return off > 0 ? 1 : -1;
}

void stepPlayer() {
  int move = 0;
  bool fire = false;
  if (manual()) {
    move = moveDir; fire = (autoFire || fireOnce) && !bulletOn; fireOnce = false;
    if (slideOn) {   // the finger sets where the cannon goes: 1 px per step, 2 when it is far off
      const int off = slideTarget - px;
      move = off == 0 ? 0 : (abs(off) >= 4 ? 2 : 1) * (off > 0 ? 1 : -1);
      if (abs(move) > abs(off)) move = off;
    }
  }
  else { move = think(fire); }
  px += move;
  if (px < 0) px = 0;
  if (px > CANNON_MAX) px = CANNON_MAX;
  if (fire && !bulletOn) {
    bulletOn = true; bulletX = px + 3; bulletY = CANNON_Y - 1; nextBullet = millis() + BULLET_MS;
    nextFire = millis() + (manual() ? 0 : SKILLS[skill].fireGap + random(150));
  }
}

void runGame(ClockTime *time) {
  const unsigned long now = millis();
  if (wasManual != manual()) { wasManual = manual(); newGame(); }   // a fresh game whenever you take over or give it back
  if (lastNow && now - lastNow > 400) {   // the panel was used by something else: carry on from here, not from the past
    const unsigned long gap = now - lastNow;
    nextMarch += gap; nextBullet += gap; nextBomb += gap; nextDrop += gap; nextPlayer += gap; nextUfo += gap; nextFire += gap; phaseUntil += gap;
  }
  lastNow = now;

  const int minute = time->getMinute(), hour = time->getHour();
  if (lastMinute >= 0 && minute != lastMinute) { ufoPending = true; ufoBig = hour != lastHour; }
  lastMinute = minute; lastHour = hour;

  if (phase == READY) { if (now >= phaseUntil) phase = PLAYING; else return; }
  if (phase == DYING) {
    if (now < phaseUntil) return;
    if (lives <= 0) { phase = OVER; phaseUntil = now + OVER_MS; Serial.printf("[Invaders] game over on level %d, score %ld, %s\n", level, score, SKILLS[skill].name); return; }
    for (int i = 0; i < MAX_BOMBS; i++) bombs[i].on = false;
    // an invaded game carries on one row higher
    if (invaderY(lowestRow()) + INV_H - 1 >= INVADED_Y - 6) formY -= 8;
    px = 28; moveDir = 0;
    phase = PLAYING; nextDrop = now + 1200; nextMarch = now + 300;
    return;
  }
  if (phase == CLEARED) {
    if (now < phaseUntil) return;
    level++;
    startWave();
    return;
  }
  if (phase == OVER) {
    if (now >= phaseUntil) newGame();
    return;
  }

  if (aliveCount == 0) { phase = CLEARED; phaseUntil = now + CLEAR_MS; Serial.printf("[Invaders] wave %d cleared, score %ld\n", level, score); return; }

  int guard = 0;
  while (now >= nextPlayer && guard++ < 8) { nextPlayer += PLAYER_MS; stepPlayer(); }
  if (nextPlayer + 200 < now) nextPlayer = now;
  guard = 0;
  while (bulletOn && now >= nextBullet && guard++ < 16) { nextBullet += BULLET_MS; stepBullet(); }
  if (nextBullet + 200 < now) nextBullet = now;
  if (phase != PLAYING) return;
  guard = 0;
  while (now >= nextBomb && guard++ < 4) { nextBomb += BOMB_MS; stepBombs(); if (phase != PLAYING) return; }
  if (nextBomb + 200 < now) nextBomb = now;
  guard = 0;
  while (aliveCount > 0 && now >= nextMarch && guard++ < 4) { nextMarch += marchMs(); marchFormation(); if (phase != PLAYING) return; }
  if (nextMarch + 400 < now) nextMarch = now;
  if (now >= nextDrop) { nextDrop = now + dropMs(); dropBomb(); }
  if (ufoOn) {
    guard = 0;
    while (now >= nextUfo && guard++ < 4) {
      nextUfo += UFO_MS; ufoX += ufoDir;
      if (ufoX < -UFO_W - 1 || ufoX > 64) { ufoOn = false; break; }
    }
  } else if (ufoPending && aliveCount > 3) spawnUfo();
}

// ---- the picture ----

// A clock digit of 6 x 12 pixels in the block font of Pong and the football face
void bigDigit(MatrixPanel_I2S_DMA &d, int value, int x, int y, uint16_t colour) { blockDigit(d, value, x, y, 6, 12, colour); }

// The time in the top band, on the left: white for a moment when the mystery ship is hit
void drawClock(MatrixPanel_I2S_DMA &d, ClockTime *time) {
  Settings *st = Settings::getInstance();
  const bool flash = millis() < flashUntil;
  const uint16_t colour = flash ? kClockFlash : st->arcHours ? pacmancolors::sprite(st->arcHours) : kClock;
  const uint16_t minutesColour = flash ? kClockFlash : st->arcMinutes ? pacmancolors::sprite(st->arcMinutes) : kClock;
  const int hour = time->getHour(), minute = time->getMinute();
  const int x0 = 1, y0 = 1;   // the clock takes columns 1..31, the lives start at 33 when there are four
  if (time->is24hFormat() || hour >= 10) bigDigit(d, hour / 10, x0, y0, colour);
  bigDigit(d, hour % 10, x0 + 7, y0, colour);
  if ((millis() / 1000) % 2 == 0) { box(d, x0 + 14, y0 + 3, 2, 2, colour); box(d, x0 + 14, y0 + 8, 2, 2, colour); }
  bigDigit(d, minute / 10, x0 + 18, y0, minutesColour);
  bigDigit(d, minute % 10, x0 + 25, y0, minutesColour);
}

// The score above the cannons left, stacked on the right of the clock
void drawHud(MatrixPanel_I2S_DMA &d) {
  char text[24];
  snprintf(text, sizeof(text), "%05ld", score % 100000);
  const uint16_t scoreColour = Settings::getInstance()->arcScore ? pacmancolors::sprite(Settings::getInstance()->arcScore) : kWhite;
  miniText(d, text, 44, 2, scoreColour);   // five digits, ending one column in from the edge
  const Swatch icon[] = {{'#', kCannon}};
  const int ships = lives < 0 ? 0 : lives > 4 ? 4 : lives;   // the ships you have left, right-aligned under the score (the most there can be is four)
  for (int i = 0; i < ships; i++) sprite(d, CANNON, CANNON_H, 64 - (ships * 8 - 1) + i * 8, 9, icon, 1);
  box(d, 0, HUD_LINE, 64, 1, manual() ? rgb(60, 220, 90) : rgb(40, 44, 60));   // a green divider while you play
}

// The level is a dim "L2" behind the game, in the thick block font of Pong's background clock
void drawLevel(MatrixPanel_I2S_DMA &d) {
  char text[8];
  snprintf(text, sizeof(text), "%d", level % 100);
  const int count = 1 + (int)strlen(text), x0 = (64 - (count * 16 - 4)) / 2;
  thickGlyph(d, 10, x0, 20, kLevel);
  for (int i = 1; i < count; i++) thickGlyph(d, text[i - 1] - '0', x0 + i * 16, 20, kLevel);
}

// While the computer plays, the date is the backdrop: the day number in the thick block digits, the weekday and the month stacked beside it
void drawDate(MatrixPanel_I2S_DMA &d, ClockTime *time) {
  static const char *DAYS[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  static const char *MONTHS[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  const int day = time->getDay() % 100, weekday = time->getWeekday(), month = time->getMonth();
  const int digits = day >= 10 ? 2 : 1, width = digits * 14 - 2, x0 = 6 + (26 - width) / 2;   // the number is centred in columns 6..31
  if (day >= 10) thickGlyph(d, day / 10, x0, 21, kLevel);
  thickGlyph(d, day % 10, x0 + (digits - 1) * 14, 21, kLevel);
  boldText(d, DAYS[weekday >= 0 && weekday < 7 ? weekday : 0], 40, 22, kLevel);
  boldText(d, MONTHS[month >= 1 && month <= 12 ? month - 1 : 0], 40, 32, kLevel);
}

void drawInvaders(MatrixPanel_I2S_DMA &d, bool flash) {
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) {
      if (!alive[r][c]) continue;
      const Swatch palette[] = {{'#', flash ? kWhite : invaderColour(r)}};
      sprite(d, invaderSprite(r), INV_H, invaderX(c), invaderY(r), palette, 1);
    }
}

void drawEverything(MatrixPanel_I2S_DMA &d, ClockTime *time) {
  const unsigned long now = millis();
  d.fillScreen(0);
  const int backFade = phase == OVER ? 70 : phase == READY ? 150 : 255;   // game over: the field is dimmed so the text reads; slightly for READY
  const int fieldFade = !manual() && backFade > 170 ? 170 : backFade;      // and a bit while the computer plays
  view.fade = backFade;
  if (manual()) drawLevel(d); else drawDate(d, time);
  view.fade = 255;
  drawClock(d, time);
  drawHud(d);
  view.fade = fieldFade;
  for (int s = 0; s < SHIELDS; s++)
    for (int y = 0; y < SHIELD_H; y++)
      for (int x = 0; x < SHIELD_W; x++)
        if (shield[s][y][x]) put(d, shieldX(s) + x, SHIELD_Y + y, kShield);
  const bool clearing = phase == CLEARED;
  if (!(clearing && (now / 150) % 2)) drawInvaders(d, clearing);
  if (ufoOn) { const Swatch palette[] = {{'#', kUfo}, {'o', kUfoLights}}; sprite(d, UFO_ROWS, 3, ufoX, UFO_Y, palette, 2); }
  if (popupUntil > now) {
    char text[24];
    snprintf(text, sizeof(text), "%ld", popup);
    miniText(d, text, popupX < 1 ? 1 : popupX > 64 - miniWidth(text) ? 64 - miniWidth(text) : popupX, UFO_Y - 1, kUfoLights);
  }
  for (int i = 0; i < MAX_BOMBS; i++) {
    if (!bombs[i].on) continue;
    const int x = bombs[i].x, y = bombs[i].y;
    const bool zig = ((y / 2) & 1) != bombs[i].kind;
    put(d, x + (zig ? 0 : 2), y, kBomb); put(d, x + 1, y + 1, kBomb); put(d, x + (zig ? 2 : 0), y + 2, kBomb); put(d, x + 1, y, kBomb); put(d, x + 1, y + 2, kBomb);
  }
  if (bulletOn) { put(d, bulletX, bulletY, kBullet); put(d, bulletX, bulletY + 1, kBullet); }
  for (int i = 0; i < MAX_FX; i++)
    if (bursts[i].until > now) { const Swatch palette[] = {{'#', kWhite}}; sprite(d, BURST, INV_H, bursts[i].x, bursts[i].y, palette, 1); }

  if (phase == DYING) {   // the cannon flickers apart
    const unsigned long t = now - (phaseUntil - DEATH_MS);
    for (int y = 0; y < CANNON_H; y++)
      for (int x = 0; x < CANNON_W; x++) {
        const uint32_t h = hash((t / 70) * 977 + y * 31 + x);
        if (h % 3 == 0) put(d, px + x, CANNON_Y + y, h & 8 ? kWhite : kBomb);
        else if (h % 3 == 1 && t < 900) put(d, px + x, CANNON_Y + y - (int)(t / 120), kRed);
      }
  } else if (phase != OVER) {
    const Swatch palette[] = {{'#', kCannon}};
    sprite(d, CANNON, CANNON_H, px, CANNON_Y, palette, 1);
  }

  view.fade = 255;
  if (phase == READY && (now / 250) % 2 == 0) boldText(d, "READY", (64 - boldWidth("READY")) / 2, 38, kWhite);
  if (phase == CLEARED) {
    const int number = blockNumber(d, level + 1, 0, 0, 8, 12, kWhite, true), x = (64 - (boldWidth("WAVE") + 5 + number)) / 2;
    boldText(d, "WAVE", x, 41, kWhite);
    blockNumber(d, level + 1, x + boldWidth("WAVE") + 5, 36, 8, 12, kWhite);
  }
  if (phase == OVER) {
    boldText(d, "GAME OVER", (64 - boldWidth("GAME OVER")) / 2, 26, kRed);
    const int number = blockNumber(d, hiScore % 100000, 0, 0, 8, 12, kHud, true), x = (64 - (boldWidth("HI") + 5 + number)) / 2;
    boldText(d, "HI", x, 43, kHud);
    blockNumber(d, hiScore % 100000, x + boldWidth("HI") + 5, 38, 8, 12, kHud);
  }
}

}  // namespace

void Clockface::steer(int dir) {
  if ((dir < 0 || dir > 4) && !(dir >= 200 && dir <= 300)) { manualUntil = 0; moveDir = 0; autoFire = false; fireOnce = false; slideOn = false; return; }
  if (!manual()) Serial.printf("[Invaders] you took over\n");
  manualUntil = millis() + MANUAL_MS;
  if (dir >= 200) { slideOn = true; slideTarget = (CANNON_MAX * (dir - 200) + 50) / 100; moveDir = 0; return; }   // the touch controller: the cannon's place in percent
  if (dir != 4) slideOn = false;
  if (dir == 1) moveDir = 1;
  else if (dir == 3) moveDir = -1;
  else if (dir == 0) autoFire = true;
  else if (dir == 4) fireOnce = true;   // a tap: one shot
  else { moveDir = 0; autoFire = false; }
}

Clockface::Clockface(MatrixPanel_I2S_DMA *display) { _display = display; }

void Clockface::setup(ClockTime *dateTime) {
  _dateTime = dateTime;
  randomSeed(esp_random());
  lastMinute = -1; lastHour = -1; flashUntil = 0; lastNow = 0; manualUntil = 0; hiScore = 0;
  newGame();
}

void Clockface::update() {
  runGame(_dateTime);
  drawEverything(*_display, _dateTime);
  _display->flipDMABuffer();
}
