// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "HaMedia.h"

#include <Arduino.h>
#include <atomic>
#include <stdarg.h>
#include <memory>
#include <mutex>
#include <new>
#include <string.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <rom/tjpgd.h>
#include "Console.h"

extern std::atomic<bool> firmwareUpdating;

namespace ha {
namespace media {

namespace {

std::mutex lock;
Snapshot current{};
bool haveSnapshot = false;
std::atomic<uint32_t> changes{0};

// ---- Album art ----
constexpr uint32_t kRetryMs = 60000;
constexpr size_t kWorkArea = 4096;  // the decoder's own memory (it needs about 3.1 KB)
std::atomic<bool> fetching{false};
char haveFor[kPicture] = "";      // the picture the ready pixels belong to
char failedFor[kPicture] = "";
uint32_t failedAt = 0;
uint16_t *ready = nullptr;
uint16_t coverId = 0;             // changes with every cover that becomes ready        // kArt*kArt RGB565, written by the fetch task, read under the lock
char wanted[kPicture] = "";
char note[48] = "No picture yet";  // the last step of the art fetch, for the settings page

void setNote(const char *format, ...) __attribute__((format(printf, 1, 2)));
void setNote(const char *format, ...) {
  va_list args;
  va_start(args, format);
  char text[sizeof(note)];
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  std::lock_guard<std::mutex> guard(lock);
  strlcpy(note, text, sizeof(note));
}

// A character of the page: the 3x5/4x5 fonts only have plain ASCII
char fold(uint32_t cp) {
  static const char kLatin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";  // U+00C0..U+00FF
  if (cp < 0x20) return ' ';
  if (cp < 0x7f) return (char)cp;
  if (cp >= 0xC0 && cp <= 0xFF) return kLatin1[cp - 0xC0];
  if (cp == 0x2018 || cp == 0x2019) return '\'';
  if (cp == 0x201C || cp == 0x201D) return '"';
  if (cp == 0x2013 || cp == 0x2014) return '-';
  return '?';
}

void copyField(char *to, size_t size, const char *from, size_t length) {
  if (length >= size) length = size - 1;
  memcpy(to, from, length);
  to[length] = 0;
}

struct Fetch {
  Client *stream;
  uint16_t *out;               // kArt*kArt RGB565
  uint8_t *column, *row;       // per source column/row of the decoded image: destination index + 1 when that one is the sample, else 0
  int width, height;           // of the decoded (scaled) image
};

// Up to size bytes from the connection: waits for data in flight, and only gives up when it ends or goes quiet for 5 s
size_t readSome(Client *stream, uint8_t *buffer, size_t size) {
  size_t got = 0;
  uint32_t last = millis();
  while (got < size) {
    const int ready = stream->available();
    if (ready > 0) {
      const int n = stream->read(buffer + got, std::min<size_t>(ready, size - got));
      if (n > 0) { got += n; last = millis(); continue; }
    } else if (!stream->connected() || millis() - last > 5000) break;
    delay(1);
  }
  return got;
}

UINT readJpeg(JDEC *jd, BYTE *buffer, UINT size) {
  Fetch *fetch = (Fetch *)jd->device;
  if (buffer) return readSome(fetch->stream, buffer, size);
  UINT skipped = 0;
  uint8_t scrap[64];
  while (skipped < size) {
    const size_t got = readSome(fetch->stream, scrap, std::min<UINT>(sizeof(scrap), size - skipped));
    if (!got) break;
    skipped += got;
  }
  return skipped;
}

UINT writeJpeg(JDEC *jd, void *bitmap, JRECT *rect) {
  Fetch *fetch = (Fetch *)jd->device;
  const uint8_t *rgb = (const uint8_t *)bitmap;
  for (int y = rect->top; y <= rect->bottom; y++) {
    const int dy = fetch->row[y];
    for (int x = rect->left; x <= rect->right; x++, rgb += 3) {
      const int dx = fetch->column[x];
      if (dx && dy) fetch->out[(dy - 1) * kArt + dx - 1] = (rgb[0] & 0xF8) << 8 | (rgb[1] & 0xFC) << 3 | rgb[2] >> 3;
    }
  }
  return 1;
}

bool download(const Config &c, const char *path, uint16_t *out) {  // the picture, sampled down to kArt x kArt, into out
  if (path[0] != '/') { setNote("Picture is not on Home Assistant"); return false; }  // only pictures Home Assistant serves itself
  std::unique_ptr<WiFiClient> plain;
  std::unique_ptr<WiFiClientSecure> secure;
  std::unique_ptr<HTTPClient> http(new (std::nothrow) HTTPClient);
  std::unique_ptr<Fetch> fetch(new (std::nothrow) Fetch{});
  std::unique_ptr<uint8_t[]> work(new (std::nothrow) uint8_t[kWorkArea]);
  std::unique_ptr<char[]> bearer(new (std::nothrow) char[kTokenMax + 8]);
  std::unique_ptr<JDEC> jd(new (std::nothrow) JDEC);
  if (!http || !fetch || !work || !bearer || !jd) { setNote("Out of memory"); return false; }
  fetch->out = out;

  bool started;
  if (c.tls) {
    secure.reset(new (std::nothrow) WiFiClientSecure);
    if (!secure) return false;
    secure->setInsecure();
    started = http->begin(*secure, c.host, c.port, path, true);
  } else {
    plain.reset(new (std::nothrow) WiFiClient);
    if (!plain) return false;
    started = http->begin(*plain, c.host, c.port, path);
  }
  if (!started) { setNote("Cannot connect"); return false; }
  http->useHTTP10(true);  // no chunked answer: the stream is the picture itself
  http->setTimeout(4000);
  snprintf(bearer.get(), kTokenMax + 8, "Bearer %s", c.token);
  http->addHeader("Authorization", bearer.get());
  const int code = http->GET();
  if (code != 200) { http->end(); setNote(code > 0 ? "Picture request: HTTP %d" : "Picture request failed (%d)", code); return false; }

  fetch->stream = http->getStreamPtr();
  const int prepared = jd_prepare(jd.get(), readJpeg, work.get(), kWorkArea, fetch.get());
  bool ok = prepared == JDR_OK;
  if (!ok) setNote("Not a baseline JPEG (code %d)", prepared);
  if (ok) consoleLog("[HA] album art: %ux%u JPEG\n", (unsigned)jd->width, (unsigned)jd->height);
  if (ok) {
    // The biggest reduction (1/1 to 1/8) that still leaves at least kArt pixels each way
    uint8_t scale = 0;
    while (scale < 3 && (jd->width >> (scale + 1)) >= (UINT)kArt && (jd->height >> (scale + 1)) >= (UINT)kArt) scale++;
    fetch->width = jd->width >> scale;
    fetch->height = jd->height >> scale;
    ok = fetch->width >= 8 && fetch->height >= 8;
    std::unique_ptr<uint8_t[]> maps(ok ? new (std::nothrow) uint8_t[fetch->width + fetch->height]() : nullptr);
    if (ok && !maps) { setNote("Out of memory"); ok = false; }
    if (ok) {
      fetch->column = maps.get();
      fetch->row = maps.get() + fetch->width;
      // Each destination pixel takes the middle source pixel of its share of the picture
      for (int i = 0; i < kArt; i++) {
        fetch->column[std::min(fetch->width - 1, (i * fetch->width + fetch->width / 2) / kArt)] = i + 1;
        fetch->row[std::min(fetch->height - 1, (i * fetch->height + fetch->height / 2) / kArt)] = i + 1;
      }
      const int decoded = jd_decomp(jd.get(), writeJpeg, scale);
      ok = decoded == JDR_OK;
      if (!ok) setNote("JPEG decode failed (code %d, %ux%u)", decoded, (unsigned)jd->width, (unsigned)jd->height);
    } else setNote("Picture too small");
  }
  http->end();
  return ok;
}

void fetchTask(void *) {
  char picture[kPicture];
  uint16_t *pixels;
  {
    std::lock_guard<std::mutex> guard(lock);
    strlcpy(picture, wanted, sizeof(picture));
    haveFor[0] = 0;  // the cover is rewritten in place: nobody draws it meanwhile
    if (!ready) ready = (uint16_t *)malloc(kArt * kArt * sizeof(uint16_t));
    pixels = ready;
  }
  bool ok = false;
  if (!pixels) setNote("Out of memory");
  else if (WiFi.status() == WL_CONNECTED && !firmwareUpdating) ok = download(config(), picture, pixels);
  {
    std::lock_guard<std::mutex> guard(lock);
    if (ok) { strlcpy(haveFor, picture, sizeof(haveFor)); coverId++; }
    else { strlcpy(failedFor, picture, sizeof(failedFor)); failedAt = millis(); }
  }
  if (ok) setNote("Loaded");
  char shown[sizeof(note)];
  artNote(shown, sizeof(shown));
  consoleLog("[HA] album art: %s\n", shown);
  fetching = false;
  vTaskDelete(nullptr);
}

}  // namespace

size_t unescape(const char *json, char *out, size_t capacity) {
  size_t n = 0;
  auto put = [&](char c) { if (n + 1 < capacity) out[n++] = c; };
  for (const unsigned char *p = (const unsigned char *)json; *p && *p != '"';) {
    uint32_t cp;
    if (*p == '\\') {
      p++;
      switch (*p) {
        case 'u': {
          cp = 0;
          for (int i = 1; i <= 4 && p[i]; i++) cp = cp << 4 | (p[i] <= '9' ? p[i] - '0' : (p[i] | 32) - 'a' + 10);
          p += p[1] && p[2] && p[3] && p[4] ? 5 : 1;
          break;
        }
        case 'n': case 't': case 'r': cp = ' '; p++; break;
        case 0: continue;
        default: cp = *p++; break;
      }
    } else if (*p < 0x80) {
      cp = *p++;
    } else {  // UTF-8
      const int extra = *p >= 0xF0 ? 3 : *p >= 0xE0 ? 2 : 1;
      cp = *p & (0x3F >> extra);
      p++;
      for (int i = 0; i < extra && (*p & 0xC0) == 0x80; i++) cp = cp << 6 | (*p++ & 0x3F);
    }
    put(fold(cp));
  }
  out[n] = 0;
  return n;
}

size_t buildTemplate(const Config &config, char *out, size_t capacity) {
  (void)config;
  char (*players)[kEntity] = mediaPlayers();
  size_t at = 0;
  auto add = [&](const char *text) {
    const size_t length = strlen(text);
    if (at + length + 1 > capacity) { at = capacity; return; }
    memcpy(out + at, text, length);
    at += length;
  };
  bool any = false;
  add("{% set ns = namespace(r=[]) %}{% for e in [");
  for (int i = 0; i < kRooms; i++) {
    if (!players[i][0]) continue;
    add(any ? ",'" : "'");
    add(players[i]);
    add("'");
    any = true;
  }
  if (!any) return 0;
  // One entry per playing player: room|title|artist|picture. Text is cut and cleaned here, the picture is dropped when too long to keep whole.
  add("] %}{% if is_state(e,'playing') %}{% set up = state_attr(e,'media_position_updated_at') %}{% set dur = state_attr(e,'media_duration')|int(0) %}{% set pos = ((state_attr(e,'media_position')|float(0)) + ((as_timestamp(now()) - as_timestamp(up)) if up else 0))|int(0) %}{% set pic = state_attr(e,'entity_picture_local') or state_attr(e,'entity_picture') or '' %}"
      "{% set ns.r = ns.r + [(state_attr(e,'friendly_name') or e)|string|replace('|',' ')|replace(';',' ')|truncate(15,true,'') ~ '|'"
      " ~ (state_attr(e,'media_title') or '')|string|replace('|',' ')|replace(';',' ')|truncate(47,true,'') ~ '|'"
      " ~ (state_attr(e,'media_artist') or '')|string|replace('|',' ')|replace(';',' ')|truncate(47,true,'') ~ '|'"
      " ~ (pic if pic|length < 256 else '!' ~ pic|length)|replace(';','')|replace('|','') ~ '|' ~ pos ~ '|' ~ dur] %}{% endif %}{% endfor %}{{ ns.r|join(';') }}");
  if (at >= capacity) return 0;
  out[at] = 0;
  return at;
}

void parseResult(const char *text, Snapshot &out) {
  memset(&out, 0, sizeof(out));
  const char *at = text;
  while (*at && out.count < kRooms) {
    const char *end = strchr(at, ';');
    if (!end) end = at + strlen(at);
    const char *part[6] = {at, nullptr, nullptr, nullptr, nullptr, nullptr};
    size_t length[6] = {};
    int field = 0;
    for (const char *c = at; c <= end && field < 6; c++) {
      if (c == end || *c == '|') {
        length[field] = c - part[field];
        if (++field < 6) part[field] = c + 1;
      }
    }
    if (field == 6) {
      Player &p = out.player[out.count++];
      copyField(p.room, sizeof(p.room), part[0], length[0]);
      copyField(p.title, sizeof(p.title), part[1], length[1]);
      copyField(p.artist, sizeof(p.artist), part[2], length[2]);
      copyField(p.picture, sizeof(p.picture), part[3], length[3]);
      p.position = strtoul(part[4], nullptr, 10);
      p.duration = strtoul(part[5], nullptr, 10);
    }
    at = *end ? end + 1 : end;
  }
}

void clear() {
  std::lock_guard<std::mutex> guard(lock);
  if (!current.count) return;
  memset(&current, 0, sizeof(current));
  changes++;
}

void publish(const Snapshot &snapshot) {
  std::lock_guard<std::mutex> guard(lock);
  current = snapshot;
  for (int i = 0; i < current.count; i++) current.player[i].at = millis();
  haveSnapshot = true;
  changes++;
}

bool snapshot(Snapshot &out) {
  std::lock_guard<std::mutex> guard(lock);
  out = current;
  return haveSnapshot;
}

uint32_t version() { return changes; }

void artNote(char *out, size_t size) {
  std::lock_guard<std::mutex> guard(lock);
  strlcpy(out, note, size);
}

void release() {
  if (fetching) return;
  std::lock_guard<std::mutex> guard(lock);
  free(ready);
  ready = nullptr;
  haveFor[0] = 0;
}

ArtState art(const char *picture, const uint16_t **pixels, uint16_t *id) {
  if (!picture[0]) { setNote("The player has no picture"); return ART_NONE; }
  if (picture[0] == '!') { setNote("Picture address too long (%s)", picture + 1); return ART_NONE; }
  if (picture[0] != '/') { setNote("Picture is not on Home Assistant: %.14s", picture); return ART_NONE; }
  {
    std::lock_guard<std::mutex> guard(lock);
    if (ready && !strcmp(haveFor, picture)) { *pixels = ready; *id = coverId; return ART_READY; }
    if (!strcmp(failedFor, picture) && millis() - failedAt < kRetryMs) return ART_FAILED;
    strlcpy(wanted, picture, sizeof(wanted));
  }
  if (fetching.exchange(true)) return ART_LOADING;  // one fetch at a time; the page asks again
  // Data memory only: getMaxAllocHeap() counts instruction RAM that no buffer can use
  if (firmwareUpdating || WiFi.status() != WL_CONNECTED || heap_caps_get_free_size(MALLOC_CAP_8BIT) < 30000 || heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 14000) {
    fetching = false;
    setNote("Waiting for memory (%u free)", (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return ART_LOADING;
  }
  if (xTaskCreate(fetchTask, "haArt", config().tls ? 12288 : 9216, nullptr, 1, nullptr) != pdPASS) { fetching = false; setNote("Could not start the fetch"); }
  return ART_LOADING;
}

}  // namespace media
}  // namespace ha
