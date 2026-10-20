// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Wled.h"
#include "GzipAsset.h"
#include "Console.h"
#include <DiskCache.h>
#include <NetworkBudget.h>
#include <NoticeImage.h>
#include <errno.h>
#include <unistd.h>
#include <esp_crt_bundle.h>
#include <esp_random.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <mbedtls/ssl.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <algorithm>
#include <memory>
#include <new>
#include <stdlib.h>
#if __has_include("klipy_key.h")
#include "klipy_key.h"
#define UM_KLIPY_KEY 1
#endif

// The canvas: a 64x64 RGB565 buffer that the pixel editor (/canvas), the segment's "i" array and saved drawings write to,
// shown by the Paint effect. Its own small API (/api/canvas/...), independent of WLED's JSON protocol. Drawings are
// stored packed in the flash file system, up to 8 slots; a slot with several frames is an animation.
namespace wled {
namespace canvas {
UM_GZIP_ASSET(canvasHtmlGz, "canvas_html")

namespace {
constexpr int kMaxFrames = 64;
constexpr uint32_t kMagic = 0x31564357;  // "WCV1"
constexpr size_t kBufferBytes = kPixels * 2;
constexpr size_t kCanvasHeadroom = 8000;  // data heap that stays free after the canvas buffer itself
constexpr size_t kHeadroom = 18000;  // data heap (8-bit capable) that stays free after the canvas, a last-frame copy or an import frame is allocated; the clock normally runs with 15 to 50 KB

struct Header { uint32_t magic; uint8_t frames, reserved; uint16_t delay; char name[16]; };

WebServer *srv = nullptr;
uint16_t *buf = nullptr;
// Indexed playback reuses the first 4096 bytes of the editor buffer; no RGB888 framebuffer.
const uint8_t *rgbPalette = nullptr;
int paletteCount = 0;
bool indexedCanvas = false;
void materializeCanvas();
bool mounted = false, booted = false;
int shown = -1;  // saved drawing on show, unedited

// An imported animation lives in memory only: it goes when WLED lets go of the panel, so it never uses the flash
constexpr int kRamSlot = kDrawingSlots;
bool importingNow = false;  // an import is uploading: the panel shows a progress bar, not the half-built pictures
uint32_t importingAt = 0;
int importTotal = 0;
struct RamStore { uint8_t *frame[kMaxFrames]; uint16_t size[kMaxFrames]; int frames = 0; size_t bytes = 0; uint16_t delay = 100; } ram;

struct Animation { int slot = -1, frames = 0, index = 0; uint32_t nextAt = 0, delay = 100, offsets[kMaxFrames]; } anim;
uint32_t rateAt = 0, rateFrames = 0, lateMax = 0, lateCount = 0, decodeMaxUs = 0;
void resetRate() { rateAt = millis(); rateFrames = lateMax = lateCount = decodeMaxUs = 0; }
uint16_t *last = nullptr;  // the frame saved last, so the next one is stored as a difference; freed when the editor goes quiet
int lastSlot = -1, lastFrames = 0;
uint32_t lastAt = 0, cycleNext = 0;
int cycleLast = -1;

uint8_t *upload = nullptr;
size_t uploadSize = 0, uploadAt = 0;
void freeUpload() { free(upload); upload = nullptr; uploadSize = uploadAt = 0; }

void freeLast() { free(last); last = nullptr; lastSlot = -1; }
void freeRam() {
  const size_t freed = ram.bytes + uploadSize;
  materializeCanvas();
  rgbPalette = nullptr; paletteCount = 0;
  freeUpload();
  for (int i = 0; i < ram.frames; ++i) {
    free(ram.frame[i]); ram.frame[i] = nullptr; ram.size[i] = 0;
  }
  ram.frames = 0; ram.bytes = 0;
  if (anim.slot == kRamSlot) { anim.slot = -1; anim.frames = 0; }
  if (shown == kRamSlot) shown = -1;
  if (lastSlot == kRamSlot) lastSlot = -1;
  if (freed) Serial.printf("[Canvas] freed %u frame bytes, free=%u block=%u\n", (unsigned)freed,
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
void trimLast(uint32_t now) { if (last && (int32_t)(now - lastAt - 15000) >= 0) freeLast(); }

bool mount() { if (!mounted) mounted = beginDiskCache(); return mounted; }
void path(int id, char *out) { snprintf(out, 16, "/cv%d.bin", id); }

bool readHeader(File &f, Header &h) {
  return f.read((uint8_t *)&h, sizeof(h)) == sizeof(h) && h.magic == kMagic && h.frames >= 1 && h.frames <= kMaxFrames;
}

// ---- Frames: u16 (byte count in the low 14 bits, mode in the top 2), then the payload ----
// RAM imports also use mode 3: one RGB888 palette, packed indexes or index runs/deltas.
// mode 0: runs of (count 1..255 u8, value u16). mode 1 (never the first frame): against the frame before it,
// either a run (count 1..127, value u16) or 128+n = "the next n pixels are unchanged". mode 2: the raw pixels.
constexpr uint16_t kLengthMask = 0x3FFF;

struct Sink {  // writes to a file, or only counts when there is none
  File *f; uint8_t *mem = nullptr; uint8_t chunk[96]; size_t used = 0, total = 0; bool ok = true;
  Sink() : f(nullptr) {}  // only counts
  explicit Sink(File *file) : f(file) {}
  explicit Sink(uint8_t *memory) : f(nullptr), mem(memory) {}
  void put(uint8_t b) { ++total; if (mem) { mem[total - 1] = b; return; } if (!f) return; chunk[used++] = b; if (used == sizeof(chunk)) flush(); }
  void flush() { if (f && used && f->write(chunk, used) != used) ok = false; used = 0; }
};

void encode(int mode, const uint16_t *pixels, const uint16_t *prev, Sink &out) {
  for (int i = 0; i < kPixels;) {
    if (mode == 2) { out.put(pixels[i] & 255); out.put(pixels[i] >> 8); ++i; continue; }
    const int limit = mode == 1 ? 127 : 255;
    int n = 1;
    if (mode == 1 && pixels[i] == prev[i]) {
      while (i + n < kPixels && n < limit && pixels[i + n] == prev[i + n]) ++n;
      out.put(128 + n);
    } else {
      while (i + n < kPixels && n < limit && pixels[i + n] == pixels[i]) ++n;
      out.put(n); out.put(pixels[i] & 255); out.put(pixels[i] >> 8);
    }
    i += n;
  }
}

// The smallest of the three codings; prev is the frame before this one, none for the first
size_t frameSize(const uint16_t *pixels, const uint16_t *prev, int &mode) {
  Sink a;
  encode(0, pixels, nullptr, a);
  size_t best = a.total;
  mode = 0;
  if (prev) { Sink d; encode(1, pixels, prev, d); if (d.total < best) { best = d.total; mode = 1; } }
  if (kBufferBytes < best) { best = kBufferBytes; mode = 2; }
  return best;
}

bool writeFrame(File &f, const uint16_t *pixels, const uint16_t *prev) {
  int mode;
  const size_t bytes = frameSize(pixels, prev, mode);
  const uint16_t head = (uint16_t)(bytes | mode << 14);
  if (f.write((const uint8_t *)&head, 2) != 2) return false;
  Sink out(&f);
  encode(mode, pixels, prev, out);
  out.flush();
  return out.ok;
}

struct Reader {
  File *f; const uint8_t *mem; size_t memAt = 0; uint8_t chunk[96]; size_t at = 0, have = 0; uint16_t left;
  Reader(File &file, uint16_t bytes) : f(&file), mem(nullptr), left(bytes) {}
  Reader(const uint8_t *memory, uint16_t bytes) : f(nullptr), mem(memory), left(bytes) {}
  int get() {
    if (mem) return left ? (--left, mem[memAt++]) : -1;
    if (at >= have) {
      const size_t want = left < sizeof(chunk) ? left : sizeof(chunk);
      if (!want || f->read(chunk, want) != want) return -1;
      have = want; at = 0; left -= want;
    }
    return chunk[at++];
  }
};

// out holds the frame before this one when the frame is a difference
bool decodeBody(Reader &in, int mode, uint16_t bytes, uint16_t *out) {
  if (mode > 2 || (mode == 2 && bytes != kBufferBytes)) return false;
  int at = 0;
  while (at < kPixels) {
    if (mode == 2) {
      const int lo = in.get(), hi = in.get();
      if (hi < 0) return false;
      out[at++] = lo | hi << 8;
      continue;
    }
    const int n = in.get();
    if (n < 0) return false;
    if (mode == 1 && n > 128) { at += n - 128; continue; }
    const int lo = in.get(), hi = in.get();
    if (n == 0 || hi < 0) return false;
    for (int k = 0; k < n && at < kPixels; ++k) out[at++] = lo | hi << 8;
  }
  return at == kPixels;
}

bool readFrame(File &f, uint16_t *out) {
  uint16_t head;
  if (f.read((uint8_t *)&head, 2) != 2) return false;
  Reader in(f, head & kLengthMask);
  return decodeBody(in, head >> 14, head & kLengthMask, out);
}

// RAM-only mode 3: flags, optional u16 palette count + RGB888 palette, then indexes.
// Flag bit 2 makes LZ data XOR differences against the preceding frame.
// Flags bit 7 introduces the palette; low bits: 0 packed, 1 runs, 2 delta runs/skips, 3 LZ.
// Validate before touching the displayed frame, including indexes in skipped regions.
bool readIndexed(const uint8_t *blob, size_t size, bool apply) {
  if (!buf || size < 3) return false;
  size_t at = 2;
  const int flags = blob[at++], coding = flags & 3;
  const bool difference = flags & 4;
  if ((flags & ~0x87) || (difference && (coding != 3 || (flags & 128)))) return false;
  const uint8_t *palette = rgbPalette;
  int colors = paletteCount;
  if (flags & 128) {
    if (at + 2 > size || coding == 2) return false;
    colors = blob[at] | blob[at + 1] << 8; at += 2;
    if (colors < 1 || colors > 256 || at + colors * 3 > size) return false;
    palette = blob + at; at += colors * 3;
  } else if (!palette || !indexedCanvas) return false;
  auto *indexes = reinterpret_cast<uint8_t *>(buf);
  int pixel = 0;
  if (coding == 0) {
    int bits = 1;
    while ((1 << bits) < colors) ++bits;
    if (size - at != (kPixels * bits + 7) / 8) return false;
    uint32_t pending = 0; int available = 0;
    while (pixel < kPixels) {
      while (available < bits) { pending |= (uint32_t)blob[at++] << available; available += 8; }
      const int index = pending & ((1 << bits) - 1);
      pending >>= bits; available -= bits;
      if (index >= colors) return false;
      if (apply) indexes[pixel] = index;
      ++pixel;
    }
  } else if (coding == 3) {
    // XOR differences decode into the unused half of the existing 8 KB canvas.
    // This preserves the previous indexes until the complete result has been validated.
    uint8_t *decoded = difference ? indexes + kPixels : indexes;
    while (pixel < kPixels && at < size) {
      const int token = blob[at++];
      if (token < 128) {
        const int count = token + 1;
        if (at + count > size || pixel + count > kPixels) return false;
        for (int i = 0; i < count; ++i) {
          const int index = blob[at++];
          if (!difference && index >= colors) return false;
          if (apply || difference) decoded[pixel] = index;
          ++pixel;
        }
      } else {
        const int count = token - 128 + 3;
        if (at + 2 > size || pixel + count > kPixels) return false;
        const int distance = blob[at] | blob[at + 1] << 8; at += 2;
        if (distance < 1 || distance > pixel) return false;
        for (int i = 0; i < count; ++i) {
          if (apply || difference) decoded[pixel] = decoded[pixel - distance];
          ++pixel;
        }
      }
    }
  } else {
    while (pixel < kPixels && at < size) {
      const int n = blob[at++];
      if (coding == 2 && n > 128) {
        const int count = n - 128;
        if (pixel + count > kPixels) return false;
        pixel += count;
      } else {
        if (!n || (coding == 2 && n > 127) || at >= size || pixel + n > kPixels) return false;
        const int index = blob[at++];
        if (index >= colors) return false;
        if (apply) memset(indexes + pixel, index, n);
        pixel += n;
      }
    }
  }
  if (pixel != kPixels || at != size) return false;
  if (difference) {
    const uint8_t *delta = indexes + kPixels;
    for (int i = 0; i < kPixels; ++i)
      if ((indexes[i] ^ delta[i]) >= colors) return false;
    if (apply)
      for (int i = 0; i < kPixels; ++i) indexes[i] ^= delta[i];
  }
  if (apply) { rgbPalette = palette; paletteCount = colors; indexedCanvas = true; }
  return true;
}

void materializeCanvas() {
  if (!indexedCanvas || !buf || !rgbPalette) return;
  const auto *indexes = reinterpret_cast<const uint8_t *>(buf);
  for (int i = kPixels - 1; i >= 0; --i) {
    const uint8_t *c = rgbPalette + indexes[i] * 3;
    buf[i] = (c[0] & 0xF8) << 8 | (c[1] & 0xFC) << 3 | c[2] >> 3;
  }
  indexedCanvas = false;
}

bool readMem(const uint8_t *blob, uint16_t *out) {
  const uint16_t head = blob[0] | blob[1] << 8;
  if (head >> 14 == 3) return out == buf && readIndexed(blob, (head & kLengthMask) + 2, true);
  indexedCanvas = false;
  Reader in(blob + 2, head & kLengthMask);
  return decodeBody(in, head >> 14, head & kLengthMask, out);
}

size_t slotBytes(int id) {  // the saved file's size, before the filesystem rounds it up to whole blocks
  char name[16];
  path(id, name);
  File f = LittleFS.open(name, "r");
  const size_t size = f ? f.size() : 0;
  if (f) f.close();
  return size;
}
bool slotInfo(int id, Header &h) {
  if (!mount()) return false;
  char name[16];
  path(id, name);
  File f = LittleFS.open(name, "r");
  if (!f) return false;
  const bool ok = readHeader(f, h);
  f.close();
  return ok;
}

// Opens a slot, notes where each frame starts and decodes the first one into out
bool openSlot(int id, uint16_t *out) {
  if (id < 0 || id >= kDrawingSlots || !mount()) return false;
  char name[16];
  path(id, name);
  File f = LittleFS.open(name, "r");
  Header h;
  if (!f || !readHeader(f, h)) { if (f) f.close(); return false; }
  Animation next;
  next.slot = id; next.frames = h.frames; next.delay = h.delay < 20 ? 20 : h.delay;
  for (int i = 0; i < h.frames; ++i) {
    next.offsets[i] = f.position();
    uint16_t head;
    if (f.read((uint8_t *)&head, 2) != 2 || !f.seek(f.position() + (head & kLengthMask))) { f.close(); return false; }
  }
  const bool ok = f.seek(next.offsets[0]) && readFrame(f, out);
  f.close();
  if (ok) { anim = next; resetRate(); }
  return ok;
}

bool showFrame(int index) {
  if (!buf || anim.slot < 0) return false;
  if (anim.slot == kRamSlot) return index < ram.frames && readMem(ram.frame[index], buf);
  char name[16];
  path(anim.slot, name);
  File f = LittleFS.open(name, "r");
  const bool ok = f && f.seek(anim.offsets[index]) && readFrame(f, buf);
  if (f) f.close();
  return ok;
}

void sanitize(const String &in, char *out) {
  size_t n = 0;
  for (size_t i = 0; i < in.length() && n < 15; ++i) {
    const char c = in[i];
    if (isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_' || c == '.') out[n++] = c;
  }
  out[n] = 0;
}

inline uint16_t pack(uint8_t r, uint8_t g, uint8_t b) { return (r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3; }

// ---- HTTP ----
void json(int status, const String &body) { srv->sendHeader("Cache-Control", "no-store"); srv->send(status, "application/json", body); }
void message(int status, const char *text) {
  if (status >= 400) consoleLog("[Canvas] %d: %s\n", status, text);
  json(status, String("{\"message\":\"") + text + "\"}");
}

// Who writes to the canvas: a line whenever the writer changes or after a quiet spell, plus every clear, save, load and delete (serial)
void noteWriter(const char *what) {
  static uint32_t lastIp = 0, lastAt = 0;
  const uint32_t ip = (uint32_t)srv->client().remoteIP();
  const uint32_t now = millis();
  if (ip != lastIp || (int32_t)(now - lastAt) > 3000 || what[0] == '!') {
    Serial.printf("[Canvas] %s from %u.%u.%u.%u (shown=%d frames=%d ram=%d free=%u block=%u)\n", what[0] == '!' ? what + 1 : what, (unsigned)(ip & 255), (unsigned)(ip >> 8 & 255), (unsigned)(ip >> 16 & 255), (unsigned)(ip >> 24), shown, anim.frames, ram.frames, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    lastIp = ip;
  }
  lastAt = now;
}

// Nothing may draw over a playing animation: a page that still holds an older picture would put it back over the new one
bool playing() { return anim.slot == kRamSlot && anim.frames > 1 && shown == kRamSlot; }

// Short of memory: refuse the request that would allocate (its body, a frame blob) before it pushes the heap into allocation failures
bool lowMemory() {
  if (heap_caps_get_free_size(MALLOC_CAP_8BIT) >= 9000) return false;
  noteWriter("!refused, low memory");
  message(503, "The clock is short of memory right now: try again in a moment.");
  return true;
}

// Predict room before uploading: retain the runtime reserve plus a small allowance
// for request strings and allocator overhead. Every frame is checked again on arrival.
long importRoom() {
  long room = (long)heap_caps_get_free_size(MALLOC_CAP_8BIT) - (long)kHeadroom - (buf ? 0 : (long)kBufferBytes);
  room -= 2048;  // the status GET has a smaller request footprint than frame POSTs
  return room > 0 ? room : 0;
}

bool frameFits(size_t bytes) {
  return heap_caps_get_free_size(MALLOC_CAP_8BIT) >= bytes + kHeadroom &&
         heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= bytes + 256;
}

void info() {
  trimLast(millis());
  String out = String("{\"active\":") + (buf ? "true" : "false") + ",\"slot\":" + shown + ",\"cycle\":" + options().cycleSec + ",\"frames\":" + (anim.frames > 1 ? anim.frames : 1) + ",\"room\":" + importRoom() +
               ",\"importBytes\":" + ram.bytes + ",\"headroom\":" + kHeadroom +
               ",\"free\":" + (mount() ? (int)(LittleFS.totalBytes() - LittleFS.usedBytes()) : 0) + ",\"total\":" + (mount() ? (int)LittleFS.totalBytes() : 0) + ",\"slots\":[";
  bool first = true;
  for (int id = 0; id < kDrawingSlots; ++id) {
    Header h;
    if (!slotInfo(id, h)) continue;
    char name[17];
    memcpy(name, h.name, 16); name[16] = 0;
    out += String(first ? "" : ",") + "{\"id\":" + id + ",\"name\":\"" + name + "\",\"frames\":" + h.frames + ",\"delay\":" + h.delay + ",\"bytes\":" + (unsigned)slotBytes(id) + "}";
    first = false;
  }
  if (ram.frames) out += String(first ? "" : ",") + "{\"id\":" + kRamSlot + ",\"name\":\"Import\",\"frames\":" + ram.frames + ",\"delay\":" + ram.delay + "}";
  json(200, out + "]}");
}

void open() {
  inputActivity();
  const bool fresh = !buf;
  State &s = state();
  s.on = true;
  s.fx = kPaintEffect;
  lookChanged();
  if (!buffer(true)) { message(503, "The clock is still busy with its data feeds or short of memory: try again in a moment."); return; }
  if (fresh) { memset(buf, 0, kBufferBytes); indexedCanvas = false; shown = -1; anim.frames = 0; }
  info();
}

// The whole buffer as base64 of little-endian RGB565, in chunks
void pixels() {
  Serial.printf("[Canvas] readback start: free=%u block=%u\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  uint16_t *b = buffer(false);
  srv->setContentLength(CONTENT_LENGTH_UNKNOWN);
  srv->sendHeader("Cache-Control", "no-store");
  srv->send(200, "text/plain", "");
  char text[1100]; uint8_t raw[768];
  for (size_t at = 0; at < kBufferBytes; at += sizeof(raw)) {
    const size_t count = kBufferBytes - at < sizeof(raw) ? kBufferBytes - at : sizeof(raw);
    for (size_t i = 0; i < count; i += 2) {
      const size_t pixel = (at + i) / 2;
      uint16_t color = b ? b[pixel] : 0;
      if (b && indexedCanvas) {
        const uint8_t *c = rgbPalette + reinterpret_cast<uint8_t *>(b)[pixel] * 3;
        color = (c[0] & 0xF8) << 8 | (c[1] & 0xFC) << 3 | c[2] >> 3;
      }
      raw[i] = color & 255; raw[i + 1] = color >> 8;
    }
    size_t written = 0;
    mbedtls_base64_encode((unsigned char *)text, sizeof(text), &written, raw, count);
    srv->sendContent(text, written);
  }
  srv->sendContent("");
  Serial.println("[Canvas] readback complete");
}

// Deltas: "index:rrggbb;index:rrggbb;..."
void deltas() {
  inputActivity();
  if (lowMemory()) return;
  if (playing()) { noteWriter("blocked pixels"); message(409, "An animation is playing: press Clear before drawing."); return; }
  noteWriter("pixels");
  if (!buffer(true)) { message(503, "Not enough memory for the canvas."); return; }
  const String &body = srv->arg("plain");  // a reference: a copy would double the heap this request needs
  const char *p = body.c_str();
  int count = 0;
  while (*p) {
    char *end;
    const long index = strtol(p, &end, 10);
    if (*end != ':') break;
    const unsigned long rgb = strtoul(end + 1, &end, 16);
    if (index >= 0 && index < kPixels) { setPixel(index, rgb >> 16, rgb >> 8, rgb); ++count; }
    p = *end == ';' ? end + 1 : end;
    if (end == p && *p) break;
  }
  json(200, String("{\"pixels\":") + count + "}");
}

// Whole rows: ?y=first row, body = base64 of the rows' RGB565 (up to 16 rows)
void rows() {
  inputActivity();
  if (lowMemory()) return;
  if (playing()) { noteWriter("blocked rows"); message(409, "An animation is playing: press Clear before drawing."); return; }
  noteWriter("rows");
  uint16_t *b = buffer(true);
  if (!b) { message(503, "Not enough memory for the canvas."); return; }
  const int y = srv->arg("y").toInt();
  const String &body = srv->arg("plain");
  uint8_t raw[16 * kWidth * 2 + 4];
  size_t written = 0;
  if (y < 0 || y >= kHeight || mbedtls_base64_decode(raw, sizeof(raw), &written, (const uint8_t *)body.c_str(), body.length()) != 0 || written == 0 || written % (kWidth * 2) || y + (int)(written / (kWidth * 2)) > kHeight) {
    message(400, "Send 1 to 16 whole rows.");
    return;
  }
  materializeCanvas();
  memcpy(b + y * kWidth, raw, written);
  shown = -1; anim.frames = 0;
  json(200, "{}");
}

void clear() {
  inputActivity();
  noteWriter("!clear");
  uint16_t *b = buffer(true);
  if (!b) { message(503, "Not enough memory for the canvas."); return; }
  indexedCanvas = false;
  memset(b, 0, kBufferBytes);
  shown = -1; anim.frames = 0;
  json(200, "{}");
}

void saveRam();

// Stores the buffer: ?slot=N&name=..&delay=ms starts the drawing, &append=1 adds it as the next frame of the animation
void save() {
  if (indexedCanvas && playing()) { message(409, "Clear the imported animation before saving editor frames."); return; }
  materializeCanvas();
  if (srv->hasArg("ram")) { saveRam(); return; }
  const int id = srv->arg("slot").toInt();
  if (!buf || id < 0 || id >= kDrawingSlots) { message(400, "Choose a slot from 1 to 8."); return; }
  if (!mount()) { message(503, "The flash file system is not available."); return; }
  const bool append = srv->hasArg("append");
  char file[16];
  path(id, file);
  Header h{kMagic, 1, 0, (uint16_t)constrain(srv->arg("delay").toInt() ? srv->arg("delay").toInt() : 100, 20, 60000), {}};
  File f;
  const uint16_t *prev = nullptr;
  size_t oldSize = 0;  // append: where the new frame starts, so a failed write leaves the earlier frames intact
  if (append) {
    f = LittleFS.open(file, "r+");
    Header old;
    if (!f || !readHeader(f, old) || old.frames >= 24 || (old.frames + 1) * old.delay > 5000) { if (f) f.close(); message(400, "That drawing has no room for more frames."); return; }
    if (!(last && lastSlot == id && lastFrames == old.frames) && heap_caps_get_free_size(MALLOC_CAP_8BIT) >= kBufferBytes + kHeadroom) {
      if (!last) last = (uint16_t *)malloc(kBufferBytes);
      lastSlot = -1;
      if (last && f.seek(sizeof(Header))) {  // rebuild the last frame by replaying the stored ones
        bool whole = true;
        for (int i = 0; i < old.frames && whole; ++i) whole = readFrame(f, last);
        if (whole) { lastSlot = id; lastFrames = old.frames; }
      }
    }
    if (last && lastSlot == id && lastFrames == old.frames) prev = last;
    h = old; ++h.frames;
    oldSize = f.size();
    f.seek(oldSize);
  }
  int mode;
  const size_t need = frameSize(buf, prev, mode) + 4096;
  if (LittleFS.totalBytes() - LittleFS.usedBytes() + (append || !LittleFS.exists(file) ? 0 : 12288) < need) { if (f) f.close(); message(507, "Not enough storage left: remove a drawing first."); return; }
  if (!append) {
    char name[16];
    sanitize(srv->arg("name"), name);
    memcpy(h.name, name[0] ? name : "Drawing", 16);
    f = LittleFS.open(file, "w");
    if (f) f.write((const uint8_t *)&h, sizeof(h));
  }
  bool ok = f && writeFrame(f, buf, prev);
  bool headerTorn = false;
  if (ok && append) { f.seek(0); ok = f.write((const uint8_t *)&h, sizeof(h)) == sizeof(h); headerTorn = !ok; }
  if (f) f.close();
  // A failed new drawing is removed. A failed append is cut back to the frames it had; only when that is not possible (or the header itself was torn) is the drawing removed
  if (!ok) {
    const String full = String(LittleFS.mountpoint()) + file;
    if (!append || headerTorn || truncate(full.c_str(), oldSize) != 0) LittleFS.remove(file);
  }
  if (!ok) { message(500, "Could not store the drawing."); return; }
  if (!last && heap_caps_get_free_size(MALLOC_CAP_8BIT) >= kBufferBytes + kHeadroom) last = (uint16_t *)malloc(kBufferBytes);
  if (last) { memcpy(last, buf, kBufferBytes); lastSlot = id; lastFrames = h.frames; lastAt = millis(); }
  shown = id; options().paintSlot = id; touched();
  openSlot(id, buf);  // note the frame offsets for playback; the buffer already holds the first frame's look
  info();
}

bool loadRam();

// Explicitly keep the played import. Five seconds maximum; preflight before touching a saved slot.
void keep() {
  const int id = srv->arg("slot").toInt();
  if (id < 0 || id >= kDrawingSlots || !ram.frames || importingNow || shown != kRamSlot || !buf) { message(400, "Play an imported animation and choose a free slot first."); return; }
  Header existing;
  if (slotInfo(id, existing)) { message(409, "That slot is occupied. Choose an empty slot."); return; }
  if (!mount()) { message(503, "Storage is unavailable."); return; }
  LittleFS.remove("/canvas-save.tmp");  // recover an interrupted save; occupied slots are never touched
  if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < kBufferBytes + kCanvasHeadroom) { message(507, "Not enough memory to save this animation."); return; }
  std::unique_ptr<uint16_t[]> pixels(new (std::nothrow) uint16_t[kPixels]);
  if (!pixels) { message(507, "Not enough memory to save this animation."); return; }
  const int frames = std::min(24, std::min(ram.frames, std::max(1, 5000 / (int)ram.delay)));
  Header h{kMagic, (uint8_t)frames, 0, (uint16_t)std::min(5000, (int)ram.delay), {}};
  sanitize(srv->arg("name"), h.name);
  if (!h.name[0]) strlcpy(h.name, "Saved GIF", sizeof(h.name));
  if (!strncmp(h.name, "Import", 6)) { message(400, "Choose a name that does not start with Import."); return; }
  std::unique_ptr<uint16_t[]> previous;
  if (heap_caps_get_free_size(MALLOC_CAP_8BIT) >= kBufferBytes + kCanvasHeadroom) previous.reset(new (std::nothrow) uint16_t[kPixels]);
  auto decode = [&](int i) {
    if (!readMem(ram.frame[i], buf)) return false;
    if (indexedCanvas && rgbPalette) {
      const uint8_t *indexes = reinterpret_cast<uint8_t *>(buf);
      for (int n = 0; n < kPixels; n++) {
        const uint8_t *c = rgbPalette + indexes[n] * 3;
        pixels[n] = pack(c[0], c[1], c[2]);
      }
    } else memcpy(pixels.get(), buf, kBufferBytes);
    return true;
  };
  size_t need = sizeof(h);
  bool ok = true;
  for (int i = 0; i < frames && ok; i++) {
    ok = decode(i);
    int mode; if (ok) need += 2 + frameSize(pixels.get(), i && previous ? previous.get() : nullptr, mode);
    if (ok && previous) memcpy(previous.get(), pixels.get(), kBufferBytes);
    delay(1);
  }
  const size_t free = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (srv->hasArg("estimate")) {
    loadRam();
    if (!ok) { message(400, "Could not decode the played animation."); return; }
    json(200, String("{\"bytes\":") + need + ",\"free\":" + free + ",\"frames\":" + frames + ",\"fits\":" + (free >= need + 8192 ? "true" : "false") + "}");
    return;
  }
  if (!ok || free < need + 8192) {
    loadRam(); message(507, "Animation does not fit. Trim it or remove a saved item."); return;
  }
  File f = LittleFS.open("/canvas-save.tmp", "w");
  ok = f && f.write((const uint8_t *)&h, sizeof(h)) == sizeof(h);
  for (int i = 0; i < frames && ok; i++) {
    ok = decode(i) && writeFrame(f, pixels.get(), i && previous ? previous.get() : nullptr);
    if (ok && previous) memcpy(previous.get(), pixels.get(), kBufferBytes);
    delay(1);
  }
  if (f) f.close();
  char dest[16]; path(id, dest);
  if (ok) ok = LittleFS.rename("/canvas-save.tmp", dest);
  if (!ok) LittleFS.remove("/canvas-save.tmp");
  loadRam();
  if (!ok) { message(500, "Could not save the animation."); return; }
  consoleLog("[Canvas] Saved \"%s\" to slot %d (%d frames, about %u bytes)\n", h.name, id + 1, frames, (unsigned)need);
  info();
}

// ?ram=1: the buffer becomes the next frame of the imported animation in memory (&append=1), or its first; playback starts with slot 8
void saveRam() {
  if (ram.frames && (ram.frame[0][1] >> 6) == 3 && srv->hasArg("append")) {
    message(409, "Clear the imported animation before adding editor frames."); return;
  }
  materializeCanvas();
  if (lowMemory()) return;
  if (srv->hasArg("total")) importTotal = srv->arg("total").toInt();
  noteWriter(srv->hasArg("append") ? "save frame" : "!save first frame");
  if (!buf) { message(400, "Open the canvas first."); return; }
  const bool append = srv->hasArg("append");
  if (!append) freeRam();
  else if (!ram.frames || ram.frames >= kMaxFrames) { message(400, "That drawing has no room for more frames."); return; }
  const uint16_t delay = (uint16_t)constrain(srv->arg("delay").toInt() ? srv->arg("delay").toInt() : 100, 20, 60000);
  const uint16_t *prev = nullptr;
  if (append) {
    if (!(last && lastSlot == kRamSlot && lastFrames == ram.frames) && heap_caps_get_free_size(MALLOC_CAP_8BIT) >= kBufferBytes + kHeadroom) {
      if (!last) last = (uint16_t *)malloc(kBufferBytes);
      lastSlot = -1;
      if (last) {  // rebuild the last frame by replaying the stored ones
        bool whole = true;
        for (int i = 0; i < ram.frames && whole; ++i) whole = readMem(ram.frame[i], last);
        if (whole) { lastSlot = kRamSlot; lastFrames = ram.frames; }
      }
    }
    if (last && lastSlot == kRamSlot && lastFrames == ram.frames) prev = last;
  }
  int mode;
  const size_t bytes = frameSize(buf, prev, mode);
  if (!frameFits(bytes + 2)) { message(507, "Not enough memory for more frames."); return; }
  uint8_t *blob = (uint8_t *)malloc(bytes + 2);
  if (!blob) { message(507, "Not enough memory for more frames."); return; }
  const uint16_t head = (uint16_t)(bytes | mode << 14);
  blob[0] = head & 255; blob[1] = head >> 8;
  Sink out(blob + 2);
  encode(mode, buf, prev, out);
  ram.frame[ram.frames] = blob; ram.size[ram.frames] = bytes + 2; ++ram.frames; ram.bytes += bytes + 2; ram.delay = delay;
  if (!last && heap_caps_get_free_size(MALLOC_CAP_8BIT) >= kBufferBytes + kHeadroom) last = (uint16_t *)malloc(kBufferBytes);
  if (last) { memcpy(last, buf, kBufferBytes); lastSlot = kRamSlot; lastFrames = ram.frames; lastAt = millis(); }
  json(200, String("{\"frames\":") + ram.frames + ",\"bytes\":" + ram.bytes + "}");
}

// Browser-compressed frames arrive in small chunks to bound WebServer body allocations.
void frameChunk() {
  inputActivity();
  if (lowMemory()) return;
  if (playing() || !importingNow || !buf) { message(409, "Start an import first."); return; }
  const int offset = srv->arg("offset").toInt(), size = srv->arg("size").toInt();
  if (size < 3 || size > (int)kBufferBytes + 2 || offset < 0) { message(400, "Invalid frame size."); return; }
  const String &body = srv->arg("plain");
  uint8_t chunk[1536]; size_t count = 0;
  if (body.length() > 2048 || mbedtls_base64_decode(chunk, sizeof(chunk), &count, (const uint8_t *)body.c_str(), body.length()) != 0 || !count || offset + count > (size_t)size) {
    message(400, "Invalid frame chunk."); return;
  }
  if (!offset) {
    freeUpload(); freeLast();
    if (srv->hasArg("first")) freeRam();
    if (ram.frames >= kMaxFrames || !frameFits(size)) {
      message(507, "Not enough memory for more frames."); return;
    }
    upload = (uint8_t *)malloc(size);
    if (!upload) { message(507, "Not enough memory for more frames."); return; }
    uploadSize = size;
  }
  if (!upload || uploadSize != (size_t)size || uploadAt != (size_t)offset) { message(409, "Restart the frame upload."); return; }
  memcpy(upload + uploadAt, chunk, count); uploadAt += count; importingAt = millis();
  if (uploadAt == uploadSize) {
    const uint16_t head = upload[0] | upload[1] << 8;
    const int mode = head >> 14;
    const bool mixed = ram.frames && ((mode == 3) != ((ram.frame[0][1] >> 6) == 3));
    if ((head & kLengthMask) != uploadSize - 2 || mixed || (!ram.frames && mode == 1) ||
        (mode == 3 && ((!ram.frames) != !!(upload[2] & 128)))) {
      freeUpload(); message(400, "Invalid frame encoding."); return;
    }
    Reader in(upload + 2, uploadSize - 2);
    const bool valid = mode == 3 ? readIndexed(upload, uploadSize, false) : decodeBody(in, mode, uploadSize - 2, buf) && !in.left;
    if (!valid) {
      freeUpload(); message(400, "Invalid frame pixels."); return;
    }
    if (mode == 3) readIndexed(upload, uploadSize, true);
    else indexedCanvas = false;
    ram.frame[ram.frames] = upload; ram.size[ram.frames] = uploadSize;
    ++ram.frames; ram.bytes += uploadSize;
    upload = nullptr; uploadSize = uploadAt = 0;
    ram.delay = (uint16_t)constrain(srv->arg("delay").toInt(), 20, 60000);
    importTotal = srv->arg("total").toInt();
  }
  json(200, "{}");
}

bool loadRam() {
  freeUpload();
  uint16_t *p = buffer(true);
  if (!p || !ram.frames || !readMem(ram.frame[0], p)) return false;
  anim.slot = kRamSlot; anim.frames = ram.frames; anim.index = 0; anim.delay = ram.delay < 20 ? 20 : ram.delay;
  anim.nextAt = millis() + anim.delay;
  shown = kRamSlot;
  resetRate();
  importingNow = false;
  Serial.printf("[Canvas] playback ready: %d frames, delay=%u ms\n", ram.frames, (unsigned)anim.delay);
  touched();
  return true;
}

void slot() {
  inputActivity();
  const int id = srv->arg("slot").toInt();
  noteWriter(srv->arg("op") == "delete" ? "!delete slot" : "!load slot");
  if (id == kRamSlot) {
    if (srv->arg("op") == "delete") { freeRam(); freeLast(); importingNow = srv->hasArg("import"); importingAt = millis(); importTotal = 0; info(); return; }  // the last-frame copy goes too: the memory is wanted back
    state().fx = kPaintEffect; state().on = true; lookChanged();
    if (!loadRam()) { message(404, "That slot is empty."); return; }
    info();
    return;
  }
  if (id < 0 || id >= kDrawingSlots) { message(400, "Choose a slot from 1 to 8."); return; }
  if (srv->arg("op") == "delete") {
    if (mount()) { char file[16]; path(id, file); LittleFS.remove(file); consoleLog("[Canvas] Slot %d deleted\n", id + 1); }
    if (shown == id) { shown = -1; anim.frames = 0; }
    if (options().paintSlot == id) { options().paintSlot = 255; touched(); }
    info();
    return;
  }
  state().fx = kPaintEffect; state().on = true; lookChanged();
  if (!loadSlot(id)) { message(404, "That slot is empty."); return; }
  info();
}

// A small TLS client for the GIF search. The Arduino one cannot ask the server for small records (max_fragment_length), which saves most of the
// 16 KB receive buffer; a server that ignores the request just sends full records, so nothing breaks.
struct Tls {
  int sock = -1;
  bool up = false;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  Tls() { mbedtls_ssl_init(&ssl); mbedtls_ssl_config_init(&conf); }
  ~Tls() { if (up) mbedtls_ssl_close_notify(&ssl); if (sock >= 0) ::close(sock); mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&conf); }
  static int sendBytes(void *ctx, const unsigned char *b, size_t n) {
    const int r = ::send(*(int *)ctx, b, n, 0);
    return r >= 0 ? r : (errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_INTERNAL_ERROR);
  }
  static int recvBytes(void *ctx, unsigned char *b, size_t n) {
    const int r = ::recv(*(int *)ctx, b, n, 0);
    return r >= 0 ? r : (errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_SSL_INTERNAL_ERROR);
  }
  static int random(void *, unsigned char *b, size_t n) { esp_fill_random(b, n); return 0; }
  // frag: MBEDTLS_SSL_MAX_FRAG_LEN_*: the record size asked of the server
  bool open(const char *host, uint16_t port, int frag, uint32_t timeoutMs) {
    addrinfo hints = {}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    char portText[8];
    snprintf(portText, sizeof(portText), "%u", port);
    if (getaddrinfo(host, portText, &hints, &res) != 0 || !res) return false;
    sock = ::socket(res->ai_family, res->ai_socktype, 0);
    if (sock < 0) { freeaddrinfo(res); return false; }
    timeval tv = {(time_t)(timeoutMs / 1000), (suseconds_t)(timeoutMs % 1000 * 1000)};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    const bool connected = ::connect(sock, res->ai_addr, res->ai_addrlen) == 0;
    freeaddrinfo(res);
    if (!connected) return false;
    if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) return false;
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);  // the key travels in the request: the certificate must check out
    if (esp_crt_bundle_attach(&conf)) return false;
    mbedtls_ssl_conf_rng(&conf, random, nullptr);
    if (frag) mbedtls_ssl_conf_max_frag_len(&conf, (unsigned char)frag);
    if (mbedtls_ssl_setup(&ssl, &conf) || mbedtls_ssl_set_hostname(&ssl, host)) return false;
    mbedtls_ssl_set_bio(&ssl, &sock, sendBytes, recvBytes, nullptr);
    const uint32_t end = millis() + timeoutMs;
    int r;
    while ((r = mbedtls_ssl_handshake(&ssl)) != 0)
      if ((r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) || millis() > end) return false;
    return up = true;
  }
  bool write(const char *text) {
    size_t left = strlen(text);
    const uint32_t end = millis() + 8000;
    while (left) {
      const int r = mbedtls_ssl_write(&ssl, (const unsigned char *)text, left);
      if (r > 0) { text += r; left -= r; } else if ((r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) || millis() > end) return false;
    }
    return true;
  }
  int read(unsigned char *b, size_t n) {  // bytes, 0 = the server closed, -1 = error or nothing for 8 s
    const uint32_t end = millis() + 8000;
    for (;;) {
      const int r = mbedtls_ssl_read(&ssl, b, n);
      if (r >= 0) return r;
      if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
      if ((r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) || millis() > end) return -1;
    }
  }
};

// GIF search: the clock asks KLIPY with the key kept in firmware and hands the browser only the GIF addresses, /api/canvas/search?q=...&page=...
// The answer is read as a stream (one page runs to about 90 KB): every string that is a .gif address is kept under the size name seen last.
void search() {
#ifndef UM_KLIPY_KEY
  message(501, "This firmware has no KLIPY key.");
#else
  const String q = srv->arg("q");
  if (q.isEmpty() || q.length() > 60) { message(400, "Type something to search for."); return; }
  struct FeedPark { ~FeedPark() { notice::holdFeeds(notice::HOLD_GIF_SEARCH, false); } } feedPark;  // the feeds resume when the search is over
  for (int i = 0; i < 40 && heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 32000; i++) {
    notice::holdFeeds(notice::HOLD_GIF_SEARCH, true);  // park the feeds and wait (up to 2 s) for their memory
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 32000) {
    // a loaded GIF holds the memory: the page repeats the search with &free=1, which removes it from the clock (the same call as deleting an import)
    if (ram.frames && !srv->hasArg("free")) { json(503, "{\"message\":\"A loaded GIF is using the memory the search needs.\",\"needsFree\":true}"); return; }
    if (ram.frames) { freeRam(); freeLast(); }
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 32000) { message(503, "Not enough memory to search right now."); return; }
  }
  char key[sizeof(kKlipyKey) + 1];
  for (size_t i = 0; i < sizeof(kKlipyKey); ++i) key[i] = kKlipyKey[i] ^ "ultimatrix"[i % 10];
  key[sizeof(kKlipyKey)] = 0;
  String request = String("GET /api/v1/") + key + "/gifs/search?per_page=24&customer_id=clock&page=" + constrain(srv->arg("page").toInt(), 1, 50) + "&q=";
  memset(key, 0, sizeof(key));
  for (size_t i = 0; i < q.length(); ++i) {
    char c = q[i], hex[4];
    if (isalnum((unsigned char)c)) request += c; else { snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c); request += hex; }
  }
  request += " HTTP/1.0\r\nHost: api.klipy.com\r\nUser-Agent: ultimatrix\r\nConnection: close\r\n\r\n";
  static const char *const sizes[] = {"xs", "sm", "md", "hd"};
  constexpr int kMax = 24;
  String found[4];
  int counts[4] = {0, 0, 0, 0}, size = 3, status = 0;
  unsigned bytes = 0;
  {
    std::unique_ptr<Tls> tls(new (std::nothrow) Tls);
    if (!tls) { message(503, "Not enough memory to search right now."); return; }
    if (!tls->open("api.klipy.com", 443, MBEDTLS_SSL_MAX_FRAG_LEN_4096, 8000)) { message(502, "Could not reach KLIPY."); return; }
    const bool sent = tls->write(request.c_str());
    request = String();
    if (!sent) { message(502, "Could not reach KLIPY."); return; }
    std::unique_ptr<unsigned char[]> chunk(new (std::nothrow) unsigned char[512]);
    if (!chunk) { message(503, "Not enough memory to search right now."); return; }
    String token, field, slug;
    bool inString = false, escape = false, afterString = false, body = false;
    char head[16] = {0};
    int headLen = 0;
    uint32_t tail = 0;  // the last four header bytes
    for (int n; bytes < 600000 && !firmwareUpdating.load() && (n = tls->read(chunk.get(), 512)) > 0;) {
      bytes += n;
      vTaskDelay(1);
      for (int i = 0; i < n; ++i) {
        const char c = chunk[i];
        if (!body) {  // the status line, then the headers up to the empty line
          if (headLen < 12) head[headLen++] = c;
          tail = tail << 8 | (uint8_t)c;
          if (tail == 0x0D0A0D0A) { body = true; status = atoi(head + 9); if (status != 200) break; }
          continue;
        }
        if (inString) {
          if (escape) { escape = false; if (c != '/') token += '\\'; token += c; }  // \/ is a plain slash, other escapes stay as they are
          else if (c == '\\') escape = true;
          else if (c == '"') { inString = false; afterString = true; }
          else if (token.length() < 300) token += c;
          continue;
        }
        if (afterString) {
          if (c == ' ' || c == '\n' || c == '\r') continue;
          afterString = false;
          if (c == ':') {
            field = token;
            for (int k = 0; k < 4; ++k) if (token == sizes[k]) size = k;
          } else if (field == "slug") {
            slug = token;
            for (size_t k = 0; k < slug.length(); k++) if (!isalnum((unsigned char)slug[k]) && slug[k] != '-' && slug[k] != '_') { slug = ""; break; }
            field = "";
          } else if (token.startsWith("https://") && token.endsWith(".gif") && token.indexOf(' ') < 0 && counts[size] < kMax) {
            if (counts[size]) found[size] += ',';
            found[size] += "{\"url\":\""; found[size] += token; found[size] += "\",\"slug\":\""; found[size] += slug; found[size] += "\"}";
            counts[size]++;
          }
        }
        if (c == '"') { inString = true; token = ""; }
      }
      if (body && status != 200) break;
    }
  }
  if (status != 200) { message(status == 429 ? 429 : 502, status == 429 ? "KLIPY rate limit: try again later." : status ? "KLIPY refused the search." : "Could not reach KLIPY."); return; }
  int best = 3;  // the smallest size that has results: the clock panel is 64x64
  for (int i = 3; i >= 0; --i) if (counts[i]) best = i;
  json(200, String("{\"gifs\":[") + found[best] + "]}");
#endif
}

void cycle() {
  const int seconds = constrain(srv->arg("sec").toInt(), 0, 3600);
  options().cycleSec = seconds;
  cycleNext = millis() + seconds * 1000UL;
  touched();
  info();
}
}  // namespace

uint16_t *buffer(bool create) {
  if (buf || !create) return buf;
  if (!feedsQuiet()) return nullptr;  // a feed that is still downloading needs the heap more than the canvas does: Paint stays black until the feeds are gone
  if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < kBufferBytes + kCanvasHeadroom) return nullptr;  // the canvas is what WLED needs: the feeds are parked while it is on, so it can take more of the heap than the optional import
  buf = (uint16_t *)calloc(1, kBufferBytes);
  if (buf && !booted) {  // the first time after a restart: the drawing that was on show
    booted = true;
    if (options().paintSlot < kDrawingSlots && openSlot(options().paintSlot, buf)) shown = options().paintSlot;
    else memset(buf, 0, kBufferBytes);
  }
  return buf;
}

void shrink() {
  if (last) { freeLast(); return; }
  if (ram.frames) Serial.printf("[Wled] import freed: data heap is short (%u)\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
  freeRam();
}

void clearImports() {
  freeRam();
  if (!mount()) return;
  for (int id = 0; id < kDrawingSlots; ++id) {
    Header h;
    if (!slotInfo(id, h) || strncmp(h.name, "Import", 16) != 0) continue;
    char file[16];
    path(id, file);
    LittleFS.remove(file);
    consoleLog("[Canvas] Removed temporary import in slot %d\n", id + 1);
    if (shown == id) { shown = -1; anim.frames = 0; }
    if (options().paintSlot == id) { options().paintSlot = 255; touched(); }
  }
  freeLast();
}

void release() {
  freeRam();
  freeLast();
  free(buf);
  buf = nullptr;
  anim.frames = 0;
}

void setPixel(int index, uint8_t r, uint8_t g, uint8_t b) {
  uint16_t *p = buffer(true);
  if (!p || index < 0 || index >= kPixels) return;
  materializeCanvas();
  p[index] = pack(r, g, b);
  shown = -1;
  anim.frames = 0;
  if (options().cycleSec) cycleNext = millis() + (options().cycleSec < 30 ? 30 : options().cycleSec) * 1000UL;  // hands off while someone draws
}

bool indexedFrame(const uint8_t *&indexes, const uint8_t *&palette) {
  if (!indexedCanvas || !rgbPalette || !buf) return false;
  indexes = reinterpret_cast<const uint8_t *>(buf); palette = rgbPalette;
  return true;
}

bool slotName(int id, char *out, size_t size) {
  Header h;
  if (id < 0 || id >= kDrawingSlots || !slotInfo(id, h)) return false;
  char name[17]; memcpy(name, h.name, 16); name[16] = 0;
  strlcpy(out, name, size);
  return true;
}

int currentSlot() { return shown; }

bool loadSlot(int id) {
  uint16_t *p = buffer(true);
  const bool wasIndexed = indexedCanvas;
  materializeCanvas();
  if (!p || !openSlot(id, p)) {
    if (wasIndexed) loadRam();
    return false;
  }
  shown = id;
  anim.index = 0;
  anim.nextAt = millis() + anim.delay;
  options().paintSlot = id;
  touched();
  return true;
}

void maintain(uint32_t now) {
  trimLast(now);
  if (importingNow && now - importingAt > 120000) {
    freeRam(); freeLast(); importingNow = false; importTotal = 0;
  }
}

// While an import uploads over the Paint effect the panel keeps its last picture: the import reuses the canvas buffer, so it cannot be drawn
bool holdsPanel() { return importingNow && state().fx == kPaintEffect; }

bool importing(int &have, int &total) {
  have = ram.frames; total = importTotal;
  return importingNow;
}

void tick(uint32_t now) {
  if (options().cycleSec && (int32_t)(now - cycleNext) >= 0) {
    cycleNext = now + options().cycleSec * 1000UL;
    for (int step = 1; step <= kDrawingSlots; ++step) {
      const int id = (cycleLast + step) % kDrawingSlots;
      Header h;
      if (slotInfo(id, h)) { cycleLast = id; loadSlot(id); break; }
    }
  }
  if (anim.frames > 1 && shown >= 0 && (int32_t)(now - anim.nextAt) >= 0) {
    const uint32_t late = now - anim.nextAt;
    if (late > lateMax) lateMax = late;
    if (late >= 40) ++lateCount;
    anim.index = (anim.index + 1) % anim.frames;
    anim.nextAt += anim.delay;  // from the planned time, not from now: the average pace stays the delay however long a main-loop pass takes
    if ((int32_t)(now - anim.nextAt) >= 0) anim.nextAt = now + anim.delay;  // after a stall there is no burst of catch-up frames
    const uint32_t started = micros();
    showFrame(anim.index);
    const uint32_t decodeUs = micros() - started;
    if (decodeUs > decodeMaxUs) decodeMaxUs = decodeUs;
    ++rateFrames;
  }
  if (anim.frames > 1 && shown >= 0 && (int32_t)(now - rateAt) >= 10000) {  // serial: how fast it really plays
    Serial.printf("[Canvas] playing %u frames in %u ms (planned delay %u ms, %d frames, late>=40ms %u, max late %u ms, max decode %u us)\n",
        (unsigned)rateFrames, (unsigned)(now - rateAt), (unsigned)anim.delay, anim.frames,
        (unsigned)lateCount, (unsigned)lateMax, (unsigned)decodeMaxUs);
    rateAt = now; rateFrames = lateMax = lateCount = decodeMaxUs = 0;
  }
}

void attach(WebServer &server, void (*postRoute)(const char *, WebServer::THandlerFunction)) {
  srv = &server;
  clearImports();  // an import lasts until WLED lets go of the panel or the clock restarts
  server.on("/gifs", HTTP_GET, []() { sendGzip(*srv, "text/html; charset=utf-8", canvasHtmlGz, canvasHtmlGzEnd); });
  server.on("/canvas", HTTP_GET, []() { sendGzip(*srv, "text/html; charset=utf-8", canvasHtmlGz, canvasHtmlGzEnd); });
  server.on("/api/canvas", HTTP_GET, info);
  server.on("/api/canvas/pixels", HTTP_GET, pixels);
  postRoute("/api/canvas/open", open);
  postRoute("/api/canvas/pixels", deltas);
  postRoute("/api/canvas/rows", rows);
  postRoute("/api/canvas/clear", clear);
  postRoute("/api/canvas/save", save);
  postRoute("/api/canvas/keep", keep);
  postRoute("/api/canvas/frame", frameChunk);
  postRoute("/api/canvas/slot", slot);
  server.on("/api/canvas/search", HTTP_GET, search);
  postRoute("/api/canvas/cycle", cycle);
}

}  // namespace canvas
}  // namespace wled
