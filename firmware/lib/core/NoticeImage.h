// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once
#include <atomic>
#include <memory>
#include <stdint.h>

namespace notice {
struct Image {
  enum Status { LOADING, READY, FAILED };
  std::atomic<Status> status{LOADING};
  uint16_t pixels[64 * 64] = {};
};
// One bounded HTTP(S) JPEG download at a time, outside the display task. Null means try later.
std::shared_ptr<Image> loadImage(const char *url);
}
