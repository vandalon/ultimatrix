#include "FirmwareUpdate.h"
#include "UpdateTrust.h"
#include "Console.h"
#include "NetworkBudget.h"
#include <esp_http_client.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_app_desc.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <memory>
#include <time.h>
#include <Preferences.h>
#ifdef UM_GAMEPAD
#include "Gamepad.h"
#endif

extern "C" bool verifyRollbackLater() { return true; }

namespace {
constexpr char kMode[] = UM_MODE_ID;
constexpr size_t kServerMax = 96;
constexpr size_t kMinFreeBlock = 16 * 1024;  // estimate for the HTTP client and hashing; tune from device logs
constexpr size_t kMinFreeBlockTls = 28 * 1024;  // GitHub is HTTPS: one TLS session at a time
constexpr size_t kMinFreeBlockCheck = 12 * 1024;  // a manifest check is small (TLS buffers are allocated as records arrive); a check succeeded on a clean boot with 1.6 KB largest block left
constexpr uint32_t kTaskStack = 12288;  // TLS handshake plus certificate checks need more than plain HTTP
constexpr int kMaxRedirects = 4;

// Where updates come from: the project's GitHub release (default) or a private server saved in NVS.
// GitHub release assets are flat, so a file is named "<mode>-<name>"; a private server uses "<mode>/<name>".
bool customServer(char *out, size_t size) {
  Preferences prefs; out[0] = 0;
  if (!prefs.begin("um-updates", true)) return false;
  strlcpy(out, prefs.getString("server", "").c_str(), size); prefs.end();
  return out[0] != 0;
}
void fileUrl(char *url, size_t size, const char *mode, const char *name) {
  char server[kServerMax];
  if (customServer(server, sizeof(server))) snprintf(url, size, "%s/%s/%s", server, mode, name);
  else snprintf(url, size, "https://github.com/%s/releases/latest/download/%s-%s", kUpdateRepo, mode, name);
}
// Replay history is kept per source, so test releases on a private server do not block the GitHub ones.
void historyKey(char *key, size_t size, const char *mode) {
  char server[kServerMax];
  snprintf(key, size, customServer(server, sizeof(server)) ? "%s-p" : "%s", mode);
}
// Releases are numbered by packaging time, so none can be older than the running build. This floor survives a factory
// reset (which wipes the history above), so a stale signed image cannot be replayed to a clock that has no entry for it.
// The margin covers build-machine and device time zones.
uint32_t releaseFloor() {
  struct tm built = {};
  const esp_app_desc_t *app = esp_app_get_description();
  char stamp[40]; snprintf(stamp, sizeof(stamp), "%s %s", app->date, app->time);
  if (!strptime(stamp, "%b %d %Y %H:%M:%S", &built)) return 0;
  built.tm_isdst = 0;
  time_t at = mktime(&built);
  const time_t kMargin = 2 * 24 * 3600;
  return at > kMargin ? (uint32_t)(at - kMargin) : 0;
}
size_t minFreeBlock() {
  char url[9]; fileUrl(url, sizeof(url), "", "");
  return strncmp(url, "https://", 8) ? kMinFreeBlock : kMinFreeBlockTls;
}
struct HttpFile {
  esp_http_client_handle_t handle = nullptr;
  bool tls = false;
  HttpFile(const char *mode, const char *name) {
    char url[256]; fileUrl(url, sizeof(url), mode, name);
    tls = !strncmp(url, "https://", 8);
    esp_http_client_config_t config{};
    config.url = url; config.timeout_ms = 10000; config.buffer_size = tls ? 3072 : 1024; config.buffer_size_tx = tls ? 2048 : 512;  // GitHub redirects to a ~700 character signed asset URL
    config.disable_auto_redirect = true;  // followed by hand in open(), HTTPS only
    config.skip_cert_common_name_check = true;  // TLS without certificate checks, like the feeds; the signed manifest protects the image
    handle = esp_http_client_init(&config);
    if (handle) esp_http_client_set_header(handle, "Accept-Encoding", "identity");
  }
  bool open() {
    if (!handle) return false;
    for (int hop = 0; hop <= kMaxRedirects; ++hop) {
      esp_err_t opened = esp_http_client_open(handle, 0);
      int64_t headers = opened == ESP_OK ? esp_http_client_fetch_headers(handle) : -1;
      if (opened != ESP_OK || headers < 0) {
        consoleLog("[Update] hop %d: open %s, headers %d, free %u, largest %u\n", hop, esp_err_to_name(opened), (int)headers,
                   (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return false;
      }
      int code = esp_http_client_get_status_code(handle);
      if (code == 200) return true;
      consoleLog("[Update] hop %d: HTTP %d\n", hop, code);
      bool redirect = code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
      if (!tls || !redirect || esp_http_client_set_redirection(handle) != ESP_OK) { consoleLog("[Update] redirect not followed\n"); return false; }
      char next[9] = "";  // a redirect must stay on HTTPS
      esp_http_client_get_url(handle, next, sizeof(next));
      esp_http_client_close(handle);
      if (strncmp(next, "https://", 8)) return false;
    }
    return false;
  }
  int read(uint8_t *bytes, size_t size) { return esp_http_client_read(handle, reinterpret_cast<char *>(bytes), size); }
  ~HttpFile() { if (handle) esp_http_client_cleanup(handle); }
};
int smallFile(const char *mode, const char *name, uint8_t *bytes, size_t capacity) {
  HttpFile file(mode, name); if (!file.open()) return -1;
  size_t used = 0;
  while (used < capacity) {
    int got = file.read(bytes + used, capacity - used);
    if (got < 0) return -1;
    if (!got) return used;
    used += got;
  }
  uint8_t extra;
  return file.read(&extra, 1) == 0 ? (int)used : -1;
}
bool signedManifest(const uint8_t *bytes, size_t size, const uint8_t *signature, size_t signatureSize) {
  uint8_t hash[32]; mbedtls_sha256(bytes, size, hash, 0);
  mbedtls_pk_context key; mbedtls_pk_init(&key);
  bool valid = mbedtls_pk_parse_public_key(&key, reinterpret_cast<const uint8_t *>(kUpdatePublicKey), sizeof(kUpdatePublicKey)) == 0 &&
      mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, hash, sizeof(hash), signature, signatureSize) == 0;
  mbedtls_pk_free(&key); return valid;
}
const char *text(cJSON *root, const char *name) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  return cJSON_IsString(item) ? item->valuestring : "";
}
bool hashBytes(const char *hex, uint8_t out[32]) {
  if (strlen(hex) != 64) return false;
  for (int i = 0; i < 32; ++i) {
    unsigned value = 0;
    for (int j = 0; j < 2; ++j) {
      char c = hex[i * 2 + j];
      unsigned digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 99;
      if (digit > 15) return false;
      value = value * 16 + digit;
    }
    out[i] = value;
  }
  return true;
}
struct ImageWrite {
  esp_ota_handle_t handle = 0;
  bool active = false;
  ~ImageWrite() { if (active) esp_ota_abort(handle); }
};
struct Manifest {
  char image[96], version[32];
  uint8_t sha256[32], elfSha256[32];
  uint32_t size, release;
  bool current, replayed;  // same build as the running one; release not newer than the last installed
};
// Downloads and verifies the signed manifest for a mode. Returns null, or the message to show.
const char *fetchManifest(const char *target, Manifest &out, const esp_partition_t *partition) {
  uint8_t manifest[769], signature[128];
  int size = smallFile(target, "manifest.json", manifest, sizeof(manifest) - 1);
  int signatureSize = size > 0 ? smallFile(target, "manifest.sig", signature, sizeof(signature)) : -1;
  if (size <= 0 || signatureSize <= 0) return "Could not download the update manifest.";
  if (!signedManifest(manifest, size, signature, signatureSize)) return "Update signature is invalid. Nothing was flashed.";
  manifest[size] = 0;
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> info(cJSON_Parse(reinterpret_cast<char *>(manifest)), cJSON_Delete);
  if (!info) return "Update manifest could not be read.";
  cJSON *schema = cJSON_GetObjectItemCaseSensitive(info.get(), "schema");
  cJSON *length = cJSON_GetObjectItemCaseSensitive(info.get(), "size");
  const char *image = text(info.get(), "image");
  bool filename = strlen(image) > 4 && strlen(image) < sizeof(out.image) && strstr(image, "..") == nullptr;
  for (const char *p = image; *p; ++p) filename &= isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == '.';
  if (!schema || schema->valueint != 1 || strcmp(text(info.get(), "mode"), target) || !filename || !partition ||
      !cJSON_IsNumber(length) || length->valuedouble < 288 || length->valuedouble > partition->size ||
      length->valuedouble != length->valueint || !hashBytes(text(info.get(), "sha256"), out.sha256) ||
      !hashBytes(text(info.get(), "elfSha256"), out.elfSha256)) return "Update manifest does not match this clock.";
  out.size = length->valueint;
  strlcpy(out.image, image, sizeof(out.image));
  strlcpy(out.version, text(info.get(), "version"), sizeof(out.version));
  out.current = !memcmp(out.elfSha256, esp_app_get_description()->app_elf_sha256, sizeof(out.elfSha256));
  cJSON *release = cJSON_GetObjectItemCaseSensitive(info.get(), "release");
  if (!out.current && (!cJSON_IsNumber(release) || release->valuedouble < 1 || release->valuedouble > UINT32_MAX ||
      release->valuedouble != (uint32_t)release->valuedouble)) return "Update release identifier is invalid.";
  out.release = out.current ? 0 : (uint32_t)release->valuedouble;
  out.replayed = false;
  if (!out.current) {
    Preferences history;
    if (!history.begin("um-updates", false)) return "Cannot open update history.";
    char key[24]; historyKey(key, sizeof(key), target);
    uint32_t accepted = history.getUInt(key, 0);
    uint8_t digest[32]; char digestKey[28]; snprintf(digestKey, sizeof(digestKey), "%sd", key);
    bool known = history.getBytes(digestKey, digest, sizeof(digest)) == sizeof(digest);
    // The same authenticated release may be installed again (switching back to a face); older ones, or other content under the same number, may not
    out.replayed = out.release < releaseFloor() || out.release < accepted || (out.release == accepted && known && memcmp(digest, out.elfSha256, sizeof(digest)));
    history.end();
  }
  return nullptr;
}
}

const char *FirmwareUpdate::version() const { return esp_app_get_description()->version; }
void FirmwareUpdate::status(Status &out) { std::lock_guard<std::mutex> guard(_lock); out = _status; }
void FirmwareUpdate::message(const char *value, bool failed) {
  std::lock_guard<std::mutex> guard(_lock);
  strlcpy(_status.message, value, sizeof(_status.message)); _status.failed = failed;
  consoleLog("[Firmware] %s%s\n", failed ? "FAILED: " : "", value);
}
void FirmwareUpdate::progress(uint32_t bytes, uint32_t total) {
  std::lock_guard<std::mutex> guard(_lock); _status.received = bytes; _status.total = total;
}

bool FirmwareUpdate::knownMode(const char *mode) {
  static const char *const modes[] = {"home", "football", "f1", "motogp", "tetris", "word", "mario", "luigi", "pacman", "pacmanclassic"};
  for (const char *known : modes) if (mode && !strcmp(mode, known)) return true;
  return false;
}

bool FirmwareUpdate::server(char *out, size_t size) { return customServer(out, size); }

// Empty restores the GitHub release. Accepts only a plain http(s) base URL; every update is still signature-checked.
bool FirmwareUpdate::setServer(const char *url) {
  char value[kServerMax]; strlcpy(value, url ? url : "", sizeof(value));
  size_t length = strlen(value);
  while (length && value[length - 1] == '/') value[--length] = 0;
  if (length) {
    if (strlen(url) >= sizeof(value) || (strncmp(value, "http://", 7) && strncmp(value, "https://", 8))) return false;
    for (const char *p = value; *p; ++p) if (!isprint((unsigned char)*p) || isspace((unsigned char)*p) || *p == '"' || *p == '\\') return false;
  }
  Preferences prefs;
  if (!prefs.begin("um-updates", false)) return false;
  if (length) prefs.putString("server", value); else prefs.remove("server");
  prefs.remove("chk"); prefs.remove("availElf"); prefs.remove("availVer");  // the cached check was about the old source
  prefs.end();
  std::lock_guard<std::mutex> guard(_lock);
  _latest.available = false; _latest.version[0] = 0; _latest.error[0] = 0; _latest.checkedAt = 0; _nextCheck = 0;
  return true;
}

void FirmwareUpdate::scheduleAfterReboot(const char *mode) {
  Preferences prefs;
  if (prefs.begin("um-updates", false)) { prefs.putString("pending", mode && *mode ? mode : kMode); prefs.end(); }
}

static bool bootForUpdate = false;  // this boot was restarted to install an update: no second restart

bool FirmwareUpdate::takeScheduled(char *mode, size_t size) {
  Preferences prefs;
  if (!prefs.begin("um-updates", false)) return false;
  String pending = prefs.getString("pending", "");
  if (pending.length()) prefs.remove("pending");
  prefs.end();
  if (!pending.length() || !knownMode(pending.c_str())) return false;
  strlcpy(mode, pending.c_str(), size);
  bootForUpdate = true;
  return true;
}

bool FirmwareUpdate::request(const char *mode, bool force) {
  std::lock_guard<std::mutex> guard(_lock);
  if (_status.busy || _status.ready || _latest.checking || _workerRunning) return false;
  std::unique_lock<std::mutex> gate(networkBudget(), std::try_to_lock);
  if (!gate.owns_lock() || (liveDataActive() && !force)) {
    _status.lowMemory = false;
    strlcpy(_status.message, "A live match or session is running. It is not paused to free memory for the update; try again when it has ended.", sizeof(_status.message));
    return false;
  }
  if (_lastAttempt && millis() - _lastAttempt < 30000) { strlcpy(_status.message, "Wait 30 seconds before checking again.", sizeof(_status.message)); return false; }
  _lastAttempt = millis();
  strlcpy(_target, mode && *mode ? mode : kMode, sizeof(_target));
  _status = Status{"Checking signed update manifest.", 0, 0, true, false, false, false};
  firmwareUpdating = true;
  consoleLog("[Firmware] Install queued for %s\n", _target);
  _pending = Work::Install; _force = force;
  _stoppingAt = millis();
  strlcpy(_status.message, "Stopping feeds before updating.", sizeof(_status.message));
  return true;
}

void FirmwareUpdate::task(void *self) {
  auto &update = *static_cast<FirmwareUpdate *>(self);
  firmwareUpdating = true;
  {
    // Feed tasks and their stacks have already been reclaimed by service().
    std::unique_lock<std::mutex> gate(networkBudget());
#ifdef UM_HOME
    const bool alwaysReboot = !bootForUpdate;  // the Home face keeps too much live data to download beside it: always install after a restart
#elif defined(UM_GAMEPAD)
    const bool alwaysReboot = !bootForUpdate && gamepad::enabled();  // Bluetooth holds ~75 KB: install after a restart, which never starts it
#else
    const bool alwaysReboot = false;
#endif
    if (alwaysReboot || heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < minFreeBlock()) {
      update.message(alwaysReboot ? "Restarting to install the update." : "Not enough free memory while the clock is running. Restarting to install the update.", true);
      std::lock_guard<std::mutex> guard(update._lock); update._status.lowMemory = true;
    } else update.run();
  }
  { std::lock_guard<std::mutex> guard(update._lock); update._finishedAt = millis() ? millis() : 1; }
  vTaskSuspend(nullptr);  // service() deletes this suspended worker before resuming feeds
}

void FirmwareUpdate::run() {
  const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
  Manifest info;
  if (const char *error = fetchManifest(_target, info, partition)) { message(error, true); return; }
  if (info.current) { message("Your firmware is already up to date."); return; }
  if (info.replayed) { message("This update release has already been used or superseded.", true); return; }
  const uint8_t *expected = info.sha256, *elfHash = info.elfSha256;
  uint32_t releaseId = info.release;
  Preferences history;
  uint32_t total = info.size;
  progress(0, total); message("Downloading and verifying application firmware.");
  HttpFile file(_target, info.image);
  if (!file.open()) { message("Could not open the firmware image.", true); return; }
  uint8_t buffer[1024];
  constexpr size_t headerSize = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
  size_t headerBytes = 0;
  while (headerBytes < headerSize) {
    int got = file.read(buffer + headerBytes, headerSize - headerBytes);
    if (got <= 0) { message("Firmware header is incomplete.", true); return; }
    headerBytes += got;
  }
  esp_image_header_t imageHeader; esp_app_desc_t description;
  memcpy(&imageHeader, buffer, sizeof(imageHeader));
  memcpy(&description, buffer + sizeof(imageHeader) + sizeof(esp_image_segment_header_t), sizeof(description));
  if (imageHeader.magic != ESP_IMAGE_HEADER_MAGIC || imageHeader.chip_id != ESP_CHIP_ID_ESP32 ||
      description.magic_word != ESP_APP_DESC_MAGIC_WORD || strncmp(description.project_name, "ultimatrix", sizeof(description.project_name)) ||
      strncmp(description.version, info.version, sizeof(description.version)) || memcmp(description.app_elf_sha256, elfHash, sizeof(info.elfSha256))) {
    message("This is not a matching Ultimatrix application image.", true); return;
  }
  ImageWrite write;
  if (esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &write.handle) != ESP_OK) { message("Cannot begin update. Startup may still need verification.", true); return; }
  write.active = true;
  mbedtls_sha256_context hash; mbedtls_sha256_init(&hash); mbedtls_sha256_starts(&hash, 0);
  uint32_t received = 0;
  int got = headerBytes;
  bool valid = true;
  while (received < total) {
    if (got <= 0 || got > (int)(total - received) || esp_ota_write(write.handle, buffer, got) != ESP_OK) { valid = false; break; }
    mbedtls_sha256_update(&hash, buffer, got); received += got; progress(received, total);
    if (received < total) got = file.read(buffer, std::min<size_t>(sizeof(buffer), total - received));
    vTaskDelay(1);
  }
  uint8_t actual[32], extra;
  mbedtls_sha256_finish(&hash, actual); mbedtls_sha256_free(&hash);
  if (!valid || received != total || memcmp(actual, expected, sizeof(actual)) || file.read(&extra, 1) != 0) {
    message("Firmware transfer or checksum failed. Current image retained.", true); return;
  }
  esp_err_t result = esp_ota_end(write.handle); write.active = false;
  if (result != ESP_OK || esp_ota_set_boot_partition(partition) != ESP_OK) { message("Firmware validation failed. Current image retained.", true); return; }
  // Record the authenticated release only once the image is active, so a failed activation does not use the release up.
  if (history.begin("um-updates", false)) {
    char key[24]; historyKey(key, sizeof(key), _target);
    char digestKey[28]; snprintf(digestKey, sizeof(digestKey), "%sd", key);
    if (releaseId >= history.getUInt(key, 0)) { history.putUInt(key, releaseId); history.putBytes(digestKey, elfHash, sizeof(info.elfSha256)); }
    history.end();
  }
  if (history.begin("um-updates", false)) {  // the cached "update available" is about the build that was just replaced
    history.remove("availElf"); history.remove("availVer"); history.remove("chk"); history.end();
  }
  message(strcmp(_target, kMode) ? "Clock face installed. Restarting Ultimatrix." : "Firmware verified. Restarting Ultimatrix.");
  std::lock_guard<std::mutex> guard(_lock); _status.ready = true;
}

namespace {
constexpr uint32_t kConfirmDeadlineMs = 180000;  // a pending image that is not healthy by now is rolled back
constexpr uint32_t kCheckEveryMs = 24 * 3600 * 1000UL;  // once a day
constexpr uint32_t kRetryMs = 30 * 60 * 1000UL;
constexpr uint32_t kFirstCheckS = 45;  // after the clock has settled
constexpr time_t kValidEpoch = 1700000000;
uint32_t at(uint32_t when) { return when ? when : 1; }  // 0 means "not scheduled"
}

void FirmwareUpdate::begin() {
#ifdef UM_BRIDGE
  {  // An old clock has no update server saved here: use the bridge's, and keep it for the face that gets installed
    char saved[kServerMax];
    if (!customServer(saved, sizeof(saved))) setServer(UM_BRIDGE_SERVER);
  }
#endif
  Preferences prefs;
  if (!prefs.begin("um-updates", true)) return;
  std::lock_guard<std::mutex> guard(_lock);
  // The cache belongs to the clock mode that wrote it; another mode's result (after a face switch or a reflash) is ignored
  if (strcmp(prefs.getString("chkMode", "").c_str(), kMode)) { prefs.end(); return; }
  _latest.checkedAt = prefs.getUInt("chk", 0);
  uint8_t elf[32];
  if (prefs.getBytes("availElf", elf, sizeof(elf)) == sizeof(elf) && memcmp(elf, esp_app_get_description()->app_elf_sha256, sizeof(elf))) {
    _latest.available = true;
    strlcpy(_latest.version, prefs.getString("availVer", "").c_str(), sizeof(_latest.version));
  }
  prefs.end();
  consoleLog("[Firmware] Cached: checked=%u available=%d %s\n", (unsigned)_latest.checkedAt, _latest.available, _latest.version);
}

void FirmwareUpdate::latest(Latest &out) { std::lock_guard<std::mutex> guard(_lock); out = _latest; }

bool FirmwareUpdate::startCheck(bool force) {  // _lock held
  if (_latest.checking || _status.busy || _status.ready || _workerRunning || _pending != Work::None) return false;
  std::unique_lock<std::mutex> gate(networkBudget(), std::try_to_lock);
  if (!gate.owns_lock() || (liveDataActive() && !force)) return false;
  consoleLog("[Firmware] Check queued%s\n", force ? " (forced during a live session)" : "");
  _force = force;
  _latest.checking = true;
  firmwareUpdating = true;
  _pending = Work::Check;
  _stoppingAt = millis();
  return true;
}

bool FirmwareUpdate::checkNow(bool force) {
  std::lock_guard<std::mutex> guard(_lock);
  if (_lastManual && millis() - _lastManual < 30000) return false;
  if (!startCheck(force)) return false;
  _lastManual = at(millis());
  return true;
}

bool FirmwareUpdate::checkDue() {
#ifdef UM_BRIDGE
  return false;  // the bridge has no signed release of its own
#endif
  time_t epoch = time(nullptr);
  std::lock_guard<std::mutex> guard(_lock);
  return epoch >= kValidEpoch && (!_latest.checkedAt || (uint32_t)epoch < _latest.checkedAt || (uint32_t)epoch - _latest.checkedAt >= kCheckEveryMs / 1000);
}

void FirmwareUpdate::service(uint32_t now) {
  bool stopped = reapFeedTasks();
  std::lock_guard<std::mutex> guard(_lock);
  if (_workerRunning) {
    if (_finishedAt && eTaskGetState(_worker) == eSuspended &&
        xTaskGetCurrentTaskHandleForCore(0) != _worker && xTaskGetCurrentTaskHandleForCore(1) != _worker) {
      vTaskDelete(_worker);
      _worker = nullptr; _workerRunning = false; _finishedAt = 0;
      _status.busy = false; _latest.checking = false;
      firmwareUpdating = false;
      consoleLog("[Firmware] Worker stopped; feeds may resume.\n");
    }
    return;
  }
  if (_pending == Work::None) return;
  // signed: request() may run later in the same loop pass than the caller's `now`, which would wrap an unsigned difference
  if ((liveDataActive() && !_force) || (int32_t)(now - _stoppingAt) >= 30000) {
    bool checking = _pending == Work::Check;
    _pending = Work::None; _status.busy = false; _latest.checking = false;
    _nextCheck = at(now + kRetryMs);
    const char *error = (liveDataActive() && !_force) ? "A live match or session is running, so memory is not freed for the check. It tries again later." : "Feeds did not stop in time; try again later.";
    if (checking) strlcpy(_latest.error, error, sizeof(_latest.error));
    else { strlcpy(_status.message, error, sizeof(_status.message)); _status.failed = true; }
    firmwareUpdating = false;
    return;
  }
  if (!stopped) return;
  bool checking = _pending == Work::Check;
  _pending = Work::None;
  _workerRunning = true;
  consoleLog("[Firmware] Feeds stopped: free=%u largest=%u; creating %s worker.\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), checking ? "check" : "update");
  if (xTaskCreatePinnedToCore(checking ? checkTask : task, checking ? "fwcheck" : "firmware",
      kTaskStack, this, 1, &_worker, 0) != pdPASS) {
    _worker = nullptr; _workerRunning = false; _latest.checking = false;
    _status.busy = false;
    _nextCheck = at(now + kRetryMs);
    if (checking) strlcpy(_latest.error, "Not enough free memory to check right now.", sizeof(_latest.error));
    else {
      _status.failed = true; _status.lowMemory = true;
      strlcpy(_status.message, "Not enough free memory. Restarting to install the update.", sizeof(_status.message));
    }
    firmwareUpdating = false;
  }
}

void FirmwareUpdate::poll(uint32_t now, bool online) {
  service(now);  // lifecycle must progress during boot and while offline, too
#ifdef UM_BRIDGE
  return;
#endif
  if (!online || time(nullptr) < kValidEpoch || firmwareUpdating) return;
  std::lock_guard<std::mutex> guard(_lock);
  if (!_nextCheck) {  // first call: honour the last check so reboots do not cause extra ones
    uint32_t epoch = time(nullptr), age = _latest.checkedAt && epoch > _latest.checkedAt ? epoch - _latest.checkedAt : UINT32_MAX;
    uint32_t wait = age < kCheckEveryMs / 1000 ? kCheckEveryMs / 1000 - age : kFirstCheckS;
    _nextCheck = at(now + wait * 1000);
  }
  if ((int32_t)(now - _nextCheck) >= 0) startCheck();
}

void FirmwareUpdate::checkTask(void *self) {
  auto &update = *static_cast<FirmwareUpdate *>(self);
  update.check();
  { std::lock_guard<std::mutex> guard(update._lock); update._finishedAt = millis() ? millis() : 1; }
  vTaskSuspend(nullptr);
}

void FirmwareUpdate::check() {
  bool ok = false, available = false;
  Manifest info{};
  const char *error = nullptr;
  {
    std::unique_lock<std::mutex> gate(networkBudget());  // never alongside a feed download
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    consoleLog("[Firmware] Update check heap: free=%u largest=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)largest);
    if (largest >= kMinFreeBlockCheck) {
      error = fetchManifest(kMode, info, esp_ota_get_next_update_partition(nullptr));
      ok = !error;
      available = ok && !info.current && !info.replayed;
    } else error = "Not enough free memory to check right now.";
    consoleLog("[Firmware] Update check: %s (current=%d replayed=%d)\n", error ? error : available ? "update available" : "up to date", info.current, info.replayed);
  }
  time_t epoch = time(nullptr);
  if (ok) {
    Preferences prefs;
    if (prefs.begin("um-updates", false)) {
      prefs.putString("chkMode", kMode);
      if (epoch >= kValidEpoch) prefs.putUInt("chk", (uint32_t)epoch);
      if (available) { prefs.putBytes("availElf", info.elfSha256, sizeof(info.elfSha256)); prefs.putString("availVer", info.version); }
      else { prefs.remove("availElf"); prefs.remove("availVer"); }
      prefs.end();
    }
  }
  std::lock_guard<std::mutex> guard(_lock);
  _latest.checking = false;
  _nextCheck = at(millis() + (ok ? kCheckEveryMs : kRetryMs));
  strlcpy(_latest.error, error ? error : "", sizeof(_latest.error));
  if (!ok) return;
  _latest.available = available;
  strlcpy(_latest.version, available ? info.version : "", sizeof(_latest.version));
  if (epoch >= kValidEpoch) _latest.checkedAt = (uint32_t)epoch;
  if (available) consoleLog("[Firmware] Update available: %s\n", info.version);
}

bool FirmwareUpdate::pendingVerification() {
  esp_ota_img_states_t state;
  return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
}

void FirmwareUpdate::rollbackIfPending() {
  if (pendingVerification()) esp_ota_mark_app_invalid_rollback_and_reboot();
}

void FirmwareUpdate::confirmForProvisioning(bool healthy) {
  if (healthy && pendingVerification()) esp_ota_mark_app_valid_cancel_rollback();
}

void FirmwareUpdate::confirmBoot(bool healthy, uint32_t now, uint32_t failures) {
  if (!_checkedBoot) {
    esp_ota_img_states_t state;
    _pendingBoot = esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
    _checkedBoot = true;
  }
  if (!_pendingBoot) return;
  if (now > kConfirmDeadlineMs && !(healthy && failures == _lastFailures)) {  // never became healthy: go back to the previous image
    consoleLog("[Firmware] New image failed its startup check. Rolling back.\n");
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
  if (!healthy || failures != _lastFailures) { _healthySince = 0; _lastFailures = failures; return; }
  if (!_healthySince) _healthySince = now;
  if (now - _healthySince >= 30000 && esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
    _pendingBoot = false; consoleLog("[Firmware] Healthy startup confirmed.\n");
  }
}
