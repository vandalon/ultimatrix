// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>
#include <TimeService.h>
#include <MemoryLimits.h>
#include <NetworkBudget.h>

// What the football and the F1 tickers share for reading ESPN's feeds.

// Feeds the HTTP stream to the JSON reader (JsonScan) without busy-waiting. Stream's own
// timed read spins while waiting for data, which starves the idle task on
// core 0 and trips the task watchdog during a slow download.
struct YieldingReader {
  Stream &stream;
  unsigned long timeoutMs;
  size_t count = 0;
  bool oversized = false;
  bool dry = false;  // the stream gave nothing for the whole timeout: the server stopped sending or the connection closed

  YieldingReader(Stream &s, unsigned long timeout) : stream(s), timeoutMs(timeout) {}

  int read() {
    if (firmwareUpdating) { dry = true; return -1; }
    if (count >= limits::responseBytes) { oversized = true; return -1; }
    if (++count % 1024 == 0) vTaskDelay(1);
    unsigned long start = millis();
    do {
      if (firmwareUpdating) { dry = true; return -1; }
      int c = stream.read();
      if (c >= 0) return c;
      vTaskDelay(1);
    } while (millis() - start < timeoutMs);
    dry = true;
    return -1;
  }

  size_t readBytes(char *buffer, size_t length) {
    size_t n = 0;
    for (; n < length; n++) {
      int c = read();
      if (c < 0) break;
      buffer[n] = c;
    }
    return n;
  }
};

// ESPN's "2026-10-09T08:30Z" as a UTC time_t, 0 when it can't be read
static inline time_t parseEspnDate(const char *date) {
  int y, mo, d, h, mi;
  if (sscanf(date, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5) return 0;
  return ultime::makeTime(h, mi, 0, d, mo, y);
}
