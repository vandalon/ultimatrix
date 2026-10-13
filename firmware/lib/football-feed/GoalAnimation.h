// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Adafruit_GFX.h>
#include <functional>

#include "TeamKit.h"

// Full screen goal animation for a 64x32 or 64x64 panel, about 6 seconds: "net cam" from behind
// the goal. The ball flies out of the distance and grows until it hits the net (flash, ripples,
// shaking screen), then GOAL! slams in from the camera with fireworks over the cheering stands
// and the new score comes up.
//
// Only uses Adafruit_GFX drawing, so tools/goal-preview can render it on a
// computer. showFrame(ms) must put the drawn frame on screen and wait ms.
// homeKit and awayKit are both teams' colours: the scorer's kit colours the flash, the
// fireworks and the stripe, both are used for the names.
void playGoalAnimation(Adafruit_GFX *display, const char *home, const char *away, int homeScore, int awayScore,
                       bool homeScored, const TeamKit &homeKit, const TeamKit &awayKit,
                       const std::function<void(int ms)> &showFrame);

// Only the second half: GOAL! with confetti and the new score, about 2.5 seconds. Fits the
// panel's height: on a 64x64 panel the background fills it all, GOAL! sits in the middle and the
// score and names come up in a black band below it.
void playGoalCelebration(Adafruit_GFX *display, const char *home, const char *away, int homeScore, int awayScore,
                         bool homeScored, const TeamKit &homeKit, const TeamKit &awayKit,
                         const std::function<void(int ms)> &showFrame);

// The same animation one frame at a time, for a face that keeps running while it plays. Frames have to be drawn in
// order (drawFrame(0), drawFrame(1) ...); show = false draws one without putting it on the display, to catch up.
// Only one can play at a time.
class GoalAnimation {
 public:
  static const int FRAME_MS = 40;
  ~GoalAnimation() { end(); }
  // false when there is no memory for it: then skip the celebration
  bool begin(Adafruit_GFX *display, const char *home, const char *away, int homeScore, int awayScore, bool homeScored,
             const TeamKit &homeKit, const TeamKit &awayKit);
  int frames() const;
  // Draws frame index into the display; the caller shows it
  void drawFrame(int index, bool show = true);
  void end();

 private:
  Adafruit_GFX *_display = nullptr;
  GFXcanvas1 *_text = nullptr;
  char _home[12], _away[12];
  int _homeScore = 0, _awayScore = 0, _nextBurst = 0;
  bool _homeScored = false;
  TeamKit _homeKit = {}, _awayKit = {};
};

// Text in the classic 5x7 font with the top half of each letter in top and the
// bottom half in bottom (two-tone team names). size 1 or 2.
void drawTwoToneText(Adafruit_GFX *d, int x, int y, const char *text, uint16_t top, uint16_t bottom, uint8_t size);
