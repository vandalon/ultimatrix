// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md.
#include "MqttPixels.h"
#include "NetworkBudget.h"
#include "NoticeImage.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <new>
#include <cstring>

namespace {
constexpr size_t kPixels = 64 * 64, kBytes = kPixels * 2, kReserve = 8192;
bool room(size_t bytes) {
  return heap_caps_get_free_size(MALLOC_CAP_8BIT) >= bytes + kReserve &&
         heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= bytes + 256;
}
struct Reader {
  const char *p;
  void space() { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p; }
  bool take(char c) { space(); if (*p != c) return false; ++p; return true; }
  bool string(const char *&text, size_t &len) {
    if (!take('"')) return false;
    text = p;
    while (*p && *p != '"') { if ((uint8_t)*p < 32 || *p == '\\') return false; ++p; }
    len = p - text;
    if (!*p) return false;
    ++p; return true;
  }
  bool number(uint32_t &n) {
    space(); n = 0;
    if (*p < '0' || *p > '9') return false;
    if (*p == '0' && p[1] >= '0' && p[1] <= '9') return false;
    do { if (n > 100000) return false; n = n * 10 + (*p++ - '0'); } while (*p >= '0' && *p <= '9');
    return true;
  }
  bool boolean(bool &b) {
    space();
    if (!strncmp(p, "true", 4)) { p += 4; b = true; return true; }
    if (!strncmp(p, "false", 5)) { p += 5; b = false; return true; }
    return false;
  }
};
bool same(const char *s, size_t n, const char *value) { return strlen(value) == n && !memcmp(s, value, n); }
bool colour(const char *s, size_t n, uint16_t &out) {
  if (n != 6) return false;
  uint32_t rgb = 0;
  for (size_t i = 0; i < n; ++i) {
    const char c = s[i];
    int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (v < 0) return false;
    rgb = (rgb << 4) | v;
  }
  out = ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F);
  return true;
}
struct Command {
  bool clear = false, stop = false;
  uint32_t duration = 30;
  const char *data = nullptr;
  size_t dataSize = 0;
  unsigned seen = 0;
  bool compact = false;
};
// Two passes: validate without changing anything, then write into a private staging frame.
bool parse(const char *json, Command &cmd, uint16_t *frame = nullptr) {
  Reader r{json};
  if (!r.take('{')) return false;
  if (!r.take('}')) do {
    const char *key; size_t n;
    if (!r.string(key, n) || !r.take(':')) return false;
    unsigned flag = 0;
    if (same(key, n, "clear")) { flag = 1; if (!r.boolean(cmd.clear)) return false; }
    else if (same(key, n, "duration")) { flag = 2; if (!r.number(cmd.duration) || cmd.duration > 86400) return false; }
    else if (same(key, n, "stop")) { flag = 4; if (!r.boolean(cmd.stop)) return false; }
    else if (same(key, n, "width") || same(key, n, "height")) {
      flag = same(key, n, "width") ? 8 : 16;
      uint32_t value; if (!r.number(value) || value != 64) return false;
    } else if (same(key, n, "format")) {
      flag = 32; const char *value; size_t size;
      if (!r.string(value, size)) return false;
      cmd.compact = same(value, size, "rgb565_base64");
      if (!cmd.compact && !same(value, size, "xy_rgb")) return false;
    } else if (same(key, n, "data")) {
      flag = 64; if (!r.string(cmd.data, cmd.dataSize) || cmd.dataSize != 10924) return false;
    } else if (same(key, n, "pixels")) {
      flag = 128; unsigned count = 0;
      if (!r.take('[')) return false;
      if (!r.take(']')) do {
        uint32_t x, y; uint16_t value; const char *hex; size_t size;
        if (++count > 512 || !r.take('[') || !r.number(x) || x > 63 || !r.take(',') ||
            !r.number(y) || y > 63 || !r.take(',') || !r.string(hex, size) || !colour(hex, size, value) || !r.take(']')) return false;
        if (frame) frame[y * 64 + x] = value;
        if (r.take(']')) break;
        if (!r.take(',')) return false;
      } while (true);
    } else return false;
    if (cmd.seen & flag) return false;
    cmd.seen |= flag;
    if (r.take('}')) break;
    if (!r.take(',')) return false;
  } while (true);
  r.space();
  if (*r.p) return false;
  if (cmd.stop) return !(cmd.seen & (1 | 64 | 128));
  if (cmd.data) return cmd.compact && !(cmd.seen & 128);
  return !cmd.compact && (cmd.clear || (cmd.seen & 128));
}
}

void MqttPixels::setStatus(const char *message) {
  std::lock_guard<std::mutex> guard(_mutex);
  strlcpy(_status, message, sizeof(_status));
}
bool MqttPixels::takeStatus(char *out, size_t size) {
  std::lock_guard<std::mutex> guard(_mutex);
  if (!_status[0]) return false;
  strlcpy(out, _status, size); _status[0] = 0; return true;
}
void MqttPixels::cancelReceive() {
  std::lock_guard<std::mutex> guard(_mutex);
  _incoming.reset();
}
void MqttPixels::receive(esp_mqtt_event_handle_t event, bool first) {
  if (first && !event->retain && event->total_data_len > 256 && event->total_data_len <= (int)kMaxPayload && !room(event->total_data_len + 1)) {
    // Too little memory for a big command: park the feeds and give them up to 10 s to hand theirs back. The client holds its API
    // lock while this runs, so the parking happens from here and not from poll(); the rest of the message waits in the socket.
    for (int i = 0; i < 200 && !room(event->total_data_len + 1); i++) {
      notice::holdFeeds(notice::HOLD_MQTT_PIXELS, true);
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
  std::lock_guard<std::mutex> guard(_mutex);
  if (first) {
    _incoming.reset(); _received = 0;
    // Retained drawing commands must not unexpectedly take over after reconnecting.
    if (event->retain || event->total_data_len <= 0 || event->total_data_len > (int)kMaxPayload || _pending ||
        (event->total_data_len > 256 && !room(event->total_data_len + 1))) {
      strlcpy(_status, "Pixel command refused: retained, busy, too large or low memory", sizeof(_status)); return;
    }
    _size = event->total_data_len;
    _incoming.reset(new (std::nothrow) char[_size + 1]);
    _receiveAt = millis();
    if (!_incoming) { strlcpy(_status, "Not enough memory for pixel command", sizeof(_status)); return; }
  }
  if (!_incoming) return;
  if (event->current_data_offset != (int)_received || event->total_data_len != (int)_size ||
      event->data_len < 0 || size_t(event->data_len) > _size - _received) {
    _incoming.reset(); strlcpy(_status, "Incomplete pixel command", sizeof(_status)); return;
  }
  if (memchr(event->data, 0, event->data_len)) {
    _incoming.reset(); strlcpy(_status, "Pixel command contains a null byte", sizeof(_status)); return;
  }
  memcpy(_incoming.get() + _received, event->data, event->data_len);
  _received += event->data_len;
  if (_received == _size) { _incoming[_size] = 0; _pending = std::move(_incoming); }
}
void MqttPixels::apply(const char *json, uint32_t now) {
  Command cmd;
  if (!parse(json, cmd)) { setStatus("Invalid pixel command; see docs/MQTT_PIXELS.md"); return; }
  if (cmd.stop) { _frame.reset(); _timed = false; setStatus("ok"); return; }
  if (!room(kBytes)) { setStatus("Not enough memory for pixel frame"); return; }
  std::unique_ptr<uint16_t[]> next(new (std::nothrow) uint16_t[kPixels]);
  if (!next) { setStatus("Not enough memory for pixel frame"); return; }
  if (!cmd.clear && _frame) memcpy(next.get(), _frame.get(), kBytes);
  else memset(next.get(), 0, kBytes);
  if (cmd.data) {
    size_t written = 0;
    if (mbedtls_base64_decode(reinterpret_cast<unsigned char *>(next.get()), kBytes, &written,
                               reinterpret_cast<const unsigned char *>(cmd.data), cmd.dataSize) || written != kBytes) {
      setStatus("Invalid RGB565 frame data"); return;
    }
  } else {
    Command checked;
    if (!parse(json, checked, next.get())) { setStatus("Invalid pixel command"); return; }
  }
  _frame.swap(next);
  _timed = cmd.duration != 0;
  _until = now + cmd.duration * 1000;
  setStatus("ok");
}
void MqttPixels::poll(uint32_t now, bool enabled) {
  std::unique_ptr<char[]> pending;
  bool receiving;
  {
    std::lock_guard<std::mutex> guard(_mutex);
    receiving = _incoming || _pending;
    if (_incoming && uint32_t(now - _receiveAt) < 0x80000000u && now - _receiveAt > 10000) {
      _incoming.reset(); strlcpy(_status, "Pixel upload timed out", sizeof(_status));
    }
    pending = std::move(_pending);
    if (!enabled) _incoming.reset();
  }
  if (!enabled) { _frame.reset(); _timed = false; }
  else if (pending) apply(pending.get(), now);
  if (_timed && (int32_t)(now - _until) >= 0) { _frame.reset(); _timed = false; }
  notice::holdFeeds(notice::HOLD_MQTT_PIXELS, active() || (receiving && enabled));  // also while a command waits to be applied
}
bool MqttPixels::draw(MatrixPanel_I2S_DMA &display) {
  if (!_frame) return false;
  for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) display.drawPixel(x, y, _frame[y * 64 + x]);
  display.flipDMABuffer();
  return true;
}
