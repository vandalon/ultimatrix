// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

// A small line-based console on TCP port 23 for the local network: one client at a time, a fixed 48-byte line buffer
// (no String per character), polled from the existing background work so it never blocks drawing.
// There is no authentication and no encryption: anyone on the LAN can use it, which is why the settings page
// switches it on and it is off by default.

#include <Arduino.h>
#include <WiFi.h>

class TelnetConsole {
 public:
  // Mode commands: return true when handled (and say something on out)
  using Handler = bool (*)(const char *command, Print &out);

  // modeHelp: one "  name  what it does\r\n" line per mode command, shown by help
  // Called on every poll with the client, or null while nobody is connected: lets a command report back later.
  // It prints its own prompt after anything it says.
  using Watcher = void (*)(Print *out);
  void setWatcher(Watcher watcher) { watcher_ = watcher; }
  // Who may connect: called with the client's address; false refuses it (the same network filter as the web interface)
  using Filter = bool (*)(IPAddress);
  void setFilter(Filter filter) { filter_ = filter; }

  void begin(Handler handler, const char *modeHelp);
  void poll();
  void stop();

 private:
  void accept();
  void run(const char *line);
  void prompt() { client_.print(locked_ ? "PIN: " : "> "); }
  void setLogging(bool on);
  void redraw();
  void complete();
  void browseHistory(int direction);
  void remember(const char *line);

  WiFiServer server_{23, 1};
  WiFiClient client_;
  Handler handler_ = nullptr;
  Watcher watcher_ = nullptr;
  Filter filter_ = nullptr;
  const char *modeHelp_ = "";
  bool started_ = false;
  bool locked_ = false;  // waiting for the admin PIN (only when one is set)
  uint8_t tries_ = 0;
  bool logging_ = false;  // "log on": mirror the serial log to this session
  char line_[48] = {};
  uint8_t length_ = 0;
  uint8_t esc_ = 0;  // 0 text, 1 after ESC, 2 inside ESC [ / ESC O waiting for the final byte (arrow keys)
  static constexpr uint8_t kHistory = 8;
  char history_[kHistory][48] = {};  // previous commands, oldest first
  char draft_[48] = {};              // the line being typed while browsing history
  uint8_t historyCount_ = 0;
  uint8_t historyPos_ = 0;           // == historyCount_ when not browsing
  uint8_t iac_ = 0;  // 0 text, 1 after IAC, 2 inside an option (WILL/WONT/DO/DONT) waiting for its byte
  uint32_t lastInput_ = 0;
};
