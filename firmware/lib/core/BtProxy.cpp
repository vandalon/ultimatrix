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
#include "esp_log.h"
#include "esp_mac.h"
#include "lwip/sockets.h"
#include <netinet/tcp.h>
#include <Arduino.h>
#include <WiFi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#ifndef UM_MODE_NAME
#define UM_MODE_NAME "Ultimatrix"
#endif

// The wire format is the ESPHome native API without encryption: a 0x00 byte, the payload length and the message type as
// protobuf varints, then the protobuf payload. Only the few messages Home Assistant needs for a passive proxy are
// implemented; everything else is read and ignored.
extern std::atomic<uint32_t> memoryAllocationFailures;   // main.cpp: raised when a feed or buffer allocation fails
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
constexpr size_t kLowHeap = 10 * 1024, kOkHeap = 16 * 1024;   // pause below, resume above: the proxy itself needs only about 3 KB, and the classic ESP32 idles near 30 KB free with the controller running
constexpr uint32_t kIdleMs = 120000;                           // Home Assistant pings every 20 s

struct Advert { uint8_t mac[6]; int8_t rssi; uint8_t type, len; uint8_t data[kAdvMax]; };
Advert *ring = nullptr;                      // one writer (the Bluetooth host task), one reader (the proxy task)
std::atomic<uint32_t> head{0}, tail{0}, sentCount{0}, droppedCount{0};
std::atomic<bool> synced{false}, scanning{false}, clientUp{false}, started{false}, scanWanted{false}, activeScan{false}, restartScan{false};
int8_t enabledCache = -1;
bool (*allowFn)(uint32_t) = nullptr;
const char *stateText = "off";
std::atomic<bool> held{false};
std::atomic<int> stopStage{0};               // shutdown(): 1 = asked, 2 = the serve task has let go and ended               // hold(): no scanning while another link needs the heap

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
// ---- scanner: the controller's HCI interface (VHCI), no host stack -----------------------------------------------------
// A scan-only proxy needs five HCI commands and one event, so the NimBLE host (heap, a task and ~100 KB of flash) is not used.
SemaphoreHandle_t cmdSem = nullptr;
volatile uint16_t waitOpcode = 0;
volatile uint8_t cmdStatus = 0xFF;
constexpr uint16_t kOpReset = 0x0C03, kOpSetEventMask = 0x0C01, kOpLeScanParams = 0x200B, kOpLeScanEnable = 0x200C;

void hostSendAvailable() {}
// Runs in the controller task: keep it short. Packet = 0x04, event code, length, parameters.
int hostRecv(uint8_t *d, uint16_t len) {
  if (len < 3 || d[0] != 0x04) return 0;
  const uint8_t evt = d[1], plen = d[2];
  const uint8_t *p = d + 3;
  if (len < 3u + plen) return 0;
  if (evt == 0x0E && plen >= 4) {                   // Command Complete: packets allowed, opcode, status
    if ((uint16_t)(p[1] | p[2] << 8) == waitOpcode) { cmdStatus = p[3]; if (cmdSem) xSemaphoreGive(cmdSem); }
  } else if (evt == 0x0F && plen >= 4) {            // Command Status: status, packets allowed, opcode
    if ((uint16_t)(p[2] | p[3] << 8) == waitOpcode) { cmdStatus = p[0]; if (cmdSem) xSemaphoreGive(cmdSem); }
  } else if (evt == 0x3E && plen >= 2 && p[0] == 0x02 && ring) {   // LE Meta, LE Advertising Report
    // The report fields come as arrays: event types, address types, addresses, data lengths, the data, then the RSSIs.
    const uint8_t n = p[1];
    if (n == 0 || n > 8) return 0;
    const uint8_t *addrType = p + 2 + n, *addr = p + 2 + 2 * n, *dlen = p + 2 + 8 * n;
    const uint8_t *data = dlen + n;
    size_t total = 0;
    for (uint8_t i = 0; i < n; i++) total += dlen[i];
    if (2u + 10u * n + total > plen) return 0;
    const uint8_t *rssi = data + total;
    for (uint8_t i = 0; i < n; i++) {
      const uint32_t h = head.load(std::memory_order_relaxed);
      if (h - tail.load(std::memory_order_acquire) >= kRing) { droppedCount++; data += dlen[i]; continue; }
      Advert &a = ring[h % kRing];
      memcpy(a.mac, addr + 6 * i, 6);
      a.rssi = (int8_t)rssi[i]; a.type = addrType[i];
      a.len = dlen[i] > kAdvMax ? kAdvMax : dlen[i];
      if (a.len) memcpy(a.data, data, a.len);
      data += dlen[i];
      head.store(h + 1, std::memory_order_release);
    }
  }
  return 0;
}
const esp_vhci_host_callback_t vhciCallbacks = {hostSendAvailable, hostRecv};

bool hciCommand(uint16_t opcode, const uint8_t *params, uint8_t n) {
  uint8_t pk[4 + 8];
  if (n > 8 || !cmdSem) return false;
  pk[0] = 0x01; pk[1] = opcode & 0xFF; pk[2] = opcode >> 8; pk[3] = n;
  if (n) memcpy(pk + 4, params, n);
  for (int i = 0; i < 100 && !esp_vhci_host_check_send_available(); i++) vTaskDelay(1);
  while (xSemaphoreTake(cmdSem, 0) == pdTRUE) {}
  waitOpcode = opcode; cmdStatus = 0xFF;
  esp_vhci_host_send_packet(pk, 4 + n);
  return xSemaphoreTake(cmdSem, pdMS_TO_TICKS(500)) == pdTRUE && cmdStatus == 0;
}
bool setScan(bool on) {
  const uint8_t en[2] = {(uint8_t)(on ? 1 : 0), 0};   // filter duplicates off: repeated adverts are what room presence measures
  return hciCommand(kOpLeScanEnable, en, 2);
}
void startScan() {
  if (!synced || scanning) return;
  // 320 ms interval / 300 ms window: nearly continuous, leaves Wi-Fi its share (the ESPHome default). Active scanning asks devices for
  // their scan response (phones keep their name and service data there).
  const uint8_t par[7] = {(uint8_t)(activeScan ? 1 : 0), 0x00, 0x02, 0xE0, 0x01, 0x00 /* public address */, 0x00 /* accept all */};
  if (hciCommand(kOpLeScanParams, par, 7) && setScan(true)) scanning = true;
  else consoleLog("[BtProxy] scan did not start\n");
}
void stopScan() { if (scanning.exchange(false)) setScan(false); }

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
  if (activeScan) { n += putField(p + n, 2, 1); n += putField(p + n, 3, 1); }   // mode, configured_mode: ACTIVE (passive is 0 and left out)
  return sendBuilt(c, kScannerStateRes, n);
}

bool handle(Client &c, uint32_t type, const uint8_t *b, size_t len) {
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
      n += putText(p + n, 4, "2026.11.1");      // the ESPHome API level this implements; Home Assistant checks it for old proxies
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
    case kScannerSetModeReq: {
      size_t pos = 0; uint64_t key, mode = 0;
      if (len && getVarint(b, len, pos, key) && (key >> 3) == 1) getVarint(b, len, pos, mode);
      if (activeScan.exchange(mode == 1) != (mode == 1)) restartScan = true;   // the scan restarts with the new mode
      return sendScannerState(c);
    }
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
    if (!handle(c, (uint32_t)type, c.rx + q, (size_t)len)) return false;
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
  uint32_t seenFailures = memoryAllocationFailures.load(), backoffUntil = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(c.fd >= 0 ? 15 : 200));
    if (stopStage.load()) {   // shutdown(): let go of everything and end the task (its stack goes back to the heap)
      stopScan(); closeClient(c);
      if (lfd >= 0) close(lfd);
      stopStage = 2;
      vTaskDelete(nullptr);
    }
    const size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (freeHeap < kLowHeap) low = true; else if (low && freeHeap > kOkHeap) low = false;
    const uint32_t failures = memoryAllocationFailures.load();
    if (failures != seenFailures) { seenFailures = failures; backoffUntil = millis() + 8000; }   // a feed ran out of memory: let go of the client and the sockets for a few seconds
    if (backoffUntil && (int32_t)(millis() - backoffUntil) < 0) {
      stopScan(); closeClient(c);
      if (lfd >= 0) { close(lfd); lfd = -1; }
      stateText = "paused";
      continue;
    }
    if (low && c.fd >= 0 && !feedsHalted() && WiFi.status() == WL_CONNECTED) {   // a feed refresh dips the heap for a moment: stop scanning but keep Home Assistant connected, it would only reconnect
      stopScan(); stateText = "paused";
      continue;
    }
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
    if (c.subscribed && scanWanted && !held.load()) {   // hold(): no scanning, but the link keeps answering Home Assistant's pings, so it does not drop
      if (restartScan.exchange(false)) stopScan();
      if (!scanning) { startScan(); sendScannerState(c); }
      if (!flush(c)) { closeClient(c); continue; }
      stateText = "scanning";
    } else { stopScan(); tail.store(head.load()); stateText = held.load() ? "paused" : "idle"; }
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

void boot() {
#if CONFIG_IDF_TARGET_ESP32   // the S3 has no classic Bluetooth memory to give back
  esp_bt_controller_mem_release(active() ? ESP_BT_MODE_CLASSIC_BT : ESP_BT_MODE_BTDM);
#endif
}

void begin() {
  if (!active() || started.exchange(true)) return;
  ring = (Advert *)calloc(kRing, sizeof(Advert));
  if (!ring) { consoleLog("[BtProxy] no memory for the queue\n"); return; }
  const size_t before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  ESP_LOGW("BtProxy", "before start: heap intact %d, internal free %u (largest %u), 8-bit free %u\n", (int)heap_caps_check_integrity_all(false),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)before);
  setEnabled(false);   // crash-loop breaker: stays off if the controller init aborts; switched back on once it is up
  cmdSem = xSemaphoreCreateBinary();
  esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  esp_err_t err = esp_bt_controller_init(&cfg);
  if (err == ESP_OK) err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  if (err == ESP_OK) err = esp_vhci_host_register_callback(&vhciCallbacks);
  if (err != ESP_OK) { consoleLog("[BtProxy] Bluetooth did not start: 0x%x (free %u)\n", (unsigned)err, (unsigned)before); return; }
  setEnabled(true);
  static const uint8_t mask[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0x00, 0x20};   // the default plus LE Meta events
  if (!hciCommand(kOpReset, nullptr, 0) || !hciCommand(kOpSetEventMask, mask, 8)) { consoleLog("[BtProxy] controller did not answer\n"); return; }
  synced = true;
  stateText = "idle";
  if (xTaskCreate(serve, "btproxy", 4096, nullptr, 1, nullptr) != pdPASS) { consoleLog("[BtProxy] task not started\n"); return; }
  consoleLog("[BtProxy] started: free heap %u -> %u\n", (unsigned)before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
}

void setFilter(bool (*allowed)(uint32_t ip)) { allowFn = allowed; }
void hold(bool on) { held = on; }
bool shutdown() {
  if (!started.load() || stopStage.load()) return false;
  stopStage = 1;
  for (int i = 0; i < 50 && stopStage.load() != 2; i++) vTaskDelay(pdMS_TO_TICKS(20));   // the task checks every 15-200 ms
  if (stopStage.load() != 2) synced = false;   // the task did not answer in time: the controller goes anyway
  esp_bt_controller_disable();
  esp_bt_controller_deinit();
#if CONFIG_IDF_TARGET_ESP32
  esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);   // the controller's own memory joins the heap; it can only start again after a restart
#endif
  stateText = "stopped";
  consoleLog("[BtProxy] stopped for an update: free heap %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
  return true;
}
const char *state() { return active() ? stateText : "off"; }
uint32_t sent() { return sentCount.load(); }
uint32_t dropped() { return droppedCount.load(); }
bool clientConnected() { return clientUp.load(); }
}  // namespace btproxy
