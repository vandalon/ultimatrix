#pragma once

// The birthday screen as a short story that repeats every 24 seconds: a party with fireworks, bunting and balloons,
// then a shooting star to wish on, the candles are blown out in the dark, the party poppers go off and the candles
// light themselves again under a finale of fireworks. The name, the age and the time stay readable throughout.
// A function of the time only; every ambient movement repeats within the 24 seconds, so the loop has no seam.

#include "BirthdayCanvas.h"

namespace birthday::party {

constexpr uint32_t LOOP_MS = 24000;
constexpr float LOOP = 24.0f;
constexpr float WISH = 12.0f;  // the fireworks are over, a shooting star crosses the sky
constexpr float BLOW = 15.0f;  // the candles go out one by one
constexpr float POP = 16.6f;   // the poppers go off and the candles light again
constexpr int CANDLES = 5;
constexpr int8_t CANDLE_X[CANDLES] = {22, 27, 32, 37, 42};
constexpr int CANDLE_TOP = 37;  // first row of the candle bodies

struct Burst { int8_t x, y; float start, hue, rise; uint8_t kind; };  // kind: 0 peony, 1 willow, 2 two rings
constexpr Burst BURSTS[] = {
  {14, 22, 0.3f, 0.00f, 0.9f, 0}, {48, 18, 1.8f, 0.55f, 0.9f, 2}, {30, 25, 3.3f, 0.13f, 0.9f, 1}, {10, 30, 4.8f, 0.80f, 0.9f, 0},
  {52, 28, 6.3f, 0.33f, 0.9f, 2}, {24, 20, 7.8f, 0.95f, 0.9f, 1}, {44, 24, 9.0f, 0.60f, 0.9f, 0},
  // the finale
  {32, 20, POP + 0.0f, 0.12f, 0.6f, 0}, {14, 26, POP + 0.4f, 0.85f, 0.5f, 2}, {50, 22, POP + 0.7f, 0.50f, 0.5f, 0},
  {24, 16, POP + 1.2f, 0.30f, 0.5f, 1}, {42, 28, POP + 1.6f, 0.00f, 0.5f, 2}, {32, 24, POP + 2.1f, 0.65f, 0.5f, 0},
  {8, 22, POP + 2.6f, 0.17f, 0.5f, 1}, {56, 26, POP + 2.9f, 0.92f, 0.5f, 0}};

inline float smooth(float a, float b, float x) {
  float u = fminf(1.0f, fmaxf(0.0f, (x - a) / (b - a)));
  return u * u * (3 - 2 * u);
}

// k whole turns per loop
inline float turn(float t, int k) { return 6.2832f * k * t / LOOP; }

// How the party looks at time t of the loop
struct Mood {
  float t;
  float dim;         // 1 = the room is lit, lower while the candles are out
  float lit[CANDLES];   // 0 = flame out, 1 = burning
  float outAt[CANDLES]; // when the flame goes out
  float swell;       // the flames grow while the wish is made
};

inline Mood moodAt(float t) {
  Mood m;
  m.t = t;
  m.dim = 1 - 0.62f * smooth(BLOW - 0.3f, BLOW + 1.0f, t) + 0.62f * smooth(POP, POP + 0.9f, t);
  m.swell = 1 + 0.5f * smooth(WISH, BLOW, t) * (t < BLOW + 1.5f ? 1 : 0);
  for (int i = 0; i < CANDLES; i++) {
    float out = BLOW + i * 0.28f, on = POP + 0.15f + i * 0.18f;
    m.outAt[i] = out;
    m.lit[i] = t < out ? 1.0f : (t < on ? 0.0f : smooth(on, on + 0.3f, t));
  }
  return m;
}

inline void stars(const Mood &m) {
  float wish = smooth(WISH - 1, WISH, m.t) * (1 - smooth(BLOW + 1, POP, m.t));
  for (int i = 0; i < 18; i++) {
    int x = (i * 37 + 5) % 62 + 1, y = 12 + (i * 53) % 24;
    float a = sinf(turn(m.t, 3 + i % 4) + i * 1.7f);
    if (a > 0.45f) px(x, y, {120, 120, 170}, (a - 0.45f) * (1.0f + wish));
  }
}

// A star crossing the sky to wish on
inline void shootingStar(float t) {
  float u = (t - (WISH + 0.5f)) / 0.9f;
  if (u < 0 || u > 1.3f) return;
  for (int k = 0; k < 9; k++) {
    float v = u - k * 0.035f;
    if (v < 0 || v > 1) continue;
    float x = 60 - v * 38, y = 8 + v * 20 + v * v * 4;
    float a = (1 - k / 9.0f) * (1 - smooth(0.85f, 1.3f, u));
    px(x, y, k ? RGB{255, 220, 140} : RGB{255, 255, 255}, a);
  }
}

inline void burst(const Burst &b, float tl) {
  float dt = tl - b.start;
  float life = b.kind == 1 ? 2.8f : 2.3f;
  if (dt < 0 || dt > b.rise + life) return;
  if (dt < b.rise) {  // the rocket climbs from behind the cake, leaving a short tail
    for (int k = 0; k < 4; k++) {
      float u = dt / b.rise - k * 0.06f;
      if (u < 0) continue;
      float ry = 58 - (58 - b.y) * sinf(u * 1.5708f);
      px(b.x, ry, k ? RGB{255, 140, 50} : RGB{255, 235, 170}, k ? 0.5f / k : 0.9f);
    }
    return;
  }
  float e = dt - b.rise;
  float fi = fminf(1.0f, e / 0.2f);
  float fo = fmaxf(0.0f, 1 - fmaxf(0.0f, e - 0.45f) / (life - 0.45f));
  float br = fi * fo * (2 - fo);
  float fall = b.kind == 1 ? 11.0f : 6.0f;  // willows droop
  RGB heart = hsv(b.hue, 0.45f, 1);
  float bloom = br * fmaxf(0.0f, 1 - e / 1.6f);  // the soft glow at the heart of the burst
  for (int x = -4; x <= 4; x++)
    for (int y = -4; y <= 4; y++) {
      float d = hypotf(x, y);
      if (d < 4.5f) px(b.x + x, b.y + y, e < 0.2f ? RGB{255, 245, 220} : heart, (e < 0.2f ? 0.55f : 0.35f) * bloom * (1 - d / 4.5f));
    }
  const int n = 30;
  for (int k = 0; k < n; k++) {
    bool inner = b.kind == 2 && k % 2;
    float a = k / (float)n * 6.2832f + b.start + noise(k * 7 + (int)(b.start * 10)) * 0.08f;
    float speed = b.kind == 1 ? 10 + (k % 3) * 3.0f : (b.kind == 2 ? (inner ? 7.0f : 17.0f) : 12 + (k % 3) * 4.5f);
    float hue = b.kind == 1 ? 0.12f : b.hue + (inner ? 0.45f : (k % 2) * 0.05f);
    float sat = b.kind == 1 ? 0.5f : 0.6f;
    RGB c = hsv(hue, sat, 1);
    float tw = 0.8f + 0.2f * sinf(e * 19 + k * 2.3f);  // twinkle
    float lag[3] = {0, 0.07f, 0.14f};
    for (int s = 0; s < 3; s++) {
      float es = e - lag[s];
      if (es < 0) continue;
      float r = 1 - expf(-es * (b.kind == 1 ? 1.3f : 1.7f));
      float x = b.x + cosf(a) * speed * r, y = b.y + sinf(a) * speed * r + fall * es * es;
      if (s == 0) star(x, y, c, br * tw, 0.5f);
      else px(x, y, c, br * tw * (s == 1 ? 0.55f : 0.28f));
    }
  }
}

inline void bunting(float t) {
  for (int x = 0; x < W; x++) {
    float y = 0.5f + 2.2f * sinf(3.1416f * x / 63) + sinf(turn(t, 2) + x * 0.15f) * 0.3f;
    px(x, y, {80, 80, 95}, 0.8f);
  }
  for (int i = 0; i < 12; i++) {
    float x = 3.5f + i * 5.1f;
    float y = 0.5f + 2.2f * sinf(3.1416f * x / 63) + sinf(turn(t, 2) + x * 0.15f) * 0.3f + 1;
    float shine = 0.78f + 0.22f * sinf(turn(t, 6) + i * 1.3f);
    RGB c = hsv(i * 0.17f + 0.02f, 0.85f, shine);
    float sway = sinf(turn(t, 3) + i * 0.9f) * 0.4f;
    for (int dx = -1; dx <= 1; dx++) px(x + dx, y, c);
    for (int dx = -1; dx <= 1; dx++) px(x + dx + sway * 0.5f, y + 1, c);
    px(x + sway, y + 2, c);
  }
}

inline void balloons(float t) {
  static const int8_t half[7] = {1, 2, 3, 3, 3, 2, 1};  // half width per row, top to bottom
  static const float xs[4] = {4.5f, 58.5f, 12.0f, 51.0f};
  static const float phase[4] = {0.0f, 0.4f, 0.7f, 0.17f};
  for (int b = 0; b < 4; b++) {
    float u = fmodf(t / 8.0f + phase[b], 1.0f);
    float y = 72 - u * 82, x = xs[b] + sinf(turn(t, 3) + phase[b] * 9) * 2;
    RGB c = hsv(b * 0.23f + 0.02f, 0.9f, 1);
    RGB shade = {c.r * 0.6f, c.g * 0.6f, c.b * 0.6f};
    for (int dy = -3; dy <= 3; dy++)
      for (int dx = -half[dy + 3] + 1; dx < half[dy + 3]; dx++)
        px(x + dx, y + dy, (dy >= 2 || (dx >= 2 && dy >= 0)) ? shade : c);
    px(x - 1, y - 1, {255, 255, 255}, 0.85f);
    px(x, y + 4, c);
    for (int k = 0; k < 5; k++) px(x + sinf(turn(t, 4) + k) * 0.7f, y + 5 + k, {170, 170, 170}, 0.7f);
  }
}

inline void ambientConfetti(float t, float amount) {
  if (amount < 0.02f) return;
  for (int i = 0; i < 18; i++) {
    int k = 3 + i % 3;  // falls k times per loop
    float x = fmodf(i * 29 + sinf(turn(t, 3 + i % 3) + i) * 3 + 64, 64.0f);
    float y = fmodf(t * 70.0f * k / LOOP + i * 17, 70.0f) - 4;
    RGB c = hsv(i * 0.137f, 1, 1);
    if ((int)(t * 4 + i) % 3) px(x, y, c, 0.85f * amount);
    else rect((int)x, (int)y, 1, 2, c, amount);
  }
}

// The cake without its flames
inline void cake() {
  const RGB pink = {255, 140, 190};
  rect(11, 61, 42, 1, {200, 200, 210});
  rect(14, 49, 36, 12, {120, 62, 32});
  rect(14, 49, 36, 2, pink);
  const int drips[8] = {15, 16, 21, 27, 36, 42, 47, 48};
  for (int x : drips) rect(x, 51, 1, 1 + x % 3, pink);
  rect(14, 60, 36, 1, {95, 48, 24});
  px(16, 55, {255, 230, 120});
  px(47, 57, {120, 220, 255});
  px(17, 58, {255, 160, 220});
  px(46, 54, {255, 230, 120});
  rect(20, 42, 24, 7, {150, 80, 42});
  rect(20, 42, 24, 2, {255, 255, 255});
  for (int x = 21; x < 43; x += 4) rect(x, 44, 1, 1 + (x / 4) % 2, {255, 255, 255});
  for (int i = 0; i < CANDLES; i++)
    for (int r = 0; r < 5; r++) px(CANDLE_X[i], CANDLE_TOP + r, r % 2 ? RGB{235, 235, 235} : hsv(i / 5.0f, 0.65f, 1));
}

inline void flames(const Mood &m) {
  for (int i = 0; i < CANDLES; i++) {
    int cx = CANDLE_X[i];
    float lit = m.lit[i];
    if (lit > 0.02f && bandTop + BAND > 26 && bandTop < 44) {
      for (int x = -7; x <= 7; x++)  // the light of the flame on its surroundings
        for (int y = -7; y <= 7; y++) {
          float d = hypotf(x, y * 1.2f);
          if (d < 7.5f) px(cx + x, 35 + y, {255, 175, 70}, 0.2f * lit * m.swell * (1 - d / 7.5f) * (1 - d / 7.5f));
        }
    }
    if (lit > 0.02f) {
      float lean = smooth(m.outAt[i] - 0.3f, m.outAt[i], m.t) * (m.t < m.outAt[i] ? 1 : 0);  // the breath before it goes out
      float fl = sinf(m.t * 17 + i * 2.1f) + sinf(m.t * 29 + i);
      int tip = 34 - (fl > 0.8f ? 1 : 0) + (lean > 0.5f ? 1 : 0);
      float lx = lean * 1.0f;
      px(cx, 36, {255, 205, 50}, lit);
      px(cx + lx * 0.5f, 35, {255, 145, 25}, lit);
      px(cx + lx, tip, {255, 235, 130}, lit * 0.85f);
      px(cx - 1, 36, {255, 160, 40}, lit * 0.25f);
      px(cx + 1, 36, {255, 160, 40}, lit * 0.25f);
    }
    // smoke curls up from a flame that has just gone out
    for (int w = 0; w < 3; w++) {
      float age = m.t - m.outAt[i] - w * 0.2f;
      if (age < 0 || age > 1.7f) continue;
      float a = 0.55f * powf(1 - age / 1.7f, 1.4f);
      px(cx + sinf(age * 5 + i + w) * (0.8f + age), 35 - age * 7, {200, 200, 210}, a);
    }
    // a spark of magic as it lights again
    float on = POP + 0.15f + i * 0.18f, s = m.t - on;
    if (s >= 0 && s < 0.5f)
      for (int k = 0; k < 4; k++) {
        float a = k * 1.5708f + 0.8f;
        star(cx + cosf(a) * s * 14, 35 + sinf(a) * s * 14, {255, 240, 170}, 1 - s / 0.5f, 0.0f);
      }
  }
}

// One popper per bottom corner; they point at the middle of the room
inline void popper(bool right, float t) {
  auto at = [&](int x) { return right ? 63 - x : x; };
  const int rows[5][2] = {{1, 2}, {1, 3}, {2, 4}, {2, 5}, {3, 6}};
  for (int r = 0; r < 5; r++)
    for (int x = rows[r][0]; x <= rows[r][1]; x++) px(at(x), 62 - r, r % 2 ? RGB{255, 215, 80} : RGB{255, 100, 170});
  px(at(4), 57, {255, 255, 255});
  px(at(5), 57, {255, 255, 255});
  float f = t - POP;
  if (f >= 0 && f < 0.25f) star(at(7), 56, {255, 255, 255}, 1 - f / 0.25f, 0.6f);
}

// The confetti the poppers shoot: arcs out, then drifts down fluttering
inline void popConfetti(bool right, float t) {
  float e0 = t - POP;
  if (e0 < 0 || e0 > 5.5f) return;
  const float d = 3.0f, g = 24.0f;  // drag and gravity: it ends up sinking at g/d = 8 px/s
  for (int k = 0; k < 36; k++) {
    float seed = k + (right ? 100 : 0);
    float e = e0 - (noise(seed * 3 + 1) + 1) * 0.04f;
    if (e < 0) continue;
    float angle = (48 + 32 * (noise(seed * 5 + 2) + 1) * 0.5f) * 0.01745f;
    float speed = 85 + 70 * (noise(seed * 7 + 3) + 1) * 0.5f;
    float k1 = (1 - expf(-d * e)) / d;
    float x = 7.5f + speed * cosf(angle) * k1 + sinf(e * 6 + k) * 1.2f * k1 * 3;
    float y = 56 + (-speed * sinf(angle) + g / d) * k1 - g / d * e;
    if (right) x = 63 - x;
    float life = fmaxf(0.0f, fminf(1.0f, (5.5f - e0) / 1.2f));
    RGB c = hsv(noise(seed * 11 + 4) * 0.5f + 0.5f, 0.9f, 1);
    if (sinf(e * 9 + k) > -0.2f) px(x, y, c, life);
    else rect((int)lroundf(x), (int)lroundf(y), 1, 2, c, life);
  }
}

// One frame: the name, the age (a greeting when age is 0) and the time ("17:11"). ms is any steadily running clock.
template <typename Display>
void draw(Display &display, uint32_t ms, const char *name, int age, const char *time) {
  float t = (ms % LOOP_MS) / 1000.0f;
  Mood m = moodAt(t);
  float amb = fminf(1.0f, 1 - smooth(WISH - 1, WISH, t) + smooth(POP + 2.5f, POP + 4.0f, t));
  render(display, [&] {
    stars(m);
    shootingStar(t);
    for (const Burst &b : BURSTS) burst(b, t);
    bunting(t);
    balloons(t);
    ambientConfetti(t, amb);
    cake();
    if (m.dim < 0.999f) dim(m.dim);
    flames(m);
    popConfetti(false, t);
    popConfetti(true, t);
    float f = t - POP;
    bool flash = f >= 0 && f < 0.35f;
    centerText(name, 8, flash ? RGB{255, 255, 255} : RGB{0, 0, 0}, flash ? -1 : t * 0.25f);
    if (age > 0) {
      char sub[12];
      snprintf(sub, sizeof(sub), "%d", age);
      bigText(sub, 16, t);
    } else {  // no birth year known
      centerText("HAPPY", 16, {255, 215, 90});
      centerText("BIRTHDAY!", 23, {255, 215, 90});
    }
    timeText(time, 53, {255, 140, 190});
    popper(false, t);
    popper(true, t);
  });
}

}  // namespace birthday::party
