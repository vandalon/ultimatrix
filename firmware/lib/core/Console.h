#pragma once
#include <Arduino.h>
#include <atomic>
#include <mutex>
#include <stdarg.h>
// Mirrors diagnostics to the telnet client that has switched logging on ("log on"); a null sink otherwise.
// Call sites still print to USB serial themselves, or use consoleLog() to do both.
class DiagnosticSink : public Print {
public:
  using Print::write;
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *data, size_t count) override {
    if (!active_.load(std::memory_order_relaxed)) return count;
    std::lock_guard<std::mutex> guard(lock_);
    if (target_) target_->write(data, count);
    return count;
  }
  // The telnet console attaches its client and detaches (null) before the client goes away.
  void attach(Print *target) {
    std::lock_guard<std::mutex> guard(lock_);
    target_ = target;
    active_.store(target != nullptr, std::memory_order_relaxed);
  }
  bool active() const { return active_.load(std::memory_order_relaxed); }

private:
  std::mutex lock_;
  Print *target_ = nullptr;
  std::atomic<bool> active_{false};
};
inline DiagnosticSink Console;

// A Print that goes to USB serial and the telnet log (for libraries that take a Print for their debug output)
class SerialAndConsole : public Print {
public:
  using Print::write;
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *data, size_t count) override {
    Serial.write(data, count);
    Console.write(data, count);
    return count;
  }
};
inline SerialAndConsole SerialConsole;

// printf to USB serial and the telnet log
inline void consoleLog(const char *format, ...) {
  char line[192];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  Serial.print(line);
  Console.print(line);
}
