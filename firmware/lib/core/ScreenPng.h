// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace screenpng {
// One LED is a kScale x kScale cell: a black line on its left and top, the rest filled. 3 gives a 192x192 picture.
constexpr int kScale = 3, kWidth = 64 * kScale, kRowBytes = kWidth + 1;
static_assert(kRowBytes <= 258 || kRowBytes - 258 >= 3, "a repeated row is copied in matches of 3 to 258 bytes");

inline uint8_t index(const uint8_t *rgb) {
  return (rgb[0] & 0xE0) | ((rgb[1] >> 3) & 0x1C) | (rgb[2] >> 6);
}

// A bounded sink also supports a count-only pass (data == nullptr).
struct Sink {
  uint8_t *data;
  size_t capacity, size = 0;
  uint32_t bits = 0;
  unsigned used = 0;
  Sink(uint8_t *output, size_t limit) : data(output), capacity(limit) {}
  void byte(uint8_t v) { if (data && size < capacity) data[size] = v; ++size; }
  void be32(uint32_t v) { byte(v >> 24); byte(v >> 16); byte(v >> 8); byte(v); }
  void put(unsigned value, unsigned count) {
    bits |= value << used; used += count;
    while (used >= 8) { byte(bits); bits >>= 8; used -= 8; }
  }
  void flush() { if (used) byte(bits); bits = 0; used = 0; }
  void huffman(unsigned symbol) {
    unsigned code, count;
    if (symbol < 144) { code = symbol + 0x30; count = 8; }
    else if (symbol < 256) { code = symbol - 144 + 0x190; count = 9; }
    else if (symbol < 280) { code = symbol - 256; count = 7; }
    else { code = symbol - 280 + 0xC0; count = 8; }
    unsigned reversed = 0;
    for (unsigned i = 0; i < count; ++i) { reversed = (reversed << 1) | (code & 1); code >>= 1; }
    put(reversed, count);
  }
  void match(unsigned length, unsigned distance) {
    static constexpr uint16_t base[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static constexpr uint8_t extra[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    unsigned code = 28;
    while (length < base[code]) --code;
    huffman(257 + code); put(length - base[code], extra[code]);
    // Only distances 1 (run), kScale (repeated LED cell) and kRowBytes (row) are used.
    static constexpr uint16_t first[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769};
    static constexpr uint8_t bits[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8};
    unsigned c = 19;
    while (distance < first[c]) --c;
    unsigned reversed = 0;
    for (unsigned i = 0; i < 5; ++i) reversed = (reversed << 1) | ((c >> i) & 1);
    put(reversed, 5); put(distance - first[c], bits[c]);
  }
  void crc(size_t start) {
    uint32_t value = 0xFFFFFFFF;
    if (data && size <= capacity) {
      for (size_t i = start; i < size; ++i) {
        value ^= data[i];
        for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (0xEDB88320u & (0u - (value & 1)));
      }
    }
    be32(value ^ 0xFFFFFFFF);
  }
  void chunk(const char *type, const uint8_t *payload, size_t length) {
    be32(length); const size_t start = size;
    for (int i = 0; i < 4; ++i) byte(type[i]);
    for (size_t i = 0; i < length; ++i) byte(payload[i]);
    crc(start);
  }
};

// No zlib window or enlarged framebuffer: the only matches refer to a known
// horizontal run or a repeated row. PNG filter is always None.
// Returns required size for a count pass, zero for a failed read/overflow.
inline size_t encode(bool (*readRow)(int16_t, uint8_t *), uint8_t *output, size_t capacity) {
  Sink out{output, capacity};
  const uint8_t signature[] = {137,80,78,71,13,10,26,10};
  for (uint8_t v : signature) out.byte(v);
  const uint8_t ihdr[] = {0, 0, kWidth >> 8, kWidth & 255, 0, 0, kWidth >> 8, kWidth & 255, 8, 3, 0, 0, 0};  // indexed 8 bit, width and height patched below
  out.chunk("IHDR", ihdr, sizeof(ihdr));
  uint8_t palette[256 * 3];
  for (unsigned i = 0; i < 256; ++i) {
    palette[i * 3] = (i >> 5) * 255 / 7;
    palette[i * 3 + 1] = ((i >> 2) & 7) * 255 / 7;
    palette[i * 3 + 2] = (i & 3) * 255 / 3;
  }
  out.chunk("PLTE", palette, sizeof(palette));
  const size_t lengthAt = out.size;
  out.be32(0);
  const size_t crcAt = out.size;
  for (int i = 0; i < 4; ++i) out.byte("IDAT"[i]);
  const size_t dataAt = out.size;
  out.byte(0x78); out.byte(0x01); // zlib header; no dictionary
  out.put(3, 3); // final block, fixed Huffman codes
  uint32_t a = 1, b = 0;
  uint8_t rgb[64 * 3], row[kWidth];
  auto writeRow = [&](bool blank) {
    out.huffman(0); // filter byte
    for (int x = 0; x < kWidth;) {
      if (!blank && x >= kScale) {
        unsigned length = 0;
        while (length < 258 && x + length < kWidth && row[x + length] == row[x + length - kScale]) ++length;
        if (length >= 3) { out.match(length, kScale); x += length; continue; }
      }
      const uint8_t value = blank ? 0 : row[x];
      int end = x + 1;
      while (end < kWidth && (blank ? 0 : row[end]) == value) ++end;
      out.huffman(value);
      unsigned remaining = end - x - 1;
      while (remaining >= 3) {
        const unsigned n = remaining > 258 ? 258 : remaining;
        out.match(n, 1); remaining -= n;
      }
      while (remaining > 0) { out.huffman(value); --remaining; }
      x = end;
    }
  };
  for (int y = 0; y < 64; ++y) {
    if (!readRow(y, rgb)) return 0;
    uint32_t sum = 0, weighted = 0;
    // Each logical LED is a square cell with a black line on its left and top.
    // Only one expanded row exists at a time, never a full-size framebuffer.
    for (int x = 0; x < kWidth; ++x) {
      row[x] = x % kScale == 0 ? 0 : index(rgb + (x / kScale) * 3);
      sum += row[x]; weighted += (kWidth - x) * row[x];
    }
    for (int repeat = 0; repeat < kScale; ++repeat) {
      const bool blank = repeat == 0;
      if (repeat >= 2) {
        for (int left = kRowBytes; left > 0;) {  // identical preceding filtered row
          const int n = left > 258 ? 258 : left;
          out.match(n, kRowBytes); left -= n;
        }
      } else {
        writeRow(blank);
      }
      // Adler-32 for this entire row, using its byte sum and
      // weighted sum rather than visiting the same pixels again.
      b = (b + kRowBytes * a + (blank ? 0 : weighted)) % 65521;
      a = (a + (blank ? 0 : sum)) % 65521;
    }
  }
  out.huffman(256); out.flush();
  out.be32((b << 16) | a);
  const uint32_t length = out.size - dataAt;
  if (output && out.size <= capacity) {
    for (unsigned i = 0; i < 4; ++i) output[lengthAt + i] = length >> (24 - i * 8);
  }
  out.crc(crcAt);
  out.chunk("IEND", nullptr, 0);
  return !output || out.size <= capacity ? out.size : 0;
}
} // namespace screenpng
