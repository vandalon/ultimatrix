// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include "FaceKit.h"

namespace facekit {

// A repeating course shared by both faces. The camera moves; the clock is drawn in screen coordinates.
namespace plumberworld {
constexpr int kLength = 384, kJumpMs = 1600;
constexpr float kAnimationSpeed = 1.2f;
constexpr float kWalkSpeed = 10, kRunSpeed = 20, kJumpSpeed = 30;
static const int OBSTACLES[2] = {88, 232};
enum Item : uint8_t { Coin, Mushroom, Star, SuperMushroom };
static const struct { int x, height; Item kind; } ITEMS[] = {
  {52, 12, Coin}, {96, 22, Coin}, {112, 22, Coin}, {158, 8, Mushroom},
  {202, 12, Coin}, {240, 22, Coin}, {256, 22, Coin}, {322, 22, Star},
};

struct BlockMushroom {
  bool active = false, red = false;
  uint32_t since = 0;
  float x = 0, camera = 0;
  int y = 0, startX = 0, edgeX = 0, blockTop = 0;

  void spawn(int blockX, int top, float scroll, uint32_t now, bool super) {
    active = true; red = super; since = now; camera = scroll;
    startX = blockX + 5; edgeX = blockX + 19; blockTop = top;
    x = startX; y = top;
  }

  // Emerge, slide off the right edge, fall, then travel left along the ground.
  int update(uint32_t now, float dt, float scroll, int ground, float playerX, int feetY) {
    float moved = scroll - camera;
    if (moved < -192) moved += kLength;
    camera = scroll;
    if (!active) return -1;
    const uint32_t age = now - since;
    if (age < 350) y = blockTop - 8 * ease(age, 350) / 1000;
    else if (age < 1250) {
      x = startX + (edgeX - startX) * (age - 350) / 900.0f;
      y = blockTop - 8;
    } else if (age < 2000) {
      const float p = (age - 1250) / 750.0f;
      x = edgeX; y = blockTop - 8 + (int)((ground - blockTop) * p * p);
    } else {
      y = ground - 8; x -= 12 * dt + moved;
      if (playerX + 11 > x + 1 && playerX + 2 < x + 7) {
        if (feetY >= ground - 2) {
          active = false;
          return red ? SuperMushroom : Mushroom;
        }
        // Keep the pickup within reach until an airborne character lands.
        x = playerX + 4;
      }
      if (x < -8) active = false;
    }
    return -1;
  }
};

struct Course {
  float camera = 0;
  uint16_t collected = 0;
  uint16_t lap = 0;
  int lastObstacle = -1;
  uint32_t pickupAt = 0, starAt = 0;
  Item pickup = Coin;
  bool hasPickup = false, hasStar = false;
  bool running = false;
  uint32_t movingMs = 0, paceUntil = 7000, bursts = 0;

  // Count time spent moving on the ground, so clock jumps and scene changes take priority.
  float groundSpeed(float dt) {
    movingMs += (uint32_t)(dt * 1000.0f + 0.5f);
    if (movingMs >= paceUntil) {
      running = !running;
      const uint32_t seed = hash(++bursts);
      paceUntil = movingMs + (running ? 2000 + seed % 2000 : 6000 + seed % 5000);
    }
    return running ? kRunSpeed : kWalkSpeed;
  }

  int x(int position) const {
    int screen = position - (int)camera;
    if (screen < -32) screen += kLength;
    return screen;
  }

  void advance(float distance) {
    camera += distance;
    if (camera >= kLength) { camera -= kLength; collected = 0; lastObstacle = -1; lap++; }
  }

  int jumpAhead(float playerX, bool tube = true) {
    for (int i = 0; i < 2; i++) {
      const float gap = x(OBSTACLES[i]) - (playerX + 13);
      if (lastObstacle != i && gap >= 0 && gap <= 10) { lastObstacle = i; return i == 0 && tube ? 28 : 16; }
    }
    // Reach the floating star even when there is no obstacle beneath it.
    return !(collected & (1 << 7)) && x(ITEMS[7].x) > playerX + 13 && x(ITEMS[7].x) <= playerX + 23 ? 20 : 0;
  }

  int collect(float playerX, int playerY, int playerHeight, int ground, uint32_t now) {
    int coins = 0;
    for (int i = 0; i < 8; i++) {
      if (collected & (1 << i)) continue;
      const auto &item = ITEMS[i];
      const int left = x(item.x) - 4, top = ground - item.height;
      const int height = item.kind == Coin ? 6 : 8;
      if (left + 8 <= playerX + 2 || left >= playerX + 11 || top + height <= playerY) continue;
      // The feet must also reach the item while jumping.
      if (top >= playerY + playerHeight) continue;
      collected |= 1 << i;
      pickup = item.kind; pickupAt = now; hasPickup = true;
      if (item.kind == Coin) coins++;
      if (item.kind == Star) { starAt = now; hasStar = true; }
    }
    return coins;
  }
};

inline int sceneryX(int position, float camera, int divisor = 2, int period = 96) {
  int x = (position - (int)(camera / divisor)) % period;
  if (x < -32) x += period;
  return x;
}

inline void drawStar(MatrixPanel_I2S_DMA &d, int x, int y, uint32_t now) {
  static const char *const ROWS[8] = {"...yy...", "...yy...", "yyyyyyyy", ".yykkyy.", "..yyyy..", ".yyyyyy.", ".yy..yy.", "yy....yy"};
  const Swatch pal[] = {{'y', (now / 120) % 2 ? rgb(255, 224, 40) : rgb(255, 255, 176)}, {'k', rgb(32, 24, 0)}};
  sprite(d, ROWS, 8, x, y, pal, 2);
}

struct TubeColors { uint16_t body, light, dark, line; };
static const TubeColors GREEN_TUBE = {rgb(0, 168, 0), rgb(128, 224, 40), rgb(0, 72, 0), rgb(0, 32, 0)};
static const TubeColors CYAN_TUBE = {rgb(0, 168, 168), rgb(128, 224, 224), rgb(0, 72, 72), rgb(0, 32, 32)};
static const TubeColors GREY_TUBE = {rgb(88, 88, 88), rgb(168, 168, 168), rgb(40, 40, 40), rgb(16, 16, 16)};
static const TubeColors TEAL_TUBE = {rgb(0, 120, 136), rgb(80, 208, 208), rgb(0, 56, 64), rgb(0, 32, 40)};
static const TubeColors GHOST_TUBE = {rgb(88, 72, 110), rgb(168, 152, 192), rgb(32, 16, 48), rgb(32, 16, 48)};

inline void drawTube(MatrixPanel_I2S_DMA &d, int x, int ground, const TubeColors &c, uint32_t now, bool plant) {
  const int y = ground - 12;
  if (plant) {
    const int t = now % 12000;
    int rise = 0;
    if (t >= 3000 && t < 4200) rise = 8 * ease(t - 3000, 1200) / 1000;
    else if (t >= 4200 && t < 7500) rise = 8;
    else if (t >= 7500 && t < 8700) rise = 8 - 8 * ease(t - 7500, 1200) / 1000;
    if (rise > 0) {
      static const char *const CLOSED[8] = {"..rrrrr..", ".rrwrwrr.", "rrwwrwwrr", "rrrrrrrrr", "rrrrrrrrr", ".rrrrrrr.", "..wwwww..", "........."};
      static const char *const OPEN[8] = {"..rrrrr..", ".rrwrwrr.", "rrwwrwwrr", "rrrrrrrrr", "wkkkkkkkw", "wkkkkkkkw", "rrrrrrrrr", ".rrrrrrr."};
      static const Swatch PAL[] = {{'r', rgb(232, 40, 24)}, {'w', rgb(255, 255, 255)}, {'k', rgb(40, 0, 16)}};
      box(d, x + 6, y - rise + 7, 2, rise, rgb(0, 150, 0));
      sprite(d, (now / 380) % 2 ? CLOSED : OPEN, 8, x + 3, y - rise, PAL, 3);
    }
  }
  box(d, x, y, 14, 4, c.body); box(d, x + 1, y, 2, 4, c.light); box(d, x + 11, y, 2, 4, c.dark);
  box(d, x, y, 14, 1, c.line); box(d, x, y + 3, 14, 1, c.line); box(d, x, y, 1, 4, c.line); box(d, x + 13, y, 1, 4, c.line);
  box(d, x + 1, y + 4, 12, 8, c.body); box(d, x + 2, y + 4, 2, 8, c.light); box(d, x + 10, y + 4, 2, 8, c.dark);
  box(d, x + 1, y + 4, 1, 8, c.line); box(d, x + 12, y + 4, 1, 8, c.line);
}

// An eight-pixel ledge; the jump carries the feet above it from takeoff to landing.
inline void drawLedge(MatrixPanel_I2S_DMA &d, int x, int ground, uint16_t body, uint16_t light, uint16_t mortar) {
  box(d, x, ground - 8, 16, 8, body);
  box(d, x, ground - 8, 16, 1, light);
  box(d, x, ground - 4, 16, 1, mortar);
  box(d, x + 7, ground - 7, 1, 3, mortar);
  box(d, x + 3, ground - 3, 1, 3, mortar);
  box(d, x + 11, ground - 3, 1, 3, mortar);
}

}  // namespace plumberworld
}  // namespace facekit
