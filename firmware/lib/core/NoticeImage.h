// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once
#include <atomic>
#include <memory>
#include <stddef.h>
#include <stdint.h>

namespace notice {
constexpr size_t kDownloadUrlMax = 1024; // longest accepted picture URL (signed media URLs are long)
struct Image {
  enum Status { LOADING, READY, FAILED };
  std::atomic<Status> status{LOADING};
  uint16_t pixels[64 * 64] = {};
};
enum FeedHolder : uint8_t { HOLD_NOTICE = 1, HOLD_DLNA = 2, HOLD_MQTT_PIXELS = 4 };
// Parks the football/F1/MotoGP/Home feeds and hands back their heap while a picture loads or shows; call every poll
void holdFeeds(FeedHolder who, bool hold);
// One bounded HTTP(S) JPEG/PNG download at a time, outside the display task. Null means try later.
std::shared_ptr<Image> loadImage(const char *url, bool fill);
}
