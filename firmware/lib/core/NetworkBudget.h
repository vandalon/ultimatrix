#pragma once
#include <mutex>

// Short-lived downloads and live-feed handshakes share a turn. The live
// WebSocket releases this after connecting; the F1 service already parks
// secure downloads while that persistent connection owns its TLS buffers.
inline std::mutex &networkBudget() {
  static std::mutex lock;
  return lock;
}
