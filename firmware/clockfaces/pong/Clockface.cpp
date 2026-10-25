// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
// Pong as a clock: the time is the dim backdrop, hours in the left player's colour and minutes in the right one's. The two computer
// players rally until the minute turns, and then the left one lets the ball go by so the right one scores and the minute changes (the
// right one does it at the hour, which changes the hour).
// Play the left paddle yourself from the settings page (or a game controller): a match to 7, then the clock takes over again.
#include "Clockface.h"
#include <FaceKit.h>
#include <MiniFont.h>
#include <Settings.h>
#include <PacmanColors.h>
#include <NumericFont.h>
#include <math.h>

using namespace facekit;

namespace {

const int COURT_TOP = 13, COURT_BOTTOM = 63;                // the walls are the rows above and under this range
const int PAD_H = 9, PAD_W = 2, LEFT_X = 0, RIGHT_X = 62;   // paddles: left at x 0..1, right at x 62..63
const int BALL = 2, MATCH_POINTS = 7;
const float START_SPEED = 46.0f, MAX_SPEED = 104.0f;        // pixels per second
const float CLOCK_PAD_SPEED = 96.0f, HUMAN_PAD_SPEED = 88.0f;
// The two computers that play while nobody does: both at difficulty 5 of 7 (the same figures as the Difficulty slider gives at that step)
const float CLOCK_LAG_MS = 184.0f, CLOCK_EXTRAP = 0.63f, CLOCK_SPEED = 73.5f, CLOCK_NOISE = 0.86f;   // difficulty level 6 of 9
// 1: the computer sees the ball as it was a reaction time ago (human-like: slow and flat balls are caught, fast steep ones get past).
// 0: the earlier Difficulty levels (paddle speed, where it starts tracking, how much it predicts), kept in newMatch().
#define PONG_REACTION_LAG 1

const unsigned long SERVE_MS = 850, POINT_MS = 1100, OVER_MS = 3200, MANUAL_MS = 20000, STEP_MS = 8;
const int TRAIL = 5, PARTICLES = 14;

uint16_t kLeft = rgb(255, 150, 40), kRight = rgb(60, 200, 255);   // the two sides: also the colours of the hours and the minutes (Settings: arcHours, arcMinutes)
const uint16_t kBall = rgb(255, 255, 255), kWhite = rgb(255, 255, 255);
const uint16_t kWall = rgb(32, 32, 36), kNet = rgb(32, 32, 36), kHud = rgb(110, 120, 150);

enum Phase : uint8_t { SERVE, RALLY, POINT, MATCH_OVER };
struct Particle { float x, y, vx, vy; unsigned long until; uint16_t colour; };

Phase phase = SERVE;
unsigned long phaseUntil, lastNow, manualUntil, leftFlash, rightFlash, scoreFlashLeft, scoreFlashRight;
float ballX, ballY, ballVX, ballVY, speed;           // top-left of the 2 x 2 ball
float padL, padR;                                    // top of each paddle
float trailX[TRAIL], trailY[TRAIL];
float leftAim, rightAim, leftOff, rightOff;
int shownHour, shownMinute;                          // the scores
int moveDir;
bool slideOn;                                        // the paddle follows a sliding finger (slideTarget) instead of a held direction
float slideAnchor, slideTarget;
bool inMatch, humanServe, serveToRight;
int serveLoser;                                      // -1 left, 1 right, 0 none yet
Particle particles[PARTICLES];
char overText[24];
float aiSpeed, aiNoise, aiOffScale = 0.7f, aiReactX, aiLagMs = 300, aiExtrap;   // aiExtrap: how much of the ball's seen sideways speed it allows for
float histY[128], histVY[128]; int histPos;   // the ball's height and speed at each 8 ms step, for the reaction lag
float aiPredicts;   // 0 follows the ball, 1 knows where it lands
int matchLeft, matchRight;                           // the score of a match you play
int tallyLeft, tallyRight;                           // the points of the computer players, who start again from 0 at 10
unsigned long trailAt;

bool manual() { return millis() < manualUntil; }
float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
float randf() { return random(1000) / 1000.0f; }

void burst(float x, float y, int count, uint16_t colour, float power) {
  int made = 0;
  for (int i = 0; i < PARTICLES && made < count; i++) {
    if (particles[i].until > millis()) continue;
    const float angle = randf() * 6.2832f, v = power * (0.4f + randf());
    particles[i] = {x, y, cosf(angle) * v, sinf(angle) * v, millis() + 300 + random(400), colour};
    made++;
  }
}

// Where the ball arrives at the paddle face (centre y), bouncing off the walls on the way
float predict(float faceX, bool towardsRight) {
  if (ballVX == 0) return ballY + 1;
  const float travel = towardsRight ? faceX - (ballX + BALL) : faceX - ballX;
  const float t = travel / ballVX;
  if (t < 0) return ballY + 1;
  const float lo = COURT_TOP, hi = COURT_BOTTOM - BALL, span = hi - lo;
  float y = ballY + ballVY * t - lo;
  y = fmodf(y, span * 2);
  if (y < 0) y += span * 2;
  if (y > span) y = span * 2 - y;
  return y + lo + 1;
}

void serve(bool towardsRight) {
  ballX = 31; ballY = COURT_TOP + 8 + random(COURT_BOTTOM - COURT_TOP - 20);
  speed = START_SPEED;
  for (int i = 0; i < 128; i++) { histY[i] = ballY + 1; histVY[i] = 0; }
  const float angle = (randf() - 0.5f) * 0.9f;
  ballVX = (towardsRight ? 1 : -1) * speed * cosf(angle);
  ballVY = speed * sinf(angle);
  for (int i = 0; i < TRAIL; i++) { trailX[i] = ballX; trailY[i] = ballY; }
  leftOff = (randf() - 0.5f) * (PAD_H - 3); rightOff = (randf() - 0.5f) * (PAD_H - 3);
  phase = SERVE; phaseUntil = millis() + SERVE_MS;
  serveToRight = towardsRight;
}

void newMatch() {
  matchLeft = matchRight = 0;
  shownHour = matchLeft; shownMinute = matchRight;
#if PONG_REACTION_LAG
  {   // Difficulty 1..9 (setting 0..8): the reaction delay steps down from 290 to 132 ms (a human's simple reaction time is about 215), and the other figures follow it along the points that were found by playing: 280 ms is a novice, 235 ms easy, 210 ms an even match, 190 ms very hard for a casual player
    static const float LAGS[9] = {290, 245, 212, 204, 196, 184, 165, 148, 132};   // found by playing: 235 ms was easy, 210 even, 190 very hard, so the steps get small there
    static const float KNOT_LAG[6] = {280, 230, 190, 165, 140, 120}, KNOT_SPEED[6] = {58, 65, 72, 78, 84, 92}, KNOT_EXTRAP[6] = {0.10f, 0.38f, 0.60f, 0.72f, 0.85f, 0.92f}, KNOT_NOISE[6] = {1.4f, 1.15f, 0.9f, 0.75f, 0.6f, 0.5f}, KNOT_OFF[6] = {0.60f, 0.58f, 0.50f, 0.45f, 0.40f, 0.35f};
    const int n = Settings::getInstance()->pongLevel > 8 ? 0 : Settings::getInstance()->pongLevel;
    const float lag = LAGS[n];
    int k = 0;
    while (k < 4 && lag < KNOT_LAG[k + 1]) k++;
    const float f = clampf((KNOT_LAG[k] - lag) / (KNOT_LAG[k] - KNOT_LAG[k + 1]), 0, 1);
    auto at = [&](const float *v) { return v[k] + (v[k + 1] - v[k]) * f; };
    aiLagMs = lag; aiSpeed = at(KNOT_SPEED) + random(8); aiExtrap = at(KNOT_EXTRAP);
    aiNoise = at(KNOT_NOISE) + randf() * 0.8f; aiOffScale = at(KNOT_OFF);
  }
#else
  {   // how well the computer plays against you (Difficulty setting): paddle speed, aim wobble, how widely it misplaces its returns, and whether it works out where the ball lands
    static const struct { int speed, spread; float noise, noiseSpread, offScale, reactX, predicts; } LEVELS[4] = {   // evenly spaced steps from novice to expert; it misses by being slow and late, never by aiming off the paddle
      {27, 8, 2.8f, 1.0f, 0.7f, 38, 0.08f}, {31, 8, 2.5f, 1.0f, 0.65f, 35, 0.14f}, {34, 8, 2.3f, 1.0f, 0.6f, 32, 0.2f}, {46, 8, 1.5f, 0.8f, 0.5f, 22, 0.5f}};
    const auto &l = LEVELS[Settings::getInstance()->pongLevel > 3 ? 3 : Settings::getInstance()->pongLevel];
    aiSpeed = l.speed + random(l.spread); aiNoise = l.noise + randf() * l.noiseSpread; aiOffScale = l.offScale; aiReactX = l.reactX; aiPredicts = l.predicts;
  }   // a computer you can beat
#endif
  serveLoser = 0;
  serve(random(2) == 0);
}

void backToClock(ClockTime *time) {
  inMatch = false; moveDir = 0; tallyLeft = tallyRight = 0;   // a fresh score when the computers take over again
  shownHour = time->getHour(); shownMinute = time->getMinute();
  serve(random(2) == 0);
}

// The score the ball goes to: the side that scored gets the point, the loser receives the next serve
void scorePoint(bool rightScored, ClockTime *time) {
  const int loser = rightScored ? -1 : 1;
  if (inMatch) {
    if (rightScored) matchRight++; else matchLeft++;
    shownHour = matchLeft; shownMinute = matchRight;
  } else {
    shownHour = time->getHour(); shownMinute = time->getMinute();
    if (rightScored) tallyRight++; else tallyLeft++;
    if (tallyLeft >= 10 || tallyRight >= 10) tallyLeft = tallyRight = 0;
  }
  if (rightScored) scoreFlashRight = millis() + 900; else scoreFlashLeft = millis() + 900;
  burst(rightScored ? 1.0f : 62.0f, ballY + 1, 12, rightScored ? kRight : kLeft, 38.0f);
  serveLoser = loser;
  Serial.printf("[Pong] %s scored: %d - %d\n", rightScored ? "right" : "left", shownHour, shownMinute);
  phase = POINT; phaseUntil = millis() + POINT_MS;
  if (inMatch && (matchLeft >= MATCH_POINTS || matchRight >= MATCH_POINTS)) {
    phase = MATCH_OVER; phaseUntil = millis() + OVER_MS;
    snprintf(overText, sizeof(overText), "%s", matchLeft > matchRight ? "YOU WIN" : "COMPUTER\nWINS");
    Serial.printf("[Pong] match over %d-%d\n", matchLeft, matchRight);
  }
  ballVX = ballVY = 0;
}

// What the clock expects: when the displayed time lags the real one, the side that has to lose the next point
// (-1: the left player misses, 1: the right one misses, 0: nobody)
int forcedMiss(ClockTime *time) {
  if (inMatch) return 0;
  const int hour = time->getHour(), minute = time->getMinute();
  if (hour == shownHour && minute == shownMinute) return 0;
  return hour != shownHour || minute < shownMinute ? 1 : -1;
}

void hit(bool leftPaddle) {
  const float padTop = leftPaddle ? padL : padR;
  const float offset = clampf((ballY + 1 - (padTop + PAD_H * 0.5f)) / (PAD_H * 0.5f + 1), -1, 1);
  speed = speed * 1.06f > MAX_SPEED ? MAX_SPEED : speed * 1.06f;
  const float angle = offset * 0.95f;
  ballVX = (leftPaddle ? 1 : -1) * speed * cosf(angle);
  ballVY = speed * sinf(angle);
  (leftPaddle ? leftFlash : rightFlash) = millis() + 140;
  burst(leftPaddle ? LEFT_X + PAD_W + 1.0f : RIGHT_X - 1.0f, ballY + 1, 4, leftPaddle ? kLeft : kRight, 22.0f);
  // the next hit lands somewhere else on the paddle
  (leftPaddle ? rightOff : leftOff) = (randf() - 0.5f) * (inMatch ? PAD_H * aiOffScale : PAD_H - 2.5f);   // in a match the computer often aims badly
}

void movePaddle(float &top, float target, float maxSpeed, float dt) {
  const float want = target - PAD_H * 0.5f;
  const float delta = clampf(want - top, -maxSpeed * dt, maxSpeed * dt);
  top = clampf(top + delta, COURT_TOP, COURT_BOTTOM - PAD_H);
}

void think(float dt, ClockTime *time) {
  histY[histPos] = ballY + 1; histVY[histPos] = ballVY; histPos = (histPos + 1) % 128;
  const int miss = forcedMiss(time);
  const float mid = (COURT_TOP + COURT_BOTTOM) * 0.5f;
  const bool toLeft = ballVX < 0, toRight = ballVX > 0;
  // the right player: always the computer
  {
    float target = mid;
#if PONG_REACTION_LAG
    const float lag = inMatch ? aiLagMs : CLOCK_LAG_MS, extrap = inMatch ? aiExtrap : CLOCK_EXTRAP;
    const int seen = (histPos - 1 - (int)(lag / STEP_MS) + 256) % 128;
    if (!toRight && phase != POINT) target = mid + (histY[seen] - mid) * 0.5f;   // while the ball is away it drifts halfway towards where it sees it, so it is not caught in the middle of the court when the ball turns
    if (toRight && phase == RALLY) {   // it only knows where the ball was a reaction time ago, and allows for some of the way it was going
      const float y = clampf(histY[seen] + histVY[seen] * extrap * lag / 1000.0f, COURT_TOP + 1, COURT_BOTTOM - 1);
#else
    if (toRight && phase == RALLY && (!inMatch || ballX > aiReactX)) {
      const float y = inMatch ? ballY + 1 + (predict(RIGHT_X, true) - ballY - 1) * aiPredicts : predict(RIGHT_X, true);   // the weaker levels mostly follow the ball
#endif
      if (miss == 1) target = y < mid ? y + PAD_H * 0.5f + 5 : y - PAD_H * 0.5f - 5;
      else target = y - rightOff + (randf() - 0.5f) * (inMatch ? aiNoise : CLOCK_NOISE);
    }
    movePaddle(padR, target, inMatch ? aiSpeed : CLOCK_SPEED, dt);
  }
  // the left player: the computer, or you
  if (inMatch && manual() && slideOn) {
    const float step = 260.0f * dt, d = slideTarget - padL;   // follows the finger closely, and stops when it does
    padL = clampf(padL + clampf(d, -step, step), COURT_TOP, COURT_BOTTOM - PAD_H);
  } else if (inMatch && manual()) {
    padL = clampf(padL + moveDir * HUMAN_PAD_SPEED * dt, COURT_TOP, COURT_BOTTOM - PAD_H);
  } else {
    float target = mid;
#if PONG_REACTION_LAG
    const int seen = (histPos - 1 - (int)(CLOCK_LAG_MS / STEP_MS) + 256) % 128;
    if (!toLeft && phase != POINT) target = mid + (histY[seen] - mid) * 0.5f;
    if (toLeft && phase == RALLY) {
      const float y = clampf(histY[seen] + histVY[seen] * CLOCK_EXTRAP * CLOCK_LAG_MS / 1000.0f, COURT_TOP + 1, COURT_BOTTOM - 1);
      if (miss == -1) target = y < mid ? y + PAD_H * 0.5f + 5 : y - PAD_H * 0.5f - 5;
      else target = y - leftOff + (randf() - 0.5f) * CLOCK_NOISE;
    }
    movePaddle(padL, target, CLOCK_SPEED, dt);
#else
    if (toLeft && phase == RALLY) {
      const float y = predict(LEFT_X + PAD_W, false);
      if (miss == -1) target = y < mid ? y + PAD_H * 0.5f + 5 : y - PAD_H * 0.5f - 5;
      else target = y - leftOff;
    }
    movePaddle(padL, target, CLOCK_PAD_SPEED, dt);
#endif
  }
}

void stepBall(float dt, ClockTime *time) {
  ballX += ballVX * dt; ballY += ballVY * dt;
  if (ballY < COURT_TOP) { ballY = COURT_TOP + (COURT_TOP - ballY); ballVY = -ballVY; burst(ballX + 1, COURT_TOP, 2, kWall, 14.0f); }
  if (ballY > COURT_BOTTOM - BALL) { ballY = (COURT_BOTTOM - BALL) - (ballY - (COURT_BOTTOM - BALL)); ballVY = -ballVY; burst(ballX + 1, COURT_BOTTOM, 2, kWall, 14.0f); }
  const float cy = ballY + 1;
  if (ballVX < 0 && ballX <= LEFT_X + PAD_W && ballX > LEFT_X - 2) {
    if (cy >= padL - 1 && cy <= padL + PAD_H + 1) { ballX = LEFT_X + PAD_W; hit(true); }
  } else if (ballVX > 0 && ballX + BALL >= RIGHT_X && ballX + BALL < RIGHT_X + 3) {
    if (cy >= padR - 1 && cy <= padR + PAD_H + 1) { ballX = RIGHT_X - BALL; hit(false); }
  }
  if (ballX + BALL < 0) scorePoint(true, time);
  else if (ballX > 64) scorePoint(false, time);
}

void run(ClockTime *time) {
  const unsigned long now = millis();
  unsigned long gap = lastNow ? now - lastNow : 0;
  lastNow = now;
  if (gap > 400) { phaseUntil += gap; gap = STEP_MS; }   // the panel was used by something else: carry on from here
  if (!manual()) {   // game speed (settings) while the computer plays: scale the simulated time, keeping the remainder
    static const unsigned SPEEDS[5] = {50, 75, 100, 125, 150};
    static unsigned long carry = 0;
    carry += gap * SPEEDS[Settings::getInstance()->pongSpeed > 4 ? 2 : Settings::getInstance()->pongSpeed];
    gap = carry / 100; carry %= 100;
  }

  if (inMatch && !manual()) backToClock(time);
  if (!inMatch && manual() && phase != POINT) { inMatch = true; newMatch(); }

  if (phase == SERVE) {
    if (now >= phaseUntil) phase = RALLY;
    else {
      for (unsigned long t = 0; t < gap; t += STEP_MS) think(STEP_MS / 1000.0f, time);
      return;
    }
  } else if (phase == POINT) {
    for (unsigned long t = 0; t < gap; t += STEP_MS) think(STEP_MS / 1000.0f, time);
    if (now >= phaseUntil) serve(serveLoser > 0);   // towards whoever lost
    return;
  } else if (phase == MATCH_OVER) {
    if (now >= phaseUntil) { manualUntil = 0; backToClock(time); }
    return;
  }

  while (gap > 0) {   // fixed small steps: a fast ball must not jump over a paddle
    const unsigned long step = gap < STEP_MS ? gap : STEP_MS;
    const float dt = step / 1000.0f;
    think(dt, time);
    stepBall(dt, time);
    gap -= step;
    if (phase != RALLY) break;
  }
  if (now - trailAt >= 20) {
    trailAt = now;
    for (int i = TRAIL - 1; i > 0; i--) { trailX[i] = trailX[i - 1]; trailY[i] = trailY[i - 1]; }
    trailX[0] = ballX; trailY[0] = ballY;
  }
}

// ---- the picture ----

// The names and the winner text: a 7 px tall font with 2 px strokes (5 wide; Y T N 1 are 6), '\n' starts a new line
struct BoldGlyph { char c; const char *rows[7]; };
const BoldGlyph BOLD[] = {
  {'C', {".XXX.", "XXXXX", "XX...", "XX...", "XX...", "XXXXX", ".XXX."}},
  {'O', {".XXX.", "XXXXX", "XX.XX", "XX.XX", "XX.XX", "XXXXX", ".XXX."}},
  {'U', {"XX.XX", "XX.XX", "XX.XX", "XX.XX", "XX.XX", "XXXXX", ".XXX."}},
  {'Y', {"XX..XX", "XX..XX", ".XXXX.", "..XX..", "..XX..", "..XX..", "..XX.."}},
  {'M', {"XX.XX", "XXXXX", "XXXXX", "XX.XX", "XX.XX", "XX.XX", "XX.XX"}},
  {'P', {"XXXX.", "XXXXX", "XX.XX", "XXXXX", "XXXX.", "XX...", "XX..."}},
  {'E', {"XXXXX", "XXXXX", "XX...", "XXXX.", "XX...", "XXXXX", "XXXXX"}},
  {'R', {"XXXX.", "XXXXX", "XX.XX", "XXXX.", "XXXX.", "XX.XX", "XX.XX"}},
  {'T', {"XXXXXX", "XXXXXX", "..XX..", "..XX..", "..XX..", "..XX..", "..XX.."}},
  {'W', {"XX.XX", "XX.XX", "XX.XX", "XX.XX", "XXXXX", "XXXXX", "XX.XX"}},
  {'I', {"XX", "XX", "XX", "XX", "XX", "XX", "XX"}},
  {'N', {"XX..XX", "XXX.XX", "XXXXXX", "XX.XXX", "XX..XX", "XX..XX", "XX..XX"}},
  {'S', {".XXXX", "XXXXX", "XX...", ".XXX.", "...XX", "XXXXX", "XXXX."}},
  {'L', {"XX...", "XX...", "XX...", "XX...", "XX...", "XXXXX", "XXXXX"}},
  {'1', {"..XX..", "XXXX..", "XXXX..", "..XX..", "..XX..", "XXXXXX", "XXXXXX"}},
  {'2', {".XXX.", "XXXXX", "...XX", ".XXX.", "XX...", "XXXXX", "XXXXX"}},
};
const BoldGlyph *boldGlyph(char c) {
  for (const BoldGlyph &g : BOLD) if (g.c == c) return &g;
  return nullptr;
}
int boldWidth(const char *text) {   // of the first line
  int w = 0;
  for (; *text && *text != '\n'; text++) {
    const BoldGlyph *g = boldGlyph(*text);
    w += (g ? (int)strlen(g->rows[0]) : 3) + 1;
  }
  return w > 0 ? w - 1 : 0;
}
void boldText(MatrixPanel_I2S_DMA &d, const char *text, int x, int y, uint16_t colour) {
  for (; *text && *text != '\n'; text++) {
    const BoldGlyph *g = boldGlyph(*text);
    if (!g) { x += 4; continue; }
    for (int row = 0; row < 7; row++)
      for (int col = 0; g->rows[row][col]; col++)
        if (g->rows[row][col] == 'X') put(d, x + col, y + row, colour);
    x += (int)strlen(g->rows[0]) + 1;
  }
}
// Centred lines, 2 px apart
void boldCentred(MatrixPanel_I2S_DMA &d, const char *text, int centreY, uint16_t colour) {
  int lines = 1;
  for (const char *c = text; *c; c++) if (*c == '\n') lines++;
  int y = centreY - (lines * 7 + (lines - 1) * 2) / 2;
  for (const char *line = text; line; ) {
    boldText(d, line, (64 - boldWidth(line)) / 2, y, colour);
    y += 9;
    line = strchr(line, '\n');
    if (line) line++;
  }
}

void bigDigit(MatrixPanel_I2S_DMA &d, int value, int x, int y, uint16_t colour) {   // the score: the same block font, 6 x 10 with 2 px strokes
  for (int row = 0; row < 10; row++)
    for (int col = 0; col < 6; col++)
      if (numericfont::pixel(value, col, row, 6, 10) || (value == 1 && col == 0 && row >= 8)) put(d, x + col, y + row, colour);   // the 1's foot reaches 2 px left of the stem, like the right
}

// The time as a big dim backdrop in the court: hours in the left player's colour, minutes in the right player's
void drawBackdrop(MatrixPanel_I2S_DMA &d, int hour, int minute, bool twelve) {
  static const int LEVELS[6] = {0, 45, 80, 110, 150, 200};   // the Background clock setting
  const int level = LEVELS[Settings::getInstance()->pongBackdrop > 5 ? 3 : Settings::getInstance()->pongBackdrop];
  if (level == 0) return;
  const uint16_t hc = kLeft, mc = kRight;
  const int x0 = 3, y0 = 28;   // hours 3..27, minutes 36..60: 4 px either side of the centreline, 3 px at the edges
  // the block digits of the alternative number font (football, F1), one pixel thicker: 12 x 22. Built once into row bitmasks, then drawn as runs
  static uint16_t rows[10][22];
  static bool ready = false;
  if (!ready) {
    for (int value = 0; value < 10; value++) {
      auto lit = [&](int px, int py) { return px >= 0 && py >= 0 && py < 21 && (numericfont::pixel(value, px, py, 11, 21) || (value == 1 && px == 0 && py >= 18) || (value == 1 && px <= 2 && py < 7 && numericfont::pixel(1, px + 1, py, 11, 21))); };   // the 1's foot is even either side of the stem
      for (int row = 0; row < 22; row++) {
        uint16_t mask = 0;
        for (int col = 0; col < 12; col++)
          if (lit(col, row) || lit(col - 1, row) || lit(col, row - 1) || lit(col - 1, row - 1)) mask |= 1 << (11 - col);   // a 11 x 21 digit, one pixel fatter each way
        rows[value][row] = mask;
      }
    }
    ready = true;
  }
  auto digit = [&](int value, int x, uint16_t colour) {   // greedy rectangles: a run of a row continues down while the rows below have it too
    const uint16_t c = dim(colour, level);
    uint16_t left[22];
    memcpy(left, rows[value], sizeof(left));
    for (int row = 0; row < 22; row++)
      for (int col = 0; col < 12;) {
        if (!(left[row] >> (11 - col) & 1)) { col++; continue; }
        const int start = col;
        uint16_t run = 0;
        while (col < 12 && (left[row] >> (11 - col) & 1)) { run |= 1 << (11 - col); col++; }
        int height = 1;
        while (row + height < 22 && (left[row + height] & run) == run) { left[row + height] &= ~run; height++; }
        left[row] &= ~run;
        box(d, x + start, y0 + row, col - start, height, c);
      }
  };
  if (!twelve || hour >= 10) digit(hour / 10, x0, hc);
  digit(hour % 10, x0 + 13, hc);
  digit(minute / 10, x0 + 33, mc);
  digit(minute % 10, x0 + 46, mc);
}

// Two digits, right-aligned to `right` (the left player's score) or left-aligned from `left` (the right player's)
void drawScore(MatrixPanel_I2S_DMA &d, int value, int x, bool twoDigits, uint16_t colour) {
  if (twoDigits) { bigDigit(d, value / 10, x, 1, colour); x += 10; }
  bigDigit(d, value % 10, x, 1, colour);
}

// The weekday (over the hours) and the day and month (over the minutes), each above or below the big clock
void drawDate(MatrixPanel_I2S_DMA &d, ClockTime *time) {
  static const char *DAYS[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  static const char *MONTHS[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  static const int LEVELS[6] = {0, 45, 80, 110, 150, 200};
  const int backdrop = LEVELS[Settings::getInstance()->pongBackdrop > 5 ? 3 : Settings::getInstance()->pongBackdrop];
  const int level = backdrop < 80 ? 80 : backdrop;   // readable even when the big clock is dim or off
  const int weekday = time->getWeekday(), month = time->getMonth();
  char text[16];
  const int ABOVE_Y = 21, BELOW_Y = 52;
  if (Settings::getInstance()->pongDay)
    miniText(d, DAYS[weekday >= 0 && weekday < 7 ? weekday : 0], 3, Settings::getInstance()->pongDay == 2 ? BELOW_Y : ABOVE_Y, dim(kLeft, level));
  if (Settings::getInstance()->pongDate) {
    snprintf(text, sizeof(text), "%d %s", time->getDay() % 100, MONTHS[month >= 1 && month <= 12 ? month - 1 : 0]);
    miniText(d, text, 36, Settings::getInstance()->pongDate == 2 ? BELOW_Y : ABOVE_Y, dim(kRight, level));
  }
}

void drawFrame(MatrixPanel_I2S_DMA &d, ClockTime *time) {
  const unsigned long now = millis();
  d.fillScreen(0);
  if (inMatch) drawBackdrop(d, time->getHour(), time->getMinute(), !time->is24hFormat());
  else drawBackdrop(d, shownHour, shownMinute, !time->is24hFormat());
  if (Settings::getInstance()->pongDate || Settings::getInstance()->pongDay) drawDate(d, time);
  // the walls and the net
  box(d, 0, COURT_TOP - 1, 64, 1, kWall);
  box(d, 0, COURT_BOTTOM, 64, 1, kWall);
  box(d, 31, COURT_TOP, 2, COURT_BOTTOM - COURT_TOP, kNet);

  // the score on top: your match, or the points of the two computer players (the time is the backdrop)
  const int scoreDim = manual() ? 255 : 170;   // dimmer while the computers play, like the paddles
  const uint16_t lc = dim(now < scoreFlashLeft && (now / 90) % 2 ? kWhite : kLeft, scoreDim), rc = dim(now < scoreFlashRight && (now / 90) % 2 ? kWhite : kRight, scoreDim);
  drawScore(d, inMatch ? matchLeft : tallyLeft, 23, false, lc);
  drawScore(d, inMatch ? matchRight : tallyRight, 35, false, rc);

  // paddles
  const bool lf = now < leftFlash, rf = now < rightFlash;
  const int autoDim = manual() ? 255 : 170;   // the computers playing: paddles and ball a bit dimmer than when you play
  box(d, LEFT_X, (int)(padL + 0.5f), PAD_W, PAD_H, dim(lf ? kLeft : kWhite, autoDim));
  box(d, RIGHT_X, (int)(padR + 0.5f), PAD_W, PAD_H, dim(rf ? kRight : kWhite, autoDim));

  // the ball with its trail
  if (phase == RALLY || phase == SERVE) {
    for (int i = TRAIL - 1; i >= 1; i--) {
      const int level = 90 - i * 16;
      if (level > 0) box(d, (int)(trailX[i] + 0.5f), (int)(trailY[i] + 0.5f), BALL, BALL, dim(kBall, level * autoDim / 255));
    }
    const bool blink = phase == SERVE && (now / 120) % 2;
    const uint16_t ballColour = speed > 90 ? rgb(255, 150, 60) : speed > 76 ? rgb(255, 230, 120) : kBall;
    if (!blink) box(d, (int)(ballX + 0.5f), (int)(ballY + 0.5f), BALL, BALL, dim(ballColour, autoDim));
  }
  for (int i = 0; i < PARTICLES; i++)
    if (particles[i].until > now) {
      const float life = (particles[i].until - now) / 700.0f;
      put(d, (int)particles[i].x, (int)particles[i].y, dim(particles[i].colour, (int)(clampf(life, 0, 1) * 255)));
    }
  if (inMatch && manual() && phase != MATCH_OVER) {   // you are playing, not the computers
    boldText(d, "YOU", 1, 4, kLeft);
    boldText(d, "CMP", 63 - boldWidth("CMP"), 4, kRight);
  }
  if (!manual()) {   // the computers play each other
    boldText(d, "P1", 1, 4, dim(kLeft, autoDim));
    boldText(d, "P2", 63 - boldWidth("P2"), 4, dim(kRight, autoDim));
  }
  if (phase == MATCH_OVER) boldCentred(d, overText, 38, kWhite);
}

void moveParticles(float dt) {
  for (int i = 0; i < PARTICLES; i++)
    if (particles[i].until > millis()) { particles[i].x += particles[i].vx * dt; particles[i].y += particles[i].vy * dt; particles[i].vx *= 0.98f; particles[i].vy *= 0.98f; }
}

}  // namespace

void Clockface::steer(int dir) {
  if (dir < 0 || (dir > 3 && dir < 40) || (dir > 160 && dir < 200) || dir > 300) { manualUntil = 0; moveDir = 0; slideOn = false; return; }
  if (!manual()) Serial.printf("[Pong] you took over\n");
  manualUntil = millis() + MANUAL_MS;
  if (dir >= 200) {   // the dial: 200..300 is the paddle's place in the court, in percent
    slideOn = true; moveDir = 0;
    slideTarget = COURT_TOP + (COURT_BOTTOM - PAD_H - COURT_TOP) * (dir - 200) / 100.0f;
    return;
  }
  if (dir >= 40) {   // sliding: 99 = finger down (remember the paddle), 98 = finger up (stay), 40..160 = the finger's offset from where it went down, in pixels + 100
    if (dir == 98) { slideOn = false; moveDir = 0; return; }
    if (dir == 99 || !slideOn) { slideAnchor = padL; slideTarget = padL; slideOn = true; moveDir = 0; if (dir == 99) return; }
    slideTarget = clampf(slideAnchor + (dir - 100), COURT_TOP, COURT_BOTTOM - PAD_H);
    return;
  }
  slideOn = false;
  moveDir = dir == 0 ? -1 : dir == 2 ? 1 : 0;
}

Clockface::Clockface(MatrixPanel_I2S_DMA *display) { _display = display; }

void Clockface::setup(ClockTime *dateTime) {
  _dateTime = dateTime;
  randomSeed(esp_random());
  inMatch = false; manualUntil = 0; moveDir = 0; lastNow = 0; tallyLeft = tallyRight = 0; leftFlash = rightFlash = scoreFlashLeft = scoreFlashRight = 0; trailAt = 0;
  for (int i = 0; i < PARTICLES; i++) particles[i].until = 0;
  padL = padR = (COURT_TOP + COURT_BOTTOM - PAD_H) * 0.5f;
  shownHour = dateTime->getHour(); shownMinute = dateTime->getMinute();
  leftOff = rightOff = 0;
  serve(random(2) == 0);
}

void Clockface::update() {
  Settings *st = Settings::getInstance();
  kLeft = st->arcHours ? pacmancolors::sprite(st->arcHours) : rgb(255, 150, 40);
  kRight = st->arcMinutes ? pacmancolors::sprite(st->arcMinutes) : rgb(60, 200, 255);
  const unsigned long before = lastNow;
  run(_dateTime);
  const float dt = lastNow > before && before ? (lastNow - before) / 1000.0f : 0.016f;
  moveParticles(dt > 0.1f ? 0.1f : dt);
  const uint32_t t0 = micros();
  drawFrame(*_display, _dateTime);
  const uint32_t t1 = micros();
  _display->flipDMABuffer();
  const uint32_t t2 = micros();
  // while you play, once every 10 s: frames, and the average/worst gap between frames, drawing time and flip wait (ms)
  static uint32_t lastStart = 0, statsAt = 0, frames = 0, gapSum = 0, gapMax = 0, drawSum = 0, drawMax = 0, flipSum = 0, flipMax = 0;
  if (manual()) {
    if (lastStart) { const uint32_t gap = t0 - lastStart; gapSum += gap; if (gap > gapMax) gapMax = gap; }
    drawSum += t1 - t0; if (t1 - t0 > drawMax) drawMax = t1 - t0;
    flipSum += t2 - t1; if (t2 - t1 > flipMax) flipMax = t2 - t1;
    frames++;
  }
  lastStart = t0;
  if (millis() - statsAt >= 10000) {
    if (frames > 1) Serial.printf("[Pong] %u frames: gap %.1f/%.1f ms, draw %.1f/%.1f ms, flip %.1f/%.1f ms (avg/worst)\n", (unsigned)frames, gapSum / 1000.0f / (frames - 1), gapMax / 1000.0f, drawSum / 1000.0f / frames, drawMax / 1000.0f, flipSum / 1000.0f / frames, flipMax / 1000.0f);
    statsAt = millis(); frames = gapSum = gapMax = drawSum = drawMax = flipSum = flipMax = 0;
  }
}
