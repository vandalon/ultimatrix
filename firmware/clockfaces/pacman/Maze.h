// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
// The Pac-Man maze and its generator. A maze is a grid of cells with walls between them, the way the arcade mazes work:
// mirrored left and right, corridors one cell wide, no dead ends, a tunnel that wraps round the side, a ghost house in the
// middle with a door at the top, and a power pill near each corner. Header only, no drawing.
#pragma once

#include <stdint.h>
#include <string.h>

namespace pacmaze {

const int COLS = 16, ROWS = 13;   // far bigger than the panel: the face scrolls over it
const int HOUSE_ROW = 6, HOUSE_LEFT = 7, HOUSE_RIGHT = 8;   // the two cells of the ghost house
const int START_ROW = 8;                                     // Pac-Man starts below the house
enum Dir : int8_t { Up = 0, Right = 1, Down = 2, Left = 3 };
const int8_t DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};

inline bool isHouse(int c, int r) { return r == HOUSE_ROW && (c == HOUSE_LEFT || c == HOUSE_RIGHT); }

struct Maze {
  uint16_t right[ROWS];  // bit c: no wall between (c, r) and (c + 1, r); the last bit is the tunnel round the side
  uint16_t down[ROWS];   // bit c: no wall between (c, r) and (c, r + 1)
  uint16_t pill[ROWS];   // bit c: a pill in the cell
  uint16_t power[ROWS];  // bit c: a power pill
  int8_t tunnelRow;
  int pills;

  bool open(int c, int r, int d) const {
    switch (d) {
      case Up: return r > 0 && (down[r - 1] >> c & 1);
      case Down: return r < ROWS - 1 && (down[r] >> c & 1);
      case Right: return right[r] >> c & 1;
      default: return right[r] >> ((c + COLS - 1) % COLS) & 1;
    }
  }
  bool hasPill(int c, int r) const { return pill[r] >> c & 1; }
  bool hasPower(int c, int r) const { return power[r] >> c & 1; }
  void eat(int c, int r) {
    if (hasPill(c, r)) { pill[r] &= ~(1 << c); pills--; }
    power[r] &= ~(1 << c);
  }
};

class MazeBuilder {
 public:
  explicit MazeBuilder(uint32_t seed) : _state(seed ? seed : 0x9E3779B9u) {}

  // Builds a maze; the same seed gives the same maze
  // Bounded: over 20000 seeds the worst took 19 attempts (2.3 on average). Past the limit a seed that is known to
  // give a valid maze at once is used, so the work never runs on.
  void build(Maze &m) {
    for (int i = 0; i < MAX_ATTEMPTS; i++) {
      attempt(m);
      if (valid(m)) return;
    }
    _state = FALLBACK_SEED;
    for (int tries = 0; tries < 100; tries++) {  // seed 1 is valid at once; the bound only keeps a future generator change from hanging the clock
      attempt(m);
      if (valid(m)) return;
    }
  }

 private:
  static const int MAX_ATTEMPTS = 40;
  static const uint32_t FALLBACK_SEED = 1;  // valid on its first attempt
  uint32_t _state;
  uint32_t next() { _state ^= _state << 13; _state ^= _state >> 17; _state ^= _state << 5; return _state; }
  int below(int n) { return (int)(next() % (uint32_t)n); }

  // Opens a wall and, to keep the maze mirrored, its mirror image
  static void openRight(Maze &m, int c, int r) { m.right[r] |= 1 << c; m.right[r] |= 1 << (COLS - 2 - c); }
  static void openDown(Maze &m, int c, int r) { m.down[r] |= 1 << c; m.down[r] |= 1 << (COLS - 1 - c); }
  static void openEdgeUnchecked(Maze &m, int c, int r, int d) {
    if (d == Right) openRight(m, c, r);
    else if (d == Left) openRight(m, c - 1, r);
    else if (d == Down) openDown(m, c, r);
    else openDown(m, c, r - 1);
  }
  // A fully open 2x2 block joins parallel corridors into a double-width passage.
  // The ghost house is a room, not part of the walkable corridors.
  static bool hasWideCorridor(const Maze &m) {
    for (int r = 0; r < ROWS - 1; r++)
      for (int c = 0; c < COLS - 1; c++) {
        if (isHouse(c, r) || isHouse(c + 1, r) || isHouse(c, r + 1) || isHouse(c + 1, r + 1)) continue;
        if ((m.right[r] >> c & 1) && (m.right[r + 1] >> c & 1) &&
            (m.down[r] >> c & 1) && (m.down[r] >> (c + 1) & 1)) return true;
      }
    return false;
  }
  static bool canOpenEdge(const Maze &m, int c, int r, int d) {
    Maze candidate = m;
    openEdgeUnchecked(candidate, c, r, d);  // check the mirrored opening at the same time
    return !hasWideCorridor(candidate);
  }
  static bool openEdge(Maze &m, int c, int r, int d) {
    if (!canOpenEdge(m, c, r, d)) return false;
    openEdgeUnchecked(m, c, r, d);
    return true;
  }
  static int degree(const Maze &m, int c, int r) {
    int n = 0;
    for (int d = 0; d < 4; d++) {
      if (!m.open(c, r, d)) continue;
      const int nc = (c + DX[d] + COLS) % COLS, nr = r + DY[d];
      if (nr < 0 || nr >= ROWS || isHouse(nc, nr)) continue;
      n++;
    }
    return n;
  }

  void attempt(Maze &m) {
    memset(&m, 0, sizeof(m));
    // 1. A random spanning tree of the left half (a depth-first walk), so that every cell can be reached
    const int half = COLS / 2;
    bool seen[ROWS][COLS / 2] = {};
    int stack[ROWS * COLS / 2][2], top = 0;
    stack[top][0] = 0; stack[top][1] = 0; top++;
    seen[0][0] = true;
    while (top) {
      const int c = stack[top - 1][0], r = stack[top - 1][1];
      int options[4], n = 0;
      for (int d = 0; d < 4; d++) {
        const int nc = c + DX[d], nr = r + DY[d];
        if (nc < 0 || nc >= half || nr < 0 || nr >= ROWS || seen[nr][nc] || isHouse(nc, nr) || !canOpenEdge(m, c, r, d)) continue;
        options[n++] = d;
      }
      if (!n) { top--; continue; }
      const int d = options[below(n)];
      openEdge(m, c, r, d);
      const int nc = c + DX[d], nr = r + DY[d];
      seen[nr][nc] = true;
      stack[top][0] = nc; stack[top][1] = nr; top++;
    }
    // 2. The two halves meet on a few rows, and the ghosts' door opens above the house
    openEdge(m, half - 1, 1, Right);
    openEdge(m, half - 1, ROWS - 2, Right);
    for (int r = 0; r < ROWS; r++) if (below(3) == 0) openEdge(m, half - 1, r, Right);
    openEdge(m, HOUSE_LEFT, HOUSE_ROW - 1, Down);
    // 3. Loops: the arcade mazes are full of them
    for (int r = 0; r < ROWS; r++)
      for (int c = 0; c < half; c++) {
        if (isHouse(c, r)) continue;
        if (c + 1 < half && !isHouse(c + 1, r) && below(100) < 28) openEdge(m, c, r, Right);
        if (r + 1 < ROWS && !isHouse(c, r + 1) && below(100) < 28) openEdge(m, c, r, Down);
      }
    // 4. The tunnel, on the row of the house
    m.tunnelRow = HOUSE_ROW;
    m.right[HOUSE_ROW] |= 1 << (COLS - 1);
    // 5. No dead ends: every cell gets a second way out
    for (bool again = true; again;) {
      again = false;
      for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
          if (isHouse(c, r) || degree(m, c, r) >= 2) continue;
          int options[4], n = 0;
          for (int d = 0; d < 4; d++) {
            const int nc = c + DX[d], nr = r + DY[d];
            if (m.open(c, r, d) || nc < 0 || nc >= COLS || nr < 0 || nr >= ROWS || isHouse(nc, nr) || !canOpenEdge(m, c, r, d)) continue;
            options[n++] = d;
          }
          if (!n) continue;
          openEdge(m, c, r, options[below(n)]);
          again = true;
        }
    }
    // 6. Pills everywhere but the house and where Pac-Man starts; the power pills near the four corners
    for (int r = 0; r < ROWS; r++)
      for (int c = 0; c < COLS; c++)
        if (!isHouse(c, r)) { m.pill[r] |= 1 << c; m.pills++; }
    m.pill[START_ROW] &= ~(1 << HOUSE_LEFT); m.pills--;
    static const int8_t CORNERS[4][2] = {{0, 1}, {COLS - 1, 1}, {0, ROWS - 2}, {COLS - 1, ROWS - 2}};
    for (const auto &corner : CORNERS) m.power[corner[1]] |= 1 << corner[0];
  }

  // Everything can be reached from where Pac-Man starts, and nothing dead-ends
  static bool valid(const Maze &m) {
    if (hasWideCorridor(m)) return false;
    bool seen[ROWS][COLS] = {};
    int stack[ROWS * COLS][2], top = 0, count = 0;
    stack[top][0] = HOUSE_LEFT; stack[top][1] = START_ROW; top++;
    seen[START_ROW][HOUSE_LEFT] = true;
    while (top) {
      top--;
      const int c = stack[top][0], r = stack[top][1];
      count++;
      for (int d = 0; d < 4; d++) {
        if (!m.open(c, r, d)) continue;
        const int nc = (c + DX[d] + COLS) % COLS, nr = r + DY[d];
        if (nr < 0 || nr >= ROWS || seen[nr][nc] || isHouse(nc, nr)) continue;
        seen[nr][nc] = true;
        stack[top][0] = nc; stack[top][1] = nr; top++;
      }
    }
    if (count != ROWS * COLS - 2) return false;
    for (int r = 0; r < ROWS; r++)
      for (int c = 0; c < COLS; c++)
        if (!isHouse(c, r) && degree(m, c, r) < 2) return false;
    return true;
  }
};

}  // namespace pacmaze
