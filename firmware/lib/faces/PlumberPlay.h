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

struct Play {
  bool active = false, air = false, moving = false;
  int8_t intent = 0;   // -1 walking left, 0 standing, 1 walking right, 2 running right
  float lift = 0, vy = 0;
  uint8_t lives = 3, spawned = 0;
  uint32_t score = 0, lastInput = 0, hurtUntil = 0, nextEnemy = 0;
  int8_t queue[8] = {};   // moves waiting for the next step: a walk and a jump can arrive in the same frame
  uint8_t head = 0, count = 0;
};

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

inline void award(Play &p, Item kind) {
  if (kind == Mushroom && p.lives < 9) p.lives++;
  else if (kind == SuperMushroom || kind == Star) p.score += 1000;
  else if (kind == Coin) p.score += 10;
}

// One step of play. Returns false when the game is over (no lives left, idle, or handed back). The Goomba type is the
// face's own (x, state 0 none / 1 walking / 2 squashed, since, spawnedMinute, score, koopa).
template <typename Plumber, typename Goomba>
bool step(Play &p, Plumber &m, Course &course, Goomba &goomba, uint32_t now, float dt, bool paused, int minute) {
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

  // Walking and running at the clock's own speeds; the camera takes over once he reaches the middle
  p.moving = false;
  if (p.intent != 0) {
    const float move = (p.intent == 2 ? kRunSpeed : kWalkSpeed) * dt;
    if (p.intent > 0) {
      m.dir = 1;
      if (floorAt(course, m.x + kBodyLeft + move, m.x + kBodyRight + move) <= p.lift + 0.5f) {
        if (m.x < 24) m.x += move; else course.advance(move);
        m.distance += move; p.moving = true;
      }
    } else {
      m.dir = -1;
      if (m.x - move > 1 && floorAt(course, m.x + kBodyLeft - move, m.x + kBodyRight - move) <= p.lift + 0.5f) {
        m.x -= move; m.distance += move; p.moving = true;
      }
    }
  }

  // Jumping, falling off a ledge and landing
  const float floor = floorAt(course, m.x + kBodyLeft, m.x + kBodyRight);
  if (!p.air && p.lift > floor + 0.01f) { p.air = true; p.vy = 0; }
  if (p.air) {
    p.lift += p.vy * dt; p.vy -= kGravity * dt;
    if (p.lift <= floor && p.vy <= 0) { p.lift = floor; p.vy = 0; p.air = false; }
  } else p.lift = floor;

  // Goombas and Koopas come by now and then: coming down onto its head is a stomp (it flattens as his feet are into it,
  // like the clock's own stomp), touching it any other way hurts
  if (goomba.state == 0 && !paused && now >= p.nextEnemy) {
    goomba = {66, 1, now, minute, 0, p.spawned++ % 3 == 2};
    p.nextEnemy = now + 7000 + (now >> 3) % 5000;
  }
  if (goomba.state == 1 && m.x + kBodyRight > goomba.x + 1 && m.x + kBodyLeft < goomba.x + 11) {
    const float head = goomba.koopa ? 11 : 12;
    if (p.air && p.vy <= 0 && p.lift <= head - 3) {
      goomba.state = 2; goomba.since = now; goomba.x = m.x + 2; goomba.score = goomba.koopa ? 200 : 100;
      p.score += goomba.score; p.vy = 28;
    } else if (p.lift < head - 6 && now >= p.hurtUntil) {
      p.hurtUntil = now + 1800;
      if (p.lives > 0) p.lives--;
      if (p.lives == 0) return false;
    }
  }
  return true;
}

}  // namespace plumberplay
}  // namespace facekit
