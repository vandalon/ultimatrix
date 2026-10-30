#pragma once
#include <Arduino.h>
#include <mutex>

class FirmwareUpdate {
public:
  struct Status { char message[96]; uint32_t received, total; bool busy, ready, failed, lowMemory; };
  // mode: clock face to install; null or empty updates the running face.
  // force: pause a live match or session (its feed stops for the duration) instead of waiting for it to end.
  bool request(const char *mode = nullptr, bool force = false) { return queue(mode, force, false); }
  // The spare slot can hold a verified face ahead of time, so a switch is a restart instead of a download.
  // stage(): like request(), but only writes the spare slot; the running image stays the one that boots. Refused while the running
  // image has not proved itself (the spare slot is its rollback target), on the Home face (too little memory) and for the running face.
  bool stage(const char *mode);
  bool spareMode(char *out, size_t size);  // the face the spare slot holds, if valid
  bool spareHolds(const char *mode);       // the spare slot holds a valid image of this face (staged, or the face the clock was on before)
  bool activateStaged(const char *mode);   // boots the spare slot's image: status().ready is then set, the caller restarts
  // The outcome of the last background check; kept in NVS so the boot screen knows before the network is up.
  struct Latest { char version[32]; char error[64]; uint32_t checkedAt; bool available, checking; };
  void begin();  // loads the cached check result
  void latest(Latest &out);
  // Checks the signed manifest only (no download), once a day. Call often; it is cheap until a check is due.
  void poll(uint32_t now, bool online);
  bool checkNow(bool force = false);
  // True when there is no cached result younger than 24 h (needs a synced clock); the boot runs a check then.
  bool checkDue();
  // After a low-memory failure: remember the face in NVS; the caller restarts, and the update starts on the next boot.
  void scheduleAfterReboot(const char *mode);
  const char *target() const { return _target; }  // the face of the latest install request
  // At boot: true (and mode filled) when an update was scheduled. The flag is cleared on read.
  static bool takeScheduled(char *mode, size_t size);
  static bool knownMode(const char *mode);
  // Update source: GitHub releases unless a private server base URL is saved (http:// or https://, no trailing slash).
  static bool server(char *out, size_t size);  // true and filled when a private server is set
  bool setServer(const char *url);  // empty = GitHub; false if the URL is not acceptable
  void status(Status &out);
  void confirmBoot(bool healthy, uint32_t now, uint32_t failures);
  // True while the running image is a new OTA image that has not yet proved itself; restarting then rolls it back.
  static bool pendingVerification();
  // Reboots into the previous image when the running one is still pending verification; otherwise returns.
  static void rollbackIfPending();
  // Before a restart into or out of Wi-Fi setup: keeps a pending image only when the diagnostic passes right now.
  static void confirmForProvisioning(bool healthy);
  const char *version() const;
private:
  static void task(void *self);
  static void checkTask(void *self);
  bool startCheck(bool force = false);
  void service(uint32_t now);
  enum class Work { None, Check, Install };
  Work _pending = Work::None;
  uint32_t _stoppingAt = 0, _finishedAt = 0;
  bool _workerRunning = false, _force = false;
  TaskHandle_t _worker = nullptr;
  bool queue(const char *mode, bool force, bool stageOnly);
  bool _stageOnly = false;
  bool _keepFeeds = false;  // preparing a face ahead of time: the feeds keep running (no firmwareUpdating), unlike an update or a check
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
