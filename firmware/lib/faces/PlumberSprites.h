// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
// Pixel-art plumbers for the Mario and Luigi faces, drawn from rows of text. Mario is 13 pixels wide and 16 high; Luigi is
// the same drawing, two rows taller, in his own colours. Header only.
#pragma once

#include "FaceKit.h"

namespace facekit {

enum class Pose : uint8_t { Stand, Walk1, Walk2, Jump };

namespace plumber {

// r: cap and shirt   s: skin   b: hair and shoes   u: overalls   y: buttons   k: eyes
static const char *const HEAD[7] = {
  "....rrrrr....",
  "...rrrrrrrrr.",
  "...bbbsskb...",
  "..bsbsssksss.",
  "..bsbbsssksss",
  "..bbsssssbbb.",
  "....ssssssss.",
};
static const char *const TORSO_STAND[5] = {
  "...rrurrrr...",
  "..rrrurrurrr.",
  ".rrrruuuurrrr",
  ".ssruyuuyurss",
  ".sssuuuuuusss",
};
static const char *const TORSO_JUMP[5] = {
  "ss.rrurrrr.ss",
  "sss.rurrur.ss",
  "sssruuuuuurs.",
  ".s.ruyuuyur..",
  "...uuuuuuuu..",
};
static const char *const LEGS_STAND[4] = {
  "..uuuu..uuuu.",
  "..uuuu..uuuu.",
  ".bbbb....bbbb",
  ".bbbbb..bbbbb",
};
static const char *const LEGS_WALK1[4] = {
  "...uuuuuuu...",
  "..uuuuuuuubb.",
  ".bbbb..uubbbb",
  ".bbb....bbbb.",
};
static const char *const LEGS_WALK2[4] = {
  "..uuuu.uuuu..",
  ".bbuuu..uuu..",
  ".bbbb....bbb.",
  "..bbb...bbbb.",
};
static const char *const LEGS_JUMP[4] = {
  "..uuu..uuuu..",
  ".uuu....uuuu.",
  "bbbb.....bbb.",
  "bbb.......bb.",
};

}  // namespace plumber

struct PlumberColors {
  uint16_t cap, skin, hair, overalls, button, eye;
};
static const PlumberColors MARIO = {rgb(228, 32, 24), rgb(252, 188, 120), rgb(120, 56, 8), rgb(40, 76, 224), rgb(255, 224, 40), rgb(16, 16, 16)};
static const PlumberColors LUIGI = {rgb(24, 190, 48), rgb(252, 188, 120), rgb(100, 52, 8), rgb(40, 76, 224), rgb(255, 224, 40), rgb(16, 16, 16)};

// The figure with his feet on row y + height - 1; `tall` stretches the body two rows (Luigi)
inline void drawPlumber(MatrixPanel_I2S_DMA &d, int x, int y, Pose pose, bool faceLeft, const PlumberColors &c, bool tall = false,
                        uint16_t hurt = 0) {
  const char *rows[20];
  int n = 0;
  for (const char *r : plumber::HEAD) rows[n++] = r;
  const char *const *torso = pose == Pose::Jump ? plumber::TORSO_JUMP : plumber::TORSO_STAND;
  for (int i = 0; i < 5; i++) rows[n++] = torso[i];
  if (tall) { rows[n++] = torso[4]; rows[n++] = plumber::LEGS_STAND[0]; }
  const char *const *legs = pose == Pose::Walk1 ? plumber::LEGS_WALK1 : pose == Pose::Walk2 ? plumber::LEGS_WALK2
                          : pose == Pose::Jump ? plumber::LEGS_JUMP : plumber::LEGS_STAND;
  for (int i = 0; i < 4; i++) rows[n++] = legs[i];
  const Swatch palette[] = {{'r', hurt ? hurt : c.cap}, {'s', c.skin}, {'b', c.hair}, {'u', c.overalls}, {'y', c.button}, {'k', c.eye}};
  sprite(d, rows, n, x, y, palette, 6, faceLeft);
}

}  // namespace facekit
