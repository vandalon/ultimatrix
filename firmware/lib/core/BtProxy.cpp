// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "BtProxy.h"
#include "Console.h"
#include "NetworkBudget.h"
#include <atomic>
#include <string.h>
#include <errno.h>
#include "esp_heap_caps.h"
#include "esp_bt.h"
#include "esp_mac.h"
#include "lwip/sockets.h"
#include <netinet/tcp.h>
#include <Arduino.h>
#include <WiFi.h>
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nvs.h"

#ifndef UM_MODE_NAME
#define UM_MODE_NAME "Ultimatrix"
#endif

// The wire format is the ESPHome native API without encryption: a 0x00 byte, the payload length and the message type as
// protobuf varints, then the protobuf payload. Only the few messages Home Assistant needs for a passive proxy are
// implemented; everything else is read and ignored.
namespace btproxy {
namespace {
enum : uint32_t {
  kHelloReq = 1, kHelloRes = 2, kConnectReq = 3, kConnectRes = 4, kDisconnectReq = 5, kDisconnectRes = 6, kPingReq = 7, kPingRes = 8,
  kDeviceInfoReq = 9, kDeviceInfoRes = 10, kListEntitiesReq = 11, kListEntitiesDone = 19,
  kSubscribeAdvReq = 66, kConnectionsFreeReq = 80, kConnectionsFreeRes = 81, kUnsubscribeAdvReq = 87, kRawAdvRes = 93,
  kScannerStateRes = 126, kScannerSetModeReq = 127,
};
constexpr uint32_t kFeaturePassive = 1, kFeatureRaw = 32, kFeatureStateAndMode = 64;
constexpr uint16_t kPort = 6053;
constexpr size_t kRing = 32, kAdvMax = 31, kBatch = 12, kRxMax = 192;
constexpr size_t kTxMax = 16 + kBatch * (kAdvMax + 28);
constexpr size_t kLowHeap = 24 * 1024, kOkHeap = 36 * 1024;   // pause below, resume above (largest-free accounting is not needed: the buffers are small)
constexpr uint32_t kIdleMs = 120000;                           // Home Assistant pings every 20 s

struct Advert { uint8_t mac[6]; int8_t rssi; uint8_t type, len; uint8_t data[kAdvMax]; };
Advert *ring = nullptr;                      // one writer (the Bluetooth host task), one reader (the proxy task)
std::atomic<uint32_t> head{0}, tail{0}, sentCount{0}, droppedCount{0};
std::atomic<bool> synced{false}, scanning{false}, clientUp{false}, started{false}, scanWanted{false};
uint8_t ownAddrType = 0;
int8_t enabledCache = -1;
bool (*allowFn)(uint32_t) = nullptr;
const char *stateText = "off";

// ---- protobuf helpers -------------------------------------------------------------------------------------------
size_t putVarint(uint8_t *b, uint64_t v) { size_t n = 0; while (v >= 0x80) { b[n++] = (uint8_t)(v | 0x80); v >>= 7; } b[n++] = (uint8_t)v; return n; }
size_t putField(uint8_t *b, uint32_t field, uint64_t v) { size_t n = putVarint(b, field << 3); return n + putVarint(b + n, v); }
size_t putText(uint8_t *b, uint32_t field, const char *s, size_t len) { size_t n = putVarint(b, (field << 3) | 2); n += putVarint(b + n, len); memcpy(b + n, s, len); return n + len; }
size_t putText(uint8_t *b, uint32_t field, const char *s) { return putText(b, field, s, strlen(s)); }
bool getVarint(const uint8_t *b, size_t len, size_t &pos, uint64_t &out) {
  out = 0;
  for (int shift = 0; pos < len && shift < 64; shift += 7) { uint8_t c = b[pos++]; out |= (uint64_t)(c & 0x7F) << shift; if (!(c & 0x80)) return true; }
  return false;
}
// ---- scanner (Bluetooth host task) --------------------------------------------------------------------------------
int scanEvent(struct ble_gap_event *ev, void *) {
  if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) { scanning = false; return 0; }
  if (ev->type != BLE_GAP_EVENT_DISC || !ring) return 0;
  const uint32_t h = head.load(std::memory_order_relaxed);
  if (h - tail.load(std::memory_order_acquire) >= kRing) { droppedCount++; return 0; }
  Advert &a = ring[h % kRing];
  memcpy(a.mac, ev->disc.addr.val, 6);
  a.rssi = ev->disc.rssi; a.type = ev->disc.addr.type;
  a.len = ev->disc.length_data > kAdvMax ? kAdvMax : ev->disc.length_data;
  if (a.len) memcpy(a.data, ev->disc.data, a.len);
  head.store(h + 1, std::memory_order_release);
  return 0;
}
void startScan() {
  if (!synced || scanning) return;
  ble_gap_disc_params p = {};
  p.itvl = 512; p.window = 480;          // 320 ms / 300 ms: nearly continuous, leaves Wi-Fi its share (the ESPHome default)
  p.passive = 1; p.filter_duplicates = 0; // repeated adverts are what room presence measures
  if (ble_gap_disc(ownAddrType, BLE_HS_FOREVER, &p, scanEvent, nullptr) == 0) scanning = true;
}
void stopScan() { if (scanning.exchange(false)) ble_gap_disc_cancel(); }
void onSync() {
  if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &ownAddrType) != 0) { consoleLog("[BtProxy] no Bluetooth address\n"); return; }
  synced = true;
}
void hostTask(void *) { nimble_port_run(); nimble_port_freertos_deinit(); }

// ---- native API connection ----------------------------------------------------------------------------------------
struct Client {
  int fd = -1;
  uint8_t *rx = nullptr, *tx = nullptr;
  size_t rxLen = 0;
  size_t skip = 0;           // bytes of an oversize message still to discard
  bool subscribed = false;
  uint32_t lastRx = 0;
};

void closeClient(Client &c) {
  if (c.fd >= 0) { close(c.fd); c.fd = -1; }
  free(c.rx); free(c.tx); c.rx = c.tx = nullptr; c.rxLen = c.skip = 0; c.subscribed = false;
  clientUp = false; scanWanted = false;
}
bool sendAll(Client &c, const uint8_t *b, size_t n) {
  size_t off = 0;
  for (int tries = 0; off < n && tries < 20; ) {
    const int r = send(c.fd, b + off, n - off, 0);
    if (r > 0) { off += r; continue; }
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      fd_set w; FD_ZERO(&w); FD_SET(c.fd, &w); timeval tv = {0, 50000};
      if (select(c.fd + 1, nullptr, &w, nullptr, &tv) <= 0) tries++;
      continue;
    }
    return false;
  }
  return off == n;
}
// payload builders write at tx + 8; sendBuilt prepends the frame header right before it so nothing is copied twice
bool sendBuilt(Client &c, uint32_t type, size_t len) {
  uint8_t head[12]; size_t n = 0;
  head[n++] = 0; n += putVarint(head + n, len); n += putVarint(head + n, type);
  uint8_t *start = c.tx + 8 - n;
  memcpy(start, head, n);
  return sendAll(c, start, n + len);
}
bool sendEmpty(Client &c, uint32_t type) { uint8_t head[4] = {0, 0, (uint8_t)type, 0}; return sendAll(c, head, 3); }

void macText(char *out, const uint8_t *m) { snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]); }
const char *deviceName() { static char name[40]; if (!name[0]) { uint8_t m[6]; esp_read_mac(m, ESP_MAC_WIFI_STA); snprintf(name, sizeof(name), "ultimatrix-%02x%02x%02x", m[3], m[4], m[5]); } return name; }

bool sendScannerState(Client &c) {
  uint8_t *p = c.tx + 8; size_t n = 0;
  n += putField(p + n, 1, scanning ? 2 : 1 /* RUNNING : STARTING */);
  // mode and configured_mode are PASSIVE (0): proto3 leaves zero values out
  return sendBuilt(c, kScannerStateRes, n);
}

bool handle(Client &c, uint32_t type) {
  uint8_t *p = c.tx + 8; size_t n = 0;
  switch (type) {
    case kHelloReq:
      n += putField(p + n, 1, 1); n += putField(p + n, 2, 10);
      n += putText(p + n, 3, "Ultimatrix " UM_MODE_NAME); n += putText(p + n, 4, deviceName());
      return sendBuilt(c, kHelloRes, n);
    case kConnectReq: return sendEmpty(c, kConnectRes);   // no password
    case kDisconnectReq: sendEmpty(c, kDisconnectRes); return false;
    case kDisconnectRes: return false;
    case kPingReq: return sendEmpty(c, kPingRes);
    case kDeviceInfoReq: {
      uint8_t m[6]; char mac[18];
      n += putText(p + n, 2, deviceName());
      esp_read_mac(m, ESP_MAC_WIFI_STA); macText(mac, m); n += putText(p + n, 3, mac);
      n += putText(p + n, 4, "2025.10.0");      // the ESPHome API level this implements; Home Assistant checks it for old proxies
      n += putText(p + n, 6, "Ultimatrix " UM_MODE_NAME);
      n += putText(p + n, 12, "Ultimatrix");
      n += putText(p + n, 13, deviceName());
      n += putField(p + n, 15, kFeaturePassive | kFeatureRaw | kFeatureStateAndMode);
      esp_read_mac(m, ESP_MAC_BT); macText(mac, m); n += putText(p + n, 18, mac);
      return sendBuilt(c, kDeviceInfoRes, n);
    }
    case kListEntitiesReq: return sendEmpty(c, kListEntitiesDone);
    case kSubscribeAdvReq:
      c.subscribed = true; scanWanted = true;
      return sendScannerState(c);
    case kUnsubscribeAdvReq: c.subscribed = false; scanWanted = false; return true;
    case kScannerSetModeReq: return sendScannerState(c);   // always passive
    case kConnectionsFreeReq: {
      n += putField(p + n, 1, 0); n += putField(p + n, 2, 0);   // no connection slots: this proxy does not connect to devices
      return sendBuilt(c, kConnectionsFreeRes, n);
    }
    default: return true;   // subscribe-states, logs, services, time...: nothing to say
  }
}

// parses what arrived; false closes the connection
bool feed(Client &c, size_t got) {
  c.rxLen += got;
  size_t pos = 0;
  for (;;) {
    if (c.skip) { size_t d = c.rxLen - pos < c.skip ? c.rxLen - pos : c.skip; pos += d; c.skip -= d; if (c.skip) break; }
    if (pos >= c.rxLen) break;
    if (c.rx[pos] != 0) return false;   // 0x01 is the encrypted transport, which this proxy does not speak
    size_t q = pos + 1; uint64_t len, type;
    if (!getVarint(c.rx, c.rxLen, q, len) || !getVarint(c.rx, c.rxLen, q, type)) { if (c.rxLen - pos > 12) return false; break; }
    if (len > kRxMax) {   // too big for the buffer and not one the proxy needs: read past it
      c.skip = len; pos = q; continue;
    }
    if (q + len > c.rxLen) break;
    if (!handle(c, (uint32_t)type)) return false;
    pos = q + len;
  }
  if (pos) { memmove(c.rx, c.rx + pos, c.rxLen - pos); c.rxLen -= pos; }
  return true;
}

bool flush(Client &c) {
  uint8_t *p = c.tx + 8; size_t n = 0; uint32_t count = 0;
  while (count < kBatch) {
    const uint32_t t = tail.load(std::memory_order_relaxed);
    if (t == head.load(std::memory_order_acquire)) break;
    const Advert &a = ring[t % kRing];
    uint8_t inner[kAdvMax + 28]; size_t m = 0;
    uint64_t addr = 0; for (int i = 5; i >= 0; i--) addr = (addr << 8) | a.mac[i];   // NimBLE keeps the address least significant byte first
    m += putField(inner + m, 1, addr);
    m += putField(inner + m, 2, (uint32_t)(((int32_t)a.rssi << 1) ^ ((int32_t)a.rssi >> 31)));   // sint32
    if (a.type) m += putField(inner + m, 3, a.type);
    m += putText(inner + m, 4, (const char *)a.data, a.len);
    n += putVarint(p + n, (1 << 3) | 2); n += putVarint(p + n, m); memcpy(p + n, inner, m); n += m;
    tail.store(t + 1, std::memory_order_release);
    count++;
  }
  if (!count) return true;
  if (!sendBuilt(c, kRawAdvRes, n)) { droppedCount += count; return false; }
  sentCount += count;
  return true;
}

void serve(void *) {
  int lfd = -1;
  Client c;
  bool low = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(c.fd >= 0 ? 15 : 200));
    const size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (freeHeap < kLowHeap) low = true; else if (low && freeHeap > kOkHeap) low = false;
    const bool halted = feedsHalted() || low || WiFi.status() != WL_CONNECTED;
    if (halted) {                                   // an update, notice, picture or WLED has the network and heap: let go of everything
      stopScan(); closeClient(c);
      if (lfd >= 0) { close(lfd); lfd = -1; }
      stateText = "paused";
      continue;
    }
    if (lfd < 0) {
      lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
      if (lfd < 0) continue;
      int one = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
      sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_port = htons(kPort); a.sin_addr.s_addr = htonl(INADDR_ANY);
      if (bind(lfd, (sockaddr *)&a, sizeof(a)) != 0 || listen(lfd, 1) != 0) { close(lfd); lfd = -1; continue; }
      fcntl(lfd, F_SETFL, O_NONBLOCK);
    }
    // a new connection replaces the old one (Home Assistant reconnects after a dropped link before the old one times out)
    sockaddr_in peer; socklen_t pl = sizeof(peer);
    const int nfd = accept(lfd, (sockaddr *)&peer, &pl);
    if (nfd >= 0) {
      if (allowFn && !allowFn(peer.sin_addr.s_addr)) close(nfd);
      else {
        closeClient(c);
        c.rx = (uint8_t *)malloc(kRxMax + 16); c.tx = (uint8_t *)malloc(kTxMax + 16);
        if (!c.rx || !c.tx) { close(nfd); closeClient(c); }
        else {
          fcntl(nfd, F_SETFL, O_NONBLOCK);
          int one = 1; setsockopt(nfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
          c.fd = nfd; c.lastRx = millis(); clientUp = true;
        }
      }
    }
    if (c.fd < 0) { stopScan(); stateText = "idle"; continue; }
    if (c.rxLen < kRxMax + 16) {
      const int r = recv(c.fd, c.rx + c.rxLen, kRxMax + 16 - c.rxLen, 0);
      if (r > 0) { c.lastRx = millis(); if (!feed(c, r)) { closeClient(c); continue; } }
      else if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) { closeClient(c); continue; }
    }
    if (millis() - c.lastRx > kIdleMs) { closeClient(c); continue; }
    if (c.subscribed && scanWanted) {
      if (!scanning) { startScan(); sendScannerState(c); }
      if (!flush(c)) { closeClient(c); continue; }
      stateText = "scanning";
    } else { stopScan(); tail.store(head.load()); stateText = "idle"; }
  }
}
}  // namespace

bool enabled() {
  if (enabledCache >= 0) return enabledCache;
  uint8_t on = 0; nvs_handle_t h; size_t n = 0;
  if (nvs_open("ultimatrix", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "btProxy", &on); nvs_close(h); }
  if (on && nvs_open("um-updates", NVS_READONLY, &h) == ESP_OK) { if (nvs_get_str(h, "pending", nullptr, &n) == ESP_OK && n > 1) on = 0; nvs_close(h); }   // an update-only boot keeps the whole heap
  enabledCache = on ? 1 : 0;
  return on;
}
bool active() { return enabled(); }

bool setEnabled(bool on) {
  nvs_handle_t h;
  esp_err_t err = nvs_open("ultimatrix", NVS_READWRITE, &h);
  if (err != ESP_OK) { consoleLog("[BtProxy] settings not saved: open %s\n", esp_err_to_name(err)); return false; }
  err = nvs_set_u8(h, "btProxy", on ? 1 : 0);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

void boot() { esp_bt_controller_mem_release(active() ? ESP_BT_MODE_CLASSIC_BT : ESP_BT_MODE_BTDM); }

void begin() {
  if (!active() || started.exchange(true)) return;
  ring = (Advert *)calloc(kRing, sizeof(Advert));
  if (!ring) { consoleLog("[BtProxy] no memory for the queue\n"); return; }
  const size_t before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const esp_err_t err = nimble_port_init();
  if (err != ESP_OK) { consoleLog("[BtProxy] Bluetooth did not start: %s (free %u)\n", esp_err_to_name(err), (unsigned)before); return; }
  ble_hs_cfg.sync_cb = onSync;
  nimble_port_freertos_init(hostTask);
  stateText = "idle";
  if (xTaskCreate(serve, "btproxy", 4096, nullptr, 1, nullptr) != pdPASS) { consoleLog("[BtProxy] task not started\n"); return; }
  consoleLog("[BtProxy] started: free heap %u -> %u\n", (unsigned)before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
}

void setFilter(bool (*allowed)(uint32_t ip)) { allowFn = allowed; }
const char *state() { return active() ? stateText : "off"; }
uint32_t sent() { return sentCount.load(); }
uint32_t dropped() { return droppedCount.load(); }
bool clientConnected() { return clientUp.load(); }
}  // namespace btproxy
