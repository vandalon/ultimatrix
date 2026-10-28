#include "ESP32-HUB75-MatrixPanel-I2S-DMA.h"

#if defined(SPIRAM_DMA_BUFFER)
// Sprite_TM saves the day again...
// https://www.esp32.com/viewtopic.php?f=2&t=30584
#include "rom/cache.h"
#endif

/* This replicates same function in rowBitStruct, but due to induced inlining it might be MUCH faster
 * when used in tight loops while method from struct could be flushed out of instruction cache between
 * loop cycles do NOT forget about buff_id param if using this.
 */
// #define getRowDataPtr(row, _dpth, buff_id) &(dma_buff.rowBits[row]->data[_dpth * dma_buff.rowBits[row]->width + buff_id*(dma_buff.rowBits[row]->width * dma_buff.rowBits[row]->colour_depth)])

// BufferID is now ignored, seperate global pointer pointer!
#define getRowDataPtr(row, _dpth) &(fb->rowBits[row]->data[_dpth * fb->rowBits[row]->width])

/* We need to update the correct uint16_t in the rowBitStruct array, that gets sent out in parallel
 * 16 bit parallel mode - Save the calculated value to the bitplane memory in reverse order to account for I2S Tx FIFO mode1 ordering
 * Irrelevant for ESP32-S2 the way the FIFO ordering works is different - refer to page 679 of S2 technical reference manual
 */
#if defined(ESP32_THE_ORIG)
#define ESP32_TX_FIFO_POSITION_ADJUST(x_coord) (((x_coord)&1U) ? (x_coord - 1) : (x_coord + 1))
#else
#define ESP32_TX_FIFO_POSITION_ADJUST(x_coord) x_coord
#endif






#ifdef HUB75_RING_STREAM
#include "esp_cpu.h"

/* Streaming mode. The panel's bit planes are not kept for the whole frame: a small ring of rows is refilled from a
 * compact framebuffer by the row-end interrupt. Each row's DMA descriptors point at ring slot (row % n), so the chain is
 * fixed and the only work per row is regenerating the slot that was just sent, n rows ahead.
 *
 * The compact framebuffer is already spread for the interrupt: six bytes per column/row pair hold two 24-bit
 * groups (colour bits 0-3 and 4-7). Each colour-bit lane contains exactly R1 G1 B1 R2 G2 B2, with no padding.
 * Drawing spreads the colours using a 16-entry table. The interrupt extracts six-bit lanes without RGB conversion.
 */
namespace {
#define ESP_CPU_FREQ_MHZ_FOR_DIAG 240u // the clock runs at 240 MHz
constexpr uint32_t kRingMaxRows = 32;
constexpr uint32_t kRingMaxHeight = 64;

struct RingState
{
  uint16_t *slot[kRingMaxRows];         // the ring: width * depth words each, the layout of a rowBitStruct
  uint8_t *crow[2][kRingMaxRows];       // six bytes per column/row pair, two groups of four six-bit lanes
  const uint16_t *tmpl;                 // LAT and OE words per plane, in wire order (no row address, no pixel bits)
  uint32_t width, depth, rows, n, descPerRow;
  volatile uint32_t front, frontNext;   // framebuffer being sent, framebuffer to send from the next frame on
  volatile uint32_t frontLower;         // framebuffer the lower panel half is sent from (lags front by one sweep when lag is on)
  volatile uint32_t flips, waits, waitUs, waitMaxUs; // flips, draws that had to wait, total/longest wait
  uint32_t lastEntry;                   // cycle count at the previous row interrupt
  struct LateEvent { uint32_t gapUs, pc, ms; char task[9]; };
  LateEvent lateLog[8];                 // the last gaps over 1 ms: how long, where the CPU was when the interrupt finally ran, and in which task
  volatile uint32_t lateSeq;            // number of events recorded so far
  volatile uint32_t gapMax, late[4];    // longest time between two row interrupts (cycles); counts over 1, 2, 5 and 10 ms
  volatile uint32_t own, lag;           // draw calls wait for the back buffer to be released; lower half lags one sweep
  volatile uint32_t pending;            // set by a flip, cleared once the drawing side has seen the back buffer released
  uint32_t nextFill;                    // next row to generate
  uint32_t curRow;                      // row last reported sent; the chain has only n row groups, so the interrupt follows the absolute row itself
  volatile uint32_t filled, missed, maxCycles, calls;
  volatile uint64_t sumCycles;
};
RingState g_ring;

typedef uint32_t __attribute__((may_alias)) u32a;

// After a flip, the buffer drawn into is free once neither panel half is still sent from it. Waiting happens at most once per flip.
void __attribute__((noinline)) ringWait(uint32_t back)
{
  RingState &R = g_ring;
  R.pending = 0;
  if (!(R.front == back || R.frontLower == back))
    return;
  const uint32_t t0 = (uint32_t)esp_timer_get_time();
  for (uint32_t i = 0; i < 400 && (R.front == back || R.frontLower == back); ++i)
    esp_rom_delay_us(100);
  const uint32_t us = (uint32_t)esp_timer_get_time() - t0;
  R.waits = R.waits + 1;
  R.waitUs = R.waitUs + us;
  if (us > R.waitMaxUs)
    R.waitMaxUs = us;
}

inline __attribute__((always_inline)) void ringAcquire(uint32_t back)
{
  if (__builtin_expect(g_ring.pending, 0))
    ringWait(back);
}

// A four-bit value with bit p moved to bit 6p; each lane has exactly six output channels.
uint32_t DRAM_ATTR kSpread[16] = {
    0x000000, 0x000001, 0x000040, 0x000041, 0x001000, 0x001001, 0x001040, 0x001041, 0x040000, 0x040001, 0x040040, 0x040041, 0x041000, 0x041001, 0x041040, 0x041041};

inline uint32_t ringLoad24(const uint8_t *q) {
  return (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16);
}

// A six-byte column starts at offset 0 or 2 within an aligned word. Update its two words while preserving
// the neighboring column and the opposite panel half. The row width is even, so both words stay inside the row.
inline __attribute__((always_inline)) void ringPut(uint8_t *q, bool bottom, uint8_t r, uint8_t g, uint8_t b)
{
  const uint32_t sh = bottom ? 3 : 0;
  const uint32_t mask = 0x1C71C7u << sh;
  const uint32_t lo = (kSpread[r & 15] | (kSpread[g & 15] << 1) | (kSpread[b & 15] << 2)) << sh;
  const uint32_t hi = (kSpread[r >> 4] | (kSpread[g >> 4] << 1) | (kSpread[b >> 4] << 2)) << sh;
  const uintptr_t address = (uintptr_t)q;
  u32a *const words = (u32a *)(address & ~(uintptr_t)3);
  if (address & 2)
  {
    words[0] = (words[0] & ~(mask << 16)) | (lo << 16);
    words[1] = (words[1] & ~((mask >> 16) | (mask << 8))) | (lo >> 16) | (hi << 8);
  }
  else
  {
    words[0] = (words[0] & ~(mask | (mask << 24))) | lo | (hi << 24);
    words[1] = (words[1] & ~(mask >> 8)) | (hi >> 8);
  }
}

// Bits of the upper panel half (R1 G1 B1) in the three words of a column pair; the lower half holds the rest.
constexpr uint32_t kUpper = 0x1C71C7u;
constexpr uint32_t kMask0 = kUpper | (kUpper << 24), kMask1 = (kUpper >> 8) | (kUpper << 16), kMask2 = (kUpper >> 16) | (kUpper << 8);

// With Mixed, the upper half is read from px and the lower half from pl (another framebuffer).
template <uint32_t Depth, bool Mixed>
inline __attribute__((always_inline)) void ringFillPixels(uint16_t *slot, const uint16_t *tm,
                                                        const uint8_t *px, const uint8_t *pl, uint32_t width,
                                                        uint32_t addrPrev, uint32_t addrRow)
{
  const uint32_t stride = width / 2;
  u32a *const dst = (u32a *)slot;
  const u32a *const control = (const u32a *)tm;
  for (uint32_t x = 0; x < width; x += 2, px += 12, pl += 12)
  {
    // Keep both pixels' low/high lanes in registers across all planes, rather than rereading them per plane.
    // Two columns occupy 12 bytes, so all three loads stay aligned.
    const u32a *const words = (const u32a *)px;
    uint32_t a = words[0], b = words[1], c = words[2];
    if (Mixed)
    {
      const u32a *const lower = (const u32a *)pl;
      a = (a & kMask0) | (lower[0] & ~kMask0);
      b = (b & kMask1) | (lower[1] & ~kMask1);
      c = (c & kMask2) | (lower[2] & ~kMask2);
    }
    const uint32_t lo0 = a, hi0 = (a >> 24) | (b << 8);
    const uint32_t lo1 = (b >> 16) | (c << 16), hi1 = c >> 8;
    const uint32_t column = x / 2;
#pragma GCC unroll 8
    for (uint32_t p = 0; p < Depth; ++p)
    {
      const uint32_t shift = (p & 3) * 6;
      const uint32_t v0 = ((p < 4 ? lo0 : hi0) >> shift) & 0x3F;
      const uint32_t v1 = ((p < 4 ? lo1 : hi1) >> shift) & 0x3F;
      const uint32_t offset = p * stride + column;
      // Original ESP32 FIFO order: pixel x goes to the upper word, x+1 to the lower word.
      dst[offset] = control[offset] | (p ? addrRow : addrPrev) | v1 | (v0 << 16);
    }
  }
}

void IRAM_ATTR __attribute__((optimize("O3"))) ringFillRow(RingState &R, uint32_t row)
{
  if (row == 0)
  {
    R.frontLower = R.lag ? R.front : R.frontNext;
    R.front = R.frontNext; // a new frame is taken over at its first row only, so a frame is never shown half and half
  }

  uint16_t *const slot = R.slot[row % R.n];
  const uint16_t *const tm = R.tmpl;
  const uint8_t *const px = R.crow[R.front][row];
  const uint8_t *const pl = R.crow[R.frontLower][row];
  const uint32_t W = R.width, D = R.depth;

  // While a row is clocked in, the previous row is still shown: plane 0 carries the previous row's address
  const uint32_t addrPrev = (row ? row - 1 : R.rows - 1) << BITS_ADDR_OFFSET;
  const uint32_t addrRow = row << BITS_ADDR_OFFSET;

  if (D == 8 || D == 7)
  {
    const bool mixed = R.front != R.frontLower;
    if (D == 8)
    {
      if (mixed) ringFillPixels<8, true>(slot, tm, px, pl, W, addrPrev * 0x10001u, addrRow * 0x10001u);
      else ringFillPixels<8, false>(slot, tm, px, pl, W, addrPrev * 0x10001u, addrRow * 0x10001u);
    }
    else if (mixed) ringFillPixels<7, true>(slot, tm, px, pl, W, addrPrev * 0x10001u, addrRow * 0x10001u);
    else ringFillPixels<7, false>(slot, tm, px, pl, W, addrPrev * 0x10001u, addrRow * 0x10001u);
    return;
  }

  // Plane by plane, two pixels per 32 bit store. The words of a pair are swapped on the wire (ESP32_TX_FIFO_POSITION_ADJUST)
  for (uint32_t p = 0; p < D; p++)
  {
    const uint8_t *src = px + (p >> 2) * 3; // low/high 24-bit group, interleaved per column
    const uint32_t sh = (p & 3) * 6;
    const uint32_t a = (p ? addrRow : addrPrev) * 0x10001u;
    u32a *w = (u32a *)(slot + p * W);
    const u32a *t = (const u32a *)(tm + p * W);
    for (uint32_t x = 0; x < W; x += 2, src += 12)
    {
      const uint32_t v0 = (ringLoad24(src) >> sh) & 0x3F; // pixel x goes to the odd word
      const uint32_t v1 = (ringLoad24(src + 6) >> sh) & 0x3F; // pixel x + 1 goes to the even word
      *w++ = *t++ | a | v1 | (v0 << 16);
    }
  }
}

// A row has just been sent: refill its slot with the row n ahead. If this ran late, catch up. Lateness up to n-2 rows
// is invisible (the rows caught up on are still ahead of the DMA); only rows the DMA had already reached count as missed.
void IRAM_ATTR ringEof(void *, uint32_t descIndex)
{
  RingState &R = g_ring;
  const uint32_t t0 = esp_cpu_get_cycle_count();
  const uint32_t gap = t0 - R.lastEntry; // normally one row time (228 us); more means interrupts were held off
  R.lastEntry = t0;
  if (gap > R.gapMax && R.calls > 8)
    R.gapMax = gap;
  const uint32_t mhz = ESP_CPU_FREQ_MHZ_FOR_DIAG;
  if (gap > 1000 * mhz && R.calls > 8)
  {
    // EPC2 is where the interrupted code stood: the first instruction after interrupts were switched back on, so inside or right after what held them off.
    uint32_t pc;
    __asm__ __volatile__("rsr %0, epc2" : "=a"(pc));
    RingState::LateEvent &e = R.lateLog[R.lateSeq & 7];
    e.gapUs = gap / mhz;
    e.pc = pc;
    e.ms = (uint32_t)(esp_timer_get_time() / 1000);
    const char *name = pcTaskGetName(nullptr);
    for (int i = 0; i < 8; i++)
      e.task[i] = name ? name[i] : 0;
    e.task[8] = 0;
    R.lateSeq = R.lateSeq + 1;
  }
  if (gap > 1000 * mhz) { R.late[0] = R.late[0] + 1; if (gap > 2000 * mhz) { R.late[1] = R.late[1] + 1; if (gap > 5000 * mhz) { R.late[2] = R.late[2] + 1; if (gap > 10000 * mhz) R.late[3] = R.late[3] + 1; } } }
  // The chain holds n row groups (one per slot) and loops, so the descriptor only gives the slot. The row is the next one after the
  // previous interrupt whose slot matches; late by up to n-1 rows re-syncs, a whole lap (n rows) late cannot be told apart.
  const uint32_t slot = descIndex / R.descPerRow;
  if (slot >= R.n)
    return;
  uint32_t k = slot + R.n - (R.curRow % R.n); // rows % n == 0, so slot == row % n
  k %= R.n;
  if (k == 0)
    k = R.n;
  uint32_t cur = R.curRow + k;
  if (cur >= R.rows)
    cur -= R.rows;
  R.curRow = cur;
  uint32_t target = cur + R.n;
  if (target >= R.rows)
    target -= R.rows;
  int32_t d = (int32_t)target - (int32_t)R.nextFill;
  if (d < 0)
    d += R.rows;
  const uint32_t count = d + 1;
  if (count >= R.n)
    R.missed += count - R.n + 1; // rows nextFill..cur+1: sent or being sent while still holding the row n before them
  for (uint32_t i = 0; i < count; i++)
  {
    ringFillRow(R, R.nextFill);
    if (++R.nextFill >= R.rows)
      R.nextFill = 0;
  }
  R.filled += count;
  const uint32_t used = esp_cpu_get_cycle_count() - t0;
  if (used > R.maxCycles)
    R.maxCycles = used;
  R.sumCycles += used;
  R.calls = R.calls + 1;
}
} // namespace
#endif // HUB75_RING_STREAM

  /* Below the window floor (see setBrightnessOE) the dimming is done here, in linear light, so the plane weights stay binary.
   * Null while the output enable window alone sets the brightness. */
  static uint8_t g_gainTable[256];
  static const uint8_t *g_gainLut = nullptr;

  /* A dim colour can round to black in all three channels while the gain table is active. Black would lose it altogether and the old rule
   * (every nonzero input shows at least level 1, per channel) turned it grey, because the small channels of a green or blue came up to 1 as
   * well. Instead only the channels that carry at least half of the strongest one's light get level 1: a dim green stays green, a dim grey
   * stays grey. */
  static inline void keepHue(uint8_t red, uint8_t green, uint8_t blue, uint8_t &red_val, uint8_t &green_val, uint8_t &blue_val)
  {
    const unsigned r = lumConvTab_12bit[red], g = lumConvTab_12bit[green], b = lumConvTab_12bit[blue];
    const unsigned top = r > g ? (r > b ? r : b) : (g > b ? g : b);
    red_val = r * 2 >= top && r ? 1 : 0;
    green_val = g * 2 >= top && g ? 1 : 0;
    blue_val = b * 2 >= top && b ? 1 : 0;
  }

  /* Bit planes up to the transition bit are each shown for one slot, the higher ones for twice as long as the one below (the descriptor
   * chain). A value's light is the sum over its set bits, which is not monotonic in the value once the transition bit is above 0 (e.g.
   * with 1: 3 shows 2 slots, 4 shows 1). The brightness tables are lifted so a higher input never shows less light than a lower one
   * (same idea as adjust_lut_for_bcm in esphome-libs/esp-hub75). Values never decrease either: the screen readback searches the table. */
  static unsigned bcmWeight(unsigned value, int depth, int transition)
  {
    unsigned weight = 0;
    for (int bit = 0; bit < depth; bit++)
      if (value & (1u << bit)) weight += bit <= transition ? 1u : 1u << (bit - transition - 1);
    return weight;
  }

  static void bcmMonotonic(uint8_t *lut, int depth, int transition)
  {
    const unsigned max_val = (1u << depth) - 1;
    unsigned prevValue = 0, prevWeight = 0;
    for (int i = 0; i < 256; i++)
    {
      unsigned best = lut[i] > prevValue ? lut[i] : prevValue;
      while (best < max_val && bcmWeight(best, depth, transition) < prevWeight) best++;
      lut[i] = (uint8_t)best;
      prevValue = best;
      prevWeight = bcmWeight(best, depth, transition);
    }
  }

  static uint8_t g_bcmTable[256];  // the full-brightness table with the correction above; valid once begin() has built it
  static bool g_bcmReady = false;
  static inline const uint8_t *baseLut(unsigned depth) { return g_bcmReady ? g_bcmTable : cie_lut_for_depth(depth); }

#if !defined(HUB75_NO_DITHER) && !defined(NO_CIE1931) && defined(LUT_NATIVE_BIT_DEPTH)
  #define HUB75_DITHER
  /* Ordered dithering while the gain table is active: the table is also kept with 8 fractional bits (g_dt->frac), an 8x8 Bayer threshold
   * from the physical x,y is added before the cut to whole output levels, and the result goes through g_dt->map (the bcmMonotonic
   * correction applied to the identity), so a higher value never shows less light. Off (g_dither false) at full brightness and above 8 bit. */
  struct DitherTables { uint16_t frac[256]; uint8_t map[256]; };
  static DitherTables *g_dt = nullptr;  // allocated on first use: no static RAM while dithering is off
  static int g_ditherMapKey = -1;  // depth * 16 + transition the map was built for
  static unsigned g_ditherMax = 255;
  static bool g_dither = false;
  static const uint8_t kBayer8[64] = {
     0, 32,  8, 40,  2, 34, 10, 42,
    48, 16, 56, 24, 50, 18, 58, 26,
    12, 44,  4, 36, 14, 46,  6, 38,
    60, 28, 52, 20, 62, 30, 54, 22,
     3, 35, 11, 43,  1, 33,  9, 41,
    51, 19, 59, 27, 49, 17, 57, 25,
    15, 47,  7, 39, 13, 45,  5, 37,
    63, 31, 55, 23, 61, 29, 53, 21};
  static inline uint8_t ditherChannel(uint8_t c, unsigned thr)
  {
    unsigned n = (g_dt->frac[c] + thr) >> 8;
    return g_dt->map[n > g_ditherMax ? g_ditherMax : n];
  }
  static inline unsigned ditherThr(unsigned x, unsigned y) { return kBayer8[((y & 7) << 3) | (x & 7)] * 4u + 2u; }
  // A solid colour needs the per-pixel path only when one channel has a fractional part
  static inline bool ditherFractional(uint8_t r, uint8_t g, uint8_t b) { return g_dither && ((g_dt->frac[r] | g_dt->frac[g] | g_dt->frac[b]) & 0xFF); }
  #define DO_DITHER(X, Y)                                                                             \
    if (g_dither) {                                                                                   \
      const unsigned thr_ = ditherThr(X, Y);                                                          \
      red_val = ditherChannel(red, thr_); green_val = ditherChannel(green, thr_); blue_val = ditherChannel(blue, thr_); \
    }
#else
  #define DO_DITHER(X, Y)
  static inline bool ditherFractional(uint8_t, uint8_t, uint8_t) { return false; }
#endif

  /* LED Brightness Compensation */                                                               
#ifndef NO_CIE1931                                                                                
  /* CIE 1931 correction with bit-depth-optimized LUTs*/                                            
  #ifdef LUT_NATIVE_BIT_DEPTH


    // Optimized path: LUT maps 8-bit input (0-255) directly to target bit depth output
    #define DO_BRIGHTNESS_COMPENSATION()                                                              \
      const uint8_t *lut_ = g_gainLut ? g_gainLut : baseLut(m_cfg.getPixelColorDepthBits());          \
      auto red_val   = lut_[red];                                                                     \
      auto green_val = lut_[green];                                                                   \
      auto blue_val  = lut_[blue];                                                                    \
      if (g_gainLut && !(red_val | green_val | blue_val) && (red | green | blue)) keepHue(red, green, blue, red_val, green_val, blue_val); \


  #else
    // Fallback for non-standard bit depths: 12-bit LUT with shift+round to target depth

    #define DO_BRIGHTNESS_COMPENSATION()                                                              \
      uint16_t red16   = lumConvTab[red];                                                             \
      uint16_t green16 = lumConvTab[green];                                                           \
      uint16_t blue16  = lumConvTab[blue];                                                            \
                                                                                                      \
      uint8_t shift_amount = 12 - m_cfg.getPixelColorDepthBits();                                     \
      uint16_t rounding = 1 << (shift_amount - 1);                                                    \
      uint16_t max_val = (1 << m_cfg.getPixelColorDepthBits()) -1;                                    \
      auto red_val   = (red16 + rounding) >> shift_amount;                                            \
      auto green_val = (green16 + rounding) >> shift_amount;                                          \
      auto blue_val  = (blue16 + rounding) >> shift_amount;                                           \
                                                                                                      \
      red_val = red_val > max_val ? max_val : red_val;                                                \
      green_val = green_val > max_val ? max_val : green_val;                                          \
      blue_val = blue_val > max_val ? max_val : blue_val;  
  #endif
#else
  // NO_CIE1931: linear scaling with rounding
  #define DO_BRIGHTNESS_COMPENSATION()                                                               \
    uint16_t red16   = red * 256u;                                                                   \
    uint16_t green16 = green * 256u;                                                                 \
    uint16_t blue16  = blue * 256u;                                                                  \
                                                                                                     \
    uint8_t shift_amount = 16 - m_cfg.getPixelColorDepthBits();                                      \
    uint16_t rounding = (1 << (shift_amount - 1))-1;                                                 \
    uint16_t max_val = (1 << m_cfg.getPixelColorDepthBits()) -1;                                     \
    uint16_t red_val   = (red16 + rounding) >> shift_amount;                                         \
    uint16_t green_val = (green16 + rounding) >> shift_amount;                                       \
    uint16_t blue_val  = (blue16 + rounding) >> shift_amount;                                        \
    red_val = red_val > max_val ? max_val : red_val;                                                 \
    green_val = green_val > max_val ? max_val : green_val;                                           \
    blue_val = blue_val > max_val ? max_val : blue_val;                                              
#endif




/* This library is designed to take an 8 bit / 1 byte value (0-255) for each R G B colour sub-pixel.
 *
 * When CIE1931 correction is enabled, input values are passed through a perceptually-linear
 * brightness curve (lumConvTab) that maps to the target bit depth. Native lookup tables are
 * provided for common bit depths (6, 7, 8, 10, 12-bit), while other depths use runtime
 * conversion with rounding to preserve precision and minimize banding artifacts.
 *
 * When NO_CIE1931 is defined, linear scaling is used: 8-bit input is scaled to 16-bit range
 * (input * 257), then shifted down to the target bit depth with rounding.
 */

bool MatrixPanel_I2S_DMA::setupDMA(const HUB75_I2S_CFG &_cfg)
{
  
  /***
   * Step 0: Allocate basic DMA framebuffer memory for the data we send out in parallel to the HUB75 panel.
   *         Colour depth is the only consideration.
   * 
   */
  ESP_LOGI("I2S-DMA", "Free heap: %d", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  ESP_LOGI("I2S-DMA", "Free SPIRAM: %d", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  size_t allocated_fb_memory = 0;

#ifdef HUB75_RING_STREAM
  ring_on = ringEligible();
  if (ring_on)
    ESP_LOGI("I2S-DMA", "Streaming mode: ring of %d rows, compact framebuffer", m_cfg.ring_rows);
  else if (m_cfg.ring_rows)
    ESP_LOGW("I2S-DMA", "Streaming mode requested but this configuration cannot use it");
  const int slotRows = ring_on ? m_cfg.ring_rows : ROWS_PER_FRAME; // rows of bit planes kept in memory
#else
  const int slotRows = ROWS_PER_FRAME;
#endif

#ifdef HUB75_RING_STREAM
  int fbs_required = ring_on ? 1 : ((m_cfg.double_buff) ? 2 : 1); // the ring is one set of bit planes, double buffering is in the compact framebuffer
#else
  int fbs_required = (m_cfg.double_buff) ? 2 : 1;
#endif

  for (int fb = 0; fb < (fbs_required); fb++)
  {
    frame_buffer[fb].rowBits.reserve(slotRows);

    for (int malloc_num = 0; malloc_num < slotRows; malloc_num++)
    {
      auto ptr = std::make_shared<rowBitStruct>(PIXELS_PER_ROW, m_cfg.getPixelColorDepthBits());

      if (ptr->data == nullptr) {

        ESP_LOGE("I2S-DMA", "CRITICAL ERROR: Not enough memory for requested colour depth of %d bits! Please reduce pixel_color_depth_bits value.\r\n", m_cfg.getPixelColorDepthBits());

        return false;
        // TODO: should we release all previous rowBitStructs here???
      }

      allocated_fb_memory += ptr->getColorDepthSize(false); // byte required to display all colour depths for the two parallel rows
      frame_buffer[fb].rowBits.emplace_back(ptr); // save new rowBitStruct pointer into rows vector
      ++frame_buffer[fb].rows;
    }
  }
  ESP_LOGI("I2S-DMA", "Allocating %d bytes memory for DMA BCM framebuffer(s).", allocated_fb_memory);

#ifdef HUB75_RING_STREAM
  if (ring_on && !ringAllocate())
  {
    ESP_LOGE("I2S-DMA", "CRITICAL ERROR: Not enough memory for the compact framebuffer.");
    return false;
  }
#endif


  /***
   * Step 1: Check what the minimum refresh rate is, and calculate the lsbMsbTransitionBit
   *         which is the bit at which we can reduce the colour depth to achieve the minimum refresh rate.
   * 
   *         This also determines the number of DMA descriptors required per row.
   */  
   
//#define FORCE_COLOR_DEPTH 1   
  
#if !defined(FORCE_COLOR_DEPTH)

  ESP_LOGI("I2S-DMA", "Minimum visual refresh rate (scan rate from panel top to bottom) requested: %d Hz", m_cfg.min_refresh_rate);

  while (1)
  {
    int psPerClock = 1000000000000UL / m_cfg.i2sspeed;
    int nsPerLatch = ((PIXELS_PER_ROW + CLKS_DURING_LATCH) * psPerClock) / 1000; // time per row

    // add time to shift out LSBs + LSB-MSB transition bit - this ignores fractions...
    int nsPerRow = m_cfg.getPixelColorDepthBits() * nsPerLatch;

    // Now add the time for the remaining bit depths
    for (int i = lsbMsbTransitionBit + 1; i < m_cfg.getPixelColorDepthBits(); i++) {
      //nsPerRow += (1 << (i - lsbMsbTransitionBit - 1)) * (m_cfg.getPixelColorDepthBits() - i) * nsPerLatch;
	  nsPerRow += (1 << (i - lsbMsbTransitionBit - 1)) *  nsPerLatch;
	}

    int nsPerFrame = nsPerRow * ROWS_PER_FRAME;
    int actualRefreshRate = 1000000000UL / (nsPerFrame);
    calculated_refresh_rate = actualRefreshRate;

    ESP_LOGW("I2S-DMA", "lsbMsbTransitionBit of %d gives %d Hz refresh rate.", lsbMsbTransitionBit, actualRefreshRate);

    if (actualRefreshRate >= m_cfg.min_refresh_rate)
      break;

    if (lsbMsbTransitionBit < m_cfg.getPixelColorDepthBits() - 1)
      lsbMsbTransitionBit++;
    else
      break;
  }

  if (lsbMsbTransitionBit > 0)
  {
    ESP_LOGW("I2S-DMA", "lsbMsbTransitionBit of %d used to achieve refresh rate of %d Hz. Percieved colour depth to the eye may be reduced.", lsbMsbTransitionBit, m_cfg.min_refresh_rate);
  }

  ESP_LOGI("I2S-DMA", "DMA frame buffer color depths: %d", m_cfg.getPixelColorDepthBits());

#endif

#if !defined(NO_CIE1931) && defined(LUT_NATIVE_BIT_DEPTH)
  // The transition bit is final now: correct the full-brightness table for the plane weights
  g_bcmReady = false;
  if (m_cfg.getPixelColorDepthBits() >= 7)
  {
    if (const uint8_t *base = cie_lut_for_depth(m_cfg.getPixelColorDepthBits()))
    {
      memcpy(g_bcmTable, base, sizeof(g_bcmTable));
      bcmMonotonic(g_bcmTable, m_cfg.getPixelColorDepthBits(), lsbMsbTransitionBit);
      g_bcmReady = true;
    }
  }
#endif

  /***
   * Step 2:  Calculate the DMA descriptors required, which is used for memory allocation of the DMA linked list memory structure.
   *          This determines the number of passes required to shift out the colour bits in the framebuffer.
   *          We need to also take into consderation where a chain of panels (pixels) is so long, it requires more than one DMA payload,
   *          give this library's DMA output memory allocation approach is by the row.
   */
	
  int    dma_descs_per_row_1cdepth	 	= (frame_buffer[0].rowBits[0]->getColorDepthSize(true) + DMA_MAX - 1 ) / DMA_MAX;
  size_t last_dma_desc_bytes_1cdepth    = (frame_buffer[0].rowBits[0]->getColorDepthSize(true) % DMA_MAX);
  
  int    dma_descs_per_row_all_cdepths	  = (frame_buffer[0].rowBits[0]->getColorDepthSize(false) + DMA_MAX - 1 ) / DMA_MAX;
  size_t last_dma_desc_bytes_all_cdepths  = (frame_buffer[0].rowBits[0]->getColorDepthSize(false) % DMA_MAX);

  // Logging the calculated values
  ESP_LOGV("I2S-DMA", "dma_descs_per_row_1cdepth: %d", dma_descs_per_row_1cdepth);
  ESP_LOGV("I2S-DMA", "last_dma_desc_bytes_1cdepth: %zu", last_dma_desc_bytes_1cdepth);
  ESP_LOGV("I2S-DMA", "dma_descs_per_row_all_cdepths: %d", dma_descs_per_row_all_cdepths);
  ESP_LOGV("I2S-DMA", "last_dma_desc_bytes_all_cdepths: %zu", last_dma_desc_bytes_all_cdepths);
  
 
  // Calculate per-row number
  int dma_descriptors_per_row = dma_descs_per_row_all_cdepths;

  // Add descriptors for MSB bits after transition
  for (int i = lsbMsbTransitionBit + 1; i < m_cfg.getPixelColorDepthBits(); i++) {
    dma_descriptors_per_row += (1 << (i - lsbMsbTransitionBit - 1)) * dma_descs_per_row_1cdepth;
  }
  
  //dma_descriptors_per_row = 1;

  // Allocate DMA descriptors 
#ifdef HUB75_RING_STREAM
  const int chainRows = ring_on ? slotRows : ROWS_PER_FRAME; // the ring chain has one row group per slot and loops over them
#else
  const int chainRows = ROWS_PER_FRAME;
#endif
  int dma_descriptions_required = dma_descriptors_per_row * chainRows;

  ESP_LOGV("I2S-DMA", "DMA descriptors per row: %d", dma_descriptors_per_row);  
  ESP_LOGV("I2S-DMA", "DMA descriptors required per buffer: %d", dma_descriptions_required);    

  /***
   * Step 3:  Allocate the DMA descriptor memory via. the relevant platform DMA implementation class.
   */

  if (m_cfg.double_buff
#ifdef HUB75_RING_STREAM
      && !ring_on
#endif
  ) {
    dma_bus.enable_double_dma_desc();
  }

  if ( !dma_bus.allocate_dma_desc_memory(dma_descriptions_required) )
  {
    return false;
  }


  /***
   * Step 4:  Link up the DMA descriptors per the colour depth and rows.
   */

  //fbs_required = 1; // (m_cfg.double_buff) ? 2 : 1;
  for (int fb = 0; fb < (fbs_required); fb++)
  {  
	
	int _dmadescriptor_count = 0; // for tracking
	
    for (int row = 0; row < chainRows; row++)
    {
	  //ESP_LOGV("I2S-DMA", ">>> Linking DMA descriptors for output row %d", row);    	
	  const int slotIdx = row % slotRows; // the ring reuses its slots, every row of a frame links to the slot it is generated into
		
	  // Link and send all colour data, all passes of everything in one hit. 1 bit colour at least...
	  for (int dma_desc_all = 0; dma_desc_all < dma_descs_per_row_all_cdepths; dma_desc_all++) 
	  {
			size_t payload_bytes = (dma_desc_all == (dma_descs_per_row_all_cdepths-1)) ? last_dma_desc_bytes_all_cdepths:DMA_MAX;
			
			// Log the current descriptor number and the payload size being used.
			//ESP_LOGV("I2S-DMA", "Processing dma_desc_all: %d, payload_bytes: %zu, memory location: %p", dma_desc_all, payload_bytes, (frame_buffer[fb].rowBits[slotIdx]->getDataPtr(0)+(dma_desc_all*(DMA_MAX/sizeof(ESP32_I2S_DMA_STORAGE_TYPE)))));
				
		    dma_bus.create_dma_desc_link(frame_buffer[fb].rowBits[slotIdx]->getDataPtr(0)+(dma_desc_all*(DMA_MAX/sizeof(ESP32_I2S_DMA_STORAGE_TYPE))), payload_bytes, (fb==1));
			_dmadescriptor_count++;
			
			// Log the updated descriptor count after each operation.
			//ESP_LOGV("I2S-DMA", "Updated _dmadescriptor_count: %d", _dmadescriptor_count);			
	  }
	
      // Step 2: Handle additional descriptors for bits beyond the lsbMsbTransitionBit 
      for (int i = lsbMsbTransitionBit + 1; i < m_cfg.getPixelColorDepthBits(); i++) 
	  {
		  // binary time division setup: we need 2 of bit (LSBMSB_TRANSITION_BIT + 1) four of (LSBMSB_TRANSITION_BIT + 2), etc
		  // because we sweep through to MSB each time, it divides the number of times we have to sweep in half (saving linked list RAM)
		  // we need 2^(i - LSBMSB_TRANSITION_BIT - 1) == 1 << (i - LSBMSB_TRANSITION_BIT - 1) passes from i to MSB

		  for (int k = 0; k < (1 << (i - lsbMsbTransitionBit - 1)); k++)
		  {		  
			  // Link and send all colour data, all passes of everything in one hit.
			  for (int dma_desc_1cdepth = 0; dma_desc_1cdepth < dma_descs_per_row_1cdepth; dma_desc_1cdepth++) 
			  {		  
				size_t payload_bytes = (dma_desc_1cdepth == (dma_descs_per_row_1cdepth-1)) ? last_dma_desc_bytes_1cdepth:DMA_MAX;
				
				// Log the current bit and the corresponding payload size.
				//ESP_LOGV("I2S-DMA", "Processing dma_desc_1cdepth: %d, payload_bytes: %zu, memory location: %p", dma_desc_1cdepth, payload_bytes, (frame_buffer[fb].rowBits[slotIdx]->getDataPtr(i)+(dma_desc_1cdepth*(DMA_MAX/sizeof(ESP32_I2S_DMA_STORAGE_TYPE)))));
		
				dma_bus.create_dma_desc_link(frame_buffer[fb].rowBits[slotIdx]->getDataPtr(i)+(dma_desc_1cdepth*(DMA_MAX/sizeof(ESP32_I2S_DMA_STORAGE_TYPE))), payload_bytes, (fb==1));
				_dmadescriptor_count++;
				
				// Log the updated descriptor count after each operation.
			//	ESP_LOGV("I2S-DMA", "Updated _dmadescriptor_count: %d", _dmadescriptor_count);		
			  }
		  } // end K
		
      } // end all other colour depth bits

#ifdef HUB75_RING_STREAM
      if (ring_on)
        dma_bus.mark_last_desc_row_end(); // the interrupt that refills the slot just sent
#endif
	  

    } // end all rows
	
    ESP_LOGI("I2S-DMA", "Created %d DMA descriptors for buffer %d.", _dmadescriptor_count, fb);	
	
  } // end framebuffer loop
	  
	/*
	#include <iostream>
	#include <cmath> // For pow function (if needed, though bit shifts are better)

	int main() {

		int colorDepthBits = 8;
		int lsbMsbTransitionBit = 0; // Assuming lsbMsbTransitionBit is 0

		// Step 2: Handle additional descriptors for bits beyond the lsbMsbTransitionBit
		for (int i = lsbMsbTransitionBit + 1; i < colorDepthBits; i++) {
			// Calculate the number of passes required
			int passes = 1 << (i - lsbMsbTransitionBit - 1);

			std::cout << "Bit Level: " << i << ", Passes Required: " << passes << std::endl;

			// Simulate the inner loop for the number of passes
			for (int k = 0; k < passes; k++) {
				std::cout << "  Pass " << k + 1 << " for bit " << i << std::endl;
			}
		}

		return 0;
	} 
	*/
  


  /***
   * Step 5:  Set default framebuffer to fb[0]
   */  

  fb = &frame_buffer[0];

#ifdef HUB75_RING_STREAM
  if (ring_on)
  {
    g_ring.descPerRow = dma_descriptors_per_row;
    for (int i = 0; i < slotRows; i++)
      g_ring.slot[i] = frame_buffer[0].rowBits[i]->data;
  }
#endif
  

  //
  //    Setup DMA and Output to GPIO
  //
  auto bus_cfg = dma_bus.config(); // バス設定用の構造体を取得します。

  bus_cfg.bus_freq    = m_cfg.i2sspeed;
  bus_cfg.pin_wr      = m_cfg.gpio.clk;
  bus_cfg.invert_pclk = m_cfg.clkphase;

  bus_cfg.pin_d0 = m_cfg.gpio.r1;
  bus_cfg.pin_d1 = m_cfg.gpio.g1;
  bus_cfg.pin_d2 = m_cfg.gpio.b1;
  bus_cfg.pin_d3 = m_cfg.gpio.r2;
  bus_cfg.pin_d4 = m_cfg.gpio.g2;
  bus_cfg.pin_d5 = m_cfg.gpio.b2;
  bus_cfg.pin_d6 = m_cfg.gpio.lat;
  bus_cfg.pin_d7 = m_cfg.gpio.oe;
  bus_cfg.pin_d8 = m_cfg.gpio.a;
  bus_cfg.pin_d9 = m_cfg.gpio.b;
  bus_cfg.pin_d10 = m_cfg.gpio.c;
  bus_cfg.pin_d11 = m_cfg.gpio.d;
  bus_cfg.pin_d12 = m_cfg.gpio.e;
  bus_cfg.pin_d13 = -1;
  bus_cfg.pin_d14 = -1;
  bus_cfg.pin_d15 = -1;

  dma_bus.config(bus_cfg);

  ESP_LOGI("I2S-DMA", "DMA setup completed");
  
  initialized = true;

  return true;

} // end setupDMA

/* There are 'bits' set in the frameStruct that we simply don't need to set every single time we change a pixel / DMA buffer co-ordinate.
 *  For example, the bits that determine the address lines, we don't need to set these every time. Once they're in place, and assuming we
 *  don't accidentally clear them, then we don't need to set them again.
 *  So to save processing, we strip this logic out to the absolute bare minimum, which is toggling only the R,G,B pixels (bits) per co-ord.
 *
 *  Critical dependency: That 'updateMatrixDMABuffer(uint8_t red, uint8_t green, uint8_t blue)' has been run at least once over the
 *                       entire frameBuffer to ensure all the non R,G,B bitmasks are in place (i.e. like OE, Address Lines etc.)
 *
 *  Note: If you change the brightness with setBrightness() you MUST then clearScreen() and repaint / flush the entire framebuffer.
 */

/** @brief - Update pixel at specific co-ordinate in the DMA buffer
 *  this is the main method used to update DMA buffer on pixel-by-pixel level so it must be fast, real fast!
 *  Let's put it into IRAM to avoid situations when it could be flushed out of instruction cache
 *  and had to be read from spi-flash over and over again.
 *  Yes, it is always a tradeoff between memory/speed/size, but compared to DMA-buffer size is not a big deal
 *
 *  Note: Cannot pass a negative co-ord as it makes no sense in the DMA bit array lookup.
 */
void IRAM_ATTR MatrixPanel_I2S_DMA::updateMatrixDMABuffer(uint16_t x_coord, uint16_t y_coord, uint8_t red, uint8_t green, uint8_t blue)
{
  if (!initialized)
    return;

  /* 1) Check that the co-ordinates are within range, or it'll break everything big time.
   * Valid co-ordinates are from 0 to (MATRIX_XXXX-1)
   */
  if (x_coord >= PIXELS_PER_ROW || y_coord >= m_cfg.mx_height)
  {
    return;
  }

  /* LED Brightness Compensation. Because if we do a basic "red & mask" for example,
   * we'll NEVER send the dimmest possible colour, due to binary skew.
   * i.e. It's almost impossible for colour_depth_idx of 0 to be sent out to the MATRIX unless the 'value' of a colour is exactly '1'
   * https://ledshield.wordpress.com/2012/11/13/led-brightness-to-your-eye-gamma-correction-no/
   */
  DO_BRIGHTNESS_COMPENSATION() 
  DO_DITHER(x_coord, y_coord)

#ifdef HUB75_RING_STREAM
  if (ring_on)
  {
    const uint32_t back = back_buffer_id; // volatile: read once
    ringAcquire(back);
    const bool bottom = y_coord >= ROWS_PER_FRAME;
    ringPut(g_ring.crow[m_cfg.double_buff ? back : 0][bottom ? y_coord - ROWS_PER_FRAME : y_coord] + x_coord * 6, bottom,
            red_val, green_val, blue_val);
    return;
  }
#endif

  /* When using the drawPixel, we are obviously only changing the value of one x,y position,
   * however, the two-scan panels paint TWO lines at the same time
   * and this reflects the parallel in-DMA-memory data structure of uint16_t's that are getting
   * pumped out at high speed.
   *
   * So we need to ensure we persist the bits (8 of them) of the uint16_t for the row we aren't changing.
   *
   * The DMA buffer order has also been reversed (refer to the last code in this function)
   * so we have to check for this and check the correct position of the MATRIX_DATA_STORAGE_TYPE
   * data.
   */
  x_coord = ESP32_TX_FIFO_POSITION_ADJUST(x_coord);

  uint16_t _colourbitclear = BITMASK_RGB1_CLEAR, _colourbitoffset = 0;

  if (y_coord >= ROWS_PER_FRAME)
  { // if we are drawing to the bottom part of the panel
    _colourbitoffset = BITS_RGB2_OFFSET;
    _colourbitclear = BITMASK_RGB2_CLEAR;
    y_coord -= ROWS_PER_FRAME;
  }

  // Iterating through colour depth bits, which we assume are 8 bits per RGB subpixel (24bpp)
  uint8_t colour_depth_idx = m_cfg.getPixelColorDepthBits();
  do
  {
    --colour_depth_idx;

    // Extract bit at current depth index
    uint16_t mask = (1 << colour_depth_idx);
    uint16_t RGB_output_bits = 0;

    /* Per the .h file, the order of the output RGB bits is:
     * BIT_B2, BIT_G2, BIT_R2,    BIT_B1, BIT_G1, BIT_R1     */
    RGB_output_bits |= (bool)(blue_val & mask); // --B
    RGB_output_bits <<= 1;
    RGB_output_bits |= (bool)(green_val & mask); // -BG
    RGB_output_bits <<= 1;
    RGB_output_bits |= (bool)(red_val & mask); // BGR
    RGB_output_bits <<= _colourbitoffset;    // shift colour bits to the required position

    // Get the contents at this address,
    // it would represent a vector pointing to the full row of pixels for the specified colour depth bit at Y coordinate
    ESP32_I2S_DMA_STORAGE_TYPE *p = getRowDataPtr(y_coord, colour_depth_idx);

    // We need to update the correct uint16_t word in the rowBitStruct array pointing to a specific pixel at X - coordinate
    p[x_coord] &= _colourbitclear; // reset RGB bits
    p[x_coord] |= RGB_output_bits; // set new RGB bits

#if defined(SPIRAM_DMA_BUFFER)
    Cache_WriteBack_Addr((uint32_t)&p[x_coord], sizeof(ESP32_I2S_DMA_STORAGE_TYPE));
#endif

  } while (colour_depth_idx); // end of colour depth loop (8)
} // updateMatrixDMABuffer (specific co-ords change)


/* Update the entire buffer with a single specific colour - quicker */
void MatrixPanel_I2S_DMA::updateMatrixDMABuffer(uint8_t red, uint8_t green, uint8_t blue)
{
  if (!initialized)
    return;

  if (ditherFractional(red, green, blue))  // a flat colour that needs the pattern: per pixel, black and exact levels stay on the fast path below
  {
    for (uint16_t y = 0; y < m_cfg.mx_height; y++)
      for (uint16_t x = 0; x < PIXELS_PER_ROW; x++)
        updateMatrixDMABuffer(x, y, red, green, blue);
    return;
  }

  /* https://ledshield.wordpress.com/2012/11/13/led-brightness-to-your-eye-gamma-correction-no/ */
  DO_BRIGHTNESS_COMPENSATION()  

#ifdef HUB75_RING_STREAM
  if (ring_on)
  {
    ringAcquire(back_buffer_id);
    uint8_t *const data = g_ring.crow[m_cfg.double_buff ? back_buffer_id : 0][0];
    if (!(red_val | green_val | blue_val))
    {
      memset(data, 0, ROWS_PER_FRAME * PIXELS_PER_ROW * 6);
      return;
    }
    ringPut(data, false, red_val, green_val, blue_val);
    ringPut(data, true, red_val, green_val, blue_val);
    const uint32_t lo = ringLoad24(data), hi = ringLoad24(data + 3);
    const uint32_t a = lo | (hi << 24), b = (hi >> 8) | (lo << 16), c = (lo >> 16) | (hi << 8);
    u32a *q = (u32a *)data;
    // Two six-byte columns fit three aligned stores; the complete frame allocation is contiguous.
    for (uint32_t pair = 0; pair < (uint32_t)ROWS_PER_FRAME * PIXELS_PER_ROW / 2; ++pair, q += 3)
    {
      q[0] = a; q[1] = b; q[2] = c;
    }
    return;
  }
#endif

  for (uint8_t colour_depth_idx = 0; colour_depth_idx < m_cfg.getPixelColorDepthBits(); colour_depth_idx++) // colour depth - 8 iterations
  {
    // let's precalculate RGB1 and RGB2 bits than flood it over the entire DMA buffer
    uint16_t RGB_output_bits = 0;

    // Extract bit at current depth index
    uint16_t mask = (1 << colour_depth_idx);

    /* Per the .h file, the order of the output RGB bits is:
     * BIT_B2, BIT_G2, BIT_R2,    BIT_B1, BIT_G1, BIT_R1      */
    RGB_output_bits |= (bool)(blue_val & mask); // --B
    RGB_output_bits <<= 1;
    RGB_output_bits |= (bool)(green_val & mask); // -BG
    RGB_output_bits <<= 1;
    RGB_output_bits |= (bool)(red_val & mask); // BGR

    // Duplicate and shift across so we have have 6 populated bits of RGB1 and RGB2 pin values suitable for DMA buffer
    RGB_output_bits |= RGB_output_bits << BITS_RGB2_OFFSET; // BGRBGR

    // Serial.printf("Fill with: 0x%#06x\n", RGB_output_bits);

    // iterate rows
    int matrix_frame_parallel_row = fb->rowBits.size();
    do
    {
      --matrix_frame_parallel_row;

      // The destination for the pixel row bitstream
      ESP32_I2S_DMA_STORAGE_TYPE *p = getRowDataPtr(matrix_frame_parallel_row, colour_depth_idx);

      // iterate pixels in a row
      int x_coord = fb->rowBits[matrix_frame_parallel_row]->width;
      do
      {
        --x_coord;
        p[x_coord] &= BITMASK_RGB12_CLEAR; // reset colour bits
        p[x_coord] |= RGB_output_bits;     // set new colour bits

#if defined(SPIRAM_DMA_BUFFER)
        Cache_WriteBack_Addr((uint32_t)&p[x_coord], sizeof(ESP32_I2S_DMA_STORAGE_TYPE));
#endif

      } while (x_coord);

    } while (matrix_frame_parallel_row); // end row iteration
  }                                      // colour depth loop (8)
} // updateMatrixDMABuffer (full frame paint)

/**
 * @brief - clears and reinitializes colour/control data in DMA buffs
 * When allocated, DMA buffs might be dirty, so we need to blank it and initialize ABCDE,LAT,OE control bits.
 * Those control bits are constants during the entire DMA sweep and never changed when updating just pixel colour data
 * so we could set it once on DMA buffs initialization and forget.
 * This effectively clears buffers to blank BLACK and makes it ready to display output.
 * (Brightness control via OE bit manipulation is another case) - this must be done as well seperately!
 */
void MatrixPanel_I2S_DMA::clearFrameBuffer(bool _buff_id)
{
  if (!initialized)
    return;

#ifdef HUB75_RING_STREAM
  if (ring_on)
  {
    ringAcquire(back_buffer_id);
    for (int y = 0; y < ROWS_PER_FRAME; y++)
      memset(g_ring.crow[_buff_id][y], 0, PIXELS_PER_ROW * 6);
    return;
  }
#endif

  frameStruct *fb = &frame_buffer[_buff_id];

  // we start with iterating all rows in dma_buff structure
  int row_idx = fb->rowBits.size();
  do
  {
    --row_idx;

    ESP32_I2S_DMA_STORAGE_TYPE *row = fb->rowBits[row_idx]->getDataPtr(0); // set pointer to the HEAD of a buffer holding data for the entire matrix row
    ESP32_I2S_DMA_STORAGE_TYPE abcde = (ESP32_I2S_DMA_STORAGE_TYPE)row_idx;

    // get last pixel index in a row of all colourdepths
    int x_pixel = fb->rowBits[row_idx]->width * fb->rowBits[row_idx]->colour_depth;

	abcde <<= BITS_ADDR_OFFSET; // shift row y-coord to match ABCDE bits in vector from 8 to 12
	do
	{
		--x_pixel;
		if (m_cfg.line_decoder == HUB75_I2S_CFG::SM5266P)
		{
			// modifications here for row shift register type SM5266P
			// https://github.com/mrfaptastic/ESP32-HUB75-MatrixPanel-I2S-DMA/issues/164
			row[x_pixel] = abcde & (0x18 << BITS_ADDR_OFFSET); // mask out the bottom 3 bits which are the clk di bk inputs
		}
		else if (m_cfg.line_decoder  == HUB75_I2S_CFG::SM5368) 
		{
			row[ESP32_TX_FIFO_POSITION_ADJUST(x_pixel)] = 0x0000;
		}
		else
		{
			row[ESP32_TX_FIFO_POSITION_ADJUST(x_pixel)] = abcde;
		}

	} while (x_pixel != fb->rowBits[row_idx]->width); // spare the first "width's" worth of pixels as they are the LSB pixels/colordepth

	// The colour_index[0] (LSB) x_pixels must be "marked" with a previous's row address, because it is used to display
	// previous row while we pump in MSBs's for the next row.
	if (row_idx == 0) { 
		abcde = ROWS_PER_FRAME-1; // wrap around
	} else {
		abcde = row_idx-1;	
	}

    abcde <<= BITS_ADDR_OFFSET; // shift row y-coord to match ABCDE bits in vector from 8 to 12		
	do
    {
      --x_pixel;

      if (m_cfg.line_decoder == HUB75_I2S_CFG::SM5266P)
      {
        // modifications here for row shift register type SM5266P
        // https://github.com/mrfaptastic/ESP32-HUB75-MatrixPanel-I2S-DMA/issues/164
        row[x_pixel] = abcde & (0x18 << BITS_ADDR_OFFSET); // mask out the bottom 3 bits which are the clk di bk inputs
      }
      else if (m_cfg.line_decoder  == HUB75_I2S_CFG::SM5368) 
      {
        row[ESP32_TX_FIFO_POSITION_ADJUST(x_pixel)] = 0x0000;
      }
      else
      {
        row[ESP32_TX_FIFO_POSITION_ADJUST(x_pixel)] = abcde;
      }

    } while (x_pixel);

    // modifications here for row shift register type SM5266P
    // https://github.com/mrfaptastic/ESP32-HUB75-MatrixPanel-I2S-DMA/issues/164
    if (m_cfg.line_decoder == HUB75_I2S_CFG::SM5266P)
    {
      uint16_t serialCount;
      uint16_t latch;
      x_pixel = fb->rowBits[row_idx]->width - 16; // come back 8*2 pixels to allow for 8 writes
      serialCount = 8;
      do
      {
        serialCount--;
        latch = row[x_pixel] | (((((ESP32_I2S_DMA_STORAGE_TYPE)row_idx) % 8) == serialCount) << 1) << BITS_ADDR_OFFSET; // data on 'B'
        row[x_pixel++] = latch | (0x05 << BITS_ADDR_OFFSET);                                                            // clock high on 'A'and BK high for update
        row[x_pixel++] = latch | (0x04 << BITS_ADDR_OFFSET);                                                            // clock low on 'A'and BK high for update
      } while (serialCount);
    } // end SM5266P

    // row selection for SM5368 shift regs with ABC-only addressing. A is row clk, B is BK and C is row data
    if (m_cfg.line_decoder == HUB75_I2S_CFG::SM5368) 
    {
      x_pixel = fb->rowBits[row_idx]->width - 1;                                                                        // last pixel in first block)
      uint16_t c = (row_idx == 0) ? BIT_C : 0x0000;                                                                     // set row data (C) when row==0, then push through shift regs for all other rows
      row[ESP32_TX_FIFO_POSITION_ADJUST(x_pixel - 1)] |= c | BIT_B;                                                            // set row data
      row[ESP32_TX_FIFO_POSITION_ADJUST(x_pixel + 0)] |= c | BIT_A | BIT_B;                                             // set row clk and bk, carry row data
    } // end DP3246_SM5368

    // let's set LAT/OE control bits for specific pixels in each colour_index subrows
    // Need to consider the original ESP32's (WROOM) DMA TX FIFO reordering of bytes...
    uint8_t colouridx = fb->rowBits[row_idx]->colour_depth;
    do
    {
      --colouridx;

      // switch pointer to a row for a specific colour index
      row = fb->rowBits[row_idx]->getDataPtr(colouridx);

      // DP3246 needs the latch high for 3 clock cycles, so start 2 cycles earlier
      if (m_cfg.driver == HUB75_I2S_CFG::DP3246) 
      {
        row[ESP32_TX_FIFO_POSITION_ADJUST(fb->rowBits[row_idx]->width - 3)] |= BIT_LAT;   // DP3246 needs 3 clock cycle latch 
        row[ESP32_TX_FIFO_POSITION_ADJUST(fb->rowBits[row_idx]->width - 2)] |= BIT_LAT;   // DP3246 needs 3 clock cycle latch 
      } // DP3246_SM5368
      
      row[ESP32_TX_FIFO_POSITION_ADJUST(fb->rowBits[row_idx]->width - 1)] |= BIT_LAT; // -1 pixel to compensate array index starting at 0

      // ESP32_TX_FIFO_POSITION_ADJUST(dma_buff.rowBits[row_idx]->width - 1)

      // need to disable OE before/after latch to hide row transition
      // Should be one clock or more before latch, otherwise can get ghosting
      uint8_t _blank = m_cfg.latch_blanking;
      do
      {
        --_blank;

        row[ESP32_TX_FIFO_POSITION_ADJUST(0 + _blank)] |= BIT_OE;                               // disable output
        row[ESP32_TX_FIFO_POSITION_ADJUST(fb->rowBits[row_idx]->width - 1)] |= BIT_OE;          // disable output
        row[ESP32_TX_FIFO_POSITION_ADJUST(fb->rowBits[row_idx]->width - _blank - 1)] |= BIT_OE; // (LAT pulse is (width-2) -1 pixel to compensate array index starting at 0

      } while (_blank);

    } while (colouridx);

#if defined(SPIRAM_DMA_BUFFER)
    Cache_WriteBack_Addr((uint32_t)row, fb->rowBits[row_idx]->getColorDepthSize(false));
#endif

  } while (row_idx);
}

/* The output enable window of a plane is a whole number of clocks wide. At low brightness the narrowest planes get 1 or 2 clocks,
 * their weights drift from binary and colours tint and band. So the window stays at oeFloor() (the narrowest plane keeps kMinWindow
 * clocks) and the rest of the dimming is applied to the pixel values (g_gainLut), down to 1/8; only below that does it narrow. */
uint8_t MatrixPanel_I2S_DMA::oeFloor() const
{
  const int depth = m_cfg.getPixelColorDepthBits();
  const int bitshift = (depth - lsbMsbTransitionBit - 1) >> 1;
  const int narrowest = std::max(1, (int)((PIXELS_PER_ROW - m_cfg.latch_blanking) >> std::max(depth - 1 - bitshift - 2, 0)));
  constexpr int kMinWindow = 8;
  return (uint8_t)std::min(255, (kMinWindow * 256 + narrowest - 1) / narrowest);
}

uint8_t MatrixPanel_I2S_DMA::applyGain(uint8_t brt)
{
  const uint8_t floor_ = oeFloor();
  if (brt == 0 || brt >= floor_) {
    g_gainLut = nullptr;
#ifdef HUB75_DITHER
    g_dither = false;
#endif
    return brt;
  }
  unsigned gain = brt * 256u / floor_;  // 8.8
  // The gain stops at 1/16: below that the dimmest content (the night clock) would round to nothing, so the window narrows again,
  // but never below half the floor (4 clocks in the narrowest plane): at 1 or 2 clocks the plane weights drift and white turns yellow.
  // The lowest brightness settings therefore all give the same dimming, except for the red night clock (m_deepDim): it needs the dimmest output.
  constexpr unsigned kMinGain = 16;  // 1/16: the window narrows later, short pulses lose red-vs-green/blue light unevenly and greys turn pink around 6 %
  uint8_t window = floor_;
  if (gain < kMinGain) { gain = kMinGain; window = (uint8_t)(brt * 256u / kMinGain); if (!m_deepDim) window = std::max<uint8_t>(window, floor_ / 2u); }
  const int depth = m_cfg.getPixelColorDepthBits();
  const int shift = 12 - depth + 8;
  const unsigned max_val = (1u << depth) - 1;
  for (int v = 0; v < 256; v++) {
    unsigned out = (lumConvTab_12bit[v] * gain + (1u << (shift - 1))) >> shift;
    // a value may round to 0 here: a pixel that rounds to black in all channels is lifted by keepHue where it is drawn
    g_gainTable[v] = out > max_val ? max_val : out;
  }
  bcmMonotonic(g_gainTable, depth, lsbMsbTransitionBit);
  g_gainLut = g_gainTable;
#ifdef HUB75_DITHER
  if (m_dither && depth <= 8 && !g_dt) g_dt = (DitherTables *)heap_caps_malloc(sizeof(DitherTables), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  g_dither = m_dither && depth <= 8 && g_dt;
  if (g_dither) {
    for (int v = 0; v < 256; v++) {
      const unsigned f = (lumConvTab_12bit[v] * gain) >> (12 - depth);  // 8.8 fixed point of the output level
      g_dt->frac[v] = f > 0xFFFF ? 0xFFFF : f;
    }
    g_ditherMax = max_val;
    if (g_ditherMapKey != depth * 16 + lsbMsbTransitionBit) {
      for (unsigned i = 0; i < 256; i++) g_dt->map[i] = i > max_val ? max_val : i;
      bcmMonotonic(g_dt->map, depth, lsbMsbTransitionBit);
      g_ditherMapKey = depth * 16 + lsbMsbTransitionBit;
    }
  }
#endif
  return window;
}

void MatrixPanel_I2S_DMA::setBrightnessOE(uint8_t brt, const int _buff_id)
{

  if (!initialized)
    return;

  brt = applyGain(brt);

#ifdef HUB75_RING_STREAM
  if (ring_on)
  {
    if (_buff_id == 0)
      ringBuildTemplate(brt); // one template serves both framebuffers
    return;
  }
#endif

  frameStruct *fb = &frame_buffer[_buff_id];

  uint8_t _blank = m_cfg.latch_blanking; // don't want to inadvertantly blast over this
  uint8_t _depth = fb->rowBits[0]->colour_depth;
  uint16_t _width = fb->rowBits[0]->width;

  // start with iterating all rows in dma_buff structure
  int row_idx = fb->rowBits.size();
  do
  {
    --row_idx;

    // let's set OE control bits for specific pixels in each color_index subrows
    uint8_t colouridx = _depth;
    do
    {
      --colouridx;

      char bitplane = (2 * _depth - colouridx) % _depth;
      char bitshift = (_depth - lsbMsbTransitionBit - 1) >> 1;

      char rightshift = std::max(bitplane - bitshift - 2, 0);

      // Calculate the OE disable period by brightness and latch blanking.
      // First, determine the maximum pixels for this specific bitplane (accounting for PWM time weighting).
      // Then scale that maximum by brightness (0-255).
      // This ensures all bitplanes scale proportionally and reach their maximums simultaneously.
      int max_pixels_for_bitplane = (_width - _blank) >> rightshift;
      int brightness_in_x_pixels = (max_pixels_for_bitplane * brt + 128) >> 8;

      // Ensure at least 1 pixel is enabled for any brightness > 0
      if (brt > 0 && brightness_in_x_pixels == 0) {
        brightness_in_x_pixels = 1;
      }

      // Safety margin: Ensure we never exceed max_pixels - 1 to maintain blanking headroom.
      // At extreme brightness (252-255), we need at least (_blank + 1) total blanking pixels
      // to prevent ghosting and artifacts, especially with high pixel density (many white pixels).
      if (brightness_in_x_pixels > max_pixels_for_bitplane - 1) {
        brightness_in_x_pixels = max_pixels_for_bitplane - 1;
      }

      // switch pointer to a row for a specific color index
      ESP32_I2S_DMA_STORAGE_TYPE *row = fb->rowBits[row_idx]->getDataPtr(colouridx);

      // define range of Output Enable on the center of the row
      int x_coord_max = (_width + brightness_in_x_pixels + 1) >> 1;
      int x_coord_min = (_width - brightness_in_x_pixels + 0) >> 1;
      int x_coord = _width;
      do
      {
        --x_coord;

        // (the check is already including "blanking" )
        if (x_coord >= x_coord_min && x_coord < x_coord_max)
        {
          row[ESP32_TX_FIFO_POSITION_ADJUST(x_coord)] &= BITMASK_OE_CLEAR;
        }
        else
        {
          row[ESP32_TX_FIFO_POSITION_ADJUST(x_coord)] |= BIT_OE; // Disable output after this point.
        }

      } while (x_coord);

    } while (colouridx);

#if defined(SPIRAM_DMA_BUFFER)
	// Force the flush and update of the PSRAM for the memory address range of the 'row data' as
	// data changes probably aren't being sent out via DMA as they're sitting in a hadrware 'cache' 
    ESP32_I2S_DMA_STORAGE_TYPE *row_ptr = fb->rowBits[row_idx]->getDataPtr(0);
    Cache_WriteBack_Addr((uint32_t)row_ptr, fb->rowBits[row_idx]->getColorDepthSize(false));
#endif
  } while (row_idx);
}


#ifdef HUB75_RING_STREAM
bool MatrixPanel_I2S_DMA::ringEligible() const
{
  const uint8_t n = m_cfg.ring_rows;
  return n >= 3 && n < ROWS_PER_FRAME && (ROWS_PER_FRAME % n) == 0 && ROWS_PER_FRAME <= kRingMaxRows &&
         m_cfg.mx_height <= kRingMaxHeight && m_cfg.getPixelColorDepthBits() <= 8 && (PIXELS_PER_ROW & 1) == 0 &&
         m_cfg.driver != HUB75_I2S_CFG::DP3246 &&
         (m_cfg.line_decoder == HUB75_I2S_CFG::TYPE138 || m_cfg.line_decoder == HUB75_I2S_CFG::TYPE_DIRECT);
}

bool MatrixPanel_I2S_DMA::ringAllocate()
{
  RingState &R = g_ring;
  memset(&R, 0, sizeof(R));
  R.width = PIXELS_PER_ROW;
  R.depth = m_cfg.getPixelColorDepthBits();
  R.rows = ROWS_PER_FRAME;
  R.n = m_cfg.ring_rows;

  const int bufs = m_cfg.double_buff ? 2 : 1;
  bool ok = true;
  for (int b = 0; b < bufs && ok; b++)
  {
    // Allocate each frame once; retain row pointers so drawing/refill needs no extra address calculation.
    uint8_t *const data = (uint8_t *)heap_caps_calloc(ROWS_PER_FRAME, PIXELS_PER_ROW * 6,
                                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!data) { ok = false; break; }
    for (int y = 0; y < ROWS_PER_FRAME; y++)
      R.crow[b][y] = data + y * PIXELS_PER_ROW * 6;
  }
  if (ok)
  {
    ring_tmpl = (uint16_t *)heap_caps_malloc(R.depth * R.width * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ok = ring_tmpl != nullptr;
  }
  if (!ok)
  {
    for (int b = 0; b < 2; b++)
    {
      heap_caps_free(R.crow[b][0]);
      memset(R.crow[b], 0, sizeof(R.crow[b]));
    }
    heap_caps_free(ring_tmpl);
    ring_tmpl = nullptr;
    ring_on = false;
    return false;
  }
  R.tmpl = ring_tmpl;
  return true;
}

/* The LAT pulse and the OE window of each plane, as clearFrameBuffer() and setBrightnessOE() lay them down in a
 * classic row. They are the same for every row: only the address bits (added when a row is generated) differ. */
void MatrixPanel_I2S_DMA::ringBuildTemplate(uint8_t brt)
{
  const uint32_t _depth = m_cfg.getPixelColorDepthBits();
  const uint32_t _width = PIXELS_PER_ROW;
  const uint8_t _blank = m_cfg.latch_blanking;

  for (uint32_t colouridx = 0; colouridx < _depth; colouridx++)
  {
    char bitplane = (2 * _depth - colouridx) % _depth;
    char bitshift = (_depth - lsbMsbTransitionBit - 1) >> 1;
    char rightshift = std::max(bitplane - bitshift - 2, 0);

    int max_pixels_for_bitplane = (_width - _blank) >> rightshift;
    int brightness_in_x_pixels = (max_pixels_for_bitplane * brt + 128) >> 8;
    if (brt > 0 && brightness_in_x_pixels == 0)
      brightness_in_x_pixels = 1;
    if (brightness_in_x_pixels > max_pixels_for_bitplane - 1)
      brightness_in_x_pixels = max_pixels_for_bitplane - 1;

    int x_coord_max = (_width + brightness_in_x_pixels + 1) >> 1;
    int x_coord_min = (_width - brightness_in_x_pixels + 0) >> 1;

    uint16_t *row = ring_tmpl + colouridx * _width;
    for (uint32_t x = 0; x < _width; x++)
    {
      uint16_t v = (x >= (uint32_t)x_coord_min && x < (uint32_t)x_coord_max) ? 0 : BIT_OE;
      if (x == _width - 1)
        v |= BIT_LAT;
      row[ESP32_TX_FIFO_POSITION_ADJUST(x)] = v;
    }
  }
}

void MatrixPanel_I2S_DMA::ringPrefill()
{
  RingState &R = g_ring;
  R.nextFill = 0;
  for (uint32_t r = 0; r < R.n; r++)
    ringFillRow(R, r);
  R.nextFill = R.n;
  R.curRow = R.rows - 1; // the first row interrupt reports row 0
  // Default: the lower half switches one sweep after the upper half (no seam), and drawing waits for the buffer to be free.
  R.lag = (R.depth >= 7);
  R.own = R.lag;
}

bool MatrixPanel_I2S_DMA::ringStart()
{
  return dma_bus.start_row_interrupt(ringEof, nullptr);
}

bool MatrixPanel_I2S_DMA::nextLateEvent(uint32_t &seen, uint32_t &gapUs, uint32_t &pc, char *task, uint32_t *ms) const
{
  const RingState &R = g_ring;
  if (!ring_on || seen >= R.lateSeq)
    return false;
  if (R.lateSeq - seen > 8)
    seen = R.lateSeq - 8; // older events were overwritten
  const RingState::LateEvent &e = R.lateLog[seen & 7];
  gapUs = e.gapUs;
  pc = e.pc;
  if (ms)
    *ms = e.ms;
  memcpy(task, e.task, 9);
  ++seen;
  return true;
}

void MatrixPanel_I2S_DMA::setRingMode(bool own, bool lag)
{
  if (!ring_on || g_ring.depth < 7)
    return;
  g_ring.lag = lag;
  g_ring.own = own || lag; // a lagging lower half keeps the old buffer in use for a sweep longer
}

void MatrixPanel_I2S_DMA::ringFlip()
{
  g_ring.frontNext = back_buffer_id;
  g_ring.flips = g_ring.flips + 1;
  g_ring.pending = g_ring.own;
  back_buffer_id = back_buffer_id ^ 1;
}

MatrixPanel_I2S_DMA::RingStats MatrixPanel_I2S_DMA::getRingStats() const
{
  RingStats s = {};
  s.active = ring_on;
  if (ring_on)
  {
    s.rows = g_ring.filled;
    s.flips = g_ring.flips;
    s.gapMaxUs = g_ring.gapMax / ESP_CPU_FREQ_MHZ_FOR_DIAG;
    for (int i = 0; i < 4; i++)
      s.late[i] = g_ring.late[i];
    s.waits = g_ring.waits;
    s.waitUs = g_ring.waitUs;
    s.waitMaxUs = g_ring.waitMaxUs;
    s.missed = g_ring.missed;
    s.maxCycles = g_ring.maxCycles;
    s.meanCycles = g_ring.calls ? (uint32_t)(g_ring.sumCycles / g_ring.calls) : 0;
    s.rowPeriodUs = calculated_refresh_rate > 0 ? 1000000u / ((uint32_t)calculated_refresh_rate * g_ring.rows) : 0;
  }
  return s;
}
#endif // HUB75_RING_STREAM

/*
 *  overload for compatibility
 */

bool MatrixPanel_I2S_DMA::begin(int r1, int g1, int b1, int r2, int g2, int b2, int a, int b, int c, int d, int e, int lat, int oe, int clk)
{
  if (initialized)
    return true;
  // RGB
  m_cfg.gpio.r1 = r1;
  m_cfg.gpio.g1 = g1;
  m_cfg.gpio.b1 = b1;
  m_cfg.gpio.r2 = r2;
  m_cfg.gpio.g2 = g2;
  m_cfg.gpio.b2 = b2;

  // Line Select
  m_cfg.gpio.a = a;
  m_cfg.gpio.b = b;
  m_cfg.gpio.c = c;
  m_cfg.gpio.d = d;
  m_cfg.gpio.e = e;

  // Clock & Control
  m_cfg.gpio.lat = lat;
  m_cfg.gpio.oe = oe;
  m_cfg.gpio.clk = clk;

  return begin();
}

bool MatrixPanel_I2S_DMA::begin(const HUB75_I2S_CFG &cfg)
{
  if (initialized)
    return true;

  if (!setCfg(cfg))
    return false;

  return begin();
}

/**
 * @brief - Sets how many clock cycles to blank OE before/after LAT signal change
 * @param uint8_t pulses - clocks before/after OE
 * default is DEFAULT_LAT_BLANKING
 * Max is MAX_LAT_BLANKING
 * @returns - new value for m_cfg.latch_blanking
 */
uint8_t MatrixPanel_I2S_DMA::setLatBlanking(uint8_t pulses)
{
  if (pulses > MAX_LAT_BLANKING)
    pulses = MAX_LAT_BLANKING;

  if (!pulses)
    pulses = DEFAULT_LAT_BLANKING;

  m_cfg.latch_blanking = pulses;

  // remove brightness var for now.
  // setPanelBrightness(brightness);    // set brightness to reset OE bits to the values matching new LAT blanking setting
  return m_cfg.latch_blanking;
}

#ifndef NO_FAST_FUNCTIONS
/**
 * @brief - update DMA buff drawing horizontal line at specified coordinates
 * @param x_coord - line start coordinate x
 * @param y_coord - line start coordinate y
 * @param l - line length
 * @param r,g,b, - RGB888 colour
 */
void MatrixPanel_I2S_DMA::hlineDMA(int16_t x_coord, int16_t y_coord, int16_t l, uint8_t red, uint8_t green, uint8_t blue)
{
  if (!initialized)
    return;

  if ((x_coord + l) < 1 || y_coord < 0 || l < 1 || x_coord >= PIXELS_PER_ROW || y_coord >= m_cfg.mx_height)
    return;

  l = x_coord < 0 ? l + x_coord : l;
  x_coord = x_coord < 0 ? 0 : x_coord;

  l = ((x_coord + l) >= PIXELS_PER_ROW) ? (PIXELS_PER_ROW - x_coord) : l;

  // if (x_coord+l > PIXELS_PER_ROW)
  //    l = PIXELS_PER_ROW - x_coord + 1;     // reset width to end of row

  if (ditherFractional(red, green, blue))
  {
    for (int16_t i = 0; i < l; i++)
      updateMatrixDMABuffer(x_coord + i, y_coord, red, green, blue);
    return;
  }

  /* LED Brightness Compensation */
DO_BRIGHTNESS_COMPENSATION() 

#ifdef HUB75_RING_STREAM
  if (ring_on)
  {
    ringAcquire(back_buffer_id);
    const bool bottom = y_coord >= ROWS_PER_FRAME;
    uint8_t *q = g_ring.crow[m_cfg.double_buff ? back_buffer_id : 0][bottom ? y_coord - ROWS_PER_FRAME : y_coord] + x_coord * 6;
    for (int16_t i = 0; i < l; i++, q += 6)
      ringPut(q, bottom, red_val, green_val, blue_val);
    return;
  }
#endif

  uint16_t _colourbitclear = BITMASK_RGB1_CLEAR, _colourbitoffset = 0;

  if (y_coord >= ROWS_PER_FRAME)
  { // if we are drawing to the bottom part of the panel
    _colourbitoffset = BITS_RGB2_OFFSET;
    _colourbitclear = BITMASK_RGB2_CLEAR;
    y_coord -= ROWS_PER_FRAME;
  }

  // Iterating through colour depth bits (8 iterations)
  uint8_t colour_depth_idx = m_cfg.getPixelColorDepthBits();
  do
  {
    --colour_depth_idx;

    // let's precalculate RGB1 and RGB2 bits than flood it over the entire DMA buffer
    uint16_t RGB_output_bits = 0;

    // Extract bit at current depth index
    uint16_t mask = (1 << colour_depth_idx);

    /* Per the .h file, the order of the output RGB bits is:
     * BIT_B2, BIT_G2, BIT_R2,    BIT_B1, BIT_G1, BIT_R1     */
    RGB_output_bits |= (bool)(blue_val & mask); // --B
    RGB_output_bits <<= 1;
    RGB_output_bits |= (bool)(green_val & mask); // -BG
    RGB_output_bits <<= 1;
    RGB_output_bits |= (bool)(red_val & mask); // BGR
    RGB_output_bits <<= _colourbitoffset;    // shift color bits to the required position

    // Get the contents at this address,
    // it would represent a vector pointing to the full row of pixels for the specified colour depth bit at Y coordinate
    ESP32_I2S_DMA_STORAGE_TYPE *p = fb->rowBits[y_coord]->getDataPtr(colour_depth_idx);
    // inlined version works slower here, dunno why :(
    // ESP32_I2S_DMA_STORAGE_TYPE *p = getRowDataPtr(y_coord, colour_depth_idx, back_buffer_id);

    int16_t _l = l;
    do
    { // iterate pixels in a row
      int16_t _x = x_coord + --_l;

      /*
        #if defined(ESP32_THE_ORIG)
                // Save the calculated value to the bitplane memory in reverse order to account for I2S Tx FIFO mode1 ordering
                uint16_t &v = p[_x & 1U ? --_x : ++_x];
        #else
                // ESP 32 doesn't need byte flipping for TX FIFO.
                uint16_t &v = p[_x];
        #endif
      */
      uint16_t &v = p[ESP32_TX_FIFO_POSITION_ADJUST(_x)];

      v &= _colourbitclear;   // reset colour bits
      v |= RGB_output_bits;   // set new colour bits
    } while (_l);             // iterate pixels in a row
  } while (colour_depth_idx); // end of colour depth loop (8)
} // hlineDMA()

/**
 * @brief - update DMA buff drawing vertical line at specified coordinates
 * @param x_coord - line start coordinate x
 * @param y_coord - line start coordinate y
 * @param l - line length
 * @param r,g,b, - RGB888 colour
 */
void MatrixPanel_I2S_DMA::vlineDMA(int16_t x_coord, int16_t y_coord, int16_t l, uint8_t red, uint8_t green, uint8_t blue)
{
  if (!initialized)
    return;

  if (x_coord < 0 || (y_coord + l) < 1 || l < 1 || x_coord >= PIXELS_PER_ROW || y_coord >= m_cfg.mx_height)
    return;

  l = y_coord < 0 ? l + y_coord : l;
  y_coord = y_coord < 0 ? 0 : y_coord;

  // check for a length that goes beyond the height of the screen! Array out of bounds dma memory changes = screwed output #163
  l = ((y_coord + l) >= m_cfg.mx_height) ? (m_cfg.mx_height - y_coord) : l;
  // if (y_coord + l > m_cfg.mx_height)
  ///    l = m_cfg.mx_height - y_coord + 1;     // reset width to end of col

  if (ditherFractional(red, green, blue))
  {
    for (int16_t i = 0; i < l; i++)
      updateMatrixDMABuffer(x_coord, y_coord + i, red, green, blue);
    return;
  }

  DO_BRIGHTNESS_COMPENSATION() 

#ifdef HUB75_RING_STREAM
  if (ring_on)
  {
    ringAcquire(back_buffer_id);
    uint8_t **rows = g_ring.crow[m_cfg.double_buff ? back_buffer_id : 0];
    for (int16_t i = 0; i < l; i++)
    {
      const int y = y_coord + i;
      const bool bottom = y >= ROWS_PER_FRAME;
      ringPut(rows[bottom ? y - ROWS_PER_FRAME : y] + x_coord * 6, bottom, red_val, green_val, blue_val);
    }
    return;
  }
#endif

  /*
  #if defined(ESP32_THE_ORIG)
    // Save the calculated value to the bitplane memory in reverse order to account for I2S Tx FIFO mode1 ordering
    x_coord & 1U ? --x_coord : ++x_coord;
  #endif
  */
  x_coord = ESP32_TX_FIFO_POSITION_ADJUST(x_coord);

  uint8_t colour_depth_idx = m_cfg.getPixelColorDepthBits();
  do
  { // Iterating through colour depth bits (8 iterations)
    --colour_depth_idx;

    // Extract bit at current depth index
    uint16_t mask = (1 << colour_depth_idx);
    uint16_t RGB_output_bits = 0;

    /* Per the .h file, the order of the output RGB bits is:
     * BIT_B2, BIT_G2, BIT_R2,    BIT_B1, BIT_G1, BIT_R1   */
    RGB_output_bits |= (bool)(blue_val & mask); // --B
    RGB_output_bits <<= 1;
    RGB_output_bits |= (bool)(green_val & mask); // -BG
    RGB_output_bits <<= 1;
    RGB_output_bits |= (bool)(red_val & mask); // BGR

    int16_t _l = 0, _y = y_coord;
    uint16_t _colourbitclear = BITMASK_RGB1_CLEAR;
    do
    { // iterate pixels in a column

      if (_y >= ROWS_PER_FRAME)
      { // if y-coord overlapped bottom-half panel
        _y -= ROWS_PER_FRAME;
        _colourbitclear = BITMASK_RGB2_CLEAR;
        RGB_output_bits <<= BITS_RGB2_OFFSET;
      }

      // Get the contents at this address,
      // it would represent a vector pointing to the full row of pixels for the specified colour depth bit at Y coordinate
      // ESP32_I2S_DMA_STORAGE_TYPE *p = getRowDataPtr(_y, colour_depth_idx, back_buffer_id);
      ESP32_I2S_DMA_STORAGE_TYPE *p = fb->rowBits[_y]->getDataPtr(colour_depth_idx);

      p[x_coord] &= _colourbitclear; // reset RGB bits
      p[x_coord] |= RGB_output_bits; // set new RGB bits
      ++_y;
    } while (++_l != l);      // iterate pixels in a col
  } while (colour_depth_idx); // end of colour depth loop (8)
} // vlineDMA()

/**
 * @brief - update DMA buff drawing a rectangular at specified coordinates
 * this works much faster than multiple consecutive per-pixel calls to updateMatrixDMABuffer()
 * @param int16_t x, int16_t y - coordinates of a top-left corner
 * @param int16_t w, int16_t h - width and height of a rectangular, min is 1 px
 * @param uint8_t r - RGB888 colour
 * @param uint8_t g - RGB888 colour
 * @param uint8_t b - RGB888 colour
 */
void MatrixPanel_I2S_DMA::fillRectDMA(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t r, uint8_t g, uint8_t b)
{

  // h-lines are >2 times faster than v-lines
  // so will use it only for tall rects with h >2w
  if (h > 2 * w)
  {
    // draw using v-lines
    do
    {
      --w;
      vlineDMA(x + w, y, h, r, g, b);
    } while (w);
  }
  else
  {
    // draw using h-lines
    do
    {
      --h;
      hlineDMA(x, y + h, w, r, g, b);
    } while (h);
  }
}

#endif // NO_FAST_FUNCTIONS

// Readback uses the existing framebuffer, not an extra RGB shadow buffer.
bool MatrixPanel_I2S_DMA::readDisplayRow(int16_t y, uint8_t *rgb)
{
  if (!initialized || !rgb || y < 0 || y >= height()) return false;
  const int front = m_cfg.double_buff ? (back_buffer_id ^ 1) : 0;
  const uint8_t depth = m_cfg.getPixelColorDepthBits();
  if (depth > 8) return false;
#if !defined(NO_CIE1931) && defined(LUT_NATIVE_BIT_DEPTH)
  // The smallest input that shows at least this output value, for every output value: built once per row instead of a
  // binary search for each of the three channels of each pixel
  uint8_t inverse[256];
  {
    const uint8_t *lut = g_gainLut ? g_gainLut : baseLut(depth);
    unsigned input = 0;
    for (unsigned value = 0; value < 256; ++value)
    {
      while (input < 255 && lut[input] < value) ++input;
      inverse[value] = (uint8_t)input;
    }
  }
  auto channel = [&](uint8_t value) -> uint8_t { return value ? inverse[value] : 0; };
#else
  auto channel = [&](uint8_t value) -> uint8_t {
    if (!value) return 0;
    return (uint16_t)value * 255 / ((1u << depth) - 1);
  };
#endif
  for (int16_t x = 0; x < width(); ++x) {
    int16_t xx = x, yy = y, w = 1, h = 1;
    transform(xx, yy, w, h);
    const bool bottom = yy >= ROWS_PER_FRAME;
    if (bottom) yy -= ROWS_PER_FRAME;
    uint8_t r = 0, g = 0, b = 0;
#ifdef HUB75_RING_STREAM
    if (ring_on) {
      const uint8_t *q = g_ring.crow[front][yy] + xx * 6;
      const uint32_t lo = ringLoad24(q), hi = ringLoad24(q + 3);
      const unsigned half = bottom ? 3 : 0;
      for (unsigned bit = 0; bit < depth; ++bit) {
        const uint32_t lane = ((bit < 4 ? lo : hi) >> ((bit % 4) * 6 + half)) & 7;
        r |= (lane & 1) << bit; g |= ((lane >> 1) & 1) << bit; b |= ((lane >> 2) & 1) << bit;
      }
    } else
#endif
    {
      xx = ESP32_TX_FIFO_POSITION_ADJUST(xx);
      for (unsigned bit = 0; bit < depth; ++bit) {
        const uint16_t lane = frame_buffer[front].rowBits[yy]->data[bit * PIXELS_PER_ROW + xx] >> (bottom ? BITS_RGB2_OFFSET : 0);
        r |= (lane & 1) << bit; g |= ((lane >> 1) & 1) << bit; b |= ((lane >> 2) & 1) << bit;
      }
    }
    *rgb++ = channel(r); *rgb++ = channel(g); *rgb++ = channel(b);
  }
  return true;
}
