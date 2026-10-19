// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "NoticePng.h"
#include "NetworkBudget.h"
#include "Console.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <uzlib.h>
#include <algorithm>
#include <memory>
#include <new>

namespace notice {
namespace {
constexpr uint32_t IHDR = 0x49484452, PLTE = 0x504c5445, TRNS = 0x74524e53, IDAT = 0x49444154, IEND = 0x49454e44;
struct Png;
struct Inflater : uzlib_uncomp { Png *png; };
struct Png {
  size_t (*read)(void *, uint8_t *, size_t);
  void *context;
  uint8_t input[512], palette[768] = {}, alpha[256];
  size_t at = 0, available = 0;
  uint32_t type = 0, left = 0, crc = 0, width = 0, height = 0;
  unsigned channels = 0, colors = 0;
  uint8_t color = 0, depth = 8;
  uint16_t transparent[3] = {};
  bool keyed = false, valid = true;
  Inflater inflater{};
  int byte() {
    if (!valid) return -1;
    if (at == available) { at = 0; available = read(context, input, sizeof(input)); }
    if (!available) { valid = false; return -1; }
    return input[at++];
  }
  uint32_t word() {
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) { int b = byte(); if (b < 0) return 0; value = value << 8 | b; }
    return value;
  }
  bool header() {
    left = word(); type = word();
    if (!valid || left > 128 * 1024) return false;
    uint8_t bytes[4] = {uint8_t(type >> 24), uint8_t(type >> 16), uint8_t(type >> 8), uint8_t(type)};
    crc = uzlib_crc32(bytes, 4, 0xffffffff);
    return true;
  }
  int data() {
    if (!left) return -1;
    int value = byte(); if (value < 0) return -1;
    uint8_t b = value; crc = uzlib_crc32(&b, 1, crc); --left;
    return value;
  }
  bool close() {
    while (left && valid) data();
    const uint32_t expected = word();
    return valid && expected == ~crc;
  }
  int compressed() {
    while (!left) {
      if (!close() || !header() || type != IDAT) { valid = false; return -1; }
    }
    return data();
  }
};
int compressed(uzlib_uncomp *state) { return static_cast<Inflater *>(state)->png->compressed(); }
int paeth(int a, int b, int c) {
  int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}
bool reject(int line) { consoleLog("[Notice] PNG rejected (NoticePng.cpp line %d)\n", line); return false; }
bool decode(Png &p, uint16_t *pixels, bool fill) {
  static const uint8_t signature[] = {137,80,78,71,13,10,26,10};
  for (uint8_t b : signature) if (p.byte() != b) return reject(__LINE__);
  if (!p.header() || p.type != IHDR || p.left != 13) return reject(__LINE__);
  uint8_t h[13]; for (uint8_t &b : h) { int v = p.data(); if (v < 0) return reject(__LINE__); b = v; }
  p.width = uint32_t(h[0]) << 24 | uint32_t(h[1]) << 16 | uint32_t(h[2]) << 8 | h[3];
  p.height = uint32_t(h[4]) << 24 | uint32_t(h[5]) << 16 | uint32_t(h[6]) << 8 | h[7];
  p.color = h[9]; p.depth = h[8];
  consoleLog("[Notice] PNG %ux%u, depth %u, colour type %u, compression %u, filter %u, interlace %u\n", (unsigned)p.width, (unsigned)p.height, h[8], h[9], h[10], h[11], h[12]);
  // Non-interlaced PNGs; packed grayscale/palette and 8-bit colour/alpha.
  if (!p.width || !p.height || p.width > 2048 || p.height > 2048 || (h[8] != 1 && h[8] != 2 && h[8] != 4 && h[8] != 8) || h[10] || h[11] || h[12]) return reject(__LINE__);
  switch (p.color) { case 0: case 3: p.channels = 1; break; case 2: p.channels = 3; break; case 4: p.channels = 2; break; case 6: p.channels = 4; break; default: return reject(__LINE__); }
  if (p.depth != 8 && p.color != 0 && p.color != 3) return reject(__LINE__);
  if (!p.close()) return reject(__LINE__);
  memset(p.alpha, 255, sizeof(p.alpha));
  while (p.header() && p.type != IDAT) {
    if (p.type == PLTE) {
      if (p.colors || !p.left || p.left > 768 || p.left % 3) return reject(__LINE__);
      p.colors = p.left / 3;
      for (unsigned i = 0; i < p.colors * 3; i++) { int v = p.data(); if (v < 0) return reject(__LINE__); p.palette[i] = v; }
    } else if (p.type == TRNS) {
      if (p.keyed) return reject(__LINE__);
      p.keyed = true;
      if (p.color == 3) {
        if (!p.colors || !p.left || p.left > p.colors) return reject(__LINE__);
        unsigned i = 0; while (p.left) { int v = p.data(); if (v < 0) return reject(__LINE__); p.alpha[i++] = v; }
      } else if ((p.color == 0 && p.left == 2) || (p.color == 2 && p.left == 6)) {
        for (unsigned i = 0; i < p.channels; i++) { int hi = p.data(), lo = p.data(); if (hi < 0 || lo < 0) return reject(__LINE__); p.transparent[i] = (hi << 8) | lo; }
      } else return reject(__LINE__);
    } else if (!(p.type & 0x20000000) || p.type == 0x6163544c) return reject(__LINE__); // unknown critical chunk or APNG
    if (!p.close()) return reject(__LINE__);
  }
  if (!p.valid || p.type != IDAT || (p.color == 3 && !p.colors)) return reject(__LINE__);
  const size_t stride = (p.width * p.channels * p.depth + 7) / 8 + 1;
  const unsigned filterBytes = (p.channels * p.depth + 7) / 8;
  const size_t bytes = stride * p.height;
  if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < bytes + 1 + 12288 || heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < bytes + 1) {
    consoleLog("[Notice] PNG needs %u bytes of decode memory; resize the source or use JPEG\n", (unsigned)(bytes + 1 + sizeof(Png)));
    return reject(__LINE__);
  }
  std::unique_ptr<uint8_t[]> raw(new (std::nothrow) uint8_t[bytes + 1]);
  if (!raw) return reject(__LINE__);
  // The complete filtered output doubles as the LZ history, sized to this image, not a fixed 32 KiB window.
  auto &z = p.inflater;
  uzlib_uncompress_init(&z, nullptr, 0);
  z.png = &p; z.source_read_cb = compressed;
  const int cmf = p.compressed(), flags = p.compressed();
  if (cmf < 0 || flags < 0 || (cmf & 15) != 8 || (cmf >> 4) > 7 || (flags & 32) || ((cmf << 8 | flags) % 31)) return reject(__LINE__);
  z.dest_start = z.dest = raw.get(); z.checksum_type = TINF_CHKSUM_ADLER; z.checksum = 1;
  int result = TINF_OK;
  while (result == TINF_OK && z.dest < raw.get() + bytes + 1 && p.valid && !firmwareUpdating) {
    z.dest_limit = std::min(z.dest + std::min<size_t>(1024, raw.get() + bytes + 1 - z.dest), raw.get() + bytes + 1);
    result = uzlib_uncompress_chksum(&z);
    delay(1);
  }
  if (result != TINF_DONE || z.eof || !p.valid || z.dest != raw.get() + bytes || p.left || !p.close()) return reject(__LINE__);
  // Validate remaining chunks and IEND. More compressed bytes or animation chunks are rejected.
  for (;;) {
    if (!p.header()) return reject(__LINE__);
    if (p.type == IEND) { if (p.left || !p.close()) return reject(__LINE__); break; }
    if (p.type == IDAT && p.left == 0) { if (!p.close()) return reject(__LINE__); continue; }
    if (!(p.type & 0x20000000) || p.type == 0x6163544c || !p.close()) return reject(__LINE__);
  }
  // Undo filters only after inflation, so the history stays intact while decompressing.
  for (unsigned y = 0; y < p.height; y++) {
    uint8_t *row = raw.get() + y * stride + 1;
    const uint8_t filter = row[-1];
    if (filter > 4) return reject(__LINE__);
    for (size_t x = 0; x < stride - 1; x++) {
      int a = x >= filterBytes ? row[x - filterBytes] : 0;
      int b = y ? (row - stride)[x] : 0;
      int c = y && x >= filterBytes ? (row - stride)[x - filterBytes] : 0;
      row[x] += filter == 1 ? a : filter == 2 ? b : filter == 3 ? (a + b) / 2 : filter == 4 ? paeth(a,b,c) : 0;
    }
    if (firmwareUpdating) return reject(__LINE__);
    delay(1);
  }
  const unsigned sw = fill ? std::min(p.width, p.height) : p.width;
  const unsigned sh = fill ? sw : p.height;
  const unsigned ow = fill || p.width >= p.height ? 64 : std::max<unsigned>(1u, 64 * p.width / p.height);
  const unsigned oh = fill || p.height >= p.width ? 64 : std::max<unsigned>(1u, 64 * p.height / p.width);
  for (unsigned y = 0; y < oh; y++) {
    const unsigned sy = (p.height - sh) / 2 + (2 * y + 1) * sh / (2 * oh);
    for (unsigned x = 0; x < ow; x++) {
      const unsigned sx = (p.width - sw) / 2 + (2 * x + 1) * sw / (2 * ow);
      const uint8_t *line = raw.get() + sy * stride + 1;
      const uint8_t *v = line + (p.depth == 8 ? sx * p.channels : sx * p.depth / 8);
      const unsigned sample = p.depth == 8 ? line[sx] : (line[sx * p.depth / 8] >> (8 - p.depth - sx * p.depth % 8)) & ((1u << p.depth) - 1);
      // Packed formats use sample; multi-channel formats use v.

      unsigned r, g, b, alpha = 255;
      if (p.color == 3) {
        if (sample >= p.colors) return reject(__LINE__);
        const uint8_t *c = p.palette + sample * 3; r = c[0]; g = c[1]; b = c[2]; alpha = p.alpha[sample];
      } else if (p.color == 0 || p.color == 4) {
        r = g = b = p.color == 4 ? v[0] : sample * 255 / ((1u << p.depth) - 1);
        if (p.color == 4) alpha = v[1]; else if (p.keyed && sample == p.transparent[0]) alpha = 0;
      } else {
        r = v[0]; g = v[1]; b = v[2];
        if (p.color == 6) alpha = v[3]; else if (p.keyed && r == p.transparent[0] && g == p.transparent[1] && b == p.transparent[2]) alpha = 0;
      }
      r = (r * alpha + 127) / 255; g = (g * alpha + 127) / 255; b = (b * alpha + 127) / 255;
      pixels[(y + (64 - oh) / 2) * 64 + x + (64 - ow) / 2] = (r & 0xf8) << 8 | (g & 0xfc) << 3 | b >> 3;
    }
  }
  return true;
}
}
bool decodePng(size_t (*read)(void *, uint8_t *, size_t), void *context, uint16_t *pixels, bool fill) {
  std::unique_ptr<Png> png(new (std::nothrow) Png{});
  if (!png) return false;
  png->read = read; png->context = context;
  return decode(*png, pixels, fill);
}
}
