// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "TelnetConsole.h"
#include "Console.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <strings.h>

static constexpr uint32_t kIdleMs = 300000;  // a forgotten session gives the only slot back

// Log lines end in a bare \n: telnet wants CR LF
class LogTee : public Print {
 public:
  using Print::write;
  explicit LogTee(WiFiClient &client) : client_(client) {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *data, size_t count) override {
    size_t start = 0;
    for (size_t i = 0; i < count; ++i) {
      if (data[i] != '\n') continue;
      client_.write(data + start, i - start);
      client_.write((const uint8_t *)"\r\n", 2);
      start = i + 1;
    }
    if (start < count) client_.write(data + start, count - start);
    return count;
  }
 private:
  WiFiClient &client_;
};

// ESP_LOG output (Wi-Fi, TLS, drivers) goes to the serial port as before, and to the telnet log when it is on
static vprintf_like_t previousVprintf = nullptr;
static int logToConsole(const char *format, va_list args) {
  if (Console.active()) {
    char line[192];
    va_list copy;
    va_copy(copy, args);
    vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);
    Console.print(line);
  }
  return previousVprintf ? previousVprintf(format, args) : vprintf(format, args);
}

void TelnetConsole::setLogging(bool on) {
  static LogTee tee(client_);
  logging_ = on;
  Console.attach(on ? &tee : nullptr);
}

void TelnetConsole::begin(Handler handler, const char *modeHelp) {
  handler_ = handler;
  modeHelp_ = modeHelp;
  server_.begin();
  started_ = true;
  if (!previousVprintf) previousVprintf = esp_log_set_vprintf(logToConsole);
}

void TelnetConsole::stop() {
  setLogging(false);
  client_.stop();
  server_.end();
  started_ = false;
  length_ = iac_ = 0;
}

void TelnetConsole::accept() {
  WiFiClient incoming = server_.accept();
  if (!incoming) return;
  if (client_ && client_.connected()) {
    incoming.print("Console in use.\r\n");
    incoming.stop();
    return;
  }
  client_ = incoming;
  client_.setNoDelay(true);
  length_ = 0;
  iac_ = 0;
  lastInput_ = millis();
  client_.printf("Ultimatrix %s console. Type help.\r\n", UM_MODE_NAME);
  prompt();
}

void TelnetConsole::poll() {
  if (!started_) return;
  accept();
  if (!client_ || !client_.connected()) {
    if (logging_) setLogging(false);
    client_.stop();
    if (watcher_) watcher_(nullptr);
    return;
  }
  if ((int32_t)(millis() - lastInput_) > (int32_t)kIdleMs) {
    client_.print("\r\nIdle, bye.\r\n");
    setLogging(false);
    client_.stop();
    return;
  }
  if (watcher_) {
    watcher_(&client_);
    if (!client_) return;
  }
  for (int budget = 64; budget > 0 && client_.available(); --budget) {
    int c = client_.read();
    if (c < 0) break;
    lastInput_ = millis();
    if (iac_ == 1) { iac_ = (c >= 251 && c <= 254) ? 2 : 0; continue; }  // IAC command: skip it (and its option byte)
    if (iac_ == 2) { iac_ = 0; continue; }
    if (c == 255) { iac_ = 1; continue; }
    if (c == '\n' || c == '\r') {
      if (c == '\n' && length_ == 0) continue;  // the LF of a CR LF pair
      line_[length_] = 0;
      length_ = 0;
      run(line_);
      if (!client_) return;
      continue;
    }
    if ((c == 8 || c == 127) && length_) { --length_; continue; }
    if (c >= 32 && c < 127 && length_ < sizeof(line_) - 1) line_[length_++] = (char)c;
  }
}

static void printUptime(Print &out) {
  uint32_t s = millis() / 1000;
  out.printf("%ud %02uh %02um %02us", (unsigned)(s / 86400), (unsigned)(s / 3600 % 24), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
}

static void printHeap(Print &out) {
  out.printf("heap free=%u min=%u largest=%u\r\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void TelnetConsole::run(const char *line) {
  while (*line == ' ') ++line;
  size_t n = strlen(line);
  while (n && line[n - 1] == ' ') --n;
  char command[48];
  memcpy(command, line, n);
  command[n] = 0;
  if (!n) { prompt(); return; }
  if (!strcasecmp(command, "help") || !strcmp(command, "?")) {
    client_.print("  help    this list\r\n  status  uptime, address, signal, mode, heap\r\n  heap    free, minimum and largest block\r\n"
                  "  log     log on / log off: show the serial log here\r\n"
                  "  reboot  restart the clock\r\n  quit    close the console\r\n");
    client_.print(modeHelp_);
  } else if (!strcasecmp(command, "status")) {
    client_.printf("mode    %s\r\nuptime  ", UM_MODE_NAME);
    printUptime(client_);
    client_.printf("\r\nip      %s\r\nrssi    %d dBm\r\n", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    printHeap(client_);
  } else if (!strncasecmp(command, "log", 3) && (!command[3] || command[3] == ' ')) {
    const char *arg = command + 3;
    while (*arg == ' ') ++arg;
    if (!strcasecmp(arg, "on")) { setLogging(true); client_.print("Serial log on. Type log off to stop.\r\n"); }
    else if (!strcasecmp(arg, "off")) { setLogging(false); client_.print("Serial log off.\r\n"); }
    else client_.printf("Serial log is %s. Use log on or log off.\r\n", logging_ ? "on" : "off");
  } else if (!strcasecmp(command, "heap")) {
    printHeap(client_);
  } else if (!strcasecmp(command, "reboot")) {
    client_.print("Restarting.\r\n");
    client_.flush();
    delay(100);
    ESP.restart();
  } else if (!strcasecmp(command, "quit") || !strcasecmp(command, "exit")) {
    client_.print("Bye.\r\n");
    setLogging(false);
    client_.stop();
    return;
  } else if (!handler_ || !handler_(command, client_)) {
    client_.print("Unknown command. Type help.\r\n");
  }
  prompt();
}
