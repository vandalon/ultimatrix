// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Clockface.h"
#include <MiniFont.h>
#include <PlumberSprites.h>
#include <PlumberWorld.h>
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
const Theme kSnow = {rgb(110, 160, 222), rgb(120, 144, 200), rgb(255, 255, 255), rgb(48, 64, 112), rgb(184, 100, 20), rgb(252, 188, 120), rgb(20, 52, 130), rgb(96, 140, 210), true};
const Theme kWater = {rgb(8, 56, 168), rgb(40, 120, 176), rgb(104, 184, 232), rgb(0, 56, 96), rgb(184, 100, 20), rgb(252, 188, 120), rgb(255, 255, 255), 0, true};
const Theme kCloud = {rgb(48, 112, 232), rgb(172, 208, 248), rgb(222, 240, 255), rgb(112, 160, 230), rgb(184, 100, 20), rgb(252, 188, 120), rgb(14, 40, 120), 0, true};
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

const int kGround = 55;             // first row of the ground
const int kPlumberY = kGround - 16;
const int kBlockX[2] = {12, 34};            // hours and minutes, hit separately
const int kBlockTop = 11, kBlockSize = 19;   // square; its bottom edge is where Mario's head meets it
const int kWalkX = 24;             // the camera follows here during the walk
const int kHitMinutesX = 37, kHitHourX = kBlockX[0] + kBlockSize / 2 - 6;
const int kHitJumpMs = 520, kHitHeight = 9;
const float kApproachSpeed = 9.5f;
const int kPreJumpWaitMs = 200, kSwipeHalfMs = 140;
const float kExitSpeed = 28;

enum class Mode : uint8_t { Walk, TravelJump, WorldExit, WorldEnter, Pause, Approach, Wait, Hit, Stomp, Hop, FlagRun, FlagJump, FlagSlide, FlagWait, FinaleWalk, FinaleHidden, FinaleDone };
const int kPoleX = 44, kPoleTop = 19, kMarioPoleX = 31, kCastleX = 50, kFlagSlideMs = 1700, kFlagJumpMs = 350;

struct Plumber {
  float x = kWalkX, fromX = 0, vx = 0, distance = 0;
  int8_t dir = 1;
  Mode mode = Mode::Walk;
  uint32_t since = 0;
  uint32_t pauseMs = 0;
  int32_t approachedMinute = -1;
  float hitX = kHitMinutesX;
  uint8_t hitBlock = 1;
  bool hourAfterMinute = false, catchUp = false, touched = false;
  int travelHeight = 16;
  bool exitJumping = false;
  uint32_t exitJumpAt = 0;
} mario;
plumberworld::Course course;
plumberworld::BlockMushroom blockMushroom;

struct Goomba {
  float x = 0;
  uint8_t state = 0;  // 0 none, 1 walking, 2 squashed
  uint32_t since = 0;
  int32_t spawnedMinute = -1;
  int16_t score = 0;
  bool koopa = false;
} goomba;

// Two digits per block; the digits only change when Mario hits the block
struct Block { int8_t shown[2], target[2]; uint32_t bumpAt; };
Block blocks[2];
struct { bool pending; uint8_t block; } touch;  // Mario's head met one block this frame
uint32_t lastNow = 0, sceneNow = 0, animationRemainder = 0;
bool night = false;
int world = Overground, pendingWorld = -1;  // the world on show, and the one the curtain is about to bring
bool wiping = false;
// The noon finale is a scene of its own (not part of the rotation): wipe to it, Mario slides down the flagpole, walks into the
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
bool mushroomOn = false;        // the 1-UP of the hour block, and the star Mario flashes with afterwards
uint32_t mushroomAt = 0;
int coins = 0, coinDay = -1;    // the HUD coin counter: one per block hit, back to 0 each day
int flagDay = -1;               // the day Mario last slid down the noon flagpole

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
      else if ((x + (world == Finale ? 0 : (int)course.camera) + (row % 2 ? 4 : 0)) % 8 == 7) c = t.mortar;
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

void drawMushroom(MatrixPanel_I2S_DMA &d, int x, int y, bool red = false) {
  static const char *const ROWS[8] = {"..gggg..", ".ggwwgg.", "ggwwwwgg", "gwwggwwg", "gggggggg", ".ssssss.", ".ssksks.", "..ssss.."};
  const Swatch PAL[] = {{'g', red ? rgb(232, 40, 24) : rgb(0, 200, 40)}, {'w', rgb(255, 255, 255)}, {'s', rgb(252, 224, 168)}, {'k', rgb(16, 8, 8)}};
  sprite(d, ROWS, 8, x, y, PAL, 4);
}

// The noon flagpole: the flag rides down it with Mario (0 = top, 1000 = bottom)
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
      const bool mortar = fromBottom % 6 == 0 || (x0 + x + shift + (int)(course.camera / 2)) % 8 == 7;
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

// Deep water: light shafts, swaying seaweed, rising bubbles and a Cheep Cheep swimming by
void drawUnderwater(MatrixPanel_I2S_DMA &d, uint32_t now) {
  box(d, 0, 0, W, 20, rgb(16, 72, 192));
  for (int i = 0; i < 5; i++) box(d, plumberworld::sceneryX(6 + i * 20, course.camera, 4), 0, 3, kGround, rgb(24, 80, 200));
  for (const int x : {20, 28, 50, 84})
    for (int y = 0; y < 14; y++) put(d, plumberworld::sceneryX(x, course.camera) + 1 + (int)lroundf(sinf(y / 2.0f + now / 400.0f)), kGround - 1 - y, rgb(32, 184, 88));
  for (int i = 0; i < 7; i++) {
    const int y = 50 - (int)((now * 8 / 1000 + hash(i * 3 + 9) % 48) % 48);
    box(d, hash(i * 5 + 1) % 60 + 2 + (int)lroundf(sinf(now / 500.0f + i)), y, 2, 2, rgb(168, 216, 255));
  }
  static const char *const FISH[6] = {"...rrrr..", ".rrrrrrr.", "rrwkrrrrr", "rrrrrrrrr", ".rrrrrrr.", "...rrr..r"};
  static const Swatch PAL[] = {{'r', rgb(232, 64, 48)}, {'w', rgb(255, 255, 255)}, {'k', rgb(16, 16, 16)}};
  sprite(d, FISH, 6, 68 - (int)((now * 11 / 1000) % 96), 29 + (int)lroundf(2 * sinf(now / 600.0f)), PAL, 3);
}

// Bright sky and distant clouds
void drawCloudTop(MatrixPanel_I2S_DMA &d, uint32_t now, bool dark) {
  drawCloud(d, plumberworld::sceneryX(62, course.camera + now / 650.0f, 4), 7, dark);
  drawCloud(d, plumberworld::sceneryX(100, course.camera + now / 825.0f, 4), 31, dark);
  drawCloud(d, plumberworld::sceneryX(70, course.camera + now / 1025.0f, 4), 22, dark);
}

// An arched window in the wall with the night outside: a small crescent and a couple of stars
void drawGhostWindow(MatrixPanel_I2S_DMA &d, int x) {
  const int y = 7, w = 10, h = 18;
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
  drawGhostWindow(d, plumberworld::sceneryX(53, course.camera));
  static const char *const BOO[9] = {"..wwwww..", ".wwwwwww.", "wwkwwwkww", "wwkwwwkww", "wwwwwwwww", "wwwrrrwww", "wwwrrrwww", ".wwwwwww.", "..w...w.."};
  const Swatch near[] = {{'w', rgb(244, 240, 255)}, {'k', rgb(32, 32, 64)}, {'r', rgb(232, 64, 96)}};
  const Swatch far[] = {{'w', rgb(168, 152, 208)}, {'k', rgb(32, 32, 64)}, {'r', rgb(160, 64, 96)}};
  sprite(d, BOO, 9, plumberworld::sceneryX(2, course.camera) + (int)lroundf(2 * sinf(now / 1800.0f)), 26 + (int)lroundf(2 * sinf(now / 1100.0f)), far, 3);
  sprite(d, BOO, 9, plumberworld::sceneryX(47, course.camera) + (int)lroundf(3 * sinf(now / 1500.0f)), 31 + (int)lroundf(2 * sinf(now / 900.0f + 1)), near, 3);
}

// A red sky over a glowing volcano, with embers drifting up
void drawVolcano(MatrixPanel_I2S_DMA &d, uint32_t now) {
  const int vx = plumberworld::sceneryX(22, course.camera, 4);
  static const uint16_t BANDS[6] = {rgb(48, 8, 8), rgb(64, 12, 8), rgb(80, 16, 8), rgb(96, 20, 8), rgb(112, 24, 8), rgb(128, 32, 8)};
  for (int i = 0; i < 6; i++) box(d, 0, i * 9, W, 9, BANDS[i]);
  box(d, 0, 54, W, 10, BANDS[5]);
  for (int x = 0; x < 30; x++) {
    const float u = (x - 14.5f) / 15.0f, shape = 1.0f - u * u;
    const int top = kGround - 1 - (int)lroundf(24 * (shape > 0 ? powf(shape, 0.7f) : 0));
    box(d, vx + x, top, 1, kGround - top, rgb(28, 12, 12)); put(d, vx + x, top, rgb(60, 28, 28));
  }
  box(d, vx + 11, 28, 6, 2, rgb(255, 112, 24)); box(d, vx + 12, 27, 4, 1, rgb(255, 192, 64));
  box(d, vx + 13, 30, 1, 10, rgb(232, 64, 16)); box(d, vx + 9, 34, 1, 8, rgb(232, 64, 16)); box(d, vx + 18, 36, 1, 10, rgb(232, 64, 16));
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
  sprite(d, BALL, 6, plumberworld::sceneryX(49, course.camera, 1), 60 - (int)(26 * 4 * p * (1 - p)), PAL, 2);
}

// A dusk sky over a wooden hull: drifting clouds, a rail with posts, a mast with a sail, and a Bullet Bill fired from a cannon tower
void drawAirship(MatrixPanel_I2S_DMA &d, uint32_t now) {
  const int mast = plumberworld::sceneryX(6, course.camera);
  const int tower = plumberworld::sceneryX(52, course.camera);
  const int deck = kGround - 9;
  const uint16_t cloud = rgb(146, 122, 150);
  for (const int anchor : {4, 50}) {
    const int x = plumberworld::sceneryX(anchor, course.camera, 4);
    box(d, x, 36, 14, 2, cloud); box(d, x + 2, 35, 10, 1, cloud); box(d, x + 4, 34, 5, 1, cloud);
  }
  box(d, mast, 14, 2, deck - 14, rgb(108, 60, 16)); box(d, mast + 1, 14, 1, deck - 14, rgb(156, 92, 32));  // the mast stands on the deck
  box(d, mast + 2, 18, 10, 18, rgb(224, 216, 200)); box(d, mast + 2, 18, 10, 1, rgb(255, 255, 255)); box(d, mast + 11, 19, 1, 17, rgb(170, 160, 142));
  box(d, mast + 2, 36, 10, 1, rgb(108, 60, 16));
  box(d, 0, deck, W, kGround - deck, rgb(74, 42, 14));  // the hull planks
  for (int y = deck + 3; y < kGround; y += 3) box(d, 0, y, W, 1, rgb(48, 26, 6));
  box(d, 0, deck - 3, W, 1, rgb(176, 108, 32));  // rail and posts
  for (int x = 2 - (int)course.camera % 8; x < W; x += 8) box(d, x, deck - 3, 1, 3, rgb(124, 72, 20));
  box(d, 0, deck, W, 1, rgb(176, 108, 32));
  box(d, tower, 36, 10, kGround - 36, rgb(48, 48, 56)); box(d, tower, 36, 10, 1, rgb(96, 96, 104)); box(d, tower + 1, 37, 1, kGround - 38, rgb(72, 72, 82));
  box(d, tower - 3, 38, 3, 4, rgb(32, 32, 40));  // the muzzle
  const uint32_t t = now % 6000;
  if (t < 700) disc(d, tower - 5 - (int)(t / 160), 40 - (int)(t / 260), 1 + (int)(t / 300), rgb(216, 216, 224));  // the cannon's smoke
  if (t < 2900) {
    static const char *const BILL[6] = {".kkkk...", "kkkkkkk.", "kkwkkkkk", "kkkkkkkk", "kkkkkkk.", ".kkkk..."};
    static const Swatch PAL[] = {{'k', rgb(32, 32, 40)}, {'w', rgb(255, 255, 255)}};
    sprite(d, BILL, 6, tower - 11 - (int)(t * 26 / 1000), 37, PAL, 2);
  }
}

// The 7x7 digits of the Super Mario font
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

// ---- Walking the course and updating the clock ----

// Whether the clock shows other digits than the blocks are heading for
bool minutesBehind(ClockTime *c) { return blocks[1].target[0] != c->getMinute() / 10 || blocks[1].target[1] != c->getMinute() % 10; }
bool hoursBehind(ClockTime *c) { return blocks[0].target[0] != c->getHour() / 10 || blocks[0].target[1] != c->getHour() % 10; }

void startHit(Plumber &m, uint32_t now) { m.mode = Mode::Hit; m.since = now; m.touched = false; }

bool free0(const Plumber &m) { return m.mode == Mode::Walk || m.mode == Mode::Pause; }
bool changingWorld() { return pendingWorld >= 0 || wiping || mario.mode == Mode::WorldExit || mario.mode == Mode::WorldEnter; }

float jumpHeight(uint32_t t, uint32_t total, int height) {
  const float p = (float)t / total;
  return p >= 1 ? 0 : height * 4 * p * (1 - p);
}

void step(ClockTime *clock, uint32_t now, float dt) {
  const int32_t ms = msOfMinute(clock);
  const int32_t minute = minuteOfDay(clock);
  Plumber &m = mario;
  if (wiping) return;

  // A Goomba (minute 1 of 3) or a Koopa (minute 2 of 3) comes by in the second half of the minute
  if (goomba.state == 0 && minute % 3 != 0 && ms >= 16000 && ms < 36000 && goomba.spawnedMinute != minute && world != Finale && !changingWorld() && free0(mario)) {
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
      // Land on it: it walks on while Mario is in the air
      const float seconds = 0.56f;
      const float landing = goomba.x - 8 * seconds;
      m.fromX = m.x; m.vx = (landing - 2 - m.x) / seconds;
      m.mode = Mode::Stomp; m.since = now;
    }
  }
  // At noon: the finale
  if (free && !changingWorld() && !minutesBehind(clock) && !hoursBehind(clock) &&
      goomba.state == 0 && finaleStage == FinaleStage::None && !finaleWanted &&
      ((clock->getHour24() == 12 && clock->getMinute() == 0 && flagDay != clock->getDay()) || demoFlagWanted)) {
    flagDay = clock->getDay();
    demoFlagWanted = false;
    finaleWanted = true;  // update() wipes to the finale scene, then Mario runs
  }
  // Walk to the block just in time, leaving a short pause before takeoff.
  const bool hourTurn = clock->getMinute() == 59;
  const float hitX = kHitMinutesX;
  const int approachMs = (int)(1000.0f * fabsf(m.x - hitX) / (kApproachSpeed * plumberworld::kAnimationSpeed));
  if (free && !changingWorld() && m.mode != Mode::Stomp && ms >= 60000 - (kHitJumpMs / 2 + kPreJumpWaitMs) / plumberworld::kAnimationSpeed - approachMs &&
      m.approachedMinute != minute && goomba.state == 0) {
    m.approachedMinute = minute;
    m.hitBlock = 1; m.hourAfterMinute = hourTurn;
    m.hitX = hitX;
    m.catchUp = false;
    m.mode = Mode::Approach;
  }
  // The clock moved on without a hit (a Goomba in the way, the time was set): go and hit the blocks anyway
  static uint32_t behindSince = 0;
  if (!(minutesBehind(clock) || hoursBehind(clock))) behindSince = 0;
  else if (behindSince == 0) behindSince = now;
  if (free && !changingWorld() && goomba.state == 0 && behindSince && now - behindSince > 1500 && m.mode != Mode::Stomp) {
    m.catchUp = true;
    m.hitBlock = minutesBehind(clock) ? 1 : 0;
    m.hourAfterMinute = m.hitBlock == 1 && hoursBehind(clock);
    m.hitX = m.hitBlock == 1 ? kHitMinutesX : kHitHourX;
    m.mode = Mode::Approach;
    behindSince = now;
  }

  switch (m.mode) {
    case Mode::Walk: {
      if (goomba.state == 1 || wiping || finaleWanted) { m.dir = 1; break; }
      if (fabsf(m.x - kWalkX) > 0.5f) {
        m.dir = 1;
        const float move = plumberworld::kWalkSpeed * dt;
        if (m.x > kWalkX) {
          // Walk forward at the usual pace while the camera gently catches up.
          const float remaining = m.x - kWalkX;
          float pan = (4 + remaining * 2) * dt;
          if (pan >= remaining - 0.5f) pan = remaining;
          course.advance(move + pan); m.x -= pan; m.distance += move;
        } else {
          const float remaining = kWalkX - m.x;
          const float step = move < remaining ? move : remaining;
          m.x += step; m.distance += step;
        }
      } else {
        m.x = kWalkX; m.dir = 1;
        const int height = course.jumpAhead(m.x, true);
        if (height && ms + plumberworld::kJumpMs / plumberworld::kAnimationSpeed >= 60000 - kHitJumpMs / (2 * plumberworld::kAnimationSpeed)) {
          m.approachedMinute = minute; m.hitBlock = 1; m.hourAfterMinute = hourTurn;
          m.hitX = kHitMinutesX; m.catchUp = false; m.mode = Mode::Approach;
        } else if (height) { m.mode = Mode::TravelJump; m.since = now; m.travelHeight = height; }
        else {
          const float speed = course.groundSpeed(dt);
          course.advance(speed * dt); m.distance += speed * dt;
        }
      }
      break;
    }
    case Mode::TravelJump:
      course.advance(plumberworld::kJumpSpeed * dt);
      if (now - m.since >= plumberworld::kJumpMs) { m.mode = Mode::Walk; m.dir = 1; }
      break;
    case Mode::WorldExit:
      if (!m.exitJumping) {
        for (int i = 0; i < 2; i++) {
          const float gap = course.x(plumberworld::OBSTACLES[i]) - (m.x + 13);
          if (world != Finale && gap >= 0 && gap <= 10) {
            m.exitJumping = true; m.exitJumpAt = now;
            m.travelHeight = i == 0 ? 28 : 16;
            break;
          }
        }
      }
      m.x += kExitSpeed * dt; m.distance += kExitSpeed * dt;
      if (m.exitJumping && now - m.exitJumpAt >= plumberworld::kJumpMs) m.exitJumping = false;
      break;
    case Mode::WorldEnter:
      m.x += kExitSpeed * dt; m.distance += kExitSpeed * dt;
      if (m.x >= kWalkX) { m.x = kWalkX; m.mode = Mode::Walk; }
      break;
    case Mode::Pause:
      if (now - m.since >= m.pauseMs) m.mode = Mode::Walk;
      break;
    case Mode::Approach: {
      if (!m.catchUp && ms < 40000) { m.mode = Mode::Walk; break; }
      m.dir = m.hitX > m.x ? 1 : -1;
      m.x += m.dir * kApproachSpeed * dt; m.distance += kApproachSpeed * dt;
      if ((m.dir > 0 && m.x >= m.hitX) || (m.dir < 0 && m.x <= m.hitX)) { m.x = m.hitX; m.dir = 1; m.mode = Mode::Wait; }
      break;
    }
    case Mode::Wait:
      if (m.catchUp) { startHit(m, now); break; }
      if (ms < 40000) { m.mode = Mode::Pause; m.dir = -1; m.since = now; m.pauseMs = 300; break; }  // the minute turned before he got there
      // Start the jump so that his head meets the block as the minute turns
      if (ms >= 60000 - kHitJumpMs / (2 * plumberworld::kAnimationSpeed) && ms < 60000) startHit(m, now);
      break;
    case Mode::Hit: {
      const uint32_t t = now - m.since;
      // Head against the block: now the digits change (once the clock has turned, or soon after anyway)
      const bool blockBehind = m.hitBlock == 1 ? minutesBehind(clock) : hoursBehind(clock);
      if (!m.touched && t >= (uint32_t)kHitJumpMs / 2 && (blockBehind || t >= (uint32_t)kHitJumpMs * 3 / 4)) {
        m.touched = true;
        touch = {true, m.hitBlock};
      }
      if (t >= (uint32_t)kHitJumpMs) {
        if (m.hitBlock == 1 && m.hourAfterMinute) {
          m.hitBlock = 0; m.hourAfterMinute = false; m.hitX = kHitHourX;
          m.catchUp = true; m.mode = Mode::Approach;
        } else {
          m.mode = Mode::Pause; m.dir = 1; m.since = now; m.pauseMs = 200; m.catchUp = false;
        }
      }
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
      if (m.x >= kMarioPoleX) { m.x = kMarioPoleX; m.mode = Mode::FlagJump; m.since = now; }
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
  if (m.mode != Mode::WorldExit && m.mode != Mode::WorldEnter && m.mode != Mode::FlagRun) {
    if (m.x < 0) m.x = 0;
    if (m.x > 51) m.x = 51;
  }
}

}  // namespace

Clockface::Clockface(MatrixPanel_I2S_DMA* display) { _display = display; }

void Clockface::setNight(bool value) { night = value; }


void Clockface::setup(ClockTime *dateTime) {
  _dateTime = dateTime;
  lastNow = millis(); sceneNow = 0; animationRemainder = 0;
  mario = Plumber();
  course = plumberworld::Course();
  blockMushroom = plumberworld::BlockMushroom();
  goomba = Goomba();
  touch = {false, 1};
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
  const uint32_t realNow = millis(), elapsed = realNow - lastNow;
  lastNow = realNow;
  const uint64_t scaled = (uint64_t)elapsed * 6 + animationRemainder;
  sceneNow += scaled / 5; animationRemainder = scaled % 5;
  const uint32_t now = sceneNow;
  float dt = elapsed * plumberworld::kAnimationSpeed / 1000.0f;
  if (dt > 0.2f) dt = 0.2f;
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
  const bool lightSky = world == Snow || world == CloudTop || world == Finale;  // popups need dark ink here
  const int hour = _dateTime->getHour(), minute = _dateTime->getMinute();
  const int digits[4] = {hour / 10, hour % 10, minute / 10, minute % 10};
  const bool hideTens = !_dateTime->is24hFormat() && hour < 10;

  if (coinDay != _dateTime->getDay()) { coinDay = _dateTime->getDay(); coins = 0; }
  step(_dateTime, now, dt);

  // Each jump updates one block. At an hour turn, the minute block is hit first.
  if (touch.pending) {
    const int i = touch.block;
    blocks[i].target[0] = digits[i * 2]; blocks[i].target[1] = digits[i * 2 + 1];
    blocks[i].bumpAt = now;
    coins = (coins + 1) % 100;
    if (i == 1) blockMushroom.spawn(kBlockX[1], kBlockTop, course.camera, now,
                                  (hash(minuteOfDay(_dateTime) + _dateTime->getDay() * 1440) & 1) != 0);
    if (i == 0) {
      mushroomOn = true; mushroomAt = now;
      if (_dateTime->getHour24() == 12 && minute == 0 && flagDay != _dateTime->getDay()) {
        flagDay = _dateTime->getDay(); finaleWanted = true;
      } else {
        // Every hour ends with a walk out. A pinned world is entered again.
        pendingWorld = pinnedWorld() >= 0 ? pinnedWorld() : demoWorlds() ? (world + 1) % kWorlds
                     : nextWorld(now ^ (_dateTime->getDay() * 24 + _dateTime->getHour24()), night);
      }
    }
    touch.pending = false;
  }
  if (mushroomOn && now - mushroomAt >= 3700) mushroomOn = false;
  if (pinnedWorld() >= 0 && world != pinnedWorld() && !changingWorld() && finaleStage == FinaleStage::None && world != Finale)
    pendingWorld = pinnedWorld();
  // The demo visits each world and plays the finale once per round.
  if (demoWorlds() && world == Overground && !demoFlagDone && !changingWorld() && finaleStage == FinaleStage::None && now - worldSince >= 1500) {
    demoFlagWanted = true; demoFlagDone = true;
  }
  if (demoWorlds() && !changingWorld() && finaleStage == FinaleStage::None && world != Finale && now - worldSince >= 8000) {
    pendingWorld = (world + 1) % kWorlds; demoFlagDone = false;
  }
  if (night && brightWorld(world) && pinnedWorld() < 0 && !demoWorlds() && !changingWorld())
    pendingWorld = nextWorld(now, true);

  if (finaleWanted && finaleStage == FinaleStage::None && !wiping) {
    finaleWanted = false; worldBefore = world; pendingWorld = Finale;
    finaleStage = FinaleStage::Out;
  } else if (finaleStage == FinaleStage::Playing && mario.mode == Mode::FinaleDone && !changingWorld()) {
    pendingWorld = worldBefore; finaleStage = FinaleStage::Back;
  }

  if (pendingWorld >= 0 && !blockMushroom.active && !wiping && (free0(mario) || mario.mode == Mode::FinaleDone)) {
    mario.mode = Mode::WorldExit; mario.dir = 1; mario.exitJumping = false;
    goomba.state = 0;
  }
  // Only swipe when six pixels of the departing character remain on screen.
  if (mario.mode == Mode::WorldExit && mario.x >= W - 6 && !wiping) { wiping = true; wipeAt = now; }
  if (wiping) {
    const uint32_t wt = now - wipeAt;
    if (wt >= kSwipeHalfMs && pendingWorld >= 0) {
      world = pendingWorld; pendingWorld = -1; worldSince = now;
      course = plumberworld::Course(); blockMushroom.active = false; goomba.state = 0;
      mario.x = -13; mario.dir = 1; mario.exitJumping = false;
      if (finaleStage == FinaleStage::Out && world == Finale) {
        mario.mode = Mode::FlagRun; finaleStage = FinaleStage::Playing;
      } else {
        mario.mode = Mode::WorldEnter;
        if (finaleStage == FinaleStage::Back) finaleStage = FinaleStage::None;
      }
    }
    if (wt >= 2 * kSwipeHalfMs) wiping = false;
  }
  for (int i = 0; i < 2; i++) {
    if (now >= blocks[i].bumpAt + 150) { blocks[i].shown[0] = blocks[i].target[0]; blocks[i].shown[1] = blocks[i].target[1]; }
  }

  d.fillScreen(theme.sky);

  // Scenery
  switch (world) {
    case Underground: drawWall(d, 0, 0, W, kGround, theme, 38); break;  // the cave wall, dim, ending on the floor stones
    case Castle: {
      Theme wall = kCastle;
      wall.brick = rgb(44, 20, 24); wall.mortar = rgb(20, 6, 8);
      drawWall(d, 0, 0, W, kGround, wall, 255);
      const int wx = plumberworld::sceneryX(2, course.camera);
      drawCastleDecor(d, now, wx, wx + 7);
      drawCastleDecor(d, now, wx + 48, wx + 41);
      break;
    }
    case Underwater: drawUnderwater(d, now); break;
    case CloudTop: if (nightNow) drawNightSky(d, now); drawCloudTop(d, now, nightNow); break;
    case GhostHouse: drawGhostHouse(d, now, theme); break;
    case Lava: drawVolcano(d, now); break;
    case Airship: drawAirship(d, now); break;
    case Finale:
      drawCloud(d, plumberworld::sceneryX(62, course.camera + now / 650.0f, 4), 7, false);
      drawCloud(d, plumberworld::sceneryX(100, course.camera + now / 825.0f, 4), 31, false);
      drawCloud(d, plumberworld::sceneryX(70, course.camera + now / 1025.0f, 4), 22, false);
      if (mario.mode == Mode::FinaleHidden || mario.mode == Mode::FinaleDone) drawFireworks(d, (int32_t)(now - finaleHiddenAt) - 900);
      break;
    default:  // Overground and Snow
      if (nightNow) drawNightSky(d, now);
      drawCloud(d, plumberworld::sceneryX(62, course.camera + now / 650.0f, 4), 7, nightNow);
      drawCloud(d, plumberworld::sceneryX(80, course.camera + now / 975.0f, 4), 29, nightNow);
      if (world == Snow) drawFlakes(d, now);
      drawHill(d, plumberworld::sceneryX(22, course.camera), kGround - 1, nightNow, world == Snow);
      break;
  }
  const uint16_t snowWhite = nightNow ? rgb(70, 82, 130) : rgb(255, 255, 255);
  if (world != Finale) {
    const auto &tube = world == Underground ? plumberworld::CYAN_TUBE : world == Castle || world == Lava ? plumberworld::GREY_TUBE
                     : world == Underwater ? plumberworld::TEAL_TUBE : world == GhostHouse ? plumberworld::GHOST_TUBE : plumberworld::GREEN_TUBE;
    plumberworld::drawTube(d, course.x(plumberworld::OBSTACLES[0]), kGround, tube, now, (course.lap + world) % 2 == 0);
    if (world == Snow) box(d, course.x(plumberworld::OBSTACLES[0]) + 1, kGround - 13, 12, 1, snowWhite);
  }
  drawBricks(d, kGround, H - 1, theme, true);
  if (world == Castle || world == Lava) {
    box(d, 0, 60, W, 4, rgb(232, 64, 16));
    box(d, 0, 60, W, 1, (now / 160) % 2 ? rgb(255, 176, 48) : rgb(255, 140, 32));
  } else if (world == Snow) {
    box(d, 0, kGround, W, 2, snowWhite);
  } else if (world == CloudTop || world == Finale) {
    box(d, 0, kGround - 2, W, 2, snowWhite);  // the puffs along the top of the cloud ground
    for (int x = 2 - (world == Finale ? 0 : (int)course.camera % 8); x < W; x += 8) { box(d, x, kGround - 3, 5, 1, snowWhite); box(d, x + 1, kGround - 4, 3, 1, snowWhite); }
  } else if (world == Airship) {
    for (int x = 3 - (int)course.camera % 16; x < W; x += 16) { disc(d, x, 56, 1, rgb(60, 32, 8)); put(d, x, 56, rgb(200, 200, 208)); }
  }

  if (world != Finale) {
    plumberworld::drawLedge(d, course.x(plumberworld::OBSTACLES[1]), kGround, theme.brick, theme.brickLight, theme.mortar);
    for (int i = 0; i < 8; i++) {
      if (course.collected & (1 << i)) continue;
      const auto &item = plumberworld::ITEMS[i];
      const int x = course.x(item.x), y = kGround - item.height;
      if (item.kind == plumberworld::Coin) drawCoin(d, x, y, coinWidth(now + i * 170));
      else if (item.kind == plumberworld::Mushroom) drawMushroom(d, x - 4, y);
      else plumberworld::drawStar(d, x - 4, y, now);
    }
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
      miniText(d, score, (int)goomba.x - 2, kGround - 16 - (int)((now - goomba.since) / 110), lightSky ? rgb(20, 52, 130) : rgb(255, 255, 255));
    }
  }

  const bool flagging = world == Finale && mario.mode != Mode::Walk && mario.mode != Mode::Pause;
  if (flagging) {
    const uint32_t ft = now - mario.since;
    drawFlagpole(d, mario.mode == Mode::FlagSlide ? (int)(1000 * ft / kFlagSlideMs) : mario.mode == Mode::FlagRun || mario.mode == Mode::FlagJump ? 0 : 1000);
  }

  // Mario
  Pose pose = Pose::Stand;
  int lift = 0;
  switch (mario.mode) {
    case Mode::Walk:
      pose = goomba.state == 1 || finaleWanted ? Pose::Stand : walkPose(mario.distance);
      break;
    case Mode::WorldExit:
      pose = mario.exitJumping ? Pose::Jump : walkPose(mario.distance);
      if (mario.exitJumping) lift = (int)(jumpHeight(now - mario.exitJumpAt, plumberworld::kJumpMs, mario.travelHeight) + 0.5f);
      break;
    case Mode::WorldEnter: case Mode::Approach: case Mode::FlagRun: case Mode::FinaleWalk: {
      pose = walkPose(mario.distance);
      break;
    }
    case Mode::Hit: pose = Pose::Jump; lift = (int)(jumpHeight(now - mario.since, kHitJumpMs, kHitHeight) + 0.5f); break;
    case Mode::TravelJump: pose = Pose::Jump; lift = (int)(jumpHeight(now - mario.since, plumberworld::kJumpMs, mario.travelHeight) + 0.5f); break;
    case Mode::Stomp: pose = Pose::Jump; lift = (int)(jumpHeight(now - mario.since, 560, 9) + 0.5f); break;
    case Mode::Hop: pose = Pose::Jump; lift = (int)(jumpHeight(now - mario.since, 320, 5) + 0.5f); break;
    case Mode::FlagJump: pose = Pose::Jump; lift = 16 * ease(now - mario.since, kFlagJumpMs) / 1000; break;
    case Mode::FlagSlide: pose = Pose::Jump; lift = 16 - 16 * (int)(now - mario.since) / kFlagSlideMs; break;
    default: break;
  }
  PlumberColors colors = MARIO;
  if (world != Finale && !wiping && (mario.mode == Mode::Walk || mario.mode == Mode::TravelJump))
    coins = (coins + course.collect(mario.x, kPlumberY - lift, kGround - kPlumberY, kGround, now)) % 100;
  const int pickup = blockMushroom.update(now, dt, course.camera, kGround, mario.x, kGround - lift);
  if (pickup >= 0 && !wiping) {
    course.pickup = (plumberworld::Item)pickup; course.pickupAt = now; course.hasPickup = true;
  }
  const uint32_t pt = now - mushroomAt;
  if ((mushroomOn && pt >= 1100 && pt < 3700) || (course.hasStar && now - course.starAt < 3700)) {
    // The star: his colours flash for a couple of seconds
    switch ((now / 70) % 4) {
      case 0: colors.cap = rgb(255, 224, 40); colors.overalls = rgb(228, 32, 24); break;
      case 1: colors.cap = rgb(24, 190, 48); colors.overalls = rgb(255, 224, 40); break;
      case 2: colors.cap = rgb(255, 255, 255); colors.overalls = rgb(120, 200, 255); break;
      default: break;
    }
  }
  if (world != Finale && course.hasPickup && now - course.pickupAt < 900 && course.pickup != plumberworld::Coin) {
    const char *label = course.pickup == plumberworld::Mushroom ? "1UP" : course.pickup == plumberworld::SuperMushroom ? "SUPER" : "1000";
    miniText(d, label, (int)mario.x + 14, kGround - 18 - (now - course.pickupAt) / 150,
             course.pickup == plumberworld::Mushroom ? (lightSky ? rgb(0, 120, 32) : rgb(0, 224, 64)) : course.pickup == plumberworld::SuperMushroom ? rgb(232, 40, 24) : (lightSky ? rgb(150, 90, 0) : rgb(255, 224, 40)));
  }

  // Swipe the scenery; the clock remains visible in its fixed position
  if (wiping) {
    const uint32_t wt = now - wipeAt;
    if (wt < kSwipeHalfMs) box(d, 0, 0, W * wt / kSwipeHalfMs, H, rgb(0, 0, 0));
    else box(d, W * (wt - kSwipeHalfMs) / kSwipeHalfMs, 0, W - W * (wt - kSwipeHalfMs) / kSwipeHalfMs, H, rgb(0, 0, 0));
  }


  // The date, small, up in the sky like the score line of the game: weekday top left, day and month top right
  {
    static const char *const DAYS[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char *const MONTHS[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    char day[4];
    snprintf(day, sizeof(day), "%d", _dateTime->getDay());
    const char *month = MONTHS[(_dateTime->getMonth() + 11) % 12];
    miniText(d, DAYS[_dateTime->getWeekday() % 7], 1, 1, theme.text);
    // Right-aligned month; the day number sits one pixel farther left
    const int monthX = W - 1 - miniWidth(month);
    miniText(d, month, monthX, 1, theme.text);
    miniText(d, day, monthX - 3 - miniWidth(day), 1, theme.text);
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
    else if (pt < 1800) miniText(d, "1UP", kBlockX[0] + kBlockSize / 2 - miniWidth("1UP") / 2 - 1, 1, lightSky ? rgb(0, 120, 32) : rgb(0, 224, 64));
  }

  if (blockMushroom.active) drawMushroom(d, (int)blockMushroom.x, blockMushroom.y, blockMushroom.red);

  // The question blocks; the coin pops out from behind each one (so it is drawn first) and drops back in
  for (int i = 0; i < 2; i++) {
    const int lift = bumpOffset(blocks[i], now);
    const int32_t since = (int32_t)(now - blocks[i].bumpAt);
    if (i == 0 && since >= 0 && since < 700) {
      const int p = since < 350 ? ease(since, 350) : 1000 - ease(since - 350, 350);
      drawCoin(d, kBlockX[i] + kBlockSize / 2, kBlockTop - 5 * p / 1000, coinWidth(since * 2));
    }
    drawBlock(d, kBlockX[i], kBlockTop + lift, i == 0 && hideTens ? -1 : blocks[i].shown[0], blocks[i].shown[1], !theme.overground);
  }


  if (!wiping && mario.mode != Mode::FinaleHidden && mario.mode != Mode::FinaleDone)
    drawPlumber(d, (int)(mario.x + 0.5f), kPlumberY - lift, pose, mario.dir < 0, colors);
  if (world == Finale && !wiping) {  // the castle stands in front of Mario, so he walks in behind it
    const bool inside = mario.mode == Mode::FinaleHidden || mario.mode == Mode::FinaleDone;
    drawCastle(d, inside ? 8 * ease((int32_t)(now - finaleHiddenAt) - 200, 1400) / 1000 : 0);
  }
  if (world == Lava && !wiping) drawPodoboo(d, now);  // in front of Mario


  d.flipDMABuffer();
}
