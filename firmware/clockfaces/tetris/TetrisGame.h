// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

// The playable half of the Tetris face: a 10 x 21 board, seven-bag pieces, scoring and a computer player. No drawing
// and no hardware, so it also builds on a host. The computer picks the placement that scores best on height, holes,
// bumpiness and lines; its skill (0..8) adds noise to that score, so the weak ones misjudge and top out.

#include <stdint.h>
#include <string.h>

namespace tetrisgame {

constexpr int kCols = 10, kRows = 21;
constexpr uint16_t kOverMs = 1800;  // how long the finished board stays up

enum Phase : uint8_t { Falling, Clearing, Over };
enum Action : uint8_t { Rotate, Right, Down, Left, Drop };  // the steer codes 0..4

class Game {
 public:
  uint8_t board[kRows][kCols];  // 0 empty, 1..7 the piece colour
  uint8_t type, rot, next;      // piece in flight (0 I, 1 O, 2 T, 3 S, 4 Z, 5 J, 6 L) and the one after it
  int8_t x, y;                  // top left of the piece's 4 x 4 box
  uint8_t phase;
  uint8_t clearRows[4], clearCount;
  uint16_t phaseMs;             // time in the clearing / game over phase
  uint32_t score;
  uint16_t lines;
  uint8_t level;

  void reset(uint32_t seed) {
    memset(board, 0, sizeof(board));
    _rng = seed | 1;
    _bagLeft = 0;
    score = 0;
    lines = 0;
    level = 1;
    phase = Falling;
    clearCount = 0;
    phaseMs = 0;
    next = draw();
    spawn();
  }

  // Cells of a piece in a rotation, as (x, y) pairs from the top left of its box
  static void cells(uint8_t t, uint8_t r, int8_t (&out)[4][2]) {
    static const int8_t kBase[7][4][2] = {
        {{0, 1}, {1, 1}, {2, 1}, {3, 1}}, {{1, 0}, {2, 0}, {1, 1}, {2, 1}}, {{1, 0}, {0, 1}, {1, 1}, {2, 1}},
        {{1, 0}, {2, 0}, {0, 1}, {1, 1}}, {{0, 0}, {1, 0}, {1, 1}, {2, 1}}, {{0, 0}, {0, 1}, {1, 1}, {2, 1}},
        {{2, 0}, {0, 1}, {1, 1}, {2, 1}}};
    int8_t c[4][2];
    memcpy(c, kBase[t], sizeof(c));
    for (uint8_t k = 0; k < r; k++)
      for (auto &p : c) {
        const int8_t nx = -p[1];
        p[1] = p[0];
        p[0] = nx;
      }
    int8_t mx = 99, my = 99;
    for (auto &p : c) {
      if (p[0] < mx) mx = p[0];
      if (p[1] < my) my = p[1];
    }
    for (int i = 0; i < 4; i++) {
      out[i][0] = c[i][0] - mx;
      out[i][1] = c[i][1] - my;
    }
  }

  bool hit(uint8_t t, uint8_t r, int px, int py) const {
    int8_t c[4][2];
    cells(t, r, c);
    for (auto &p : c) {
      const int a = px + p[0], b = py + p[1];
      if (a < 0 || a >= kCols || b >= kRows || (b >= 0 && board[b][a])) return true;
    }
    return false;
  }

  int landingY(uint8_t t, uint8_t r, int px, int py) const {
    while (!hit(t, r, px, py + 1)) py++;
    return py;
  }

  // One move by the player. False when it did nothing.
  bool act(Action a) {
    if (phase != Falling) return false;
    switch (a) {
      case Left: return shift(-1);
      case Right: return shift(1);
      case Rotate: {
        const uint8_t nr = (rot + 1) & 3;
        static const int8_t kKicks[5] = {0, -1, 1, -2, 2};
        for (const int8_t k : kKicks)
          if (!hit(type, nr, x + k, y)) {
            rot = nr;
            x += k;
            return true;
          }
        return false;
      }
      case Down: {
        if (!hit(type, rot, x, y + 1)) {
          y++;
          score++;
          return true;
        }
        lock();
        return true;
      }
      case Drop: {
        const int ly = landingY(type, rot, x, y);
        score += 2 * (ly - y);
        y = ly;
        lock();
        return true;
      }
    }
    return false;
  }

  // Advance by dtMs. A human's piece falls by gravity; the computer plays a move every few milliseconds instead.
  void tick(uint32_t dtMs, bool human, uint8_t skill) {
    if (phase == Over) {
      phaseMs = phaseMs + dtMs > 60000 ? 60000 : phaseMs + dtMs;
      return;
    }
    if (phase == Clearing) {
      phaseMs += dtMs;
      if (phaseMs >= kFlashMs * 7) finishClear();
      return;
    }
    if (human) {
      _fall += dtMs;
      const uint32_t every = level >= 14 ? 100 : 520 - level * 30;
      while (_fall >= every && phase == Falling) {
        _fall -= every;
        if (!hit(type, rot, x, y + 1)) y++;
        else lock();
      }
      return;
    }
    _act += dtMs;
    const uint32_t every = 75 - skill * 5;  // the better it plays, the quicker it moves
    while (_act >= every && phase == Falling) {
      _act -= every;
      computerStep(skill);
    }
  }

  bool flashLit() const { return phaseMs / kFlashMs % 2 == 1; }  // the clearing rows blink

 private:
  static constexpr uint32_t kFlashMs = 90;
  uint32_t _rng, _fall = 0, _act = 0;
  uint8_t _bag[7], _bagLeft;
  int8_t _planRot = -1, _planX = 0;

  uint32_t rnd() {
    _rng ^= _rng << 13;
    _rng ^= _rng >> 17;
    _rng ^= _rng << 5;
    return _rng;
  }

  uint8_t draw() {
    if (!_bagLeft) {
      for (uint8_t i = 0; i < 7; i++) _bag[i] = i;
      for (int i = 6; i > 0; i--) {
        const int j = rnd() % (i + 1);
        const uint8_t t = _bag[i];
        _bag[i] = _bag[j];
        _bag[j] = t;
      }
      _bagLeft = 7;
    }
    return _bag[--_bagLeft];
  }

  void spawn() {
    type = next;
    next = draw();
    rot = 0;
    x = 3;
    y = 0;
    _planRot = -1;
    _fall = _act = 0;
    if (hit(type, rot, x, y)) {
      phase = Over;
      phaseMs = 0;
    }
  }

  bool shift(int dx) {
    if (hit(type, rot, x + dx, y)) return false;
    x += dx;
    return true;
  }

  void lock() {
    int8_t c[4][2];
    cells(type, rot, c);
    for (auto &p : c)
      if (y + p[1] >= 0) board[y + p[1]][x + p[0]] = type + 1;
    clearCount = 0;
    for (int r = 0; r < kRows && clearCount < 4; r++) {
      bool full = true;
      for (int q = 0; q < kCols; q++) full = full && board[r][q];
      if (full) clearRows[clearCount++] = r;
    }
    if (clearCount) {
      phase = Clearing;
      phaseMs = 0;
    } else {
      spawn();
    }
  }

  void finishClear() {
    static const uint16_t kPoints[5] = {0, 100, 300, 500, 800};
    int dst = kRows - 1;
    for (int src = kRows - 1; src >= 0; src--) {
      bool gone = false;
      for (uint8_t i = 0; i < clearCount; i++) gone = gone || clearRows[i] == src;
      if (gone) continue;
      if (dst != src) memcpy(board[dst], board[src], kCols);
      dst--;
    }
    while (dst >= 0) memset(board[dst--], 0, kCols);
    score += kPoints[clearCount] * level;
    lines += clearCount;
    level = 1 + lines / 10;
    clearCount = 0;
    phase = Falling;
    spawn();
  }

  // Best rotation and column for the piece in flight; skill 0..8 noise makes the weak ones pick worse ones
  void plan(uint8_t skill) {
    const float noise = (8 - skill) * 0.55f;
    float best = -1e9f;
    int8_t bestRot = 0, bestX = x;
    for (uint8_t r = 0; r < 4; r++)
      for (int px = -3; px < kCols; px++) {
        if (hit(type, r, px, y)) continue;
        const int py = landingY(type, r, px, y);
        uint8_t b[kRows][kCols];
        memcpy(b, board, sizeof(b));
        int8_t c[4][2];
        cells(type, r, c);
        for (auto &p : c)
          if (py + p[1] >= 0) b[py + p[1]][px + p[0]] = 1;
        uint8_t d[kRows][kCols] = {};  // the board once the full rows are gone
        int cleared = 0, dst = kRows - 1;
        for (int row = kRows - 1; row >= 0; row--) {
          bool full = true;
          for (int q = 0; q < kCols; q++) full = full && b[row][q];
          if (full) cleared++;
          else memcpy(d[dst--], b[row], kCols);
        }
        int heights[kCols] = {0}, holes = 0;
        for (int col = 0; col < kCols; col++) {
          bool seen = false;
          for (int row = 0; row < kRows; row++) {
            if (d[row][col]) {
              if (!seen) heights[col] = kRows - row;
              seen = true;
            } else if (seen) {
              holes++;
            }
          }
        }
        int aggregate = 0, bumpiness = 0;
        for (int col = 0; col < kCols; col++) aggregate += heights[col];
        for (int col = 0; col < kCols - 1; col++) bumpiness += heights[col] > heights[col + 1] ? heights[col] - heights[col + 1] : heights[col + 1] - heights[col];
        float score = -0.51f * aggregate + 0.76f * cleared - 1.08f * holes - 0.18f * bumpiness;
        score += (rnd() % 1000) / 1000.0f * noise;
        if (score > best) {
          best = score;
          bestRot = r;
          bestX = px;
        }
      }
    _planRot = bestRot;
    _planX = bestX;
  }

  // One move toward the planned spot: turn, slide, then fall
  void computerStep(uint8_t skill) {
    if (_planRot < 0) plan(skill);
    if (rot != _planRot) {
      const uint8_t nr = (rot + 1) & 3;
      if (!hit(type, nr, x, y)) rot = nr;
      else if (!hit(type, rot, x, y + 1)) y++;
      else lock();
    } else if (x < _planX && !hit(type, rot, x + 1, y)) {
      x++;
    } else if (x > _planX && !hit(type, rot, x - 1, y)) {
      x--;
    } else if (!hit(type, rot, x, y + 1)) {
      y++;
    } else {
      lock();
    }
  }
};

}  // namespace tetrisgame
