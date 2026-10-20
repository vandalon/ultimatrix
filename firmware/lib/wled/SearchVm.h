// The interpreter for the registration program the search Worker sends with its challenge (vm.js in the private ultimatrix-gifs-worker repo is the same, in JavaScript).
// 64 bytes of memory: the nonce is loaded at 0..15, the answer is memory 32..63. Instructions are [op, a, b], at most 100: nothing can loop or reach outside the memory.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

void umSha256(const uint8_t *data, size_t len, uint8_t out[32]);  // supplied by the code that includes this

namespace searchvm {
inline bool run(const uint8_t *prog, size_t len, const uint8_t *nonce, const uint8_t *facts, size_t factCount, uint8_t out[32]) {
  if (len % 3 || len > 300) return false;
  uint8_t m[64] = {0};
  memcpy(m, nonce, 16);
  for (size_t pc = 0; pc < len; pc += 3) {
    const uint8_t op = prog[pc], a = prog[pc + 1], b = prog[pc + 2], x = a & 63, y = b & 63;
    switch (op) {
      case 0: m[x] ^= m[y]; break;
      case 1: m[x] = (uint8_t)(m[x] + m[y]); break;
      case 2: { const int r = b & 7; m[x] = (uint8_t)((m[x] << r) | (m[x] >> (8 - r))); break; }
      case 3: m[x] = b; break;
      case 4: { const uint8_t t = m[x]; m[x] = m[y]; m[y] = t; break; }
      case 5: { uint8_t h[32]; umSha256(m, (a % 64) + 1, h); memcpy(m + 32, h, 32); break; }
      case 6: m[x] = b < factCount ? facts[b] : 0; break;
      case 7: m[x] = (uint8_t)(m[x] * (m[y] | 1)); break;
    }
  }
  memcpy(out, m + 32, 32);
  return true;
}
}  // namespace searchvm
