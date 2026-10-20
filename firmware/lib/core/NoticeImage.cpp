// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "NoticeImage.h"
#include "NoticePng.h"
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
  char url[kDownloadUrlMax + 1];
  std::shared_ptr<Image> image;
  bool fill = false;
  Client *stream = nullptr;
  uint32_t began = 0;
  size_t bytes = 0, prefixAt = 0;
  uint8_t prefix[8];
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
size_t imageRead(void *context, uint8_t *out, size_t size) {
  Fetch &f = *static_cast<Fetch *>(context);
  const size_t prefix = std::min(size, sizeof(f.prefix) - f.prefixAt);
  memcpy(out, f.prefix + f.prefixAt, prefix); f.prefixAt += prefix;
  return prefix + (prefix < size ? readSome(f, out + prefix, size - prefix) : 0);
}
UINT readJpeg(JDEC *jd, BYTE *buffer, UINT size) {
  Fetch &f = *static_cast<Fetch *>(jd->device);
  if (buffer) return imageRead(&f, buffer, size);
  uint8_t scrap[64];
  UINT skipped = 0;
  while (skipped < size) {
    const size_t n = imageRead(&f, scrap, std::min<size_t>(sizeof(scrap), size - skipped));
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
  // Fill samples a centred square; fit samples the whole image and leaves black bars.
  const int sourceWidth = f.fill ? std::min(f.width, f.height) : f.width;
  const int sourceHeight = f.fill ? sourceWidth : f.height;
  for (int y = 0; y < f.outHeight; y++) {
    const int sy = (f.height - sourceHeight) / 2 + (2 * y + 1) * sourceHeight / (2 * f.outHeight);
    if (sy < rect->top || sy > rect->bottom) continue;
    for (int x = 0; x < f.outWidth; x++) {
      const int sx = (f.width - sourceWidth) / 2 + (2 * x + 1) * sourceWidth / (2 * f.outWidth);
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
  if (!client || !http || !http->begin(*client, f.url)) return false;
  http->useHTTP10(true);
  http->setConnectTimeout(4000);
  http->setTimeout(4000);
  f.began = millis();
  const int code = http->GET();
  bool ok = code == 200 && http->getSize() <= (int)kMaxBytes;
  if (!ok) {
    const char *host = strstr(f.url, "://");
    host = host ? host + 3 : f.url;
    const size_t hostLen = strcspn(host, ":/?");
    bool numeric = hostLen > 0;
    for (size_t i = 0; i < hostLen; i++) if (!isdigit((uint8_t)host[i]) && host[i] != '.') numeric = false;
    const bool local = hostLen > 6 && !strncasecmp(host + hostLen - 6, ".local", 6);
    consoleLog("[Notice] Image request failed: %d (%s), size %d, host %s, heap %u, largest %u, Wi-Fi %d dBm\n", code,
        HTTPClient::errorToString(code).c_str(), (int)http->getSize(), numeric ? "is an IP address" : local ? "is a .local name" : "is a DNS name",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (int)WiFi.RSSI());
  }
  if (ok) {
    f.stream = http->getStreamPtr();
    ok = readSome(f, f.prefix, sizeof(f.prefix)) == sizeof(f.prefix);
    static const uint8_t pngSignature[] = {137,80,78,71,13,10,26,10};
    if (ok && !memcmp(f.prefix, "UMIMG001", sizeof(f.prefix))) {
      // Fixed-size panel-ready pixels: no decoder or extra image allocation.
      uint8_t row[128];
      for (int y = 0; y < 64 && ok; y++) {
        ok = readSome(f, row, sizeof(row)) == sizeof(row);
        if (ok) for (int x = 0; x < 64; x++)
          f.image->pixels[y * 64 + x] = row[x * 2] | (uint16_t(row[x * 2 + 1]) << 8);
      }
      uint8_t extra;
      if (ok) ok = readSome(f, &extra, 1) == 0 && millis() - f.began < kDeadlineMs;
    } else if (ok && f.prefix[0] == 'B' && f.prefix[1] == 'M') {
      // Uncompressed 24-bit 64x64 BMP (header is 54 bytes; the first 8 are in prefix): streamed row by row.
      uint8_t head[46];
      ok = readSome(f, head, sizeof(head)) == sizeof(head);
      auto le = [&](int at) { return uint32_t(head[at - 8]) | head[at - 7] << 8 | head[at - 6] << 16 | uint32_t(head[at - 5]) << 24; };
      const int32_t height = int32_t(le(22));
      ok = ok && le(10) == 54 && le(14) == 40 && le(18) == 64 && (height == 64 || height == -64) &&
           (head[26 - 8] | head[27 - 8] << 8) == 1 && (head[28 - 8] | head[29 - 8] << 8) == 24 && le(30) == 0;
      uint8_t row[192];
      for (int i = 0; i < 64 && ok; i++) {
        ok = readSome(f, row, sizeof(row)) == sizeof(row);
        const int y = height < 0 ? i : 63 - i;
        if (ok) for (int x = 0; x < 64; x++)
          f.image->pixels[y * 64 + x] = (row[x * 3 + 2] & 0xF8) << 8 | (row[x * 3 + 1] & 0xFC) << 3 | row[x * 3] >> 3;
      }
    } else if (ok && !memcmp(f.prefix, pngSignature, sizeof(pngSignature))) {
      ok = decodePng(imageRead, &f, f.image->pixels, f.fill);
    } else if (ok) {
      std::unique_ptr<uint8_t[]> work(new (std::nothrow) uint8_t[4096]);
      std::unique_ptr<JDEC> jd(new (std::nothrow) JDEC);
      ok = work && jd && jd_prepare(jd.get(), readJpeg, work.get(), 4096, &f) == JDR_OK;
      if (ok) {
        ok = jd->width && jd->height && jd->width <= 2048 && jd->height <= 2048;
        if (ok) {
          uint8_t scale = 0;
          while (scale < 3 && (jd->width >> (scale + 1)) >= 64 && (jd->height >> (scale + 1)) >= 64) scale++;
          f.width = jd->width >> scale;
          f.height = jd->height >> scale;
          f.outWidth = f.fill || f.width >= f.height ? 64 : std::max(1, 64 * f.width / f.height);
          f.outHeight = f.fill || f.height >= f.width ? 64 : std::max(1, 64 * f.height / f.width);
          ok = jd_decomp(jd.get(), writeJpeg, scale) == JDR_OK;
        }
      }
    }
  }
  http->end();
  return ok;
}
void task(void *arg) {
  auto *f = static_cast<Fetch *>(arg);
  bool ok = false;
  const char *why = "download or decode";
  {
    // A feed that is parking still holds the gate for a moment: wait for it instead of failing the picture
    std::unique_lock<std::mutex> gate(networkBudget(), std::defer_lock);
    const uint32_t waitFrom = millis();
    while (!gate.try_lock() && millis() - waitFrom < 10000) delay(50);
    if (!gate.owns_lock()) why = "network busy";
    else if (firmwareUpdating) why = "update running";
    else if (liveDataActive()) why = "live data active";
    else if (WiFi.status() != WL_CONNECTED) why = "Wi-Fi down";
    else ok = download(*f);
  }
  f->image->status = ok ? Image::READY : Image::FAILED;
  if (!ok) consoleLog("[Notice] Image failed (%s): use a reachable UMIMG, 24-bit BMP, JPEG or non-interlaced PNG, max 2048x2048 / 128 KiB\n", why);
  delete f;
  busy = false;
  vTaskDelete(nullptr);
}
}
void holdFeeds(FeedHolder who, bool hold) {
  static bool trimmed = false;
  if (hold) feedsHeld |= who; else feedsHeld &= ~who;
  if (!feedsHeld) { trimmed = false; return; }
  if (!trimmed && reapFeedTasks() && feedTrimFn) {  // a parked task keeps its stack until it is deleted; then disposable data goes back
    trimmed = true;
    feedTrimFn(feedTrimArg);
  }
}
std::shared_ptr<Image> loadImage(const char *url, bool fill) {
  if (!url || strlen(url) > kDownloadUrlMax) return {};  // before busy is taken: a refusal here must not leave it set
  const size_t need = url && !strncmp(url, "https://", 8) ? 48000 : 40000;  // TLS needs the bigger reserve
  const size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT), largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (firmwareUpdating || liveDataActive() || WiFi.status() != WL_CONNECTED || freeHeap < need || largest < 16000 || busy.exchange(true)) {
    static uint32_t loggedAt = 0;
    if (millis() - loggedAt > 5000) {
      loggedAt = millis();
      consoleLog("[Notice] Image waits: update %d, live %d, heap %u (need %u), largest block %u (need 16000), busy %d\n",
          (int)firmwareUpdating, (int)liveDataActive(), (unsigned)freeHeap, (unsigned)need, (unsigned)largest, (int)busy.load());
    }
    return {};
  }
  auto *f = new (std::nothrow) Fetch;
  auto *image = new (std::nothrow) Image;
  if (!f || !image) { delete f; delete image; busy = false; return {}; }
  f->image.reset(image);
  strlcpy(f->url, url, sizeof(f->url));
  f->fill = fill;
  auto result = f->image;
  const uint32_t stack = !strncmp(url, "https://", 8) ? 12288 : 9216;
  if (xTaskCreate(task, "noticeImage", stack, f, 1, nullptr) != pdPASS) { delete f; busy = false; return {}; }
  return result;
}
}
