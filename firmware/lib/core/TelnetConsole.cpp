// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "TelnetConsole.h"
#include "Console.h"
#include "AdminPin.h"
#include "FirmwareUpdate.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <lwip/sockets.h>
#include <nvs_flash.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>

static constexpr uint32_t kIdleMs = 300000;  // a forgotten session gives the only slot back

// Log lines end in a bare \n: telnet wants CR LF. Sends never wait: WiFiClient::write retries for up to 10 s on a stalled
// client while every logger (any task, ESP_LOG included) waits on the console lock, so what the socket can't take is dropped.
class LogTee : public Print {
 public:
  using Print::write;
  explicit LogTee(WiFiClient &client) : client_(client) {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *data, size_t count) override {
    size_t start = 0;
    for (size_t i = 0; i < count; ++i) {
      if (data[i] != '\n') continue;
      send(data + start, i - start);
      send((const uint8_t *)"\r\n", 2);
      start = i + 1;
    }
    if (start < count) send(data + start, count - start);
    return count;
  }
 private:
  void send(const uint8_t *data, size_t len) {
    int fd = client_.fd();
    if (len && fd >= 0) ::send(fd, data, len, MSG_DONTWAIT);
  }
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
  length_ = iac_ = esc_ = 0;
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
  iac_ = esc_ = 0;
  historyPos_ = historyCount_;
  lastInput_ = millis();
  // We echo and read key by key (not line by line), so tab and the arrow keys reach us
  static const uint8_t kCharacterMode[] = {255, 251, 1, 255, 251, 3, 255, 253, 3};  // IAC WILL ECHO, WILL SGA, DO SGA
  client_.write(kCharacterMode, sizeof(kCharacterMode));
  locked_ = adminpin::isSet();  // with a PIN set, nothing runs until it is entered
  tries_ = 0;
  if (locked_) client_.printf("Ultimatrix %s console. Enter the PIN.\r\n", UM_MODE_NAME);
  else client_.printf("Ultimatrix %s console. Type help.\r\n", UM_MODE_NAME);
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
  if (watcher_ && !locked_) {
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
    if (esc_ == 1) { esc_ = (c == '[' || c == 'O') ? 2 : 0; continue; }
    if (esc_ == 2) {  // parameter bytes are skipped; the final byte says which key
      if (c >= 0x40 && c <= 0x7e) {
        esc_ = 0;
        if (locked_) {}
        else if (c == 'A') browseHistory(-1);
        else if (c == 'B') browseHistory(1);
      }
      continue;
    }
    if (c == 27) { esc_ = 1; continue; }
    if (c == 4 && length_ == 0) { run("quit"); if (!client_) return; continue; }  // Ctrl-D on an empty line, as in a shell
    if (c == 3) { length_ = 0; historyPos_ = historyCount_; client_.print("^C\r\n"); prompt(); continue; }
    if (c == 9) { if (!locked_) complete(); continue; }
    if (c == '\n' || c == '\r') {
      if (c == '\n' && length_ == 0) continue;  // the LF of a CR LF pair
      line_[length_] = 0;
      length_ = 0;
      client_.print("\r\n");
      if (!locked_) remember(line_);  // a PIN is not kept in the history
      run(line_);
      if (!client_) return;
      continue;
    }
    if ((c == 8 || c == 127) && length_) { --length_; client_.print("\b \b"); continue; }
    if (c >= 32 && c < 127 && length_ < sizeof(line_) - 1) { line_[length_++] = (char)c; client_.write((uint8_t)(locked_ ? '*' : c)); }
  }
}

void TelnetConsole::redraw() {
  client_.print("\r\x1b[K> ");
  client_.write((const uint8_t *)line_, length_);
}

void TelnetConsole::remember(const char *line) {
  if (!*line || (historyCount_ && !strcmp(history_[historyCount_ - 1], line))) { historyPos_ = historyCount_; return; }
  if (historyCount_ == kHistory) {
    memmove(history_[0], history_[1], sizeof(history_[0]) * (kHistory - 1));
    --historyCount_;
  }
  strlcpy(history_[historyCount_++], line, sizeof(history_[0]));
  historyPos_ = historyCount_;
}

// direction -1: older, +1: newer; past the newest command comes back the line that was being typed
void TelnetConsole::browseHistory(int direction) {
  if (direction < 0) {
    if (!historyPos_) return;
    if (historyPos_ == historyCount_) { line_[length_] = 0; strlcpy(draft_, line_, sizeof(draft_)); }
    --historyPos_;
  } else {
    if (historyPos_ >= historyCount_) return;
    ++historyPos_;
  }
  strlcpy(line_, historyPos_ == historyCount_ ? draft_ : history_[historyPos_], sizeof(line_));
  length_ = strlen(line_);
  redraw();
}

// Command names for tab completion: the built-ins plus the first word of each line of the mode help
struct CommandName { const char *text; uint8_t length; };

static size_t commandNames(const char *modeHelp, CommandName *out, size_t max) {
  static const char *const kBuiltIn[] = {"help", "status", "heap", "log", "reboot", "factory-reset", "quit"};
  size_t count = 0;
  for (const char *name : kBuiltIn) out[count++] = {name, (uint8_t)strlen(name)};
  for (const char *p = modeHelp; *p && count < max;) {
    while (*p == ' ') ++p;
    const char *start = p;
    while (*p && *p != ' ' && *p != '\r' && *p != '\n') ++p;
    if (p > start) out[count++] = {start, (uint8_t)(p - start)};
    while (*p && *p != '\n') ++p;  // rest of the help line
    if (*p) ++p;
  }
  return count;
}

void TelnetConsole::complete() {
  for (uint8_t i = 0; i < length_; ++i) if (line_[i] == ' ') return;  // only the command word completes
  CommandName names[24], match[24];
  size_t total = commandNames(modeHelp_, names, 24), found = 0;
  for (size_t i = 0; i < total; ++i)
    if (names[i].length >= length_ && !strncasecmp(names[i].text, line_, length_)) match[found++] = names[i];
  if (!found) { client_.write((uint8_t)7); return; }
  size_t common = match[0].length;
  for (size_t i = 1; i < found; ++i) {
    size_t k = 0;
    while (k < common && k < match[i].length && tolower(match[i].text[k]) == tolower(match[0].text[k])) ++k;
    common = k;
  }
  if (common > sizeof(line_) - 2) common = sizeof(line_) - 2;
  bool extended = common > length_;
  if (extended) { memcpy(line_, match[0].text, common); length_ = common; }
  if (found == 1 && length_ < sizeof(line_) - 1) { line_[length_++] = ' '; extended = true; }
  if (!extended) {  // ambiguous and nothing more to add: list the choices
    client_.print("\r\n");
    for (size_t i = 0; i < found; ++i) { client_.write((const uint8_t *)match[i].text, match[i].length); client_.print("  "); }
    client_.print("\r\n");
  }
  redraw();
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
  if (locked_ && strcasecmp(command, "quit")) {  // the line is the PIN
    char token[33];
    uint32_t wait = 0;
    const int result = adminpin::login(command, token, wait);
    if (result == 0) { locked_ = false; client_.print("OK. Type help.\r\n"); prompt(); return; }
    if (result == 2) { client_.printf("Too many wrong PINs. Try again in %u seconds.\r\n", (unsigned)wait); client_.stop(); return; }
    if (++tries_ >= 3) { client_.print("Wrong PIN. Bye.\r\n"); client_.stop(); return; }
    client_.print("Wrong PIN.\r\n");
    prompt();
    return;
  }
  if (!strcasecmp(command, "help") || !strcmp(command, "?")) {
    client_.print("  help    this list\r\n  status  uptime, address, signal, mode, heap\r\n  heap    free, minimum and largest block\r\n"
                  "  log     log on / log off: show the serial log here\r\n"
                  "  reboot  restart the clock\r\n  factory-reset  erase Wi-Fi, settings and cached data\r\n  quit    close the console (q or Ctrl-D)\r\n");
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
  } else if (!strncasecmp(command, "factory-reset", 13) && (!command[13] || command[13] == ' ')) {
    const char *arg = command + 13;
    while (*arg == ' ') ++arg;
    if (strcasecmp(arg, "yes")) {
      client_.print("This erases Wi-Fi, settings and all cached data, then restarts into setup.\r\nType factory-reset yes to continue.\r\n");
    } else {
      client_.print("Erasing.\r\n");
      client_.flush();
      setLogging(false);
      const esp_partition_t *cache = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
      bool cacheOk = !cache || esp_partition_erase_range(cache, 0, cache->size) == ESP_OK;  // blank flash is re-created as an empty cache on boot
      bool settingsOk = nvs_flash_erase() == ESP_OK;  // Wi-Fi credentials, settings, caches and update history
      client_.printf("%s\r\nRestarting.\r\n", cacheOk && settingsOk ? "Done." : "Erase incomplete.");
      client_.flush();
      delay(100);
      ESP.restart();
    }
  } else if (!strcasecmp(command, "quit") || !strcasecmp(command, "q") || !strcasecmp(command, "exit")) {
    client_.print("Bye.\r\n");
    setLogging(false);
    client_.stop();
    return;
  } else if (!handler_ || !handler_(command, client_)) {
    client_.print("Unknown command. Type help.\r\n");
  }
  prompt();
}
