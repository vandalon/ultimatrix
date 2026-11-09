// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include "PlumberWorld.h"

namespace facekit {

// Play yourself on the Mario and Luigi faces: the same sprites, speeds and course as the clock, steered from the web
// page. Steer codes: 1 walk right, 5 run right, 3 walk left, 2 stand, 0 or 4 jump, -1 give him back to the clock.
namespace plumberplay {
using namespace plumberworld;
constexpr int kNone = -99;
constexpr uint32_t kIdleMs = 72000;   // scene time, which runs 1.2 times faster: a minute
constexpr float kJumpHeight = 20, kJumpSeconds = 1.6f;   // the jump over an obstacle: the arc of the clock's own jump
constexpr float kLiftSpeed = 4 * kJumpHeight / kJumpSeconds, kGravity = 8 * kJumpHeight / (kJumpSeconds * kJumpSeconds);
constexpr float kBodyLeft = 2, kBodyRight = 11;
constexpr int kPoleWorld = 366;   // the flagpole at the end of the course: touching it clears the level
constexpr int kBlockAbove = 24;   // the ? blocks float 24 px above the ground, an 8 px square (the face draws them from ground - 32)
constexpr int kBlocks = 3, kCoinTrail = 9, kFoes = 3;
static const int BLOCKS[kBlocks] = {120, 190, 270};   // world x; the middle one holds a red mushroom
static const struct { int x, height; } TRAIL[kCoinTrail] = {{128, 14}, {138, 18}, {148, 14}, {176, 14}, {186, 18}, {196, 14}, {280, 14}, {290, 18}, {300, 14}};

// A creature on the course: state 0 none, 1 walking, 2 flat (a shell for a Koopa), 3 resting shell, 4 shell sliding
struct Foe {
  float x = 0;
  uint8_t state = 0;
  bool koopa = false;
  int8_t dir = -1;
  int16_t popup = 0;   // the score shown over it; -1 is the 1UP
  uint32_t since = 0, popupAt = 0;
};

struct Play {
  bool active = false, air = false, moving = false, big = false, poleDone = false, spawnPending = false;
  int8_t intent = 0;   // -1 walking left, 0 standing, 1 walking right, 2 running right
  float lift = 0, vy = 0;
  uint8_t lives = 3, spawned = 0, combo = 0, level = 1, blocksUsed = 0, blockCoin = 0;   // blockCoin: the blocks that gave a coin
  uint16_t coinBits = 0, lap = 0;
  int16_t spawnX = 0;
  uint32_t score = 0, lastInput = 0, hurtUntil = 0, nextEnemy = 0, starUntil = 0, clearAt = 0, overAt = 0, bounceUntil = 0;
  uint32_t blockAt[kBlocks] = {};
  Foe foes[kFoes];
  int8_t queue[8] = {};   // moves waiting for the next step: a walk and a jump can arrive in the same frame
  uint8_t head = 0, count = 0;
};
inline uint32_t &best() { static uint32_t b = 0; return b; }   // the best score since the clock started

inline void push(Play &p, int cmd) {
  if (p.count == sizeof(p.queue)) return;   // a flood of moves: the newest ones wait for the next frame's turn
  p.queue[(p.head + p.count++) % sizeof(p.queue)] = (int8_t)cmd;
}
inline bool pop(Play &p, int &cmd) {
  if (!p.count) return false;
  cmd = p.queue[p.head]; p.head = (p.head + 1) % sizeof(p.queue); p.count--;
  return true;
}

// The obstacles stand on the ground: a tube 12 px high and a ledge 8 px high. This is the height under the body at x.
inline float floorAt(const Course &course, float left, float right) {
  float floor = 0;
  const float tube = course.x(OBSTACLES[0]), ledge = course.x(OBSTACLES[1]);
  if (right > tube + 1 && left < tube + 13) floor = 12;
  if (right > ledge && left < ledge + 16 && floor < 8) floor = 8;
  return floor;
}

inline void start(Play &p, uint32_t now) {
  const Play waiting = p;   // moves that arrived with the first one are kept
  p = Play();
  for (uint8_t i = 0; i < sizeof(p.queue); i++) p.queue[i] = waiting.queue[i];
  p.head = waiting.head; p.count = waiting.count;
  p.active = true; p.lastInput = now; p.nextEnemy = now + 5000;
}

// A game ends: the score is kept for the "best" line
inline void finish(Play &p, uint32_t now) {
  if (p.score > best()) best() = p.score;
  p.overAt = p.score ? now : 0;
}

inline void award(Play &p, Item kind, uint32_t now) {
  if (kind == Mushroom && p.lives < 9) p.lives++;
  else if (kind == SuperMushroom) { p.big = true; p.score += 100; }
  else if (kind == Star) { p.starUntil = now + 8000; p.score += 1000; }
  else if (kind == Coin) p.score += 10;
}

// One step of play. Returns false when the game is over (no lives left, idle, or handed back). plumberTop is the face's
// kPlumberY: the screen row of the head at rest; ground is the face's kGround.
template <typename Plumber>
bool step(Play &p, Plumber &m, Course &course, uint32_t now, float dt, bool paused, int ground, int plumberTop) {
  int c;
  while (pop(p, c)) {
    if (c < 0) return false;
    p.lastInput = now;
    if (c == 1) p.intent = 1;
    else if (c == 5) p.intent = 2;
    else if (c == 3) p.intent = -1;
    else if (c == 2) p.intent = 0;
    else if ((c == 0 || c == 4) && !p.air) { p.air = true; p.vy = kLiftSpeed; }
  }
  if (now - p.lastInput > kIdleMs) return false;
  const int headH = ground - plumberTop, blockBottom = ground - kBlockAbove;
  if (p.lap != course.lap) { p.lap = course.lap; p.coinBits = 0; p.blocksUsed = 0; p.poleDone = false; }   // a new lap: all back in place

  // Walking and running at the clock's own speeds; the camera takes over once he reaches the middle
  p.moving = false;
  if (p.intent != 0) {
    const float move = (p.intent == 2 ? kRunSpeed : kWalkSpeed) * dt;
    if (p.intent > 0) {
      m.dir = 1;
      if (floorAt(course, m.x + kBodyLeft + move, m.x + kBodyRight + move) <= p.lift + 0.5f) {
        if (m.x < 24) m.x += move;
        else { course.advance(move); for (Foe &f : p.foes) if (f.state) f.x -= move; }   // the creatures stay where they are in the world
        m.distance += move; p.moving = true;
      }
    } else {
      m.dir = -1;
      if (m.x - move > 1 && floorAt(course, m.x + kBodyLeft - move, m.x + kBodyRight - move) <= p.lift + 0.5f) {
        m.x -= move; m.distance += move; p.moving = true;
      }
    }
  }

  // Jumping, falling off a ledge and landing; a ? block stops the head from below
  const float floor = floorAt(course, m.x + kBodyLeft, m.x + kBodyRight);
  if (!p.air && p.lift > floor + 0.01f) { p.air = true; p.vy = 0; }
  if (p.air) {
    p.lift += p.vy * dt; p.vy -= kGravity * dt;
    if (p.vy > 0) {
      for (int i = 0; i < kBlocks; i++) {
        const int left = course.x(BLOCKS[i]);
        if (m.x + kBodyRight <= left || m.x + kBodyLeft >= left + 8 || plumberTop - p.lift > blockBottom) continue;
        p.lift = plumberTop - blockBottom; p.vy = 0; p.blockAt[i] = now;
        if (!(p.blocksUsed & (1 << i))) {
          p.blocksUsed |= 1 << i;
          if (i == 1 && !p.big) { p.spawnPending = true; p.spawnX = left; }
          else { p.score += 50; p.blockCoin |= 1 << i; }
        }
        break;
      }
    }
    if (p.lift <= floor && p.vy <= 0) { p.lift = floor; p.vy = 0; p.air = false; p.combo = 0; }
  } else p.lift = floor;

  // The coin trails
  for (int i = 0; i < kCoinTrail; i++) {
    if (p.coinBits & (1 << i)) continue;
    const int left = course.x(TRAIL[i].x) - 4;
    if (m.x + kBodyRight <= left || m.x + kBodyLeft >= left + 8 || p.lift >= TRAIL[i].height + 6 || p.lift + headH <= TRAIL[i].height) continue;
    p.coinBits |= 1 << i; p.score += 10;
  }

  // Creatures come by, more of them and faster at every level; none of them can pass a tube or ledge
  int walking = 0;
  for (const Foe &f : p.foes) if (f.state == 1) walking++;
  if (!paused && now >= p.nextEnemy) {
    Foe *slot = nullptr;
    for (Foe &f : p.foes) if (!f.state) { slot = &f; break; }
    const int gap = 6500 - 600 * p.level < 2500 ? 2500 : 6500 - 600 * p.level;
    if (slot && walking < (p.level + 1 > kFoes ? kFoes : p.level + 1) && floorAt(course, 67, 77) == 0) {
      *slot = Foe();
      slot->x = 66; slot->state = 1; slot->since = now; slot->koopa = p.spawned++ % 3 == 2;
    }
    p.nextEnemy = now + gap + (now >> 3) % 3000;
  }
  const float speed = p.level < 5 ? 8 + 2 * (p.level - 1) : 16;
  for (Foe &f : p.foes) {
    if (f.state == 1) {
      const float next = f.x + f.dir * speed * dt;
      if (floorAt(course, next + 1, next + 11) > 0) f.dir = -f.dir; else f.x = next;
      if (f.x > 70 || f.x < -14) f.state = 0;
    } else if (f.state == 2) {
      if (now - f.since > 900) f.state = 0;
    } else if (f.state == 3) {
      if (now - f.since > 9000 || f.x < -14) f.state = 0;
    } else if (f.state == 4) {
      const float next = f.x + f.dir * 70 * dt;
      if (floorAt(course, next + 1, next + 11) > 0) f.dir = -f.dir; else f.x = next;
      if (f.x > 70 || f.x < -14) f.state = 0;
      for (Foe &g : p.foes) {   // a sliding shell takes the creatures in its way with it
        if (&g == &f || g.state != 1 || g.x + 11 <= f.x + 1 || g.x + 1 >= f.x + 11) continue;
        g.state = 2; g.since = now; g.popup = 100; g.popupAt = now; p.score += 100;
      }
    }
  }

  // Touching a creature: coming down onto it is a stomp (combo up to a 1UP), a shell is kicked, a star kills, the rest hurts
  const bool star = now < p.starUntil;
  auto hurt = [&]() -> bool {
    if (now < p.hurtUntil || now < p.bounceUntil || star) return true;
    p.hurtUntil = now + 1800;
    if (p.big) p.big = false;
    else if (p.lives > 0) p.lives--;
    return p.lives > 0;
  };
  auto kill = [&](Foe &f, bool bounce) {   // a stomp or a star: the combo score
    static const int16_t COMBO[4] = {100, 200, 400, 800};
    f.since = now; f.popupAt = now;
    if (p.combo >= 4) { f.popup = -1; if (p.lives < 9) p.lives++; }
    else { f.popup = COMBO[p.combo]; p.score += f.popup; }
    if (p.combo < 250) p.combo++;
    if (f.koopa) { f.state = 3; } else f.state = 2;
    if (bounce) { p.vy = 28; p.bounceUntil = now + 300; }   // a moment to bounce clear of the next one
  };
  for (Foe &f : p.foes) {
    if (!f.state || f.state == 2 || m.x + kBodyRight <= f.x + 1 || m.x + kBodyLeft >= f.x + 11) continue;
    if (f.state == 1) {
      const float head = f.koopa ? 11 : 12;
      if (p.air && p.vy <= 0 && p.lift <= head - 3) kill(f, true);
      else if (star) kill(f, false);
      else if (p.lift < head - 6 && m.x + kBodyRight > f.x + 3 && m.x + kBodyLeft < f.x + 9 && !hurt()) { finish(p, now); return false; }   // hurt only when the bodies really overlap
    } else if (f.state == 3) {
      if (p.lift < 6) { f.state = 4; f.dir = m.x + 5 < f.x + 6 ? 1 : -1; f.since = now; f.popup = 200; f.popupAt = now; p.score += 200; }
    } else if (f.state == 4 && now - f.since > 400) {
      if (p.air && p.vy <= 0 && p.lift <= 7) { f.state = 3; f.since = now; p.vy = 28; p.score += 100; }
      else if (p.lift < 5 && !hurt()) { finish(p, now); return false; }
    }
  }
  if (p.lives == 0) { finish(p, now); return false; }

  // The flagpole at the end of the course: a bonus for the height you touch it at, then a harder level
  if (!p.poleDone && m.x + kBodyRight >= course.x(kPoleWorld) - 1) {
    p.poleDone = true; p.clearAt = now; p.level++;
    p.score += 500 + 50 * (uint32_t)(p.lift);
  }
  return true;
}

}  // namespace plumberplay
}  // namespace facekit
