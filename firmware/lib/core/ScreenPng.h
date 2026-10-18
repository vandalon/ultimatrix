// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace screenpng {
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
    // Only distances 1 (run), 8 (repeated LED cell), and 513 (row) are used.
    if (distance == 513) { put(9, 5); put(0, 8); } // code18, base513
    else if (distance == 8) { put(20, 5); put(1, 1); } // code5, base7
    else put(0, 5); // code0, distance1
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
  const uint8_t ihdr[] = {0,0,2,0,0,0,2,0,8,3,0,0,0};  // 512x512, indexed 8 bit
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
  uint8_t rgb[64 * 3], row[512];
  auto writeRow = [&](bool blank) {
    out.huffman(0); // filter byte
    for (int x = 0; x < 512;) {
      if (!blank && x >= 8) {
        unsigned length = 0;
        while (length < 258 && x + length < 512 && row[x + length] == row[x + length - 8]) ++length;
        if (length >= 3) { out.match(length, 8); x += length; continue; }
      }
      const uint8_t value = blank ? 0 : row[x];
      int end = x + 1;
      while (end < 512 && (blank ? 0 : row[end]) == value) ++end;
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
    // Each logical LED is a 6x6 square in an 8x8 cell, with black gaps.
    // Only one expanded row exists at a time, never a 512x512 framebuffer.
    for (int x = 0; x < 512; ++x) {
      const unsigned cellX = x & 7;
      row[x] = cellX == 0 || cellX == 7 ? 0 : index(rgb + (x / 8) * 3);
      sum += row[x]; weighted += (512 - x) * row[x];
    }
    for (int repeat = 0; repeat < 8; ++repeat) {
      const bool blank = repeat == 0 || repeat == 7;
      if ((repeat >= 2 && repeat <= 6) || (repeat == 0 && y > 0)) {
        out.match(258, 513); out.match(255, 513); // identical preceding filtered row
      } else {
        writeRow(blank);
      }
      // Adler-32 for this entire 513-byte row, using its byte sum and
      // weighted sum rather than visiting the same pixels six times.
      b = (b + 513 * a + (blank ? 0 : weighted)) % 65521;
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
