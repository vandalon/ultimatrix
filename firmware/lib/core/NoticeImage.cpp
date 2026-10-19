// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "NoticeImage.h"
#include "Notice.h"
#include "Console.h"
#include "NetworkBudget.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <rom/tjpgd.h>
#include <algorithm>
#include <new>

namespace notice {
namespace {
constexpr size_t kMaxBytes = 128 * 1024;
constexpr uint32_t kDeadlineMs = 12000;
std::atomic<bool> busy{false};
struct Fetch {
  char url[kImageUrlMax];
  std::shared_ptr<Image> image;
  Client *stream = nullptr;
  uint32_t began = 0;
  size_t bytes = 0;
  int width = 0, height = 0, outWidth = 0, outHeight = 0;
};
size_t readSome(Fetch &f, uint8_t *out, size_t size) {
  size_t got = 0;
  while (got < size && f.bytes < kMaxBytes && millis() - f.began < kDeadlineMs && !firmwareUpdating) {
    const int ready = f.stream->available();
    if (ready > 0) {
      const size_t count = std::min<size_t>(ready, std::min(size - got, kMaxBytes - f.bytes));
      const int n = f.stream->read(out + got, count);
      if (n > 0) { got += n; f.bytes += n; continue; }
    } else if (!f.stream->connected()) break;
    delay(1);
  }
  return got;
}
UINT readJpeg(JDEC *jd, BYTE *buffer, UINT size) {
  Fetch &f = *static_cast<Fetch *>(jd->device);
  if (buffer) return readSome(f, buffer, size);
  uint8_t scrap[64];
  UINT skipped = 0;
  while (skipped < size) {
    const size_t n = readSome(f, scrap, std::min<size_t>(sizeof(scrap), size - skipped));
    if (!n) break;
    skipped += n;
  }
  return skipped;
}
UINT writeJpeg(JDEC *jd, void *bitmap, JRECT *rect) {
  Fetch &f = *static_cast<Fetch *>(jd->device);
  if (millis() - f.began >= kDeadlineMs || firmwareUpdating) return 0;
  const uint8_t *rgb = static_cast<const uint8_t *>(bitmap);
  const int stride = rect->right - rect->left + 1;
  // Sample source pixel centres, fit the image and leave black bars. Small images can scale up too.
  for (int y = 0; y < f.outHeight; y++) {
    const int sy = (2 * y + 1) * f.height / (2 * f.outHeight);
    if (sy < rect->top || sy > rect->bottom) continue;
    for (int x = 0; x < f.outWidth; x++) {
      const int sx = (2 * x + 1) * f.width / (2 * f.outWidth);
      if (sx < rect->left || sx > rect->right) continue;
      const uint8_t *p = rgb + ((sy - rect->top) * stride + sx - rect->left) * 3;
      f.image->pixels[(y + (64 - f.outHeight) / 2) * 64 + x + (64 - f.outWidth) / 2] =
          (p[0] & 0xF8) << 8 | (p[1] & 0xFC) << 3 | p[2] >> 3;
    }
  }
  return 1;
}
bool download(Fetch &f) {
  std::unique_ptr<WiFiClient> client;
  const bool tls = !strncmp(f.url, "https://", 8);
  if (tls) {
    auto *secure = new (std::nothrow) WiFiClientSecure;
    if (!secure) return false;
    secure->setInsecure();  // same transport policy as feeds; no HA credentials sent to picture URLs
    client.reset(secure);
  } else client.reset(new (std::nothrow) WiFiClient);
  std::unique_ptr<HTTPClient> http(new (std::nothrow) HTTPClient);
  std::unique_ptr<uint8_t[]> work(new (std::nothrow) uint8_t[4096]);
  std::unique_ptr<JDEC> jd(new (std::nothrow) JDEC);
  if (!client || !http || !work || !jd || !http->begin(*client, f.url)) return false;
  http->useHTTP10(true);
  http->setConnectTimeout(4000);
  http->setTimeout(4000);
  f.began = millis();
  const int code = http->GET();
  bool ok = code == 200 && http->getSize() <= (int)kMaxBytes;
  if (ok) {
    f.stream = http->getStreamPtr();
    ok = jd_prepare(jd.get(), readJpeg, work.get(), 4096, &f) == JDR_OK;
    if (ok) {
      ok = jd->width && jd->height && jd->width <= 2048 && jd->height <= 2048;
      if (ok) {
        uint8_t scale = 0;
        while (scale < 3 && (jd->width >> (scale + 1)) >= 64 && (jd->height >> (scale + 1)) >= 64) scale++;
        f.width = jd->width >> scale;
        f.height = jd->height >> scale;
        f.outWidth = f.width >= f.height ? 64 : std::max(1, 64 * f.width / f.height);
        f.outHeight = f.height >= f.width ? 64 : std::max(1, 64 * f.height / f.width);
        ok = jd_decomp(jd.get(), writeJpeg, scale) == JDR_OK;
      }
    }
  }
  http->end();
  return ok;
}
void task(void *arg) {
  auto *f = static_cast<Fetch *>(arg);
  bool ok = false;
  {
    std::unique_lock<std::mutex> gate(networkBudget(), std::try_to_lock);
    if (gate.owns_lock() && !firmwareUpdating && !liveDataActive() && WiFi.status() == WL_CONNECTED) ok = download(*f);
  }
  f->image->status = ok ? Image::READY : Image::FAILED;
  if (!ok) consoleLog("[Notice] Image failed: use a reachable baseline JPEG, max 2048x2048 / 128 KiB\n");
  delete f;
  busy = false;
  vTaskDelete(nullptr);
}
}
std::shared_ptr<Image> loadImage(const char *url) {
  if (firmwareUpdating || liveDataActive() || WiFi.status() != WL_CONNECTED ||
      heap_caps_get_free_size(MALLOC_CAP_8BIT) < 48000 || heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 16000 || busy.exchange(true)) return {};
  auto *f = new (std::nothrow) Fetch;
  auto *image = new (std::nothrow) Image;
  if (!f || !image) { delete f; delete image; busy = false; return {}; }
  f->image.reset(image);
  strlcpy(f->url, url, sizeof(f->url));
  auto result = f->image;
  const uint32_t stack = !strncmp(url, "https://", 8) ? 12288 : 9216;
  if (xTaskCreate(task, "noticeImage", stack, f, 1, nullptr) != pdPASS) { delete f; busy = false; return {}; }
  return result;
}
}
