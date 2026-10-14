#pragma once

// Drawing helpers for the birthday screens. The panel can't be read back to blend with, so a scene is drawn into a
// small RGB band first and the band is copied to the panel; the scene runs once per band. Nothing is kept between
// frames (every scene is a function of the time) and no memory is allocated: the band is 3 KB.

#include <Arduino.h>
#include <algorithm>
#include <math.h>
#include <string.h>

namespace birthday {

constexpr int W = 64, H = 64, BAND = 16;

struct RGB { float r, g, b; };

inline uint8_t band[W * BAND * 3];
inline int bandTop = 0;

// 3x5 font, 15 bits per glyph, rows top to bottom: A-Z, 0-9, '!', ':', ' '
constexpr uint16_t FONT[] = {
  0x2BED, 0x6BAE, 0x3923, 0x6B6E, 0x79A7, 0x79A4, 0x396B, 0x5BED, 0x7497, 0x126A, 0x5BAD, 0x4927, 0x5FED,
  0x6B6D, 0x2B6A, 0x6BA4, 0x2B7B, 0x6BAD, 0x388E, 0x7492, 0x5B6F, 0x5B6A, 0x5BFD, 0x5AAD, 0x5A92, 0x72A7,
  0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF, 0x2482, 0x0410, 0x0000};

// 4x6 font for the age and the time, 24 bits per glyph, rows top to bottom: A R J I S ! 0-9 : ' '
constexpr char FONT2_CHARS[] = "ARJIS!0123456789: ";
constexpr uint32_t FONT2[] = {0x69F999, 0xE9EA99, 0x311196, 0xE4444E, 0x78611E, 0x444404, 0x699996, 0x262227, 0x69124F,
                              0xE1611E, 0x99F111, 0xF8E196, 0x68E996, 0xF12244, 0x696996, 0x697116, 0x040040, 0x000000};

inline RGB hsv(float h, float s, float v) {
  h = h - floorf(h);
  int i = (int)(h * 6);
  float f = h * 6 - i;
  float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
  switch (i % 6) {
    case 0: return {v * 255, t * 255, p * 255};
    case 1: return {q * 255, v * 255, p * 255};
    case 2: return {p * 255, v * 255, t * 255};
    case 3: return {p * 255, q * 255, v * 255};
    case 4: return {t * 255, p * 255, v * 255};
    default: return {v * 255, p * 255, q * 255};
  }
}

// A repeatable pseudo-random number in -1..1 for particle n
inline float noise(uint32_t n) {
  n = (n << 13) ^ n;
  return 1.0f - (float)((n * (n * n * 15731u + 789221u) + 1376312589u) & 0x7fffffffu) / 1073741824.0f;
}

inline void px(float fx, float fy, RGB c, float a = 1.0f) {
  int x = lroundf(fx), y = lroundf(fy) - bandTop;
  if (x < 0 || x >= W || y < 0 || y >= BAND || a <= 0) return;
  if (a > 1) a = 1;
  uint8_t *o = &band[(y * W + x) * 3];
  o[0] += (c.r - o[0]) * a;
  o[1] += (c.g - o[1]) * a;
  o[2] += (c.b - o[2]) * a;
}

// The pixel and its four neighbours, the neighbours fainter
inline void star(float x, float y, RGB c, float a, float halo = 0.4f) {
  px(x, y, c, a);
  px(x - 1, y, c, a * halo);
  px(x + 1, y, c, a * halo);
  px(x, y - 1, c, a * halo);
  px(x, y + 1, c, a * halo);
}

inline void rect(int x, int y, int w, int h, RGB c, float a = 1.0f) {
  int from = std::max(0, bandTop - y), to = std::min(h, bandTop + BAND - y);
  for (int j = from; j < to; j++)
    for (int i = 0; i < w; i++) px(x + i, y + j, c, a);
}

// Everything drawn so far gets darker (factor 0..1)
inline void dim(float factor) {
  for (size_t i = 0; i < sizeof(band); i++) band[i] = band[i] * factor;
}

inline int glyph(char ch) {
  if (ch >= 'A' && ch <= 'Z') return ch - 'A';
  if (ch >= '0' && ch <= '9') return 26 + ch - '0';
  if (ch == '!') return 36;
  if (ch == ':') return 37;
  return 38;
}

// hue < 0: one colour; otherwise a rainbow wave over the letters, starting at hue offset 'hue'
inline void text(const char *s, int x, int y, RGB col, float hue = -1) {
  for (int k = 0; s[k]; k++) {
    uint16_t g = FONT[glyph(s[k])];
    RGB c = hue < 0 ? col : hsv(hue - k * 0.12f, 0.7f, 1);
    for (int r = 0; r < 5; r++)
      for (int cc = 0; cc < 3; cc++)
        if (g & (1 << (14 - (r * 3 + cc)))) px(x + k * 4 + cc, y + r, c);
  }
}

inline void centerText(const char *s, int y, RGB col, float hue = -1) {
  int w = strlen(s) * 4 - 1;
  text(s, 32 - w / 2, y, col, hue);
}

// The time in the 4x6 font, 1 px per glyph tighter around the colon
inline void timeText(const char *s, int y, RGB col, int cx = 32) {
  int w = -1;
  for (const char *c = s; *c; c++) w += *c == ':' ? 3 : 5;
  int x = cx - w / 2;
  for (const char *c = s; *c; c++) {
    const char *p = strchr(FONT2_CHARS, *c);
    uint32_t g = FONT2[p ? p - FONT2_CHARS : sizeof(FONT2_CHARS) - 2];
    for (int r = 0; r < 6; r++)
      for (int cc = 0; cc < 4; cc++)
        if (g & (1UL << (23 - (r * 4 + cc)))) px(x + cc, y + r, col);
    x += *c == ':' ? 3 : 5;
  }
}

// The age at 2x the 4x6 font: gold gradient, drop shadow, a shimmer sweeping across and twinkling sparkles around it
inline void bigText(const char *s, int y, float t, int cx = 32) {
  int n = strlen(s), w = n * 10 - 2, x0 = cx - w / 2;
  float sweep = fmodf(t * 14, w + 40) - 20;
  for (int pass = 0; pass < 2; pass++) {  // 0: shadow, 1: letters
    for (int k = 0; k < n; k++) {
      const char *p = strchr(FONT2_CHARS, s[k]);
      uint32_t g = FONT2[p ? p - FONT2_CHARS : sizeof(FONT2_CHARS) - 2];
      for (int r = 0; r < 6; r++)
        for (int c = 0; c < 4; c++) {
          if (!(g & (1UL << (23 - (r * 4 + c))))) continue;
          for (int dy = 0; dy < 2; dy++)
            for (int dx = 0; dx < 2; dx++) {
              int x = x0 + k * 10 + c * 2 + dx, yy = y + r * 2 + dy;
              if (pass == 0) {
                px(x + 1, yy + 1, {110, 45, 10});
              } else {
                float f = (yy - y) / 11.0f;
                RGB col = {255, 235 - 95 * f, 90 - 60 * f};
                float a = fmaxf(0.0f, 1 - fabsf((x - x0) + (yy - y) * 0.6f - sweep) / 5.0f) * 0.75f;
                px(x, yy, {col.r + (255 - col.r) * a, col.g + (255 - col.g) * a, col.b + (255 - col.b) * a});
              }
            }
        }
    }
  }
  const int sp[6][2] = {{-5, 1}, {w + 3, 9}, {w / 2 - 3, -3}, {3, 14}, {w - 6, -2}, {-2, 7}};
  for (int i = 0; i < 6; i++) {
    float a = fmaxf(0.0f, sinf(t * 2.2f + i * 2.1f));
    a = a * a * a;
    if (a < 0.05f) continue;
    int sx = x0 + sp[i][0], sy = y + sp[i][1];
    RGB col = {255, 250, 200};
    px(sx, sy, col, a);
    const int8_t n4[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (auto &d : n4) px(sx + d[0], sy + d[1], col, a * 0.55f);
    if (a > 0.6f)
      for (auto &d : n4) px(sx + d[0] * 2, sy + d[1] * 2, col, (a - 0.6f) * 0.8f);
  }
}

// Runs the scene once per band and copies each band to the panel. scene() draws with the helpers above.
template <typename Display, typename Scene>
void render(Display &display, Scene scene) {
  for (bandTop = 0; bandTop < H; bandTop += BAND) {
    memset(band, 0, sizeof(band));
    scene();
    for (int y = 0; y < BAND; y++)
      for (int x = 0; x < W; x++) {
        const uint8_t *o = &band[(y * W + x) * 3];
        display.drawPixelRGB888(x, bandTop + y, o[0], o[1], o[2]);
      }
  }
}

}  // namespace birthday
