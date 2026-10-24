// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once
#include <stdint.h>
#include <stddef.h>

// Reads the d-pad (hat switch) and the left stick out of a HID report descriptor and the input reports of a game controller,
// so any controller that reports them in the usual places can steer. No allocation; a controller with more than kFields
// matching items is read only up to that many.
namespace hid {
enum { kHat, kX, kY };
enum { kNone = -2, kNeutral = -1 };   // direction(): the report says nothing about steering / nothing is pressed; otherwise 0 up, 1 right, 2 down, 3 left

struct Field { uint8_t id, kind, bits; uint16_t bitOffset; int32_t lmin, lmax; };

class Layout {
 public:
  static const int kFields = 8;
  Field f[kFields];
  int n = 0;

  bool usable() const { return n > 0; }

  // Walks the report map. Bit offsets count from the first data bit of each report (the report id byte is not part of a GATT notification).
  void parse(const uint8_t *map, size_t len) {
    n = 0;
    uint16_t page = 0; int32_t lmin = 0, lmax = 0; uint32_t size = 0, count = 0; uint8_t id = 0;
    uint32_t usages[16]; int nUsages = 0; uint32_t uMin = 0, uMax = 0; bool haveRange = false;
    uint16_t offsets[8]; uint8_t offsetIds[8]; int nOffsets = 0;
    size_t i = 0;
    while (i < len) {
      const uint8_t prefix = map[i++];
      if (prefix == 0xFE) { if (i >= len) break; i += 2 + map[i]; continue; }   // long item: skip
      const int bytes = (prefix & 3) == 3 ? 4 : (prefix & 3);
      if (i + bytes > len) break;
      uint32_t raw = 0; for (int b = 0; b < bytes; b++) raw |= (uint32_t)map[i + b] << (8 * b);
      i += bytes;
      int32_t sval = (int32_t)raw;
      if (bytes == 1) sval = (int8_t)raw; else if (bytes == 2) sval = (int16_t)raw;
      const int type = (prefix >> 2) & 3, tag = prefix >> 4;
      if (type == 1) {                                  // global
        if (tag == 0) page = (uint16_t)raw;
        else if (tag == 1) lmin = sval;
        else if (tag == 2) lmax = (lmin < 0) ? sval : (int32_t)raw;   // a range that starts at 0 or above is unsigned
        else if (tag == 7) size = raw;
        else if (tag == 8) id = (uint8_t)raw;
        else if (tag == 9) count = raw;
      } else if (type == 2) {                           // local
        if (tag == 0) { if (nUsages < 16) usages[nUsages++] = (bytes == 4) ? raw : ((uint32_t)page << 16 | raw); }
        else if (tag == 1) { uMin = (bytes == 4) ? raw : ((uint32_t)page << 16 | raw); haveRange = true; }
        else if (tag == 2) { uMax = (bytes == 4) ? raw : ((uint32_t)page << 16 | raw); haveRange = true; }
      } else if (type == 0) {                           // main
        if (tag == 8) {                                 // input
          int slot = -1; for (int k = 0; k < nOffsets; k++) if (offsetIds[k] == id) slot = k;
          if (slot < 0 && nOffsets < 8) { slot = nOffsets++; offsetIds[slot] = id; offsets[slot] = 0; }
          if (slot >= 0) {
            const bool variable = raw & 2, constant = raw & 1;
            for (uint32_t c = 0; c < count; c++) {
              uint32_t usage = 0;
              if (nUsages > 0) usage = usages[c < (uint32_t)nUsages ? c : nUsages - 1];
              else if (haveRange) usage = uMin + c <= uMax ? uMin + c : uMax;
              const uint16_t at = offsets[slot] + (uint16_t)(c * size);
              if (variable && !constant && (usage >> 16) == 1 && n < kFields && size > 0 && size <= 32) {
                const uint32_t u = usage & 0xFFFF;
                if (u == 0x39) f[n++] = {id, kHat, (uint8_t)size, at, lmin, lmax};
                else if (u == 0x30 && !has(id, kX)) f[n++] = {id, kX, (uint8_t)size, at, lmin, lmax};
                else if (u == 0x31 && !has(id, kY)) f[n++] = {id, kY, (uint8_t)size, at, lmin, lmax};
              }
            }
            offsets[slot] += (uint16_t)(count * size);
          }
        }
        nUsages = 0; haveRange = false;                 // local items end with every main item
      }
    }
  }

  // The direction in an input report: the d-pad first, then the left stick.
  int direction(uint8_t id, const uint8_t *data, size_t len, int previous) const {
    int result = kNone;
    for (int k = 0; k < n; k++) {
      if (f[k].id != id || f[k].kind != kHat) continue;
      int32_t v; if (!read(f[k], data, len, v)) continue;
      result = kNeutral;
      const int32_t steps = v - f[k].lmin, range = f[k].lmax - f[k].lmin + 1;
      if (v < f[k].lmin || v > f[k].lmax || range < 4) break;
      const int dir8 = range >= 8 ? (int)steps : (int)steps * 2;   // 0 up, 1 up-right, 2 right ... 7 up-left
      static const int8_t plain[8] = {0, -1, 1, -1, 2, -1, 3, -1};
      if (plain[dir8 & 7] >= 0) return plain[dir8 & 7];
      const int vertical = (dir8 == 1 || dir8 == 7) ? 0 : 2, horizontal = (dir8 == 1 || dir8 == 3) ? 1 : 3;
      return (previous == vertical || previous == horizontal) ? previous : vertical;   // a diagonal keeps the direction he already has
    }
    int32_t x = 0, y = 0, lx = 0, hx = 0, ly = 0, hy = 0; bool gotX = false, gotY = false;
    for (int k = 0; k < n; k++) {
      if (f[k].id != id) continue;
      if (f[k].kind == kX && read(f[k], data, len, x)) { gotX = true; lx = f[k].lmin; hx = f[k].lmax; }
      if (f[k].kind == kY && read(f[k], data, len, y)) { gotY = true; ly = f[k].lmin; hy = f[k].lmax; }
    }
    if (!gotX && !gotY) return result;
    // Away from the middle by more than a quarter of the whole travel (half of the way to the end)
    const int64_t dx = gotX ? ((int64_t)x * 2 - lx - hx) : 0, dy = gotY ? ((int64_t)y * 2 - ly - hy) : 0;
    const int64_t tx = gotX ? ((int64_t)hx - lx) / 2 : 1, ty = gotY ? ((int64_t)hy - ly) / 2 : 1;   // |dx| runs to (hx-lx) at the ends
    const bool right = dx > tx, left = -dx > tx, down = dy > ty, up = -dy > ty;
    if (!(right || left || up || down)) return kNeutral;
    const int64_t ax = (dx < 0 ? -dx : dx) * (ty ? ty : 1), ay = (dy < 0 ? -dy : dy) * (tx ? tx : 1);   // compare each as a share of its own range
    if ((right || left) && (!(up || down) || ax >= ay)) return right ? 1 : 3;
    return down ? 2 : 0;
  }

 private:
  bool has(uint8_t id, uint8_t kind) const { for (int k = 0; k < n; k++) if (f[k].id == id && f[k].kind == kind) return true; return false; }
  static bool read(const Field &fl, const uint8_t *data, size_t len, int32_t &out) {
    if ((size_t)(fl.bitOffset + fl.bits + 7) / 8 > len) return false;
    uint32_t v = 0;
    for (int b = 0; b < fl.bits; b++) { const unsigned at = fl.bitOffset + b; if (data[at >> 3] >> (at & 7) & 1) v |= 1u << b; }
    if (fl.lmin < 0 && fl.bits < 32 && (v >> (fl.bits - 1) & 1)) v |= ~0u << fl.bits;   // signed
    out = (int32_t)v;
    return true;
  }
};
}
