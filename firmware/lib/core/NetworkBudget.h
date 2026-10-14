#pragma once
#include <atomic>
#include <mutex>

extern std::atomic<bool> firmwareUpdating;
extern std::atomic<bool> liveEventOn;
// A football match of a favourite team is live, or any F1/MotoGP session is: what the birthday screen gives way to
extern std::atomic<bool> liveFavouriteOn;
inline std::atomic<bool> liveFeedReading{false};
inline bool liveDataActive() { return liveEventOn.load() || liveFeedReading.load(); }

// Short-lived downloads and live-feed handshakes share a turn. A connected
// live feed is also covered by liveDataActive(), after it releases this gate.
inline std::mutex &networkBudget() {
  static std::mutex lock;
  return lock;
}

#ifdef ESP_PLATFORM
#include <Arduino.h>

// Feed tasks suspend themselves after unwinding their network/parse objects.
// The clock task deletes them once they are off both cores, reclaiming their
// stacks synchronously before it creates an update worker.
struct FeedTasks {
  std::mutex lock;
  TaskHandle_t handles[4] = {};
};
inline FeedTasks &feedTasks() { static FeedTasks tasks; return tasks; }

inline BaseType_t createFeedTask(TaskFunction_t task, const char *name, uint32_t stack,
                                 void *self, UBaseType_t priority, std::atomic<TaskHandle_t> *out, BaseType_t core) {
  auto &tasks = feedTasks();
  std::lock_guard<std::mutex> guard(tasks.lock);
  if (firmwareUpdating) return pdFAIL;
  for (auto &handle : tasks.handles) {
    if (handle) continue;
    BaseType_t result = xTaskCreatePinnedToCore(task, name, stack, self, priority, &handle, core);
    if (out) out->store(result == pdPASS ? handle : nullptr);
    if (result != pdPASS) handle = nullptr;
    return result;
  }
  return pdFAIL;
}

inline void waitForFeedRegistration() {
  std::lock_guard<std::mutex> guard(feedTasks().lock);
}

inline void stopFeedTask() {
  // Pair with creation's lock: the task must be registered before suspending.
  { std::lock_guard<std::mutex> guard(feedTasks().lock); }
  vTaskSuspend(nullptr);
}

inline bool reapFeedTasks() {
  auto &tasks = feedTasks();
  std::lock_guard<std::mutex> guard(tasks.lock);
  bool stopped = true;
  for (auto &handle : tasks.handles) {
    if (!handle) continue;
    if (eTaskGetState(handle) == eSuspended &&
        xTaskGetCurrentTaskHandleForCore(0) != handle && xTaskGetCurrentTaskHandleForCore(1) != handle) {
      vTaskDelete(handle);
      handle = nullptr;
    } else stopped = false;
  }
  return stopped;
}
#endif
