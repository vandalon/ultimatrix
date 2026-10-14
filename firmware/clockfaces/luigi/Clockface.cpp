// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Clockface.h"
#include <MiniFont.h>
#include <PlumberSprites.h>
#include <Settings.h>

using namespace facekit;

namespace {

// ---- The worlds. One is shown at a time and a random other one replaces it at every hour turn (overground, underground,
// castle, snow). The two outdoor ones turn into a night sky (stars, moon) when the night clock is on; the others look the same ----
struct Theme {
  uint16_t sky, brick, brickLight, mortar, goomba, goombaFoot, text, divider;
  bool overground;  // false: darker block colour (and the night sky for the outdoor worlds)
};
const Theme kDay = {rgb(92, 148, 252), rgb(200, 76, 12), rgb(252, 152, 56), rgb(40, 8, 0), rgb(184, 100, 20), rgb(252, 188, 120), rgb(255, 255, 255), rgb(36, 82, 190), true};
const Theme kCave = {rgb(0, 0, 28), rgb(0, 84, 176), rgb(60, 140, 232), rgb(0, 12, 44), rgb(72, 120, 200), rgb(176, 200, 240), rgb(170, 196, 244), rgb(36, 46, 110), false};
const Theme kUnder = {rgb(0, 0, 0), rgb(0, 84, 176), rgb(60, 140, 232), rgb(0, 12, 44), rgb(72, 120, 200), rgb(176, 200, 240), rgb(150, 180, 240), rgb(26, 36, 96), false};
const Theme kCastle = {rgb(28, 12, 16), rgb(120, 120, 120), rgb(176, 176, 176), rgb(32, 32, 32), rgb(160, 120, 100), rgb(220, 200, 180), rgb(232, 232, 232), rgb(84, 48, 56), false};
const Theme kSnow = {rgb(168, 208, 248), rgb(120, 144, 200), rgb(255, 255, 255), rgb(48, 64, 112), rgb(184, 100, 20), rgb(252, 188, 120), rgb(20, 52, 130), rgb(96, 140, 210), true};
const Theme kWater = {rgb(8, 56, 168), rgb(40, 120, 176), rgb(104, 184, 232), rgb(0, 56, 96), rgb(184, 100, 20), rgb(252, 188, 120), rgb(255, 255, 255), 0, true};
const Theme kCloud = {rgb(88, 160, 252), rgb(172, 208, 248), rgb(222, 240, 255), rgb(112, 160, 230), rgb(184, 100, 20), rgb(252, 188, 120), rgb(14, 40, 120), 0, true};
const Theme kGhost = {rgb(40, 24, 64), rgb(88, 64, 108), rgb(144, 120, 176), rgb(32, 16, 48), rgb(150, 130, 190), rgb(200, 190, 230), rgb(230, 210, 255), 0, false};
const Theme kLava = {rgb(96, 24, 8), rgb(80, 48, 48), rgb(128, 80, 76), rgb(24, 8, 8), rgb(120, 90, 70), rgb(200, 160, 130), rgb(255, 224, 192), 0, false};
const Theme kShip = {rgb(106, 88, 120), rgb(176, 108, 32), rgb(232, 168, 80), rgb(80, 48, 8), rgb(184, 100, 20), rgb(252, 188, 120), rgb(255, 255, 255), 0, true};
enum World : uint8_t { Overground, Underground, Castle, Snow, Underwater, CloudTop, GhostHouse, Lava, Airship, kWorlds, Finale = kWorlds };

// The world setting shares the settings byte numberFont: 1 rotates at random every hour (the default); 10 is the demo that
// steps through all worlds every few seconds; 0 and 2-9 pin
// Overground, Underground, Castle, Snow, Underwater, Cloud top, Ghost house, Lava, Airship.
// Anything else (a leftover from another face) rotates too.
bool demoWorlds() { return Settings::getInstance()->numberFont == 10; }

int pinnedWorld() {
  switch (Settings::getInstance()->numberFont) {
    case 0: return Overground;
    case 2: return Underground;
    case 3: return Castle;
    case 4: return Snow;
    case 5: return Underwater;
    case 6: return CloudTop;
    case 7: return GhostHouse;
    case 8: return Lava;
    case 9: return Airship;
    default: return -1;
  }
}

const int kGround = 58;             // first row of the ground
const int kJarTop = kGround - 16;   // the Subcon jar stands on the ground
const int kPlumberY = kGround - 22;  // Luigi is six rows taller than Mario
const int kBlockX[2] = {9, 31};            // hours and minutes, 4 px apart so one jump under the gap touches both
const int kBlockTop = 11, kBlockSize = 19;   // square; its bottom edge is where Luigi's head meets it
const int kMinX = 4, kMaxX = 36;     // where Luigi strolls (the jar is on the right)
const int kHitMinutesX = 34, kHitBothX = 23;  // under the minute block; under the gap between the two
const int kHitJumpMs = 520, kHitHeight = 6;

enum class Mode : uint8_t { Walk, Pause, Approach, Wait, Hit, Stomp, Hop, FlagRun, FlagJump, FlagSlide, FlagWait, FinaleWalk, FinaleHidden, FinaleDone };
const int kPoleX = 44, kPoleTop = 19, kLuigiPoleX = 31, kCastleX = 50, kFlagSlideMs = 1700, kFlagJumpMs = 350;

struct Plumber {
  float x = 30, fromX = 0, vx = 0, distance = 0;
  int8_t dir = 1;
  Mode mode = Mode::Walk;
  uint32_t since = 0;
  uint32_t pauseMs = 0;
  int32_t approachedMinute = -1;
  float hitX = kHitMinutesX;
  bool both = false, catchUp = false, touched = false;
} mario;

struct Goomba {
  float x = 0;
  uint8_t state = 0;  // 0 none, 1 walking, 2 squashed
  uint32_t since = 0;
  int32_t spawnedMinute = -1;
  int16_t score = 0;
  bool koopa = false;
} goomba;

// Two digits per block; the digits only change when Luigi hits the block
struct Block { int8_t shown[2], target[2]; uint32_t bumpAt; };
Block blocks[2];
struct { bool pending, both; } touch;  // Luigi's head met the block(s) this frame
uint32_t lastNow = 0;
bool night = false;
int world = Overground, pendingWorld = -1;  // the world on show, and the one the curtain is about to bring
bool wiping = false;
// The noon finale is a scene of its own (not part of the rotation): wipe to it, Luigi slides down the flagpole, walks into the
// castle, fireworks, wipe back to the world that was on show
enum class FinaleStage : uint8_t { None, Out, Playing, Back };
FinaleStage finaleStage = FinaleStage::None;
int worldBefore = 0;
bool finaleWanted = false;
uint32_t finaleHiddenAt = 0;
bool demoFlagWanted = false, demoFlagDone = false;  // the demo plays the finale once per round of worlds
uint32_t worldSince = 0;        // when the world on show came in (for the demo)
bool primed = false;            // the blocks, world and coin day have been taken from a synced clock
uint32_t wipeAt = 0;

// The worlds with a light sky, water or ice: not shown at night
bool brightWorld(int w) { return w == Snow || w == Underwater || w == CloudTop || w == Airship; }

// A different world from the one on show; at night not the bright blue Underwater
int nextWorld(uint32_t seed, bool atNight) {
  for (int i = 0; i < 24; i++) {
    const int candidate = hash(seed + i * 977) % kWorlds;
    if (candidate != world && !(atNight && brightWorld(candidate))) return candidate;
  }
  return (world + 1) % kWorlds;
}
bool mushroomOn = false;        // the 1-UP of the hour block, and the star Luigi flashes with afterwards
uint32_t mushroomAt = 0;
int coins = 0, coinDay = -1;    // the HUD coin counter: one per block hit, back to 0 each day
int flagDay = -1;               // the day Luigi last slid down the noon flagpole

// ---- Drawing ----

// Twinkling stars at fixed spots below the date line, and a crescent moon in the free corner beside the minute block
// A crescent moon in the free corner beside the minute block: a pale disc with a second disc cut out of it
void drawMoon(MatrixPanel_I2S_DMA &d) {
  for (int dy = -4; dy <= 4; dy++)
    for (int dx = -4; dx <= 4; dx++) {
      if (dx * dx + dy * dy > 18) continue;
      const int cx = dx - 2, cy = dy - 1;
      if (cx * cx + cy * cy <= 12) continue;
      put(d, 57 + dx, 14 + dy, rgb(255, 246, 190));
    }
}

void drawNightSky(MatrixPanel_I2S_DMA &d, uint32_t now) {
  for (int i = 0; i < 20; i++) {
    const int x = hash(i * 7 + 1) % 64, y = 8 + hash(i * 13 + 5) % 34;
    const int level = 90 + triangle((int)(now / 5) + i * 71) * 165 / 256;
    const uint16_t c = mix(rgb(70, 90, 170), rgb(255, 255, 255), level);
    put(d, x, y, c);
    if (i % 6 == 0) {  // a few brighter ones with a small cross
      const uint16_t halo = dim(c, 90);
      put(d, x - 1, y, halo); put(d, x + 1, y, halo); put(d, x, y - 1, halo); put(d, x, y + 1, halo);
    }
  }
  drawMoon(d);
}

void drawBricks(MatrixPanel_I2S_DMA &d, int top, int bottom, const Theme &t, bool lit) {
  for (int y = top; y <= bottom; y++) {
    const int row = (y - top) / 6, inRow = (y - top) % 6;
    for (int x = 0; x < W; x++) {
      uint16_t c = t.brick;
      if (inRow == 5) c = t.mortar;
      else if ((x + (row % 2 ? 4 : 0)) % 8 == 7) c = t.mortar;
      else if (lit && y == top) c = t.brickLight;
      put(d, x, y, c);
    }
  }
}

void drawCloud(MatrixPanel_I2S_DMA &d, int x, int y, bool dark) {
  static const char *const CLOUD[5] = {"....wwww........", "..wwwwwwww.wwww.", ".wwwwwwwwwwwwwww", "wwwwwwwwwwwwwwww", ".cccccccccccccc."};
  static const Swatch PAL[] = {{'w', rgb(255, 255, 255)}, {'c', rgb(196, 222, 255)}};
  static const Swatch NIGHT[] = {{'w', rgb(70, 82, 130)}, {'c', rgb(44, 54, 96)}};
  sprite(d, CLOUD, 5, x, y, dark ? NIGHT : PAL, 2);
}

void drawHill(MatrixPanel_I2S_DMA &d, int left, int base, bool dark, bool snow) {
  const int level = dark ? 80 : 255;
  const uint16_t edge = snow ? rgb(130, 170, 215) : rgb(0, 80, 0), fill = snow ? rgb(236, 246, 255) : rgb(24, 172, 32);
  const uint16_t eye = snow ? rgb(130, 170, 215) : rgb(0, 40, 0), spot = snow ? rgb(190, 215, 240) : rgb(0, 112, 0);
  const int width = 30, height = 15;
  for (int x = 0; x < width; x++) {
    const float u = (x - (width - 1) / 2.0f) / (width / 2.0f);
    const float shape = 1.0f - u * u;
    const int top = base - (int)(height * (shape > 0 ? sqrtf(shape) * 0.95f + 0.05f * shape : 0));
    for (int y = top; y <= base; y++) put(d, left + x, y, y == top || x == 0 || x == width - 1 ? dim(edge, level) : dim(fill, level));
  }
  // Two eyes, as hills have
  box(d, left + 12, base - 9, 1, 3, dim(eye, level));
  box(d, left + 17, base - 9, 1, 3, dim(eye, level));
  box(d, left + 7, base - 4, 2, 1, dim(spot, level));
  box(d, left + 21, base - 4, 2, 1, dim(spot, level));
}

// The Subcon jar of Super Luigi Bros. 2: red with white bands
void drawJar(MatrixPanel_I2S_DMA &d) {
  const uint16_t red = rgb(200, 40, 32), light = rgb(232, 80, 64), dark = rgb(140, 24, 16), white = rgb(248, 244, 238);
  const int j = kJarTop;
  box(d, 50, j, 14, 3, red); box(d, 50, j, 14, 1, white); box(d, 50, j + 2, 14, 1, dark);
  box(d, 52, j + 3, 10, 3, red); box(d, 50, j + 6, 14, 10, red);
  box(d, 50, j + 6, 2, 10, light); box(d, 62, j + 6, 2, 10, dark);
  box(d, 50, j + 8, 14, 1, white); box(d, 50, j + 10, 14, 1, white);
  for (int x = 52; x < 62; x += 4) { put(d, x, j + 12, white); put(d, x + 1, j + 13, white); put(d, x, j + 14, white); put(d, x + 2, j + 12, white); }
  box(d, 51, j + 15, 12, 1, dark);
}

// A Shy Guy rises out of the jar, bobs about for a while and ducks back in every twelve seconds
void drawJarAndShyGuy(MatrixPanel_I2S_DMA &d, uint32_t now) {
  static const char *const SHY[10] = {"..rrrrrr..", ".rrrrrrrr.", "rrwwwwwwrr", "rrwkwwkwrr", "rrwwwwwwrr", "rrwwkkwwrr", ".rrrwwrrr.", "..rrrrrr..", "..rrrrrr..", "..yy..yy.."};
  static const Swatch PAL[] = {{'r', rgb(216, 40, 24)}, {'w', rgb(244, 244, 244)}, {'k', rgb(16, 16, 16)}, {'y', rgb(240, 160, 32)}};
  const int t = now % 12000;
  int rise = 0;
  if (t >= 3000 && t < 4200) rise = 10 * ease(t - 3000, 1200) / 1000;
  else if (t >= 4200 && t < 7500) rise = 10;
  else if (t >= 7500 && t < 8700) rise = 10 - 10 * ease(t - 7500, 1200) / 1000;
  if (rise > 0) sprite(d, SHY, 10, 52 + (rise == 10 ? (int)lroundf(1.5f * sinf(now / 260.0f)) : 0), kJarTop + 1 - rise, PAL, 4);
  drawJar(d);
}

// Bushes beside the hill
void drawBushes(MatrixPanel_I2S_DMA &d, bool dark, bool snow) {
  const int level = dark ? 80 : 255;
  const uint16_t fill = dim(snow ? rgb(236, 246, 255) : rgb(24, 172, 32), level), shade = dim(snow ? rgb(190, 215, 240) : rgb(10, 96, 16), level);
  for (const int x : {35, 41}) {
    for (int i = 0; i < 3; i++) { const int cx = x + 3 + i * 5; for (int y = -3; y <= 3; y++) for (int dx = -3; dx <= 3; dx++) if (dx * dx + y * y <= 10) put(d, cx + dx, kGround - 5 + y - (i == 1), fill); }
    box(d, x, kGround - 5, 16, 4, fill); box(d, x + 2, kGround - 2, 12, 1, shade);
  }
}

// Luigi: Mario's head under a taller cap dome, then a longer, slimmer body and legs (22 rows tall, 13 columns wide)
const char *const kLuigiDome = ".....rrr.....";
const char *const kLuigiTorsoStand[7] = {"...rrurrrr...", "...rrurrurr..", "..rrruuuurrr.", "..ssruyuuyurs", "..sssuuuuuss.", "....uuuuuuu..", "....uuuuuuu.."};
const char *const kLuigiTorsoJump[7] = {"ss.rrurrrr.ss", "sss.rurrur.ss", "sssruuuuuurs.", ".s.ruyuuyur..", "...uuuuuuuu..", "....uuuuuuu..", "....uuuuuuu.."};
const char *const kLuigiLegsStand[7] = {"...uuu.uuu...", "...uuu.uuu...", "...uuu.uuu...", "...uuu.uuu...", "...uuu.uuu...", "..bbbb.bbbb..", "..bbbbb.bbbbb"};
const char *const kLuigiLegsWalk1[7] = {"...uuuuuuu...", "...uuuuuuu...", "...uuuuuuu...", "....uuuuuu...", "...uuuuuuubb.", "..bbbb.uuubbb", "..bbb...bbbb."};
const char *const kLuigiLegsWalk2[7] = {"...uuuuuuu...", "...uuuuuuu...", "...uuuuuuu...", "...uuu.uuu...", "..bbuu..uuu..", "..bbbb...bbb.", "...bbb..bbbb."};
const char *const kLuigiLegsJump[7] = {"...uuu.uuu...", "...uuu.uuu...", "...uuu.uuu...", "...uuu.uuuu..", "..uuu...uuuu.", "bbbb.....bbb.", "bbb.......bb."};

void drawLuigi(MatrixPanel_I2S_DMA &d, int x, int y, Pose pose, bool faceLeft, const PlumberColors &c) {
  const char *rows[22];
  int n = 0;
  rows[n++] = kLuigiDome;
  for (const char *r : plumber::HEAD) rows[n++] = r;
  const char *const *torso = pose == Pose::Jump ? kLuigiTorsoJump : kLuigiTorsoStand;
  for (int i = 0; i < 7; i++) rows[n++] = torso[i];
  const char *const *legs = pose == Pose::Walk1 ? kLuigiLegsWalk1 : pose == Pose::Walk2 ? kLuigiLegsWalk2 : pose == Pose::Jump ? kLuigiLegsJump : kLuigiLegsStand;
  for (int i = 0; i < 7; i++) rows[n++] = legs[i];
  const Swatch palette[] = {{'r', c.cap}, {'s', c.skin}, {'b', c.hair}, {'u', c.overalls}, {'y', c.button}, {'k', c.eye}};
  sprite(d, rows, n, x, y, palette, 6, faceLeft);
}

void drawGoomba(MatrixPanel_I2S_DMA &d, int x, int y, bool squashed, bool step, const Theme &t) {
  static const char *const WALK[12] = {"....bbbb....", "..bbbbbbbb..", ".bbbbbbbbbb.", "bbwwbbbbwwbb", "bwwwwbbwwwwb", "bwwkwbbwkwwb",
                                       "bbwwwbbwwwbb", "bbbbbbbbbbbb", "..bbssssbb..", "...ssssss...", "dddd....dddd", ".dddd..dddd."};
  static const char *const SQUASH[5] = {"..bbbbbbbb..", ".bbwwbbwwbb.", "bbbkkbbkkbbb", "..ssssssss..", "dddddddddddd"};
  const Swatch pal[] = {{'b', t.goomba}, {'w', rgb(255, 255, 255)}, {'k', rgb(16, 8, 8)}, {'s', t.goombaFoot}, {'d', rgb(80, 40, 8)}};
  if (squashed) { sprite(d, SQUASH, 5, x, y + 7, pal, 5); return; }
  static const char *rows[12];
  for (int i = 0; i < 12; i++) rows[i] = WALK[i];
  if (step) { rows[10] = ".dddd..dddd."; rows[11] = "dddd....dddd"; }
  sprite(d, rows, 12, x, y, pal, 5);
}

// A round coin, 6 tall; it turns by narrowing, so the widest it gets is a circle. cx is the line through its middle.
void drawKoopa(MatrixPanel_I2S_DMA &d, int x, int y, bool shell, bool step) {
  static const char *const WALK[11] = {"yyy.........", "yywy..gggg..", "ykyyy.gllgg.", "yyyyygggllgg", ".yyyyggggggg", ".yyyyglgglgg",
                                       "..yyyggggggg", "..cccccccccc", "..cccccccc..", "..oo....oo..", ".ooo...ooo.."};
  static const char *const SHELL[5] = {"...gggggg...", "..gllglglg..", ".ggllgllggg.", ".cccccccccc.", "..cccccccc.."};
  static const Swatch PAL[] = {{'y', rgb(252, 200, 56)}, {'w', rgb(255, 255, 255)}, {'k', rgb(16, 8, 8)}, {'g', rgb(0, 168, 0)},
                               {'l', rgb(128, 224, 40)}, {'c', rgb(252, 224, 168)}, {'o', rgb(232, 120, 24)}};
  if (shell) { sprite(d, SHELL, 5, x, y, PAL, 7); return; }
  static const char *rows[11];
  for (int i = 0; i < 11; i++) rows[i] = WALK[i];
  if (step) { rows[9] = ".oo....oo..."; rows[10] = "ooo...ooo..."; }
  sprite(d, rows, 11, x, y, PAL, 7);
}

void drawMushroom(MatrixPanel_I2S_DMA &d, int x, int y) {
  static const char *const ROWS[8] = {"..gggg..", ".ggwwgg.", "ggwwwwgg", "gwwggwwg", "gggggggg", ".ssssss.", ".ssksks.", "..ssss.."};
  static const Swatch PAL[] = {{'g', rgb(0, 200, 40)}, {'w', rgb(255, 255, 255)}, {'s', rgb(252, 224, 168)}, {'k', rgb(16, 8, 8)}};
  sprite(d, ROWS, 8, x, y, PAL, 4);
}

// The noon flagpole: the flag rides down it with Luigi (0 = top, 1000 = bottom)
void drawFlagpole(MatrixPanel_I2S_DMA &d, int slide) {
  static const char *const FLAG[5] = {".....ww", "...wwww", ".wwgwww", "...wwww", ".....ww"};
  static const Swatch PAL[] = {{'w', rgb(255, 255, 255)}, {'g', rgb(0, 168, 0)}};
  box(d, kPoleX, kPoleTop, 1, kGround - kPoleTop, rgb(96, 208, 64));
  box(d, kPoleX - 1, kPoleTop - 3, 3, 3, rgb(0, 168, 0)); put(d, kPoleX - 1, kPoleTop - 3, rgb(128, 224, 40));
  box(d, kPoleX - 2, kGround - 5, 5, 5, rgb(200, 76, 12)); box(d, kPoleX - 2, kGround - 3, 5, 1, rgb(40, 8, 0));
  sprite(d, FLAG, 5, kPoleX - 7, kPoleTop + 24 * slide / 1000, PAL, 2);
}

void drawCoin(MatrixPanel_I2S_DMA &d, int cx, int y, int width) {
  const uint16_t gold = rgb(255, 200, 32), light = rgb(255, 250, 170), edge = rgb(200, 112, 0), slot = rgb(230, 150, 0);
  for (int row = 0; row < 6; row++) {
    const float v = (row - 2.5f) / 3.0f;
    float half = width / 2.0f * sqrtf(1.0f - v * v);
    if (half < 1.0f) half = 1.0f;
    int left = 0, right = -1;
    for (int px = -3; px < 3; px++) {
      if (fabsf(px + 0.5f) >= half) continue;
      if (right < left) left = px;
      right = px;
    }
    for (int px = left; px <= right; px++) {
      uint16_t c = gold;
      if (row == 0 || row == 5 || px == right) c = edge;
      else if (px == left) c = light;
      else if (width >= 5 && px == -1) c = slot;
      put(d, cx + px, y + row, c);
    }
  }
}

// width of a spinning coin over time: 6, 5, 3, 2, 3, 5
int coinWidth(uint32_t phase) {
  static const uint8_t W_[6] = {6, 5, 3, 2, 3, 5};
  return W_[(phase / 170) % 6];
}

// A patch of brick wall in the background, dimmed so it reads as scenery. The courses are counted up from the bottom edge,
// so a wall standing on the floor ends in a mortar line right above the floor stones, and its joints are offset from theirs.
void drawWall(MatrixPanel_I2S_DMA &d, int x0, int y0, int w, int h, const Theme &t, int level) {
  for (int y = 0; y < h; y++) {
    const int fromBottom = h - 1 - y;
    const int shift = ((fromBottom > 0 ? (fromBottom - 1) / 6 : 0) % 2 == 0) ? 4 : 0;  // the course above the floor is staggered half a stone
    for (int x = 0; x < w; x++) {
      const bool mortar = fromBottom % 6 == 0 || (x0 + x + shift) % 8 == 7;
      put(d, x0 + x, y0 + y, dim(mortar ? t.mortar : t.brick, level));
    }
  }
}

// An arched castle window with a torch beside it
void drawCastleDecor(MatrixPanel_I2S_DMA &d, uint32_t now, int windowX, int torchX) {
  box(d, windowX, 24, 6, 11, rgb(0, 0, 0)); box(d, windowX + 1, 22, 4, 2, rgb(0, 0, 0)); box(d, windowX + 2, 21, 2, 1, rgb(0, 0, 0));
  box(d, torchX, 33, 2, 4, rgb(100, 52, 8));
  const int flick = (now / 90 + torchX) % 3;
  box(d, torchX, 30 - (flick == 0), 2, 3 + (flick == 0), rgb(255, 144, 32));
  put(d, torchX, 29 - (flick == 0), rgb(255, 224, 96)); put(d, torchX + 1, 30, rgb(255, 224, 96));
}

void drawFlakes(MatrixPanel_I2S_DMA &d, uint32_t now) {
  for (int i = 0; i < 14; i++) {
    const int y = (hash(i * 11 + 3) % 52 + (int)(now * (6 + i % 4) / 1000)) % 52;
    const int x = (hash(i * 5 + 2) % 64 + (int)(2.0f * sinf(now / 700.0f + i))) & 63;
    put(d, x, y, rgb(255, 255, 255));
  }
}

// ---- The scenery of the newer worlds ----

void disc(MatrixPanel_I2S_DMA &d, int cx, int cy, int r, uint16_t c) {
  for (int y = -r; y <= r; y++)
    for (int x = -r; x <= r; x++)
      if (x * x + y * y <= r * r + r / 2) put(d, cx + x, cy + y, c);
}

// Deep water: light shafts, swaying seaweed, rising bubbles and a Blooper pulsing past
void drawUnderwater(MatrixPanel_I2S_DMA &d, uint32_t now) {
  box(d, 0, 0, W, 20, rgb(16, 72, 192));
  for (int i = 0; i < 5; i++) box(d, 6 + i * 13 + (int)((now / 1500) % 2), 0, 3, kGround, rgb(24, 80, 200));
  for (const int x : {4, 34, 44})
    for (int y = 0; y < 14; y++) put(d, x + 1 + (int)lroundf(sinf(y / 2.0f + now / 400.0f)), kGround - 1 - y, rgb(32, 184, 88));
  for (int i = 0; i < 7; i++) {
    const int y = 50 - (int)((now * 8 / 1000 + hash(i * 3 + 9) % 48) % 48);
    box(d, hash(i * 5 + 1) % 60 + 2 + (int)lroundf(sinf(now / 500.0f + i)), y, 2, 2, rgb(168, 216, 255));
  }
  static const char *const BLOOPER[9] = {"..wwww..", ".wwwwww.", "wwkwwkww", "wwwwwwww", ".wwwwww.", "..wwww..", ".w.ww.w.", "w..ww..w", "w.w..w.w"};
  static const Swatch PAL[] = {{'w', rgb(244, 244, 244)}, {'k', rgb(32, 32, 32)}};
  sprite(d, BLOOPER, 9, 44 + (int)lroundf(8 * sinf(now / 1800.0f)), 22 + (int)lroundf(5 * sinf(now / 700.0f)), PAL, 2);
}

// Bright sky, drifting clouds and floating coins
void drawCloudTop(MatrixPanel_I2S_DMA &d, uint32_t now, bool dark) {
  drawCloud(d, 62 - (int)((now / 2600) % 80), 7, dark);
  drawCloud(d, 40 - (int)((now / 3300) % 80) + 60, 31, dark);
  drawCloud(d, 10 - (int)((now / 4100) % 80) + 60, 22, dark);
  for (const int y : {26, 36}) {
    drawCoin(d, 59, y + (int)(sinf(now / 500.0f + y) * 1.5f + 1.5f), coinWidth(now + y * 20));
    drawCoin(d, 5, y + 4 + (int)(sinf(now / 500.0f + y + 2) * 1.5f + 1.5f), coinWidth(now + y * 20 + 300));
  }
}

// An arched window in the wall with the night outside: a small crescent and a couple of stars
void drawGhostWindow(MatrixPanel_I2S_DMA &d) {
  const int x = 1, y = 7, w = 10, h = 18;
  const uint16_t frame = rgb(120, 96, 148), glass = rgb(12, 12, 44);
  box(d, x, y + 2, w, h - 2, frame); box(d, x + 2, y, w - 4, 2, frame);  // the arched stone frame
  box(d, x + 1, y + 3, w - 2, h - 3, glass); box(d, x + 2, y + 1, w - 4, 2, glass);
  box(d, x + w / 2 - 1, y + 3, 2, h - 3, glass);  // (keeps the glass one piece)
  for (int dy = -3; dy <= 3; dy++)
    for (int dx = -3; dx <= 3; dx++) {
      if (dx * dx + dy * dy > 9) continue;
      if ((dx - 2) * (dx - 2) + (dy - 1) * (dy - 1) <= 6) continue;
      put(d, x + 5 + dx - 1, y + 7 + dy, rgb(255, 246, 190));
    }
  put(d, x + 2, y + 13, rgb(200, 210, 255)); put(d, x + 7, y + 4, rgb(200, 210, 255)); put(d, x + 6, y + 14, rgb(150, 160, 220));
  box(d, x - 1, y + h, w + 2, 2, frame);  // the sill
}

// A haunted wall, a window with the moon in it and two Boos drifting about
void drawGhostHouse(MatrixPanel_I2S_DMA &d, uint32_t now, const Theme &t) {
  Theme wall = t;
  wall.brick = rgb(52, 32, 76); wall.mortar = rgb(32, 16, 52);
  drawWall(d, 0, 0, W, kGround, wall, 255);
  drawGhostWindow(d);
  static const char *const BOO[9] = {"..wwwww..", ".wwwwwww.", "wwkwwwkww", "wwkwwwkww", "wwwwwwwww", "wwwrrrwww", "wwwrrrwww", ".wwwwwww.", "..w...w.."};
  const Swatch near[] = {{'w', rgb(244, 240, 255)}, {'k', rgb(32, 32, 64)}, {'r', rgb(232, 64, 96)}};
  const Swatch far[] = {{'w', rgb(168, 152, 208)}, {'k', rgb(32, 32, 64)}, {'r', rgb(160, 64, 96)}};
  sprite(d, BOO, 9, 51 + (int)lroundf(2 * sinf(now / 1800.0f)), 26 + (int)lroundf(2 * sinf(now / 1100.0f)), far, 3);
  sprite(d, BOO, 9, 38 + (int)lroundf(3 * sinf(now / 1500.0f)), 24 + (int)lroundf(2 * sinf(now / 900.0f + 1)), near, 3);
}

// A red sky over a glowing volcano, with embers drifting up
void drawVolcano(MatrixPanel_I2S_DMA &d, uint32_t now) {
  static const uint16_t BANDS[6] = {rgb(48, 8, 8), rgb(64, 12, 8), rgb(80, 16, 8), rgb(96, 20, 8), rgb(112, 24, 8), rgb(128, 32, 8)};
  for (int i = 0; i < 6; i++) box(d, 0, i * 9, W, 9, BANDS[i]);
  box(d, 0, 54, W, 10, BANDS[5]);
  for (int x = 0; x < 30; x++) {
    const float u = (x - 14.5f) / 15.0f, shape = 1.0f - u * u;
    const int top = kGround - 1 - (int)lroundf(24 * (shape > 0 ? powf(shape, 0.7f) : 0));
    box(d, 22 + x, top, 1, kGround - top, rgb(28, 12, 12)); put(d, 22 + x, top, rgb(60, 28, 28));
  }
  box(d, 33, 28, 6, 2, rgb(255, 112, 24)); box(d, 34, 27, 4, 1, rgb(255, 192, 64));
  box(d, 35, 30, 1, 10, rgb(232, 64, 16)); box(d, 31, 34, 1, 8, rgb(232, 64, 16)); box(d, 40, 36, 1, 10, rgb(232, 64, 16));
  for (int i = 0; i < 14; i++) {
    const int y = 50 - (int)((now * 10 / 1000 + hash(i * 3 + 9) % 50) % 50);
    put(d, hash(i * 5 + 1) % 64, y, (now / 140 + i) % 2 ? rgb(255, 176, 48) : rgb(255, 112, 24));
  }
}

// The lava ball that leaps out of the lava every few seconds (drawn after the ground)
void drawPodoboo(MatrixPanel_I2S_DMA &d, uint32_t now) {
  const uint32_t t = now % 4000;
  if (t >= 1400) return;
  static const char *const BALL[6] = {"..oo..", ".oyyo.", "oyyyyo", "oyyyyo", ".oyyo.", "..oo.."};
  static const Swatch PAL[] = {{'o', rgb(255, 96, 16)}, {'y', rgb(255, 208, 64)}};
  const float p = t / 1400.0f;
  sprite(d, BALL, 6, 12, 60 - (int)(26 * 4 * p * (1 - p)), PAL, 2);
}

// A dusk sky over the deck: a mast with a sail, and a Bullet Bill flying by
void drawAirship(MatrixPanel_I2S_DMA &d, uint32_t now) {
  box(d, 0, 30, W, 22, rgb(122, 96, 120)); box(d, 0, 44, W, 8, rgb(140, 106, 116));
  for (const int x : {4, 44}) { box(d, x, 36, 14, 2, rgb(154, 132, 152)); box(d, x + 2, 35, 10, 1, rgb(154, 132, 152)); }
  box(d, 6, 20, 2, 32, rgb(108, 60, 16)); box(d, 7, 20, 1, 32, rgb(156, 92, 32));  // the mast
  box(d, 8, 30, 10, 14, rgb(224, 216, 200)); box(d, 8, 30, 10, 1, rgb(255, 255, 255));
  for (int i = 0; i < 13; i++) put(d, 8 + i / 3, 31 + i, rgb(160, 152, 136));
  box(d, 52, 42, 10, 10, rgb(48, 48, 56)); box(d, 50, 44, 3, 6, rgb(32, 32, 40)); box(d, 53, 43, 8, 1, rgb(88, 88, 92));
  const uint32_t t = now % 6000;
  if (t < 700) disc(d, 48 - (int)(t / 120), 46 - (int)(t / 220), 1 + (int)(t / 250), rgb(216, 216, 224));  // the cannon's smoke
  if (t < 2900) {
    static const char *const BILL[6] = {".kkkk...", "kkkkkkk.", "kkwkkkkk", "kkkkkkkk", "kkkkkkk.", ".kkkk..."};
    static const Swatch PAL[] = {{'k', rgb(32, 32, 40)}, {'w', rgb(255, 255, 255)}};
    sprite(d, BILL, 6, 66 - (int)(t * 26 / 1000), 32, PAL, 2);
  }
}

// The 7x7 digits of the Super Luigi font
const char *const GLYPHS[10][7] = {
  {".#####.", "##..###", "##..###", "##..###", "##..###", "##..###", ".#####."},
  {"..###..", ".####..", "..###..", "..###..", "..###..", "..###..", ".#####."},
  {".#####.", "##..###", "....###", "..####.", ".###...", "###....", "#######"},
  {".######", "....##.", "..###..", "....##.", "....###", "##..###", ".#####."},
  {"...###.", "..####.", ".##.##.", "##..##.", "#######", "....##.", "....##."},
  {"######.", "##.....", "######.", "....###", "....###", "##..###", ".#####."},
  {".#####.", "##.....", "######.", "##..###", "##..###", "##..###", ".#####."},
  {"#######", "##..###", "....##.", "...##..", "..##...", "..##...", "..##..."},
  {".#####.", "##..###", "##..###", ".#####.", "##..###", "##..###", ".#####."},
  {".#####.", "##..###", "##..###", ".######", "....###", "....###", ".#####."}};

// First and last column that has ink in a digit (the "1" is narrower than the "0")
void inkBounds(int digit, int &first, int &last) {
  first = 7; last = 0;
  for (int row = 0; row < 7; row++)
    for (int col = 0; col < 7; col++)
      if (GLYPHS[digit % 10][row][col] == '#') { if (col < first) first = col; if (col > last) last = col; }
}

// The castle at the end of the level: battlements, a tower with a flag that rises, an arched door
void drawCastle(MatrixPanel_I2S_DMA &d, int flagRise) {
  const uint16_t brick = rgb(200, 76, 12), mortar = rgb(40, 8, 0), light = rgb(252, 152, 56);
  const int x = kCastleX;
  box(d, x, 36, 14, kGround - 36, brick); box(d, x, 36, 14, 1, light);
  for (int y = 41; y < kGround; y += 5) box(d, x, y, 14, 1, mortar);
  for (int c = 0; c < 14; c += 3) box(d, x + c, 34, 2, 2, brick);  // battlements
  box(d, x + 4, 28, 6, 8, brick); box(d, x + 4, 28, 6, 1, light);  // the tower
  for (int c = 4; c < 10; c += 3) box(d, x + c, 26, 2, 2, brick);
  box(d, x + 5, 31, 1, 3, rgb(0, 0, 0)); box(d, x + 8, 31, 1, 3, rgb(0, 0, 0));  // windows
  box(d, x + 5, kGround - 8, 4, 8, rgb(0, 0, 0)); box(d, x + 6, kGround - 9, 2, 1, rgb(0, 0, 0));  // the door
  box(d, x + 7, 14, 1, 12, rgb(96, 208, 64));  // flagpole on the tower, the flag rides up it
  static const char *const FLAG[4] = {"gggggg", ".ggggg", "..gggg", "...ggg"};
  static const Swatch PAL[] = {{'g', rgb(0, 200, 60)}};
  sprite(d, FLAG, 4, x + 8, 22 - flagRise, PAL, 1);
}

// Firework bursts one after the other, mostly in the open sky below the blocks: {start ms, x, y, colour}
void drawFireworks(MatrixPanel_I2S_DMA &d, int32_t age) {
  static const struct { int start, x, y; uint16_t color; } BURSTS[5] = {
    {0, 22, 40, rgb(255, 232, 32)}, {450, 6, 27, rgb(255, 40, 40)}, {900, 36, 41, rgb(255, 255, 255)},
    {1350, 60, 27, rgb(255, 150, 0)}, {1800, 12, 43, rgb(255, 64, 224)}};
  for (const auto &b : BURSTS) {
    const int32_t t = age - b.start;
    if (t < 0 || t > 1100) continue;
    const int radius = 2 + 9 * ease(t < 700 ? t : 700, 700) / 1000;
    const int level = t < 600 ? 255 : 255 - (t - 600) * 255 / 500;  // full colour first, then it fades into the sky
    const uint16_t c = mix(rgb(88, 160, 252), b.color, level);
    for (int k = 0; k < 12; k++) {
      const float a = k * 0.5236f;
      box(d, b.x + (int)lroundf(radius * cosf(a)), b.y + (int)lroundf(radius * sinf(a)), 2, 2, c);  // the spark
      put(d, b.x + (int)lroundf((radius - 4) * cosf(a + 0.2618f)), b.y + (int)lroundf((radius - 4) * sinf(a + 0.2618f)), c);
    }
    if (t < 200) box(d, b.x - 1, b.y - 1, 3, 3, rgb(255, 255, 255));
  }
}

// Brick pillar and a row of coins for the underground
void drawWallAndCoins(MatrixPanel_I2S_DMA &d, const Theme &t) {
  drawWall(d, 0, 0, W, kGround, t, 38);  // the cave wall, dim, ending on the floor stones
  for (int x = 14; x <= 34; x += 5) { box(d, x, kGround - 6, 4, 4, rgb(255, 200, 32)); box(d, x + 1, kGround - 5, 2, 2, rgb(200, 112, 0)); }
}

void drawDigit(MatrixPanel_I2S_DMA &d, int digit, int x, int y, uint16_t color) {
  for (int row = 0; row < 7; row++)
    for (int col = 0; col < 7; col++)
      if (GLYPHS[digit % 10][row][col] == '#') put(d, x + col, y + row, color);
}

// A flat orange block like the Clockwise one: brown top and left edge, black bottom and right edge, a rivet in each corner,
// two black digits (one, centred, when the tens are hidden)
void drawBlock(MatrixPanel_I2S_DMA &d, int x, int y, int tens, int units, bool cave) {
  const int n = kBlockSize;
  const uint16_t body = cave ? rgb(210, 140, 30) : rgb(231, 156, 33), edge = rgb(156, 73, 0), black = rgb(0, 0, 0);
  box(d, x, y, n, n, black);
  box(d, x, y, n - 1, n - 1, edge);
  box(d, x + 1, y + 1, n - 2, n - 2, body);
  box(d, x + 2, y + 2, 2, 2, black); box(d, x + n - 4, y + 2, 2, 2, black);
  box(d, x + 2, y + n - 4, 2, 2, black); box(d, x + n - 4, y + n - 4, 2, 2, black);
  // Centre what is actually inked, so the margin left and right matches whatever digits are shown
  int firstA, lastA, firstB, lastB;
  if (tens >= 0) {
    inkBounds(tens, firstA, lastA); inkBounds(units, firstB, lastB);
    // 8 px between the digit cells; one more when that is what makes the margins left and right come out equal
    int pitch = 8;
    if ((n - (pitch + lastB - firstA + 1)) % 2) pitch = 9;
    const int start = (n - (pitch + lastB - firstA + 1)) / 2 - firstA;
    drawDigit(d, tens, x + start, y + 6, black);
    drawDigit(d, units, x + start + pitch, y + 6, black);
  } else {
    inkBounds(units, firstB, lastB);
    drawDigit(d, units, x + (n - (lastB - firstB + 1)) / 2 - firstB, y + 6, black);
  }
}

// How far a block is lifted: it jumps up three pixels and settles again
int bumpOffset(const Block &b, uint32_t now) {
  const int32_t t = (int32_t)(now - b.bumpAt);
  if (t < 0 || t > 380) return 0;
  return -(int)(3.2f * sinf(3.14159f * t / 380.0f) + 0.5f);
}

// ---- The stroller ----

// Whether the clock shows other digits than the blocks are heading for
bool minutesBehind(ClockTime *c) { return blocks[1].target[0] != c->getMinute() / 10 || blocks[1].target[1] != c->getMinute() % 10; }
bool hoursBehind(ClockTime *c) { return blocks[0].target[0] != c->getHour() / 10 || blocks[0].target[1] != c->getHour() % 10; }

void startHit(Plumber &m, uint32_t now) { m.mode = Mode::Hit; m.since = now; m.touched = false; }

bool free0(const Plumber &m) { return m.mode == Mode::Walk || m.mode == Mode::Pause; }

float jumpHeight(uint32_t t, uint32_t total, int height) {
  const float p = (float)t / total;
  return p >= 1 ? 0 : height * 4 * p * (1 - p);
}

void step(ClockTime *clock, uint32_t now, float dt) {
  const int32_t ms = msOfMinute(clock);
  const int32_t minute = minuteOfDay(clock);
  Plumber &m = mario;

  // A Goomba (minute 1 of 3) or a Koopa (minute 2 of 3) comes by in the second half of the minute
  if (goomba.state == 0 && minute % 3 != 0 && ms >= 16000 && ms < 36000 && goomba.spawnedMinute != minute && mario.x < 30 && free0(mario)) {
    goomba = {66, 1, now, minute, 0, minute % 3 == 2};
  }
  if (goomba.state == 1) {
    goomba.x -= 8 * dt;
    if (goomba.x < -14) goomba.state = 0;
  } else if (goomba.state == 2 && goomba.koopa) {
    // The shell is kicked off to the right
    if (now - goomba.since > 400) goomba.x += 70 * dt;
    if (goomba.x > 66 || now - goomba.since > 2500) goomba.state = 0;
  } else if (goomba.state == 2 && now - goomba.since > 900) {
    goomba.state = 0;
  }

  const bool free = m.mode == Mode::Walk || m.mode == Mode::Pause;
  if (free && goomba.state == 1 && goomba.x > m.x + 13 && goomba.x < 62) {
    m.dir = 1;
    const float gap = goomba.x - (m.x + 13);
    if (gap <= 14) {
      // Land on it: it walks on while Luigi is in the air
      const float seconds = 0.56f;
      const float landing = goomba.x - 8 * seconds;
      m.fromX = m.x; m.vx = (landing - 2 - m.x) / seconds;
      m.mode = Mode::Stomp; m.since = now;
    }
  }
  // At noon: the finale
  if (free && goomba.state == 0 && finaleStage == FinaleStage::None && !finaleWanted &&
      ((clock->getHour24() == 12 && clock->getMinute() == 0 && flagDay != clock->getDay()) || demoFlagWanted)) {
    flagDay = clock->getDay();
    demoFlagWanted = false;
    finaleWanted = true;  // update() wipes to the finale scene, then Luigi runs
  }
  if (free && m.mode != Mode::Stomp && ms >= 52000 && m.approachedMinute != minute && goomba.state == 0) {
    m.approachedMinute = minute;
    m.both = clock->getMinute() == 59;  // the hour turns too
    m.hitX = m.both ? kHitBothX : kHitMinutesX;
    m.catchUp = false;
    m.mode = Mode::Approach;
  }
  // The clock moved on without a hit (a Goomba in the way, the time was set): go and hit the blocks anyway
  static uint32_t behindSince = 0;
  if (!(minutesBehind(clock) || hoursBehind(clock))) behindSince = 0;
  else if (behindSince == 0) behindSince = now;
  if (free && goomba.state == 0 && behindSince && now - behindSince > 1500 && m.mode != Mode::Stomp) {
    m.catchUp = true;
    m.both = hoursBehind(clock);
    m.hitX = m.both ? kHitBothX : kHitMinutesX;
    m.mode = Mode::Approach;
    behindSince = now;
  }

  switch (m.mode) {
    case Mode::Walk:
      m.x += m.dir * 7.0f * dt; m.distance += 7.0f * dt;
      if (goomba.state == 1) {
        // Waiting for it, facing it
        if (m.x >= 30) { m.x = 30; m.mode = Mode::Pause; m.since = now; m.pauseMs = 30000; }
        break;
      }
      if (m.x >= kMaxX) { m.x = kMaxX; m.dir = -1; m.mode = Mode::Pause; m.since = now; m.pauseMs = 1200 + hash(minute + (int)now / 7000) % 1500; }
      else if (m.x <= kMinX) { m.x = kMinX; m.dir = 1; m.mode = Mode::Pause; m.since = now; m.pauseMs = 900 + hash(minute * 3 + (int)now / 9000) % 1800; }
      break;
    case Mode::Pause:
      if (now - m.since >= m.pauseMs) m.mode = Mode::Walk;
      break;
    case Mode::Approach: {
      if (!m.catchUp && ms < 40000) { m.mode = Mode::Walk; break; }
      const float speed = 9.5f;
      m.dir = m.hitX > m.x ? 1 : -1;
      m.x += m.dir * speed * dt; m.distance += speed * dt;
      if ((m.dir > 0 && m.x >= m.hitX) || (m.dir < 0 && m.x <= m.hitX)) { m.x = m.hitX; m.dir = 1; m.mode = Mode::Wait; }
      break;
    }
    case Mode::Wait:
      if (m.catchUp) { startHit(m, now); break; }
      if (ms < 40000) { m.mode = Mode::Pause; m.dir = -1; m.since = now; m.pauseMs = 300; break; }  // the minute turned before he got there
      // Start the jump so that his head meets the block as the minute turns
      if (ms >= 60000 - kHitJumpMs / 2 && ms < 60000) startHit(m, now);
      break;
    case Mode::Hit: {
      const uint32_t t = now - m.since;
      // Head against the block: now the digits change (once the clock has turned, or soon after anyway)
      if (!m.touched && t >= (uint32_t)kHitJumpMs / 2 && (minutesBehind(clock) || t >= (uint32_t)kHitJumpMs * 3 / 4)) {
        m.touched = true;
        touch = {true, m.both};
      }
      if (t >= (uint32_t)kHitJumpMs) { m.mode = Mode::Pause; m.dir = -1; m.since = now; m.pauseMs = 500; m.catchUp = false; }
      break;
    }
    case Mode::Stomp: {
      const uint32_t t = now - m.since;
      m.x = m.fromX + m.vx * t / 1000.0f;
      if (t >= 560) {
        goomba.state = 2; goomba.since = now; goomba.x = m.x + 2; goomba.score = goomba.koopa ? 200 : 100;
        m.mode = Mode::Hop; m.since = now;
      }
      break;
    }
    case Mode::FlagRun:
      m.x += 22 * dt; m.distance += 22 * dt;
      if (m.x >= kLuigiPoleX) { m.x = kLuigiPoleX; m.mode = Mode::FlagJump; m.since = now; }
      break;
    case Mode::FlagJump:
      if (now - m.since >= (uint32_t)kFlagJumpMs) { m.mode = Mode::FlagSlide; m.since = now; }
      break;
    case Mode::FlagSlide:
      if (now - m.since >= (uint32_t)kFlagSlideMs) { m.mode = Mode::FlagWait; m.dir = -1; m.since = now; }
      break;
    case Mode::FlagWait:
      if (now - m.since >= 600) { m.mode = Mode::FinaleWalk; m.dir = 1; }
      break;
    case Mode::FinaleWalk:  // into the castle
      m.x += 22 * dt; m.distance += 22 * dt;
      if (m.x >= kCastleX - 1) { m.x = kCastleX - 1; m.mode = Mode::FinaleHidden; m.since = now; finaleHiddenAt = now; }
      break;
    case Mode::FinaleHidden:  // inside while the flag rises and the fireworks go off
      if (now - m.since >= 4400) m.mode = Mode::FinaleDone;
      break;
    case Mode::FinaleDone: break;
    case Mode::Hop:
      m.x += 6 * dt;
      if (now - m.since >= 320) { m.mode = Mode::Walk; m.dir = 1; }
      break;
  }
  if (m.x < 0) m.x = 0;
  if (m.x > 51) m.x = 51;  // (the finale walk ends at 49)
}

}  // namespace

Clockface::Clockface(MatrixPanel_I2S_DMA* display) { _display = display; }

void Clockface::setNight(bool value) { night = value; }

void Clockface::setup(ClockTime *dateTime) {
  _dateTime = dateTime;
  lastNow = millis();
  mario = Plumber();
  goomba = Goomba();
  touch = {false, false};
  pendingWorld = -1;
  wiping = false;
  mushroomOn = false;
  coins = 0;
  demoFlagWanted = false;
  finaleWanted = false;
  finaleStage = FinaleStage::None;
  primed = false;  // the clock is read at the first frame: the face is set up before the time has synced
}

void Clockface::update() {
  MatrixPanel_I2S_DMA &d = *_display;
  const uint32_t now = millis();
  float dt = (now - lastNow) / 1000.0f;
  if (dt > 0.2f) dt = 0.2f;
  lastNow = now;
  if (!primed) {
    // The first frame comes after the time sync (the boot screens run until then): start from the real time
    primed = true;
    const int h = _dateTime->getHour(), mi = _dateTime->getMinute(), digits[4] = {h / 10, h % 10, mi / 10, mi % 10};
    for (int i = 0; i < 2; i++)
      blocks[i] = {{(int8_t)digits[i * 2], (int8_t)digits[i * 2 + 1]}, {(int8_t)digits[i * 2], (int8_t)digits[i * 2 + 1]}, now - 10000};
    world = demoWorlds() ? (int)Overground : pinnedWorld() >= 0 ? pinnedWorld() : hash(_dateTime->getDay() * 24 + _dateTime->getHour24()) % kWorlds;
    worldSince = now;
    demoFlagDone = false;
    coinDay = _dateTime->getDay();
  }
  const bool nightNow = night && (world == Overground || world == Snow || world == CloudTop);
  Theme tinted;  // Cloud top at night keeps its own ground, dimmed, under the night sky
  const Theme *picked = nightNow ? &kCave : world == Underground ? &kUnder : world == Castle ? &kCastle : world == Snow ? &kSnow
                      : world == Underwater ? &kWater : world == CloudTop || world == Finale ? &kCloud
                      : world == GhostHouse ? &kGhost : world == Lava ? &kLava : world == Airship ? &kShip : &kDay;
  if (nightNow && world == CloudTop) {
    const Theme &base = kCloud;
    tinted = base;
    tinted.sky = kCave.sky; tinted.text = kCave.text; tinted.overground = false;
    tinted.brick = rgb(70, 82, 130);
    tinted.brickLight = rgb(96, 110, 160);
    tinted.mortar = rgb(44, 54, 96);
    tinted.goomba = dim(base.goomba, 90); tinted.goombaFoot = dim(base.goombaFoot, 90);
    picked = &tinted;
  }
  const Theme &theme = *picked;
  const int hour = _dateTime->getHour(), minute = _dateTime->getMinute();
  const int digits[4] = {hour / 10, hour % 10, minute / 10, minute % 10};
  const bool hideTens = !_dateTime->is24hFormat() && hour < 10;

  step(_dateTime, now, dt);

  // Luigi hit the block(s): they take the clock's digits and bump
  if (touch.pending) {
    for (int i = touch.both ? 0 : 1; i < 2; i++) {
      blocks[i].target[0] = digits[i * 2];
      blocks[i].target[1] = digits[i * 2 + 1];
      blocks[i].bumpAt = now;
    }
    // The coin counter; it starts over each day. Both blocks hit means the hour turned: a 1-UP for Luigi
    if (coinDay != _dateTime->getDay()) { coinDay = _dateTime->getDay(); coins = 0; }
    coins = (coins + (touch.both ? 2 : 1)) % 100;
    if (touch.both) {
      mushroomOn = true; mushroomAt = now;
      if (pinnedWorld() < 0 && !demoWorlds()) {
        // A new world, never the same twice in a row: the curtain swaps it in
        pendingWorld = nextWorld(now ^ (_dateTime->getDay() * 24 + _dateTime->getHour24()), night);
        wiping = true; wipeAt = now;
      }
    }
    touch.pending = false;
  }
  if (mushroomOn && now - mushroomAt >= 3700) mushroomOn = false;
  // A world picked while the clock runs (telnet): wipe over to it
  if (pinnedWorld() >= 0 && world != pinnedWorld() && !wiping && finaleStage == FinaleStage::None && world != Finale) { pendingWorld = pinnedWorld(); wiping = true; wipeAt = now; }
  // The demo: Luigi slides down the flagpole in every world, and every 8 seconds it is on to the next world, in order
  if (demoWorlds() && world == Overground && !demoFlagDone && !wiping && finaleStage == FinaleStage::None && now - worldSince >= 1500) { demoFlagWanted = true; demoFlagDone = true; }
  if (demoWorlds() && !wiping && finaleStage == FinaleStage::None && world != Finale && now - worldSince >= 8000) {
    pendingWorld = (world + 1) % kWorlds; demoFlagDone = false; wiping = true; wipeAt = now;
  }
  // Night falls while a bright world is on show: move on to another world right away
  if (night && brightWorld(world) && pinnedWorld() < 0 && !demoWorlds() && !wiping) { pendingWorld = nextWorld(now, true); wiping = true; wipeAt = now; }
  if (wiping) {
    const uint32_t wt = now - wipeAt;
    if (wt >= 300 && pendingWorld >= 0) { world = pendingWorld; pendingWorld = -1; worldSince = now; }
    if (wt >= 600) wiping = false;
  }
  // The noon finale: wipe to its scene, let Luigi play it, wipe back to the world that was on show
  if (finaleWanted && finaleStage == FinaleStage::None && !wiping) {
    finaleWanted = false;
    worldBefore = world;
    pendingWorld = Finale; wiping = true; wipeAt = now;
    finaleStage = FinaleStage::Out;
  } else if (finaleStage == FinaleStage::Out && world == Finale) {
    mario.x = 4; mario.dir = 1; mario.mode = Mode::FlagRun; mario.since = now;  // Luigi starts his run at the left edge
    goomba.state = 0;
    finaleStage = FinaleStage::Playing;
  } else if (finaleStage == FinaleStage::Playing && mario.mode == Mode::FinaleDone && !wiping) {
    pendingWorld = worldBefore; wiping = true; wipeAt = now;
    finaleStage = FinaleStage::Back;
  } else if (finaleStage == FinaleStage::Back && world == worldBefore) {
    mario.x = 18; mario.dir = 1; mario.mode = Mode::Walk;
    finaleStage = FinaleStage::None;
  }
  for (int i = 0; i < 2; i++) {
    if (now >= blocks[i].bumpAt + 150) { blocks[i].shown[0] = blocks[i].target[0]; blocks[i].shown[1] = blocks[i].target[1]; }
  }

  d.fillScreen(theme.sky);

  // Scenery
  switch (world) {
    case Underground: drawWallAndCoins(d, theme); break;
    case Castle: {
      Theme wall = kCastle;
      wall.brick = rgb(44, 20, 24); wall.mortar = rgb(20, 6, 8);
      drawWall(d, 0, 0, W, kGround, wall, 255);
      drawCastleDecor(d, now, 5, 2);
      drawCastleDecor(d, now, 54, 62);
      break;
    }
    case Underwater: drawUnderwater(d, now); break;
    case CloudTop: if (nightNow) drawNightSky(d, now); drawCloudTop(d, now, nightNow); break;
    case GhostHouse: drawGhostHouse(d, now, theme); break;
    case Lava: drawVolcano(d, now); break;
    case Airship: drawAirship(d, now); break;
    case Finale:
      drawCloud(d, 62 - (int)((now / 2600) % 80), 7, false);
      drawCloud(d, 40 - (int)((now / 3300) % 80) + 60, 31, false);
      drawCloud(d, 10 - (int)((now / 4100) % 80) + 60, 22, false);
      if (mario.mode == Mode::FinaleHidden || mario.mode == Mode::FinaleDone) drawFireworks(d, (int32_t)(now - finaleHiddenAt) - 900);
      break;
    default:  // Overground and Snow
      if (nightNow) drawNightSky(d, now);
      drawCloud(d, 62 - (int)((now / 2600) % 80), 7, nightNow);
      drawCloud(d, 20 - (int)((now / 3900) % 90) + 60, 29, nightNow);
      if (world == Snow) drawFlakes(d, now);
      drawHill(d, 1, kGround - 1, nightNow, world == Snow);
      drawBushes(d, nightNow, world == Snow);
      break;
  }
  if (world != CloudTop && world != Airship && world != Finale) drawJarAndShyGuy(d, now);
  const uint16_t snowWhite = nightNow ? rgb(70, 82, 130) : rgb(255, 255, 255);
  if (world == Snow) box(d, 51, kJarTop - 2, 12, 2, snowWhite);  // snow on the jar's rim
  drawBricks(d, kGround, H - 1, theme, true);
  if (world == Castle || world == Lava) {
    box(d, 0, 60, W, 4, rgb(232, 64, 16));
    box(d, 0, 60, W, 1, (now / 160) % 2 ? rgb(255, 176, 48) : rgb(255, 140, 32));
  } else if (world == Snow) {
    box(d, 0, kGround, W, 2, snowWhite);
  } else if (world == CloudTop || world == Finale) {
    box(d, 0, kGround - 2, W, 2, snowWhite);  // the puffs along the top of the cloud ground
    for (int x = 2; x < W; x += 8) { box(d, x, kGround - 3, 5, 1, snowWhite); box(d, x + 1, kGround - 4, 3, 1, snowWhite); }
  } else if (world == Airship) {
    for (int x = 3; x < W; x += 16) { disc(d, x, 56, 1, rgb(60, 32, 8)); put(d, x, 56, rgb(200, 200, 208)); }
  }

  // The date, small, up in the sky like the score line of the game: weekday top left, day and month top right
  {
    static const char *const DAYS[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char *const MONTHS[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    char day[4];
    snprintf(day, sizeof(day), "%d", _dateTime->getDay());
    const char *month = MONTHS[(_dateTime->getMonth() + 11) % 12];
    miniText(d, DAYS[_dateTime->getWeekday() % 7], 1, 1, theme.text);
    // Right-aligned, with 2 px between the day number and the month
    const int monthX = W - 1 - miniWidth(month);
    miniText(d, month, monthX, 1, theme.text);
    miniText(d, day, monthX - 2 - miniWidth(day), 1, theme.text);
  }

  // The coin counter between the weekday and the date
  {
    static const char *const PHASE[3][5] = {{".c.", "cdc", "cdc", "cdc", ".c."}, {".c.", "ccc", "ccc", "ccc", ".c."}, {"...", ".c.", ".c.", ".c.", "..."}};
    const Swatch gold[] = {{'c', rgb(255, 200, 32)}, {'d', rgb(200, 112, 0)}};
    char count[4];
    snprintf(count, sizeof(count), "%02d", coins);
    sprite(d, PHASE[0], 5, 26, 1, gold, 2);  // static: the full face
    miniText(d, count, 30, 1, theme.text);
  }

  // The 1-UP pops out of the hour block (drawn first, so the block hides its start), then turns into the text
  if (mushroomOn) {
    const uint32_t pt = now - mushroomAt;
    const int mx = kBlockX[0] + kBlockSize / 2 - 4;
    if (pt < 1100) drawMushroom(d, mx, kBlockTop - (pt < 500 ? 8 * ease(pt, 500) / 1000 : 8));
    else if (pt < 1800) miniText(d, "1UP", kBlockX[0] + kBlockSize / 2 - miniWidth("1UP") / 2 - 1, 1, rgb(0, 224, 64));
  }

  // The question blocks; the coin pops out from behind each one (so it is drawn first) and drops back in
  for (int i = 0; i < 2; i++) {
    const int lift = bumpOffset(blocks[i], now);
    const int32_t since = (int32_t)(now - blocks[i].bumpAt);
    if (since >= 0 && since < 700) {
      const int p = since < 350 ? ease(since, 350) : 1000 - ease(since - 350, 350);
      drawCoin(d, kBlockX[i] + kBlockSize / 2, kBlockTop - 5 * p / 1000, coinWidth(since * 2));
    }
    drawBlock(d, kBlockX[i], kBlockTop + lift, i == 0 && hideTens ? -1 : blocks[i].shown[0], blocks[i].shown[1], !theme.overground);
  }

  // The Goomba, and its score once it is flat
  if (goomba.state == 1) {
    if (goomba.koopa) drawKoopa(d, (int)goomba.x, kGround - 11, false, (now / 220) % 2);
    else drawGoomba(d, (int)goomba.x, kGround - 12, false, (now / 220) % 2, theme);
  } else if (goomba.state == 2) {
    if (goomba.koopa) drawKoopa(d, (int)goomba.x, kGround - 5, true, false);
    else drawGoomba(d, (int)goomba.x, kGround - 12, true, false, theme);
    if (now - goomba.since < 1000) {
      char score[8];
      snprintf(score, sizeof(score), "%d", goomba.score);
      miniText(d, score, (int)goomba.x - 2, kGround - 16 - (int)((now - goomba.since) / 110), rgb(255, 255, 255));
    }
  }

  const bool flagging = world == Finale && mario.mode != Mode::Walk && mario.mode != Mode::Pause;
  if (flagging) {
    const uint32_t ft = now - mario.since;
    drawFlagpole(d, mario.mode == Mode::FlagSlide ? (int)(1000 * ft / kFlagSlideMs) : mario.mode == Mode::FlagRun || mario.mode == Mode::FlagJump ? 0 : 1000);
  }

  // Luigi
  Pose pose = Pose::Stand;
  int lift = 0;
  switch (mario.mode) {
    case Mode::Walk: case Mode::Approach: case Mode::FlagRun: case Mode::FinaleWalk: {
      static const Pose CYCLE[4] = {Pose::Walk1, Pose::Stand, Pose::Walk2, Pose::Stand};
      pose = CYCLE[((int)(mario.distance / 2.6f)) % 4];
      break;
    }
    case Mode::Hit: pose = ((now - mario.since) / 70) % 2 ? Pose::Walk2 : Pose::Jump;  // Luigi's flutter kick
     lift = (int)(jumpHeight(now - mario.since, kHitJumpMs, kHitHeight) + 0.5f); break;
    case Mode::Stomp: pose = Pose::Jump; lift = (int)(jumpHeight(now - mario.since, 560, 12) + 0.5f); break;
    case Mode::Hop: pose = Pose::Jump; lift = (int)(jumpHeight(now - mario.since, 320, 7) + 0.5f); break;
    case Mode::FlagJump: pose = Pose::Jump; lift = 14 * ease(now - mario.since, kFlagJumpMs) / 1000; break;
    case Mode::FlagSlide: pose = Pose::Jump; lift = 14 - 14 * (int)(now - mario.since) / kFlagSlideMs; break;
    default: break;
  }
  // The classic Luigi of the NES game: green cap and shirt, blue overalls
  PlumberColors colors = {rgb(24, 190, 48), rgb(252, 188, 120), rgb(100, 52, 8), rgb(40, 72, 232), rgb(255, 224, 40), rgb(16, 16, 16)};
  const uint32_t pt = now - mushroomAt;
  if (mushroomOn && pt >= 1100 && pt < 3700) {
    // The star: his colours flash for a couple of seconds
    switch ((now / 70) % 4) {
      case 0: colors.cap = rgb(255, 224, 40); colors.overalls = rgb(228, 32, 24); break;
      case 1: colors.cap = rgb(24, 190, 48); colors.overalls = rgb(255, 224, 40); break;
      case 2: colors.cap = rgb(255, 255, 255); colors.overalls = rgb(120, 200, 255); break;
      default: break;
    }
  }
  if (mario.mode != Mode::FinaleHidden && mario.mode != Mode::FinaleDone)
    drawLuigi(d, (int)(mario.x + 0.5f), kPlumberY - lift, pose, mario.dir < 0, colors);
  if (world == Finale) {  // the castle stands in front of Luigi, so he walks in behind it
    const bool inside = mario.mode == Mode::FinaleHidden || mario.mode == Mode::FinaleDone;
    drawCastle(d, inside ? 8 * ease((int32_t)(now - finaleHiddenAt) - 200, 1400) / 1000 : 0);
  }
  if (world == Lava) drawPodoboo(d, now);  // in front of Luigi

  // The curtain between two worlds: black sweeps across, the new world is revealed behind it
  if (wiping) {
    const uint32_t wt = now - wipeAt;
    if (wt < 300) box(d, 0, 0, W * wt / 300, H, rgb(0, 0, 0));
    else box(d, W * (wt - 300) / 300, 0, W - W * (wt - 300) / 300, H, rgb(0, 0, 0));
  }

  d.flipDMABuffer();
}
