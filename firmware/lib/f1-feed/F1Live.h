// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>
#include <atomic>
#include <mutex>

#include "F1LiveState.h"

// Only one TLS handshake fits in the heap at a time: the ticker's downloads and the live feed's connect take turns
extern std::mutex &f1TlsLock;
// The ticker holds this for a whole refresh, and the live feed while it connects: the handshake needs about 90 KB of
// free heap, which the ticker's own downloads take part of while they run
extern std::mutex f1NetLock;
// The handshake needs about 100 KB free and the ticker's idle task keeps 12 KB of stack: while the live feed connects
// it asks the ticker to hand the stack back (as for a firmware update) and the ticker starts again afterwards
extern std::atomic<bool> f1Park;
extern std::atomic<bool> f1TaskUp;

// main.cpp counts failed allocations; its guard sheds optional services after a failure.
extern std::atomic<uint32_t> memoryAllocationFailures;

// Where the F1 code was last, kept in RTC memory: it survives a panic restart and is logged at the next boot
extern char f1Phase[16];
inline void f1At(const char *phase) { strlcpy(f1Phase, phase, sizeof(f1Phase)); }

// The live timing of a session, over the same feed the F1 apps use (livetiming.formula1.com, SignalR over a
// secure WebSocket, no key needed). A background task connects while a session is on or about to start,
// keeps the running order with its gaps and best laps up to date, and drops the connection afterwards.
class F1Live {
  public:
    // Called with whether a session is live or about to start; the connection follows
    void setWanted(bool wanted);

    // Is the feed up to date, with a running order?
    bool fresh();

    // The first places with their times, when the feed is up to date. Returns false when it isn't.
    bool get(char tag, F1LiveState::Row *rows, int &count, int &lap, int &totalLaps, char &flag, bool &finished, long &finishedUtc, bool intervals = false);

    // Seconds left on the session clock, -1 when not known
    int clockSecs(long nowUtc);
    int part();  // qualifying: 1, 2 or 3, 0 when not known

    // Is the WebSocket open? Its TLS buffers leave no room for another TLS download meanwhile
    bool connected() const { return _connected; }

    // Changes with every update, for faces that only redraw when something did
    uint32_t updates() const { return _updates; }

  private:
    static void task(void *self);
    bool negotiate(char *token, size_t size, char *cookie, size_t cookieSize);
    void run();

    bool _started = false;
    std::mutex _lock;
    F1LiveState _state;
    std::atomic<bool> _wanted{false};
    std::atomic<bool> _connected{false};
    std::atomic<uint32_t> _updates{0};
    volatile unsigned long _lastData = 0;  // millis() of the last message with data, 0 = none
};
