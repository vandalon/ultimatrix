// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <memory>
#include <mutex>
#include <stdint.h>

#include "JsonScan.h"

// The state of a live F1 session as the live timing feed (livetiming.formula1.com, SignalR) reports it:
// drivers with their code and team colour, running order, gaps and best laps. Pure logic, no network and
// no Arduino: a message goes in, the top of the order comes out.
class F1LiveState {
  public:
    static const int ROWS = 22;
    static const int MAX_DRIVERS = 24;

    struct Row {
      char code[4];
      uint16_t color;
      char time[9];  // first place: the best lap, or LEAD in a race; others: the gap
      bool fastest;  // a race: the driver with the fastest lap so far
      bool improved;  // qualifying: his own best lap of the part just got better
      bool pit;       // in the pit lane now
      bool finished;  // the driver has taken the chequered flag
    };

    void reset();

    // One message, read a character at a time (next returns -1 at its end): the answer to Subscribe
    // ({"type":3,"result":{...}}) or a live update ({"type":1,"target":"feed","arguments":[...]}). Nothing is
    // kept but what is used, so even a snapshot of the whole grid takes a few hundred bytes. lock, when given,
    // is held while the state changes, not while waiting for characters. Returns true when it held data. A message
    // that does not parse leaves the state as it was and returns false.
    bool applyStream(int (*next)(void *), void *source, std::mutex *lock = nullptr);

    // The first places. tag: 'R' race, 'S' sprint, 'P' practice, 'Q' qualifying. Returns how many rows are filled
    // (the highest place known, 0 when there are none yet).
    int top(char tag, Row *rows, bool intervals = false) const;

    int lap() const { return _lap; }
    int totalLaps() const { return _totalLaps; }
    int part() const { return _part; }  // qualifying: which part, 0 when not known
    bool finished() const { return _finished; }  // the session's own status says it is over: the chequered flag is out
    long finishedUtc() const { return _finishedUtc; }  // timestamp of the chequered flag, recovered from SessionData
    char flag() const { return _flag; }  // 'y' yellow, 's' safety car, 'v' virtual safety car, 'r' red, 0 clear
    bool hasOrder() const;

    // Seconds left of a practice or qualifying session, counted on from the last clock message; -1 when unknown
    int clockSecs(long nowUtc) const;
    // Changes when anything that is shown changes (order, times, lap, flag, part), not with every sector time
    uint32_t displayHash() const;
    bool clockRunning() const { return _clockRunning; }
    unsigned clockMessages() const { return _clockMessages; }

  private:
    struct Driver {
      uint8_t number = 0;  // 0 = free
      char code[4] = "";
      uint16_t color = 0xFFFF;
      uint8_t position = 0;
      char gap[10] = "";   // race result: gap to the leader
      char interval[10] = "";  // live race: interval to the car in the position immediately ahead
      char diff[10] = "";  // practice: gap to the fastest
      char best[10] = "";  // best lap
      char bestQ[3][10] = {"", "", ""};  // qualifying: best lap and gap to the fastest per part
      char diffQ[3][10] = {"", "", ""};
      unsigned long improvedAt = 0;  // millis() of the last better lap in qualifying, 0 = none
      uint8_t improvedPart = 0;
      bool inPit = false;
      bool finished = false;
      bool retired = false;
    };

    struct Feed;
    static void feedLeaf(void *sink, const char (*path)[JsonScan::KEY], int n, const char *value, bool text);

    Driver *driver(int number);
    void leaf(const char (*path)[JsonScan::KEY], int n, const char *value, bool text);

    // What a message may change, kept to put back when the message turns out to be broken
    struct Backup {
      Driver drivers[MAX_DRIVERS];
      int lap, totalLaps, part;
      char flag;
      bool finished, clockRunning;
      long finishedUtc, clockUtc, clockLeft;
      unsigned clockMessages;
    };
    std::unique_ptr<Backup> _backup;

    Driver _drivers[MAX_DRIVERS];
    int _lap = 0, _totalLaps = 0, _part = 0;
    char _flag = 0;
    bool _finished = false;
    long _finishedUtc = 0;
    char _statusIndex[JsonScan::KEY] = "";
    long _statusUtc = 0;
    bool _statusFinished = false, _pendingFinished = false;
    char _topic[20] = "";  // an update names its topic first: what the Status that follows is the status of
    long _clockUtc = 0, _clockLeft = 0;  // the time left, at that moment
    bool _clockRunning = false;
    unsigned _clockMessages = 0;
    long _pendingLeft = 0, _pendingUtc = 0;  // a clock message's fields, kept until it has been read whole
    bool _pendingClock = false, _pendingRunning = false;
};
