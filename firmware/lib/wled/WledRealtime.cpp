// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Wled.h"
#include <NetworkBudget.h>
#include <lwip/pbuf.h>
#include <lwip/tcpip.h>
#include <lwip/priv/tcpip_priv.h>
#include <lwip/udp.h>
#include <atomic>
#include <esp_heap_caps.h>

// Pixels arrive over UDP and go straight into the panel's back buffer: no frame buffer of our own. The listeners run
// in the network task and never wait: a packet that finds the panel busy, or the overlay not on the panel yet, is
// dropped (it still counts as activity, so the stream brings the overlay up and the next frame is drawn). The
// main loop owns the panel; `owns` is only changed under the lock, so a handler that sees it set may draw.
namespace wled {
namespace {
constexpr uint16_t kDdpPort = 4048, kWledPort = 21324, kArtNetPort = 6454, kE131Port = 5568;
constexpr uint32_t kDefaultHoldMs = 3000;     // a stream with no timeout of its own ends this long after its last packet
constexpr uint8_t kPushFlag = 0x01, kTimecodeFlag = 0x10;
constexpr int kArtUniversePixels = 170;       // 510 channels per universe
constexpr uint32_t kMinFreeHeap = 20000, kMinBlock = 8000;  // the listeners only open when this much heap is left

enum Source : uint8_t { None, Ddp, Udp, ArtNet, E131 };
const char *const kSourceNames[] = {"", "DDP", "UDP", "Art-Net", "E1.31"};

const uint16_t kPorts[4] = {kDdpPort, kWledPort, kArtNetPort, kE131Port};
udp_pcb *sockets[4] = {};
std::atomic<uint32_t> received[4], valid[4];  // packets that reached each port, and those with a valid start
MatrixPanel_I2S_DMA *panel = nullptr;
SemaphoreHandle_t panelLock = nullptr;
std::atomic<uint32_t> activeUntil{0}, packets{0}, dropped{0};
std::atomic<uint16_t> scale{256};
std::atomic<uint8_t> source{None};
std::atomic<bool> owns{false}, dirty{false}, pushed{false}, pushSeen{false};
bool listening = false, live = false;
uint16_t lastScale = 0;
uint32_t idleSince = 0;

inline void put(int index, uint8_t r, uint8_t g, uint8_t b) {
  if ((unsigned)index >= (unsigned)kPixels) return;
  const uint16_t k = scale.load(std::memory_order_relaxed);
  panel->drawPixelRGB888(index & (kWidth - 1), index / kWidth, (r * k) >> 8, (g * k) >> 8, (b * k) >> 8);
}

// Marks the stream live (this brings the overlay up) and says whether pixels may be drawn right now: only when the
// overlay owns the panel and nobody else is drawing. Never waits.
class Draw {
public:
  Draw(Source from, uint32_t holdMs) {
    activeUntil.store(millis() + holdMs, std::memory_order_relaxed);
    source.store(from, std::memory_order_relaxed);
    packets.fetch_add(1, std::memory_order_relaxed);
    if (!panel || !owns.load(std::memory_order_relaxed)) { dropped.fetch_add(1, std::memory_order_relaxed); return; }
    if (xSemaphoreTake(panelLock, 0) != pdTRUE) { dropped.fetch_add(1, std::memory_order_relaxed); return; }
    if (!owns.load(std::memory_order_relaxed)) { xSemaphoreGive(panelLock); return; }
    _ok = true;
  }
  ~Draw() { if (_ok) xSemaphoreGive(panelLock); }
  explicit operator bool() const { return _ok; }
private:
  bool _ok = false;
};

// RGB triplets (or RGBW quads) starting at pixel `first`
void rgbRun(const uint8_t *data, size_t length, int first, int step) {
  for (size_t i = 0; i + step <= length; i += step, ++first) put(first, data[i], data[i + 1], data[i + 2]);
}

void onDdp(const uint8_t *p, size_t length) {
  const bool timecode = p[0] & kTimecodeFlag;
  const size_t header = timecode ? 14 : 10;
  if (length < header) return;
  if (p[3] == 251 || p[3] == 246) return;                                // JSON control and status ids: not pixels
  const uint32_t offset = (uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7];
  size_t count = (size_t)p[8] << 8 | p[9];
  if (count > length - header) count = length - header;
  const int step = (((p[2] & 0x38) >> 3) == 3) ? 4 : 3;                 // 0x1B is RGBW, everything else RGB
  Draw draw(Ddp, kDefaultHoldMs);
  if (!draw) return;
  rgbRun(p + header, count, (int)(offset / step), step);
  dirty = true;
  if (p[0] & kPushFlag) { pushSeen = true; pushed = true; }
}

// WLED's own realtime protocols: WARLS, DRGB, DRGBW, DNRGB, DNRGBW (byte 1 is the timeout in seconds)
void onWled(const uint8_t *p, size_t length) {
  const uint8_t protocol = p[0];
  const uint32_t hold = p[1] == 255 ? 3600000UL : p[1] ? p[1] * 1000UL : 250;
  Draw draw(Udp, hold);
  if (!draw) return;
  switch (protocol) {
    case 1: for (size_t i = 2; i + 4 <= length; i += 4) put(p[i], p[i + 1], p[i + 2], p[i + 3]); break;
    case 2: rgbRun(p + 2, length - 2, 0, 3); break;
    case 3: rgbRun(p + 2, length - 2, 0, 4); break;
    case 4: if (length >= 4) rgbRun(p + 4, length - 4, (p[2] << 8) | p[3], 3); break;
    case 5: if (length >= 4) rgbRun(p + 4, length - 4, (p[2] << 8) | p[3], 4); break;
  }
  dirty = true;
}

// Art-Net ArtDMX and E1.31 data: 170 pixels per universe, universe 0 (Art-Net) or 1 (E1.31) is the first
void dmxRun(Source from, int universe, const uint8_t *data, size_t length) {
  if (universe < 0 || universe * kArtUniversePixels >= kPixels) return;
  Draw draw(from, kDefaultHoldMs);
  if (!draw) return;
  rgbRun(data, length > 510 ? 510 : length, universe * kArtUniversePixels, 3);
  dirty = true;
}

void onArtNet(const uint8_t *p, size_t length) {
  if (length < 18) return;
  const size_t count = (size_t)p[16] << 8 | p[17];
  dmxRun(ArtNet, p[14] | (p[15] & 0x7F) << 8, p + 18, count > length - 18 ? length - 18 : count);
}

void onE131(const uint8_t *p, size_t length) {
  if (length < 126 || p[125] != 0) return;                               // DMX start code 0
  size_t count = ((size_t)p[123] << 8 | p[124]);
  count = count > 0 ? count - 1 : 0;
  dmxRun(E131, (p[113] << 8 | p[114]) - 1, p + 126, count > length - 126 ? length - 126 : count);
}

// Does the packet start like its protocol? Only the first bytes are looked at; anything else is freed at once
bool startsRight(int kind, const uint8_t *head, size_t have, size_t total) {
  switch (kind) {
    case 0: return have >= 10 && (head[0] & 0xC0) == 0x40;                                                   // DDP version 1
    case 1: return have >= 2 && head[0] >= 1 && head[0] <= 5;                                                // WLED realtime protocol id
    case 2: return have >= 10 && memcmp(head, "Art-Net", 8) == 0 && head[8] == 0x00 && head[9] == 0x50;      // ArtDMX
    default: return total >= 126 && have >= 13 && memcmp(head + 4, "ASC-E1.17", 9) == 0;                     // E1.31 root layer
  }
}

// Runs in the network task; never waits
void onPacket(void *arg, udp_pcb *, pbuf *p, const ip_addr_t *, u16_t) {
  const int kind = (int)(uintptr_t)arg;
  received[kind]++;
  uint8_t head[16];
  const size_t have = pbuf_copy_partial(p, head, sizeof(head), 0), total = p->tot_len;
  if (startsRight(kind, head, have, total)) {
    valid[kind]++;
    if (!p->next) {  // one buffer holds the whole packet (the normal case up to the MTU): read it in place, no copy and no static buffer
      const uint8_t *data = (const uint8_t *)p->payload;
      switch (kind) {
        case 0: onDdp(data, total); break;
        case 1: onWled(data, total); break;
        case 2: onArtNet(data, total); break;
        default: onE131(data, total); break;
      }
    } else dropped.fetch_add(1, std::memory_order_relaxed);
  }
  pbuf_free(p);
}

// Socket set-up and tear-down must run in the network task
struct SocketCall { struct tcpip_api_call_data call; int kind; udp_pcb *pcb; };

err_t openApi(struct tcpip_api_call_data *data) {
  SocketCall *c = (SocketCall *)data;
  c->pcb = udp_new_ip_type(IPADDR_TYPE_V4);
  if (!c->pcb) return ERR_MEM;
  udp_recv(c->pcb, onPacket, (void *)(uintptr_t)c->kind);
  const err_t error = udp_bind(c->pcb, IP4_ADDR_ANY, kPorts[c->kind]);
  if (error != ERR_OK) { udp_remove(c->pcb); c->pcb = nullptr; }
  return error;
}

err_t closeApi(struct tcpip_api_call_data *data) {
  SocketCall *c = (SocketCall *)data;
  if (c->pcb) udp_remove(c->pcb);
  return ERR_OK;
}

bool heapAllows() {
  return heap_caps_get_free_size(MALLOC_CAP_8BIT) >= kMinFreeHeap && heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= kMinBlock;
}

void closeListeners() {
  if (!listening) return;
  for (int kind = 0; kind < 4; ++kind) {
    SocketCall call{};
    call.pcb = sockets[kind];
    if (call.pcb) tcpip_api_call(closeApi, &call.call);
    sockets[kind] = nullptr;
  }
  listening = false;
  realtimeStop();
}

bool openListeners() {
  if (listening) return true;
  if (!heapAllows()) return false;
  if (!panelLock) panelLock = xSemaphoreCreateMutex();
  for (int kind = 0; kind < 4; ++kind) {
    SocketCall call{};
    call.kind = kind;
    if (tcpip_api_call(openApi, &call.call) == ERR_OK) sockets[kind] = call.pcb;
  }
  listening = sockets[0] || sockets[1] || sockets[2] || sockets[3];
  return listening;
}

// The main loop's grip on the panel; short waits, never forever
class Grip {
public:
  explicit Grip(uint32_t waitMs) : _held(panelLock && xSemaphoreTake(panelLock, pdMS_TO_TICKS(waitMs)) == pdTRUE) {}
  ~Grip() { if (_held) xSemaphoreGive(panelLock); }
  bool held() const { return _held; }
private:
  bool _held;
};
}  // namespace

void inputActivity() { idleSince = millis(); }

bool enabled() { return options().enabled != 0; }
bool realtimeListening() { return listening; }

// Opens or closes everything that costs memory: listeners, and (main.cpp) the mDNS record. False: not enough heap.
static uint32_t makeRoomUntil = 0;  // after turning on without enough heap: the feeds stay parked (stacks reaped) until then, so service() can open the listeners

bool setEnabled(bool on) {
  options().enabled = on ? 1 : 0;
  touched();
  if (on) {
    if (openListeners()) return true;
    makeRoomUntil = millis() + 30000;  // the football/F1 feeds hold the heap: park them and let service() retry
    return true;
  }
  if (panelLock) { Grip grip(100); owns = false; }
  closeListeners();
  canvas::clearImports();
  canvas::release();
  state().on = false;
  return true;
}

void service(bool allow, uint32_t now, bool releaseMemory) {
  static uint32_t retryAt = 0;
  if (!enabled()) return;
  if (allow && !listening) {
    if ((int32_t)(now - retryAt) < 0) return;
    retryAt = now + 5000;
    openListeners();
  } else if (!allow && listening) {
    if (panelLock) { Grip grip(100); owns = false; }
    closeListeners();
    if (releaseMemory) canvas::release();
  }
}

void attachPanel(MatrixPanel_I2S_DMA &display) { panel = &display; }

bool realtimeActive(uint32_t now) {
  const uint32_t until = activeUntil.load(std::memory_order_relaxed);
  return until && (int32_t)(until - now) > 0;
}

void realtimeStop() { activeUntil = 0; source = None; dirty = false; pushed = false; }
const char *realtimeSource() { return kSourceNames[source.load()]; }
uint32_t realtimePackets() { return packets.load(); }
uint32_t realtimeDropped() { return dropped.load(); }
void realtimeCounts(uint32_t out[9]) { for (int i = 0; i < 4; ++i) { out[i] = received[i].load(); out[4 + i] = valid[i].load(); } out[8] = dropped.load(); }

// A live event owns the panel when the option says so (unless WLED already holds it: the feeds are parked then and their flags are stale)
static bool blockedByLive() { return options().ignoreLive && liveFavouriteOn.load() && !feedsPaused.load(); }

void poll(int hour, int minute, int second, bool h24, int weekday, uint32_t now) {
  if (!enabled()) { feedsPaused = false; wsClose(); return; }
  wsService(now);
  saveIfDue(now);
  setClock(hour, minute, second, h24);
  timersTick(hour * 60 + minute, weekday, now);
  lastScale = advance(now);
  State &s = state();
  if (blockedByLive()) { s.on = false; realtimeStop(); lastScale = 0; }  // the command is dropped, not postponed
  static uint32_t memCheckAt = 0;
  if ((int32_t)(now - memCheckAt) >= 0) {  // once a second: an import must never push the clock into allocation failures (a shed, then a lowered panel quality)
    memCheckAt = now + 1000;
    canvas::maintain(now);
    if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < 10000) canvas::shrink();
  }
  static bool wasWanted = false;
  const bool nowWanted = wanted(now);
  if (wasWanted && !nowWanted) canvas::clearImports();  // WLED let go of the panel: the imported animations go with it
  wasWanted = nowWanted;
  feedsPaused = nowWanted || (!listening && (int32_t)(makeRoomUntil - now) > 0);  // the football/F1/MotoGP feeds park while WLED has the panel (or needs room to start) and resume after
  static bool trimmed = false;
  if (!feedsPaused) trimmed = false;
  else if (reapFeedTasks() && !trimmed && feedTrimFn) {  // a parked task keeps its stack until it is deleted
    trimmed = true;  // give back disposable face data once per WLED session, before imports need it
    feedTrimFn(feedTrimArg);
  }
  const uint32_t timeout = (uint32_t)options().timeoutMin * 60000UL;
  if (!s.on || realtimeActive(now) || !timeout) idleSince = now;       // playback and playlist steps do not count as input
  else if (now - idleSince >= timeout) { s.on = false; lookChanged(); }
}

bool feedsQuiet() { return feedsPaused.load() && reapFeedTasks(); }

bool wanted(uint32_t now) {
  if (!enabled() || blockedByLive()) return false;
  return state().on || lastScale > 0 || realtimeActive(now);
}

void hidden() {
  if (!owns.load()) return;
  Grip grip(50);
  owns = false;
  live = false;
}

bool shown() { return owns.load(); }

void draw(MatrixPanel_I2S_DMA &display, uint32_t now) {
  Grip grip(50);
  if (!grip.held()) return;
  scale.store(lastScale, std::memory_order_relaxed);
  if (realtimeActive(now)) {  // pixels are written by the listeners; show them when a frame is complete
    if (!live) { live = true; display.fillScreenRGB888(0, 0, 0); }
    owns = true;
    if (dirty.load() && !(pushSeen.load() && !pushed.exchange(false))) { dirty = false; display.flipDMABuffer(); }
    return;
  }
  if (live) { live = false; restartEffect(); }
  owns = true;
  if (canvas::holdsPanel()) return;  // an import is uploading: no new frame, the panel keeps showing the last one
  render(display, now, lastScale);
  display.flipDMABuffer();
}

}  // namespace wled
