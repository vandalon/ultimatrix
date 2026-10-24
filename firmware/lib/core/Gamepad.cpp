// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Gamepad.h"
#include "Console.h"
#include "HidReport.h"
#include <atomic>
#include "esp_heap_caps.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
extern "C" void ble_store_config_init(void);

#include "nvs.h"
#include "esp_bt.h"

namespace gamepad {
namespace {
int8_t enabledCache = -1;   // decided once, early in the boot: the memory of the Bluetooth stack is kept or handed back to the heap then

const uint16_t kNoConn = 0xFFFF;
const uint32_t kPairMs = 60000;
const int kMaxChars = 10;
const size_t kMapMax = 512;
const ble_uuid16_t kHidService = BLE_UUID16_INIT(0x1812);

struct Chr { uint16_t valHandle, cccd, refHandle; uint8_t props, id, type; bool report; };

std::atomic<bool> started{false}, synced{false};
std::atomic<uint16_t> conn{kNoConn};
std::atomic<bool> connecting{false}, scanning{false}, ready{false};
std::atomic<uint32_t> pairUntil{0};
std::atomic<int> latest{hid::kNeutral};
std::atomic<uint32_t> stamp{0};
uint32_t taken = 0;
std::atomic<int> bonds{0};
uint8_t ownAddrType = 0;
char peerName[24] = "";
ble_addr_t refused; uint32_t refusedAt = 0; bool haveRefused = false;
unsigned long lastPoll = 0;

// Discovery of the HID service on the connected controller, one step at a time
Chr chrs[kMaxChars]; int nChrs = 0, step = 0, stepIndex = 0;
uint16_t svcStart = 0, svcEnd = 0, mapHandle = 0;
uint8_t mapBuf[kMapMax]; size_t mapLen = 0;
hid::Layout layout;
int lastDir = hid::kNeutral;

bool pairing() { return pairUntil.load() && (int32_t)(pairUntil.load() - millis()) > 0; }

int gapEvent(struct ble_gap_event *event, void *arg);
void nextStep();

void dropConnection(int reason = BLE_ERR_REM_USER_CONN_TERM) {
  const uint16_t c = conn.load();
  if (c != kNoConn) ble_gap_terminate(c, reason);
}

// The HID service is found, then its characteristics, descriptors, report ids and report map; at the end the reports are subscribed
int svcCb(uint16_t, const struct ble_gatt_error *error, const struct ble_gatt_svc *service, void *) {
  if (error->status == 0 && service) { svcStart = service->start_handle; svcEnd = service->end_handle; }
  else if (error->status == BLE_HS_EDONE) { if (svcEnd) { step++; stepIndex = 0; nextStep(); } else { consoleLog("[Gamepad] no HID service\n"); dropConnection(); } }
  else dropConnection();
  return 0;
}
int chrCb(uint16_t, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *) {
  if (error->status == 0 && chr) {
    if (nChrs < kMaxChars) {
      Chr &c = chrs[nChrs++]; c = Chr{chr->val_handle, 0, 0, chr->properties, 0, 1, false};
      if (chr->uuid.u.type == BLE_UUID_TYPE_16) {
        const uint16_t u = ble_uuid_u16(&chr->uuid.u);
        c.report = u == 0x2A4D;
        if (u == 0x2A4B) mapHandle = chr->val_handle;
      }
    }
  } else if (error->status == BLE_HS_EDONE) { step++; stepIndex = 0; nextStep(); }
  else dropConnection();
  return 0;
}
int dscCb(uint16_t, const struct ble_gatt_error *error, uint16_t, const struct ble_gatt_dsc *dsc, void *) {
  if (error->status == 0 && dsc) {
    int owner = -1; for (int i = 0; i < nChrs; i++) if (chrs[i].valHandle < dsc->handle) owner = i;
    if (owner >= 0 && dsc->uuid.u.type == BLE_UUID_TYPE_16) {
      const uint16_t u = ble_uuid_u16(&dsc->uuid.u);
      if (u == 0x2902) chrs[owner].cccd = dsc->handle;
      else if (u == 0x2908) chrs[owner].refHandle = dsc->handle;
    }
  } else if (error->status == BLE_HS_EDONE) { step++; stepIndex = 0; nextStep(); }
  else dropConnection();
  return 0;
}
int readCb(uint16_t, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *) {
  if (error->status == 0 && attr && attr->om) {
    if (step == 3 && stepIndex < nChrs) {                 // report reference: id, type
      uint8_t b[2] = {0, 1}; os_mbuf_copydata(attr->om, 0, OS_MBUF_PKTLEN(attr->om) < 2 ? OS_MBUF_PKTLEN(attr->om) : 2, b);
      chrs[stepIndex].id = b[0]; chrs[stepIndex].type = b[1];
      stepIndex++; nextStep();
    } else if (step == 4) {                               // report map, in pieces
      const size_t n = OS_MBUF_PKTLEN(attr->om);
      if (mapLen + n <= kMapMax) { os_mbuf_copydata(attr->om, 0, n, mapBuf + mapLen); mapLen += n; }
    }
  } else if (error->status == BLE_HS_EDONE) {
    if (step == 4) { step++; stepIndex = 0; nextStep(); }
  } else if (step == 3) { stepIndex++; nextStep(); }      // a missing report reference is not fatal
  else if (step == 4) { step++; stepIndex = 0; nextStep(); }   // nor is a missing report map
  else dropConnection();
  return 0;
}
int writeCb(uint16_t, const struct ble_gatt_error *error, struct ble_gatt_attr *, void *) {
  stepIndex++; nextStep();
  return 0;
}

// step 0 service, 1 characteristics, 2 descriptors, 3 report references, 4 report map, 5 subscribe, 6 done
void nextStep() {
  const uint16_t c = conn.load(); if (c == kNoConn) return;
  int rc = 0;
  switch (step) {
    case 0: svcEnd = 0; nChrs = 0; mapHandle = 0; mapLen = 0; layout.n = 0;
            rc = ble_gattc_disc_svc_by_uuid(c, &kHidService.u, svcCb, nullptr); break;
    case 1: rc = ble_gattc_disc_all_chrs(c, svcStart, svcEnd, chrCb, nullptr); break;
    case 2: rc = ble_gattc_disc_all_dscs(c, svcStart, svcEnd, dscCb, nullptr); break;
    case 3: while (stepIndex < nChrs && !(chrs[stepIndex].report && chrs[stepIndex].refHandle)) stepIndex++;
            if (stepIndex < nChrs) { rc = ble_gattc_read(c, chrs[stepIndex].refHandle, readCb, nullptr); break; }
            step++; stepIndex = 0; nextStep(); return;
    case 4: if (mapHandle) { rc = ble_gattc_read_long(c, mapHandle, 0, readCb, nullptr); break; }
            step++; stepIndex = 0; nextStep(); return;
    case 5: if (step == 5 && stepIndex == 0) layout.parse(mapBuf, mapLen);
            while (stepIndex < nChrs && !(chrs[stepIndex].report && chrs[stepIndex].cccd && (chrs[stepIndex].props & BLE_GATT_CHR_PROP_NOTIFY) && chrs[stepIndex].type == 1)) stepIndex++;
            if (stepIndex < nChrs) { const uint8_t on[2] = {1, 0}; rc = ble_gattc_write_flat(c, chrs[stepIndex].cccd, on, 2, writeCb, nullptr); break; }
            step++; ready = true;
            consoleLog("[Gamepad] ready: %s, %d reports, %d steering fields\n", peerName[0] ? peerName : "controller", nChrs, layout.n);
            return;
    default: return;
  }
  if (rc != 0) { consoleLog("[Gamepad] step %d failed: %d\n", step, rc); dropConnection(); }
}

void startScan();
int scanEvent(struct ble_gap_event *event, void *) {
  if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) { scanning = false; return 0; }
  if (event->type != BLE_GAP_EVENT_DISC) return 0;
  const struct ble_gap_disc_desc &d = event->disc;
  struct ble_hs_adv_fields f;
  if (ble_hs_adv_parse_fields(&f, d.data, d.length_data) != 0) return 0;
  bool hidDevice = false;
  for (int i = 0; i < f.num_uuids16; i++) if (f.uuids16[i].value == 0x1812) hidDevice = true;
  if (f.appearance_is_present && (f.appearance == 0x03C4 || f.appearance == 0x03C3)) hidDevice = true;
  if (d.event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND) hidDevice = true;   // a bonded controller calling us by address
  if (!hidDevice || connecting) return 0;
  if (haveRefused && millis() - refusedAt < 60000 && !memcmp(&refused, &d.addr, sizeof refused)) return 0;   // a controller that is not ours
  const bool pair = pairing();
  if (pair && d.rssi < -80) return 0;                      // pairing takes the one in your hand, not one across the room
  if (f.name && f.name_len) { size_t n = f.name_len < sizeof(peerName) - 1 ? f.name_len : sizeof(peerName) - 1; memcpy(peerName, f.name, n); peerName[n] = 0; }
  consoleLog("[Gamepad] %s: connecting to %s (%d dBm)\n", pair ? "pairing" : "reconnecting", peerName[0] ? peerName : "a controller", d.rssi);
  ble_gap_disc_cancel(); scanning = false;
  connecting = true;
  const int rc = ble_gap_connect(ownAddrType, &d.addr, 6000, nullptr, gapEvent, nullptr);
  if (rc != 0) { connecting = false; consoleLog("[Gamepad] connect failed: %d\n", rc); }
  return 0;
}
void startScan() {
  if (scanning || connecting || conn.load() != kNoConn || !synced) return;
  struct ble_gap_disc_params p = {};
  const bool pair = pairing();
  p.itvl = pair ? 160 : 800; p.window = pair ? 80 : 60;    // half of the time while pairing, a few percent when only waiting for a bonded one
  p.filter_duplicates = 1;
  const int rc = ble_gap_disc(ownAddrType, BLE_HS_FOREVER, &p, scanEvent, nullptr);
  if (rc == 0) scanning = true; else if (rc != BLE_HS_EALREADY) consoleLog("[Gamepad] scan failed: %d\n", rc);
}

int gapEvent(struct ble_gap_event *event, void *) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
      connecting = false;
      if (event->connect.status != 0) { conn = kNoConn; return 0; }
      conn = event->connect.conn_handle; ready = false;
      struct ble_gap_conn_desc desc;
      if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0 && !pairing()) {
        struct ble_store_key_sec key = {}; struct ble_store_value_sec value;
        key.peer_addr = desc.peer_id_addr;
        if (ble_store_read_peer_sec(&key, &value) != 0) {  // nobody we know: do not pair outside the pairing minute
          refused = desc.peer_ota_addr; refusedAt = millis(); haveRefused = true;
          dropConnection(); return 0;
        }
      }
      const int rc = ble_gap_security_initiate(event->connect.conn_handle);
      if (rc != 0) { consoleLog("[Gamepad] security failed: %d\n", rc); dropConnection(); }
      return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT:
      conn = kNoConn; connecting = false; ready = false; latest = hid::kNeutral; lastDir = hid::kNeutral;
      return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
      if (event->enc_change.status == 0) {
        pairUntil = 0; step = 0; stepIndex = 0; nextStep();
      } else {
        consoleLog("[Gamepad] encryption failed: %d\n", event->enc_change.status);
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0 && event->enc_change.status == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING))
          ble_store_util_delete_peer(&desc.peer_id_addr);  // the controller forgot us: pair again
        dropConnection();
      }
      return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      struct ble_gap_conn_desc desc;
      if (pairing() && ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
        ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
      }
      return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    case BLE_GAP_EVENT_NOTIFY_RX: {
      if (!ready) return 0;
      for (int i = 0; i < nChrs; i++) {
        if (chrs[i].valHandle != event->notify_rx.attr_handle || !chrs[i].report) continue;
        uint8_t data[32]; size_t len = OS_MBUF_PKTLEN(event->notify_rx.om); if (len > sizeof data) len = sizeof data;
        os_mbuf_copydata(event->notify_rx.om, 0, len, data);
        const uint8_t id = (chrs[i].refHandle || layout.n == 0) ? chrs[i].id : layout.f[0].id;
        const int dir = layout.direction(id, data, len, lastDir);
        if (dir != hid::kNone) { lastDir = dir; latest = dir; stamp++; }
        break;
      }
      return 0;
    }
    case BLE_GAP_EVENT_MTU: return 0;
    default: return 0;
  }
}

void onSync() {
  if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &ownAddrType) != 0) { consoleLog("[Gamepad] no Bluetooth address\n"); return; }
  synced = true;
}
void hostTask(void *) { nimble_port_run(); nimble_port_freertos_deinit(); }
}  // namespace

// Off unless switched on on the settings page (NVS key btPad), and always off for an update-only boot (the whole heap is for the download).
bool enabled() {
  if (enabledCache >= 0) return enabledCache;
  uint8_t on = 0; nvs_handle_t h; size_t n = 0;
  if (nvs_open("ultimatrix", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "btPad", &on); nvs_close(h); }
  if (on && nvs_open("um-updates", NVS_READONLY, &h) == ESP_OK) { if (nvs_get_str(h, "pending", nullptr, &n) == ESP_OK && n > 1) on = 0; nvs_close(h); }
  enabledCache = on ? 1 : 0;
  return on;
}

void setEnabled(bool on) {
  nvs_handle_t h;
  if (nvs_open("ultimatrix", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, "btPad", on ? 1 : 0); nvs_commit(h); nvs_close(h);
}

// First thing in setup(): hands the memory the Bluetooth controller does not need back to the heap (all of it unless the controller is on).
void boot() { esp_bt_controller_mem_release(enabled() ? ESP_BT_MODE_CLASSIC_BT : ESP_BT_MODE_BTDM); }

void begin() {
  if (!enabled() || started.exchange(true)) return;
  const size_t before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const esp_err_t err = nimble_port_init();
  if (err != ESP_OK) { consoleLog("[Gamepad] Bluetooth did not start: %s (free %u, largest block %u)\n", esp_err_to_name(err), (unsigned)before, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)); return; }
  ble_hs_cfg.sync_cb = onSync;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_bonding = 1; ble_hs_cfg.sm_mitm = 0; ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_store_config_init();
  nimble_port_freertos_init(hostTask);
  consoleLog("[Gamepad] Bluetooth started: free heap %u -> %u\n", (unsigned)before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
}

void poll() {
  if (!synced || millis() - lastPoll < 250) return;
  lastPoll = millis();
  int n = 0; if (ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &n) == 0) bonds = n;
  const bool want = pairing() || bonds.load() > 0;
  if (conn.load() == kNoConn && !connecting) {
    if (want && !scanning) startScan();
    else if (!want && scanning) { ble_gap_disc_cancel(); scanning = false; }
    else if (scanning && pairUntil.load() && !pairing()) { ble_gap_disc_cancel(); scanning = false; }   // the pairing minute ended: back to the slow scan
  }
}

bool take(int &dir) {
  const uint32_t s = stamp.load();
  if (s == taken) return false;
  taken = s;
  const int d = latest.load();
  if (d < 0) return false;
  dir = d;
  return true;
}

void pair() {
  if (!synced) return;
  pairUntil = millis() + kPairMs;
  if (scanning) { ble_gap_disc_cancel(); scanning = false; }   // restart with the faster pairing scan
  if (conn.load() != kNoConn) dropConnection();
}

void forget() {
  pairUntil = 0;
  dropConnection();
  if (scanning) { ble_gap_disc_cancel(); scanning = false; }
  ble_store_clear();
  bonds = 0; peerName[0] = 0;
}

const char *state() {
  if (!enabled()) return "off";
  if (!started) return "starting";
  if (!synced) return "starting";
  if (conn.load() != kNoConn) return ready ? "connected" : "connecting";
  if (connecting) return "connecting";
  if (pairing()) return "pairing";
  return bonds.load() > 0 ? "searching" : "ready";
}
const char *name() { return peerName; }
int bonded() { return bonds.load(); }
}  // namespace gamepad

// The Arduino core hands the Bluetooth memory back to the heap at boot unless the sketch says it uses LE (classic stays released).
extern "C" bool bleInUse() { return gamepad::enabled(); }
