// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

// The start-up, Wi-Fi, access-point, connected and error screens shared by every face. Nothing here blinks: what
// would flash eases in and out through a raised-cosine table instead. The Screen draws only when its Frame
// (kind, brightness step, page, text) differs from the last one drawn, so an idle screen costs a memcmp.
// No String, no waiting: the caller passes the time since the state began.

#include <stdint.h>
#include <Adafruit_GFX.h>

namespace boot {

enum class Kind : uint8_t { Logo, Connecting, Setup, Connected, Syncing, Error, Joined, Update };  // Joined: someone is on the access point

// Full-precision pixel plot for the logo gradient. r, g and b are 8.8 fixed point (0..65280 = level 0..255) so the
// plot can dither between two panel levels; main.cpp feeds the panel's drawPixelRGB888. Without one the logo falls
// back to RGB565, which is all the Adafruit_GFX interface carries and leaves a gradient only a few levels deep.
using Plot888 = void (*)(Adafruit_GFX &gfx, int x, int y, uint16_t r, uint16_t g, uint16_t b);
void setPlot888(Plot888 plot);

// 0..255 raised cosine: 0 at the start of each period, 255 in the middle. offset shifts the phase (ms).
uint8_t pulse(uint32_t ms, uint32_t period, uint32_t offset = 0);

// Which panel pixels the current icon covers and the rows it spans, worked out once per icon
struct IconCache {
  uint8_t cover[64 * 64] = {};
  int8_t icon = -1;
  uint8_t top = 0, bottom = 0;
};

struct Frame {
  uint8_t kind, level, page, glow;  // glow: how far the U is lit up as the logo (31) or dimmed to the backdrop (0)
  char text[16];
};

class Screen {
 public:
  // The network a phone should join. An empty password gives the open-network QR code.
  void setAccessPoint(const char *ssid, const char *password, const char *address);
  // since: ms since the state began (the pulses and the logo's fade-in start from it).
  // Returns true when it drew: the caller flips the buffer.
  bool update(Adafruit_GFX &gfx, Kind kind, uint32_t since, const char *text = "");
  // Forget what was drawn (after anything else used the panel).
  void invalidate() { drawn_ = false; }

 private:
  Frame last_{};
  bool drawn_ = false;
  char ssid_[33] = "";
  char address_[16] = "";
  uint8_t qr_[(6 * 4 + 17) * (6 * 4 + 17) / 8 + 2] = {};  // up to version 6 (the open network is version 1 or 2)
  bool qrReady_ = false;
  IconCache icon_;
};

}  // namespace boot
