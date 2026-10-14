#pragma once
#include <Arduino.h>
#include <mutex>

class FirmwareUpdate {
public:
  struct Status { char message[96]; uint32_t received, total; bool busy, ready, failed, lowMemory; };
  // mode: clock face to install; null or empty updates the running face.
  bool request(const char *mode = nullptr);
  // The outcome of the last background check; kept in NVS so the boot screen knows before the network is up.
  struct Latest { char version[32]; char error[64]; uint32_t checkedAt; bool available, checking; };
  void begin();  // loads the cached check result
  void latest(Latest &out);
  // Checks the signed manifest only (no download), twice a day. Call often; it is cheap until a check is due.
  void poll(uint32_t now, bool online);
  bool checkNow();
  // True when there is no cached result younger than 12 h (needs a synced clock); the boot runs a check then.
  bool checkDue();
  // After a low-memory failure: remember the face in NVS; the caller restarts, and the update starts on the next boot.
  void scheduleAfterReboot(const char *mode);
  // At boot: true (and mode filled) when an update was scheduled. The flag is cleared on read.
  static bool takeScheduled(char *mode, size_t size);
  static bool knownMode(const char *mode);
  // Update source: GitHub releases unless a private server base URL is saved (http:// or https://, no trailing slash).
  static bool server(char *out, size_t size);  // true and filled when a private server is set
  bool setServer(const char *url);  // empty = GitHub; false if the URL is not acceptable
  void status(Status &out);
  void confirmBoot(bool healthy, uint32_t now, uint32_t failures);
  // Call before a deliberate restart: a running image that is still pending verification is kept instead of rolled back.
  static void confirmBeforeRestart();
  const char *version() const;
private:
  static void task(void *self);
  static void checkTask(void *self);
  bool startCheck();
  void service(uint32_t now);
  enum class Work { None, Check, Install };
  Work _pending = Work::None;
  uint32_t _stoppingAt = 0, _finishedAt = 0;
  bool _workerRunning = false;
  TaskHandle_t _worker = nullptr;
  void check();
  void run();
  void message(const char *text, bool failed = false);
  void progress(uint32_t bytes, uint32_t total);
  std::mutex _lock;
  Status _status{"Ready to check for updates.", 0, 0, false, false, false, false};
  uint32_t _lastAttempt = 0, _healthySince = 0, _lastFailures = 0;
  Latest _latest{"", "", 0, false, false};
  uint32_t _nextCheck = 0, _lastManual = 0;
  char _target[16] = "";
  bool _checkedBoot = false, _pendingBoot = false;
};
