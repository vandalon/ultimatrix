// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include <new>
#include "GoalAnimation.h"
#include "NumericFont.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const uint16_t WHITE = 0xFFFF;
static const uint16_t DIM_WHITE = 0x4208;  // for white parts of a name while it flashes
static const uint16_t GREY = 0x8410;
static const uint16_t DARK_GREY = 0x4208;
static const uint16_t YELLOW = 0xFFE0;
static const uint16_t SKIN = 0xFD28;
static const uint16_t GRASS_LIGHT = 0x0340;
static const uint16_t GRASS_DARK = 0x0240;

static const int FRAME_MS = 40;

// Small deterministic random generator, so the preview matches the panel
static uint32_t seed;
static float randomFloat() {
  seed = seed * 1664525 + 1013904223;
  return (seed >> 8) / 16777216.0f;
}

static void drawText(Adafruit_GFX *d, int x, int y, const char *text, uint16_t color, uint8_t size) {
  for (; *text; text++, x += 6 * size) {
    if (numericfont::enabled() && *text >= '0' && *text <= '9') {
      for (int gy = 0; gy < 7; gy++)
        for (int gx = 0; gx < 4; gx++)
          if (numericfont::smallPixel(*text - '0', gx, gy)) d->fillRect(x + gx * size, y + gy * size, size, size, color);
    } else {
      d->drawChar(x, y, *text, color, color, size);
    }
  }
}

// First and last lit column of a character when drawn at this size, so things can be centred on
// what is visible instead of on the character cell
static void inkColumns(char c, uint8_t size, int &first, int &last) {
  static GFXcanvas1 glyph(6 * 3, 8 * 3);  // up to size 3
  glyph.fillScreen(0);
  char text[2] = {c, 0};
  drawText(&glyph, 0, 0, text, 1, size);
  first = 6 * size;
  last = -1;
  for (int gx = 0; gx < 6 * size; gx++)
    for (int gy = 0; gy < 8 * size; gy++)
      if (glyph.getPixel(gx, gy)) {
        first = gx < first ? gx : first;
        last = gx > last ? gx : last;
      }
}

void drawTwoToneText(Adafruit_GFX *d, int x, int y, const char *text, uint16_t top, uint16_t bottom, uint8_t size) {
  static GFXcanvas1 glyph(6 * 2, 8 * 2);  // one character, up to size 2
  for (; *text; text++, x += 6 * size) {
    glyph.fillScreen(0);
    char letter[2] = {*text, 0};
    drawText(&glyph, 0, 0, letter, 1, size);
    for (int gy = 0; gy < 8 * size; gy++)
      for (int gx = 0; gx < 6 * size; gx++)
        if (glyph.getPixel(gx, gy)) d->drawPixel(x + gx, y + gy, gy < 4 * size ? top : bottom);
  }
}

// The stadium backdrop is a strip of pitch; a "camera" View can scroll and zoom it. world
// pixels are mapped to the screen by View.
static const int GRASS_Y = 29;

struct View {
  float centerX;  // world x shown in the middle of the screen
  float zoom;     // 1 = one world pixel per LED
  int shift;      // rows added above the 32 row scene on taller panels
};

// Vertically the bottom row (the grass) stays put when zooming
static int screenY(const View &v, float wy) { return (int)floorf(31 + v.shift - (31 - wy) * v.zoom); }

static uint16_t dim(uint16_t c, float k) {
  int r = ((c >> 11) & 31) * k, g = ((c >> 5) & 63) * k, b = (c & 31) * k;
  return (r << 11) | (g << 5) | b;
}

// Same value for the same inputs, so the crowd doesn't flicker between frames
static uint32_t hash(int a, int b, int c = 0) {
  uint32_t h = a * 374761393u + b * 668265263u + c * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

// The stadium behind the pitch: stands full of spectators and advertising
// boards along the touchline, drawn dimly so the action stays readable. The
// stands go up indefinitely, so they fill the screen when the camera zooms
// out, and scroll slower than the pitch (parallax). With cheer the crowd jumps
// and cameras flash.
static const int BOARDS_Y = 25;
static void drawStadium(Adafruit_GFX *d, const View &v, uint16_t team, bool cheer, int frame) {
  const uint16_t shirts[] = {team, 0xF800, WHITE, 0x041F, YELLOW, team, 0x07E0};
  int boardsTop = screenY(v, BOARDS_Y);
  for (int y = 0; y < d->height(); y++) {
    float wy = 31 - (31 + v.shift - y) / v.zoom;
    if (wy >= GRASS_Y) break;
    for (int x = 0; x < 64; x++) {
      uint16_t c = 0;
      if (wy >= BOARDS_Y) {
        // Boards in a few colours, a lighter "logo" pattern in the middle rows
        float wx = v.centerX + (x - 32) / v.zoom;
        int panel = (int)floorf(wx / 14);
        const uint16_t boards[] = {dim(team, 0.45f), 0x0210, dim(WHITE, 0.3f), dim(0xF800, 0.4f)};
        int row = (int)wy - BOARDS_Y;
        c = row == 0 || row == 3 ? 0x1082 : boards[(panel % 4 + 4) % 4];
        if ((row == 1 || row == 2) && hash((int)floorf(wx), row) % 3 == 0) c = dim(WHITE, 0.5f);
      } else {
        // Spectators: 2 wide, 3 high (head, shirt, step), some empty seats.
        // Laid out in screen pixels rather than scaled with the zoom: zoomed
        // out, scaling would skip rows and columns and leave empty lines.
        int cx = x + (int)floorf(v.centerX * 0.6f * v.zoom);
        int col = (int)floorf(cx / 2.0f);
        int jump = cheer && ((col + frame / 2) & 1) ? 1 : 0;
        int sy = y - boardsTop + jump;  // rows above the boards are negative
        int seatRow = (int)floorf(sy / 3.0f), part = ((sy % 3) + 3) % 3;
        bool person = (cx & 1) == 0 && hash(col, seatRow) % 5 != 0;
        if (part == 2) c = 0x0841;  // the step of the stand
        else if (person) {
          uint32_t h = hash(col, seatRow);
          if (part == 0) c = cheer && hash(col, seatRow, frame) % 60 == 0 ? WHITE : dim(SKIN, 0.25f);
          else c = dim(shirts[h % 7], 0.25f);
        }
      }
      if (c) d->drawPixel(x, y, c);
    }
  }
}

static void drawPitch(Adafruit_GFX *d, const View &v) {
  for (int x = 0; x < 64; x++) {
    float wx = v.centerX + (x - 32) / v.zoom;
    uint16_t c = ((int)floorf(wx / 8)) % 2 ? GRASS_DARK : GRASS_LIGHT;
    for (int y = screenY(v, GRASS_Y); y < d->height(); y++) d->drawPixel(x, y, c);
  }
}

// Background behind the celebration, lighting every LED: rays in the club's
// two colours (or two shades of one), slowly turning around the middle like
// stadium lights
static void drawCelebrationBackground(Adafruit_GFX *d, uint16_t color, uint16_t second, int f, int height) {
  uint16_t light = dim(color, 0.45f), dark = second ? dim(second, 0.35f) : dim(color, 0.2f);
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < 64; x++) {
      float angle = atan2f(y - (height - 1) / 2.0f, (x - 31.5f) / 2) + f * 0.06f;
      d->drawPixel(x, y, (int)floorf(angle / (2 * M_PI / 14) + 100) % 2 ? light : dark);
    }
  }
}

// Text with a black outline, so it reads on any background; with second set,
// two-tone like drawTwoToneText
static void drawOutlined(Adafruit_GFX *d, int x, int y, const char *text, uint16_t color, uint8_t size, int bold,
                         uint16_t second = 0) {
  for (int dy = -1; dy <= 1 + bold; dy++)
    for (int dx = -1; dx <= 1 + bold; dx++)
      drawText(d, x + dx, y + dy, text, 0, size);
  for (int b = 0; b <= bold; b++)
    for (int c = 0; c <= bold; c++) {
      if (second) drawTwoToneText(d, x + b, y + c, text, color, second, size);
      else drawText(d, x + b, y + c, text, color, size);
    }
}

// One row with both names either side of the score (32 row panels); the scoring team flashes
static void drawScoreLine(Adafruit_GFX *d, int y, const char *home, const char *away, int homeScore, int awayScore,
                          bool homeScored, const TeamKit &homeKit, const TeamKit &awayKit, bool flash) {
  char score[12];
  snprintf(score, sizeof(score), "%d-%d", homeScore, awayScore);
  int scoreX = 32 - (strlen(score) * 6 - 1) / 2;
  int awayX = 64 - ((int)strlen(away) * 6 - 1);
  // The scorer flashes: its colours turn white, and white parts (which
  // couldn't flash to white) turn dim instead
  auto drawName = [&](int x, const char *name, const TeamKit &kit, bool flashing) {
    uint16_t top = kit.shirt, bottom = kit.second;
    if (flashing) {
      top = top == WHITE ? DIM_WHITE : WHITE;
      if (bottom) bottom = bottom == WHITE ? DIM_WHITE : WHITE;
    }
    drawOutlined(d, x, y, name, top, 1, 0, bottom);
  };
  drawName(1, home, homeKit, homeScored && flash);
  drawOutlined(d, scoreX, y, score, WHITE, 1, 0);
  drawName(awayX - 1, away, awayKit, !homeScored && flash);
}

// 64 rows: the score in big digits with the names under their numbers, slide rows lower while it
// comes up from below; the scorer's number and name flash yellow
static void drawScoreBlock(Adafruit_GFX *d, int slide, const char *home, const char *away, int homeScore,
                           int awayScore, bool homeScored, const TeamKit &homeKit, const TeamKit &awayKit,
                           bool flash) {
  uint16_t homeColor = homeScored && flash ? YELLOW : WHITE, awayColor = !homeScored && flash ? YELLOW : WHITE;
  char homeGoals[8], awayGoals[8];
  int homeLen = snprintf(homeGoals, sizeof(homeGoals), "%d", homeScore);
  int awayLen = snprintf(awayGoals, sizeof(awayGoals), "%d", awayScore);
  int size = homeLen + awayLen > 3 ? 2 : 3;  // double figures: a smaller score still fits
  int step = 6 * size, digitsY = 26 + slide;
  int x = 32 - ((homeLen + 1 + awayLen) * step - size) / 2;
  drawOutlined(d, x, digitsY, homeGoals, homeColor, size, 0);
  // The dash in the middle of the free columns between the two numbers
  int unused, homeInk, awayInk, dashWidth = 3 * size;
  inkColumns(homeGoals[homeLen - 1], size, unused, homeInk);
  inkColumns(awayGoals[0], size, awayInk, unused);
  int gapLeft = x + (homeLen - 1) * step + homeInk + 1, gapRight = x + (homeLen + 1) * step + awayInk;
  int dashX = gapLeft + (gapRight - gapLeft - dashWidth) / 2;
  d->fillRect(dashX - 1, digitsY + 3 * size - 1, dashWidth + 2, size + 2, 0);
  d->fillRect(dashX, digitsY + 3 * size, dashWidth, size, GREY);
  drawOutlined(d, x + (homeLen + 1) * step, digitsY, awayGoals, awayColor, size, 0);
  auto name = [&](int center, const char *text, const TeamKit &kit, uint16_t color) {
    int w = (int)strlen(text) * 6 - 1, nx = center - w / 2, y = 51 + slide;
    d->fillRect(nx - 1, y + 8, w + 2, 4, 0);  // the underline gets the same 1px black border
    drawOutlined(d, nx, y, text, color, 1, 0);
    d->fillRect(nx, y + 9, w, 2, kit.shirt);
  };
  name(x + (homeLen * step - size) / 2, home, homeKit, homeColor);
  name(x + (homeLen + 1) * step + (awayLen * step - size) / 2, away, awayKit, awayColor);
}

// On a 64x64 panel the background and confetti fill it all, GOAL! and the score sit in the middle
static void playCelebration(Adafruit_GFX *d, const char *home, const char *away, int homeScore, int awayScore,
                            bool homeScored, const TeamKit &homeKit, const TeamKit &awayKit,
                            const std::function<void(int)> &showFrame) {
  const int height = d->height(), top = height >= 64 ? 6 : (height - 32) / 2;
  const TeamKit &kit = homeScored ? homeKit : awayKit;
  const int FRAMES = 64;
  const char *word = "GOAL!";
  const int letters = 5;
  float letterY[letters], letterVy[letters];
  for (int i = 0; i < letters; i++) {
    letterY[i] = -16;
    letterVy[i] = 0;
  }

  const int confettiCount = 28;
  struct Confetti { float x, y, speed, phase; uint16_t color; } confetti[confettiCount];
  const uint16_t confettiColors[] = {WHITE, YELLOW, kit.second ? kit.second : (uint16_t)0xF81F};
  for (int i = 0; i < confettiCount; i++) {
    confetti[i] = {randomFloat() * 64, -randomFloat() * height, 0.35f + randomFloat() * 0.55f, randomFloat() * 6.28f,
                   confettiColors[i % 3]};
  }


  for (int f = 0; f < FRAMES; f++) {
    drawCelebrationBackground(d, kit.shirt, kit.second, f, height);

    // Confetti keeps falling, and stops being added near the end
    for (Confetti &c : confetti) {
      c.y += c.speed;
      c.x += sinf(c.phase + f * 0.2f) * 0.3f;
      if (c.y > height && f < FRAMES - 25) {
        c.y = -1;
        c.x = randomFloat() * 64;
      }
      d->drawPixel((int)roundf(c.x), (int)roundf(c.y), c.color);
    }

    // GOAL! drops in letter by letter and bounces; once it has landed the
    // colours chase along the letters
    bool landed = true;
    for (int i = 0; i < letters; i++) {
      if (f >= i * 3) {
        letterVy[i] += 0.9f;
        letterY[i] += letterVy[i];
        if (letterY[i] >= 2) {
          letterY[i] = 2;
          letterVy[i] = fabsf(letterVy[i]) < 1.5f ? 0 : -letterVy[i] * 0.45f;
        }
      }
      if (letterY[i] != 2 || letterVy[i] != 0) landed = false;
    }
    for (int i = 0; i < letters; i++) {
      uint16_t c = landed && ((f / 3 + i) % 2) ? YELLOW : WHITE;
      // Bold: the letter drawn again one pixel right and down makes the
      // strokes 3 LEDs wide instead of 2, so GOAL! lights up more LEDs
      int lx = 2 + i * 12, ly = top + (int)roundf(letterY[i]);
      char letter[2] = {word[i], 0};
      drawOutlined(d, lx, ly, letter, c, 2, 1);
    }

    // 64 rows: GOAL! on top, under it the score in big digits (black outline, the rays stay as
    // bright as above) and the names, each centred under its number with the same outline and an
    // underline in the club's colour. The score slides up from below; the scorer's number and
    // name flash yellow.
    if (f >= 18 && height >= 64) {
      int slide = (int)roundf(40 * powf(1 - fminf(1, (f - 18) / 8.0f), 2));
      drawScoreBlock(d, slide, home, away, homeScore, awayScore, homeScored, homeKit, awayKit, (f / 4) % 2);
    }
    // 32 rows: the score slides up from below; the scoring team flashes
    if (f >= 18 && height < 64) {
      int y = top + 23 + (int)roundf(9 * powf(1 - fminf(1, (f - 18) / 8.0f), 2));
      drawScoreLine(d, y, home, away, homeScore, awayScore, homeScored, homeKit, awayKit, (f / 4) % 2);
    }

    showFrame(FRAME_MS);
  }
}

// ---- The goal animation: net cam ----
// The camera hangs behind the goal. The ball comes flying out of the distance, grows until it
// hits the net (flash, ripples and a shaking screen), then GOAL! slams in from the camera with
// fireworks over the cheering stands, and the score comes up.

// Everything is drawn into this canvas first, so a frame can be shaken before it goes on screen
static GFXcanvas16 *scene;
static int panelH;  // 32 or 64 rows

static void present(Adafruit_GFX *d, int shakeX, int shakeY) {
  d->fillScreen(0);
  for (int y = 0; y < panelH; y++) {
    if (y + shakeY < 0 || y + shakeY >= panelH) continue;
    for (int x = 0; x < 64; x++) {
      uint16_t c = scene->getPixel(x, y);
      if (c && x + shakeX >= 0 && x + shakeX < 64) d->drawPixel(x + shakeX, y + shakeY, c);
    }
  }
}

static int shakeAmount(int sinceHit) { return sinceHit < 3 ? 3 : sinceHit < 6 ? 2 : sinceHit < 9 ? 1 : 0; }
static int shakeX(int f, int amount) { return f & 1 ? amount : -amount; }
static int shakeY(int f, int amount) { return f & 2 ? amount / 2 : -(amount / 2); }

static int modulo(int a, int n) { return ((a % n) + n) % n; }

static const uint16_t NET = 0x6B4D;
static const float HIT_X = 31;
static float hitY, hitRadius;  // where the ball ends up and how big it gets: about 40% of the height

// The net covers the whole screen. After a hit a ripple spreads from (cx, cy), age frames old.
// Every LED looks up which spot of the undisturbed net is shifted onto it, so there are no holes.
static void drawNet(Adafruit_GFX *d, float cx, float cy, float age, float strength, uint16_t color) {
  for (int y = 0; y < panelH; y++) {
    for (int x = 0; x < 64; x++) {
      float dx = x - cx, dy = y - cy, dist = sqrtf(dx * dx + dy * dy) + 0.01f, wave = 0;
      if (strength > 0 && dist < age * 2.0f + 3) wave = strength * sinf(dist * 0.5f - age * 0.9f) * expf(-dist / 16);
      int sx = (int)floorf(x - dx / dist * wave), sy = (int)floorf(y - dy / dist * wave);
      if (modulo(sx + sy, 5) == 0 || modulo(sx - sy, 5) == 0) d->drawPixel(x, y, color);
    }
  }
}

static void drawBall(Adafruit_GFX *d, float cx, float cy, float radius, float spin) {
  int x = (int)roundf(cx), y = (int)roundf(cy), r = (int)roundf(radius);
  d->fillCircle(x, y, r, WHITE);
  if (r < 3) return;
  const uint16_t dark = 0x2104;
  if (r < 6) {
    d->drawPixel(x + (int)roundf(cosf(spin)), y + (int)roundf(sinf(spin)), dark);
    return;
  }
  d->fillCircle(x + (int)roundf(cosf(spin) * r / 6), y + (int)roundf(sinf(spin) * r / 6), r / 3, dark);
  for (int k = 0; k < 5; k++) {
    float a = spin + k * 2 * M_PI / 5;
    d->fillCircle(x + (int)roundf(cosf(a) * r * 0.72f), y + (int)roundf(sinf(a) * r * 0.72f), r / 5, dark);
  }
}

// The ball's flight towards the camera, t = 0..1: out of the distance, faster and bigger
static void ballFlight(float t, float &x, float &y, float &radius) {
  float closer = t * t;
  x = 50 + (HIT_X - 50) * closer;
  y = hitY * 0.4f + (hitY - hitY * 0.4f) * closer;
  radius = 1 + (hitRadius - 1) * t * t * t;
}

// The animation is drawn at SUBSTEPS frames per original 40 ms step, so it moves smoothly; timings below are written in
// those original steps (ft) and the discrete flashes and flickers keep their original rate (fi)
static const int SUBSTEPS = 2;
static const int APPROACH_FRAMES = 34 * SUBSTEPS, FLASH_FRAMES = 2 * SUBSTEPS, IMPACT_FRAMES = 16 * SUBSTEPS;

static void drawApproachFrame(int f, const TeamKit &kit, Adafruit_GFX *d, bool show) {
  View v = {32, 1, panelH - 32};
  float t = f / (float)(APPROACH_FRAMES - 1), bx, by, br;
  scene->fillScreen(0);
  drawStadium(scene, v, kit.shirt, false, f / SUBSTEPS);
  drawPitch(scene, v);
  // A few fading copies behind the ball: motion blur
  for (int k = 3; k >= 1; k--) {
    float tx, ty, tr;
    ballFlight(fmaxf(0, t - k * 0.05f), tx, ty, tr);
    scene->fillCircle((int)roundf(tx), (int)roundf(ty), (int)roundf(tr * 0.85f), dim(WHITE, 0.3f / k));
  }
  ballFlight(t, bx, by, br);
  drawBall(scene, bx, by, br, f * 0.5f / SUBSTEPS);
  drawNet(scene, 0, 0, 0, 0, NET);
  if (show) present(d, 0, 0);
}

// The hit: two flashes, then the net ripples and the ball drops behind it
static void drawFlashFrame(int f, const TeamKit &kit, Adafruit_GFX *d, bool show) {
  scene->fillScreen(f < SUBSTEPS ? WHITE : kit.shirt);
  if (show) present(d, 0, 0);
}

static void drawImpactFrame(int f, const TeamKit &kit, Adafruit_GFX *d, bool show) {
  const float ft = f / (float)SUBSTEPS;
  const int fi = f / SUBSTEPS;
  View v = {32, 1, panelH - 32};
  float u = fminf(1, fmaxf(0, (ft - 5) / 10.0f));  // the ball drops from step 5
  scene->fillScreen(0);
  drawStadium(scene, v, kit.shirt, true, fi);
  drawPitch(scene, v);
  drawBall(scene, HIT_X, hitY + (panelH - 6 - hitY) * u * u, hitRadius * (1 - 0.6f * u), ft * 0.3f);
  drawNet(scene, HIT_X, hitY, ft, 3.0f * (1 - ft / (float)(IMPACT_FRAMES / SUBSTEPS)), fi < 6 && fi % 2 ? kit.shirt : NET);
  if (ft < 7) scene->drawCircle((int)HIT_X, (int)hitY, 4 + (int)(ft * 5), fi % 2 ? WHITE : kit.shirt);
  int amount = shakeAmount(fi);
  if (show) present(d, shakeX(fi, amount), shakeY(fi, amount));
}

// Fireworks: short-lived sparks that fly out from a point, slow down and fall
struct Spark {
  float x, y, vx, vy;
  float life, maxLife;
  uint16_t color;
};
static const int SPARKS = 96;
static Spark sparks[SPARKS];

static void burst(float x, float y, const uint16_t *colors, int colorCount) {
  uint16_t color = colors[(int)(randomFloat() * colorCount) % colorCount];
  int made = 0;
  for (Spark &s : sparks) {
    if (s.life > 0) continue;
    float angle = made * 2 * M_PI / 14 + randomFloat() * 0.3f, speed = 0.8f + randomFloat() * 0.5f;
    s = {x, y, cosf(angle) * speed * 1.4f, sinf(angle) * speed, 0, 0, color};
    s.maxLife = s.life = 20 + (int)(randomFloat() * 8);
    if (++made == 14) break;
  }
}

static void updateSparks(Adafruit_GFX *d) {
  const float damping = powf(0.94f, 1.0f / SUBSTEPS);
  for (Spark &s : sparks) {
    if (s.life <= 0) continue;
    s.x += s.vx / SUBSTEPS;
    s.y += s.vy / SUBSTEPS;
    s.vx *= damping;
    s.vy = s.vy * damping + 0.06f / SUBSTEPS;
    float k = s.life / s.maxLife;
    d->drawPixel((int)roundf(s.x), (int)roundf(s.y), dim(s.color, k < 0.5f ? k * 2 : 1));
    s.life -= 1.0f / SUBSTEPS;
  }
}

// Text from a small canvas, scaled about its middle with a black outline, each letter in its
// own colour. The canvas holds GOAL! at size 2 drawn twice (a pixel apart) for thick strokes.
static const int TEXT_W = 62, TEXT_H = 15;
static void drawScaledText(Adafruit_GFX *d, GFXcanvas1 &text, int centerY, float scale, const uint16_t *letterColors) {
  for (int pass = 0; pass < 9; pass++) {
    // Passes 0-7 are the outline (the eight neighbours), pass 8 the letters
    int k = pass < 4 ? pass : pass + 1;  // skip the middle of the 3x3 neighbourhood
    int ox = pass < 8 ? k % 3 - 1 : 0, oy = pass < 8 ? k / 3 - 1 : 0;
    for (int y = 0; y < panelH; y++) {
      int sy = (int)floorf((y - oy - centerY) / scale + 7);
      if (sy < 0 || sy >= TEXT_H) continue;
      for (int x = 0; x < 64; x++) {
        int sx = (int)floorf((x - ox - 32) / scale + 30.5f);
        if (sx < 0 || sx >= TEXT_W || !text.getPixel(sx, sy)) continue;
        d->drawPixel(x, y, pass < 8 ? 0 : letterColors[sx / 12 > 4 ? 4 : sx / 12]);
      }
    }
  }
}

static const int FINALE_FRAMES = 72 * SUBSTEPS, SLAM = 8, SCORE = SLAM + 6;  // SLAM and SCORE in original steps

static void drawFinaleFrame(int f, int &nextBurst, GFXcanvas1 &text, const char *home, const char *away, int homeScore, int awayScore,
                            bool homeScored, const TeamKit &homeKit, const TeamKit &awayKit, Adafruit_GFX *d, bool show) {
  const TeamKit &kit = homeScored ? homeKit : awayKit;
  const float ft = f / (float)SUBSTEPS;
  const int fi = f / SUBSTEPS;
  const int textTop = panelH >= 64 ? 6 : 1, textMid = textTop + 7;  // GOAL! sits here when it has landed
  View v = {32, 1, panelH - 32};
  const uint16_t palette[] = {kit.shirt, kit.second ? kit.second : (uint16_t)0xF81F, WHITE, YELLOW};

  scene->fillScreen(0);
  drawStadium(scene, v, kit.shirt, true, fi);
  drawPitch(scene, v);

  if (f == nextBurst) {
    if (f == SLAM * SUBSTEPS) burst(32, textMid, palette, 4);
    else burst(8 + randomFloat() * 48, 3 + randomFloat() * (panelH * 0.45f), palette, 4);
    nextBurst = f + (7 + (int)(randomFloat() * 5)) * SUBSTEPS;
  }
  if (f >= FINALE_FRAMES - 22 * SUBSTEPS) nextBurst = FINALE_FRAMES;  // no new fireworks near the end
  updateSparks(scene);

  // A shockwave from the letters as they land
  float since = ft - SLAM;
  if (since >= 0 && since < 6) scene->drawCircle(32, textMid, 6 + (int)(since * 5), (int)since % 2 ? WHITE : kit.shirt);

  // GOAL! comes in from the camera: big, then shrinking onto its place
  float p = fminf(1, ft / (float)SLAM), scale = 1 + 3 * (1 - p) * (1 - p);
  uint16_t colors[5];
  for (int i = 0; i < 5; i++) colors[i] = since >= 6 && (fi / 3 + i) % 2 ? YELLOW : WHITE;
  drawScaledText(scene, text, textMid, scale, colors);

  // A stripe in the club's colours opens up under it
  if (since >= 0) {
    int half = (int)(31 * fminf(1, since / 8.0f));
    scene->fillRect(32 - half - 1, textTop + 15, 2 * half + 2, 4, 0);
    scene->fillRect(32 - half, textTop + 16, 2 * half, 1, kit.shirt);
    scene->fillRect(32 - half, textTop + 17, 2 * half, 1, kit.second ? kit.second : DIM_WHITE);
  }

  if (ft >= SCORE) {
    float rise = powf(1 - fminf(1, (ft - SCORE) / 8.0f), 2);
    if (panelH >= 64) drawScoreBlock(scene, (int)roundf(40 * rise), home, away, homeScore, awayScore, homeScored, homeKit, awayKit, (fi / 4) % 2);
    else drawScoreLine(scene, 23 + (int)roundf(9 * rise), home, away, homeScore, awayScore, homeScored, homeKit, awayKit, (fi / 4) % 2);
  }

  int amount = since >= 0 ? shakeAmount((int)since) : 0;
  if (show) present(d, shakeX(fi, amount), shakeY(fi, amount));
}

bool GoalAnimation::begin(Adafruit_GFX *display, const char *home, const char *away, int homeScore, int awayScore,
                          bool homeScored, const TeamKit &homeKit, const TeamKit &awayKit) {
  end();
  seed = 12345;
  panelH = display->height();
  hitY = panelH / 2 - 1;
  hitRadius = panelH * 0.4f;
  // Without throwing: an out of memory exception would restart the clock, now the goal is just not celebrated
  scene = new (std::nothrow) GFXcanvas16(64, panelH);
  _text = new (std::nothrow) GFXcanvas1(TEXT_W, TEXT_H);
  if (!scene || !scene->getBuffer() || !_text || !_text->getBuffer()) {
    end();
    return false;
  }
  scene->setTextWrap(false);
  display->setTextWrap(false);
  _text->setTextWrap(false);
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < 5; i++) _text->drawChar(i * 12 + pass, pass, "GOAL!"[i], 1, 0, 2);
  memset(sparks, 0, sizeof(sparks));
  _nextBurst = SLAM * SUBSTEPS;
  _display = display;
  strlcpy(_home, home, sizeof(_home));
  strlcpy(_away, away, sizeof(_away));
  _homeScore = homeScore;
  _awayScore = awayScore;
  _homeScored = homeScored;
  _homeKit = homeKit;
  _awayKit = awayKit;
  return true;
}

int GoalAnimation::frames() const { return APPROACH_FRAMES + FLASH_FRAMES + IMPACT_FRAMES + FINALE_FRAMES; }

void GoalAnimation::drawFrame(int index, bool show) {
  if (!scene) return;
  const TeamKit &kit = _homeScored ? _homeKit : _awayKit;
  if (index < APPROACH_FRAMES) drawApproachFrame(index, kit, _display, show);
  else if (index < APPROACH_FRAMES + FLASH_FRAMES) drawFlashFrame(index - APPROACH_FRAMES, kit, _display, show);
  else if (index < APPROACH_FRAMES + FLASH_FRAMES + IMPACT_FRAMES) drawImpactFrame(index - APPROACH_FRAMES - FLASH_FRAMES, kit, _display, show);
  else
    drawFinaleFrame(index - APPROACH_FRAMES - FLASH_FRAMES - IMPACT_FRAMES, _nextBurst, *_text, _home, _away, _homeScore, _awayScore,
                    _homeScored, _homeKit, _awayKit, _display, show);
}

void GoalAnimation::end() {
  if (_display) _display->setTextWrap(true);
  _display = nullptr;
  delete scene;
  scene = nullptr;
  delete _text;
  _text = nullptr;
}

void playGoalAnimation(Adafruit_GFX *display, const char *home, const char *away, int homeScore, int awayScore,
                       bool homeScored, const TeamKit &homeKit, const TeamKit &awayKit,
                       const std::function<void(int ms)> &showFrame) {
  GoalAnimation animation;
  if (!animation.begin(display, home, away, homeScore, awayScore, homeScored, homeKit, awayKit)) return;
  for (int f = 0; f < animation.frames(); f++) {
    animation.drawFrame(f, true);
    showFrame(GoalAnimation::FRAME_MS);
  }
  animation.end();
}

void playGoalCelebration(Adafruit_GFX *display, const char *home, const char *away, int homeScore, int awayScore,
                         bool homeScored, const TeamKit &homeKit, const TeamKit &awayKit,
                         const std::function<void(int ms)> &showFrame) {
  seed = 12345;
  display->setTextWrap(false);
  playCelebration(display, home, away, homeScore, awayScore, homeScored, homeKit, awayKit, showFrame);
  display->setTextWrap(true);
}
