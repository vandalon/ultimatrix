// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include <algorithm>
#include <iterator>
#include <new>
#include "F1LiveState.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef ARDUINO
#include <Arduino.h>
#else
static unsigned long millis() { return 0; }  // the native tests: nothing is shown for a few seconds there
#endif

static long lapMs(const char *text);

void F1LiveState::reset() {
  for (Driver &d : _drivers) d = Driver();
  _lap = _totalLaps = 0;
  _flag = 0;
  _finished = false;
  _finishedUtc = 0;
  _topic[0] = 0;
}

F1LiveState::Driver *F1LiveState::driver(int number) {
  if (number <= 0 || number > 255) return nullptr;
  Driver *free = nullptr;
  for (Driver &d : _drivers) {
    if (d.number == number) return &d;
    if (!d.number && !free) free = &d;
  }
  if (free) free->number = number;
  return free;
}

// Copies a string value
static void setText(char *to, size_t size, const char *text) {
  strncpy(to, text, size - 1);
  to[size - 1] = 0;
}

// "4781D7" as RGB565; colours too dark for the LEDs become white
static uint16_t hexColor(const char *hex) {
  if (!hex || strlen(hex) != 6) return 0xFFFF;
  long v = strtol(hex, nullptr, 16);
  int r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
  if (r < 64 && g < 64 && b < 64) return 0xFFFF;
  return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// "2026-10-09T12:34:52.123Z" -> seconds since 1970 (UTC); 0 when it doesn't parse
static long parseUtc(const char *text) {
  int y, mo, d, h, mi, sec;
  if (!text || sscanf(text, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
  y -= mo <= 2;  // days from civil date (Howard Hinnant)
  long era = (y >= 0 ? y : y - 399) / 400;
  long yoe = y - era * 400;
  long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (era * 146097 + doe - 719468) * 86400L + h * 3600L + mi * 60L + sec;
}

int F1LiveState::clockSecs(long nowUtc) const {
  if (_clockUtc <= 0) return -1;
  long left = _clockLeft;
  if (_clockRunning) left -= nowUtc - _clockUtc;
  return left < 0 ? 0 : (int)left;
}

// One value of a message, told apart by where it sits. path is the keys (or array indexes) leading to it:
// {"result":{"DriverList":{"1":{"Tla":"VER"}}}} is result, DriverList, 1, Tla. A snapshot and an update differ
// only in the first two: result and the topic's name, or arguments and the argument's index.
void F1LiveState::leaf(const char (*path)[JsonScan::KEY], int n, const char *value, bool text) {
  bool snapshot = !strcmp(path[0], "result");
  if (n == 2 && text && !snapshot && !strcmp(path[0], "arguments") && !strcmp(path[1], "0")) {  // ["SessionStatus", {...}, time]
    strlcpy(_topic, value, sizeof(_topic));
    return;
  }
  // A live SessionStatus update carries its UTC timestamp as the third argument.
  if (n == 2 && text && !snapshot && !strcmp(path[0], "arguments") && !strcmp(path[1], "2")) {
    if (_pendingFinished && !strcmp(_topic, "SessionStatus")) {
      long utc = parseUtc(value);
      if (utc > 0 && (!_finishedUtc || utc < _finishedUtc)) _finishedUtc = utc;
    }
    _pendingFinished = false;
    return;
  }
  if (n < 3 || (!snapshot && strcmp(path[0], "arguments"))) return;
  const char (*r)[JsonScan::KEY] = path + 2;
  int rn = n - 2;
  const char *topic = snapshot ? path[1] : _topic;
  if (rn == 3 && !strcmp(topic, "SessionData") && !strcmp(r[0], "StatusSeries")) {
    // The subscription snapshot includes the original finish timestamp, even after a reboot.
    // Keep each history entry's fields together, whichever order they arrive in.
    if (strcmp(_statusIndex, r[1])) {
      strlcpy(_statusIndex, r[1], sizeof(_statusIndex));
      _statusUtc = 0;
      _statusFinished = false;
    }
    if (text && !strcmp(r[2], "Utc")) _statusUtc = parseUtc(value);
    else if (text && !strcmp(r[2], "SessionStatus")) _statusFinished = !strcmp(value, "Finished");
    if (_statusFinished && _statusUtc > 0 && (!_finishedUtc || _statusUtc < _finishedUtc)) _finishedUtc = _statusUtc;
  } else if (snapshot && rn == 2 && !strcmp(path[1], "DriverList")) {
    Driver *d = driver(atoi(r[0]));
    if (!d) return;
    if (!strcmp(r[1], "Tla") && text) setText(d->code, sizeof(d->code), value);
    else if (!strcmp(r[1], "TeamColour") && text) d->color = hexColor(value);
  } else if (rn == 1) {
    const char *key = r[0];
    if (!strcmp(key, "SessionPart") && !text) _part = atoi(value);  // qualifying: 1, 2 or 3
    else if (!strcmp(key, "CurrentLap") && !text) _lap = atoi(value);
    else if (!strcmp(key, "TotalLaps") && !text) _totalLaps = atoi(value);
    else if (!strcmp(key, "Status") && text) {
      const char *topic = snapshot ? path[1] : _topic;
      if (!strcmp(topic, "SessionStatus")) {  // Started, Finished (the chequered flag), Finalised, Ends
        if (!strcmp(value, "Finished") || !strcmp(value, "Finalised") || !strcmp(value, "Ends")) _finished = true;
        else if (!strcmp(value, "Started") || !strcmp(value, "Inactive")) { _finished = false; _finishedUtc = 0; }
        _pendingFinished = !snapshot && !strcmp(value, "Finished");
      } else {
        // 1 clear, 2 yellow, 4 safety car, 5 red, 6 and 7 virtual safety car
        int status = atoi(value);
        _flag = status == 5 ? 'r' : status == 4 ? 's' : (status == 6 || status == 7) ? 'v' : status == 2 ? 'y' : 0;
      }
    } else if (!strcmp(key, "Remaining") && text) {  // the session clock: the time left at a moment, ticking or stopped
      int h = 0, m = 0, sec = 0;
      if (sscanf(value, "%d:%d:%d", &h, &m, &sec) == 3) {
        _pendingLeft = h * 3600L + m * 60L + sec;
        _pendingClock = true;
      }
    } else if (!strcmp(key, "Extrapolating") && !text) _pendingRunning = !strcmp(value, "true");
    else if (!strcmp(key, "Utc") && text) _pendingUtc = parseUtc(value);
  } else if (rn >= 3 && !strcmp(r[0], "Lines")) {
    Driver *d = driver(atoi(r[1]));
    if (!d) return;
    const char *field = r[2];
    if (rn == 3) {
      if (!strcmp(field, "Position")) d->position = atoi(value);
      else if (!strcmp(field, "InPit")) d->inPit = !strcmp(value, "true");
      else if (!strcmp(field, "Retired")) d->retired = !strcmp(value, "true");
      else if (!strcmp(field, "Status")) d->finished = atoi(value) == 1088;  // individual chequered flag, including lapped finishers
      else if (!text) return;
      else if (!strcmp(field, "GapToLeader")) setText(d->gap, sizeof(d->gap), value);
      else if (!strcmp(field, "TimeDiffToFastest")) setText(d->diff, sizeof(d->diff), value);
    } else if (rn == 4 && text && !strcmp(field, "IntervalToPositionAhead") && !strcmp(r[3], "Value")) {
      setText(d->interval, sizeof(d->interval), value);
    } else if (rn == 4 && text && !strcmp(field, "BestLapTime") && !strcmp(r[3], "Value")) {
      setText(d->best, sizeof(d->best), value);
    } else if (rn == 5 && text) {
      int i = atoi(r[3]);
      if (i < 0 || i > 2) return;
      if (!strcmp(field, "BestLapTimes") && !strcmp(r[4], "Value")) {
        // A lap faster than the earlier best of this part (the first lap of a part and the snapshot at connecting don't count)
        if (d->bestQ[i][0] && lapMs(value) > 0 && lapMs(value) < lapMs(d->bestQ[i])) {
          d->improvedAt = millis();
          d->improvedPart = i;
        }
        setText(d->bestQ[i], sizeof(d->bestQ[i]), value);
      }
      else if (!strcmp(field, "Stats") && !strcmp(r[4], "TimeDiffToFastest")) setText(d->diffQ[i], sizeof(d->diffQ[i]), value);
    }
  }
}

// What the scanner calls with every value of a message: takes the state's lock around the change only
struct F1LiveState::Feed {
  F1LiveState *state;
  std::mutex *lock;
  bool any;
};

void F1LiveState::feedLeaf(void *sink, const char (*path)[JsonScan::KEY], int n, const char *value, bool text) {
  Feed *feed = (Feed *)sink;
  std::unique_lock<std::mutex> guard;
  if (feed->lock) guard = std::unique_lock<std::mutex>(*feed->lock);
  if (n >= 3 && (!strcmp(path[0], "result") || !strcmp(path[0], "arguments"))) feed->any = true;
  feed->state->leaf(path, n, value, text);
}

bool F1LiveState::applyStream(int (*next)(void *), void *source, std::mutex *lock) {
  Feed feed{this, lock, false};
  _pendingClock = false;
  _pendingFinished = false;
  _statusIndex[0] = 0;
  _statusUtc = 0;
  _statusFinished = false;
  _pendingRunning = false;
  _pendingUtc = 0;
  if (!_backup) _backup.reset(new (std::nothrow) Backup);
  Backup *b = _backup.get();
  if (b) {  // only this thread changes the state, so it can be read here without the lock
    std::copy(std::begin(_drivers), std::end(_drivers), b->drivers);
    b->lap = _lap; b->totalLaps = _totalLaps; b->part = _part; b->flag = _flag; b->finished = _finished;
    b->clockRunning = _clockRunning; b->finishedUtc = _finishedUtc; b->clockUtc = _clockUtc;
    b->clockLeft = _clockLeft; b->clockMessages = _clockMessages;
  }
  bool parsed = JsonScan::read(next, source, feedLeaf, &feed);
  std::unique_lock<std::mutex> guard;
  if (lock) guard = std::unique_lock<std::mutex>(*lock);
  if (!parsed) {  // truncated or invalid: publish nothing from it
    if (b) {
      std::copy(b->drivers, b->drivers + MAX_DRIVERS, _drivers);
      _lap = b->lap; _totalLaps = b->totalLaps; _part = b->part; _flag = b->flag; _finished = b->finished;
      _clockRunning = b->clockRunning; _finishedUtc = b->finishedUtc; _clockUtc = b->clockUtc;
      _clockLeft = b->clockLeft; _clockMessages = b->clockMessages;
    }
    return false;
  }
  if (_pendingClock) {  // a clock message: the time left, whether it ticks, and when that was
    _clockMessages++;
    _clockLeft = _pendingLeft;
    _clockRunning = _pendingRunning;
    if (_pendingUtc) _clockUtc = _pendingUtc;
    feed.any = true;
  }
  return feed.any;
}

static uint32_t hashText(uint32_t h, const char *text) {
  for (; *text; text++) h = (h ^ (uint8_t)*text) * 16777619u;
  return (h ^ 0xFF) * 16777619u;
}

uint32_t F1LiveState::displayHash() const {
  uint32_t h = 2166136261u;
  for (const Driver &d : _drivers) {
    if (!d.number) continue;
    h = hashText(h, d.code);
    h = (h ^ d.position) * 16777619u;
    h = (h ^ (d.inPit ? 1u : 0u) ^ (d.finished ? 2u : 0u) ^ (d.retired ? 4u : 0u)) * 16777619u;
    h = hashText(hashText(hashText(hashText(h, d.gap), d.interval), d.diff), d.best);
    for (int p = 0; p < 3; p++) h = hashText(hashText(h, d.bestQ[p]), d.diffQ[p]);
  }
  return (((h ^ _lap) * 16777619u ^ _totalLaps) * 16777619u ^ (uint32_t)_flag) * 16777619u ^ (uint32_t)_part ^ (_finished ? 0x80000000u : 0);
}

bool F1LiveState::hasOrder() const {
  for (const Driver &d : _drivers)
    if (d.number && d.position == 1 && d.code[0]) return true;
  return false;
}

// "1:30.123" in milliseconds, 0 when it isn't a lap time
static long lapMs(const char *text) {
  int m = 0, s = 0, ms = 0;
  if (sscanf(text, "%d:%d.%d", &m, &s, &ms) != 3) return 0;
  return (m * 60L + s) * 1000L + ms;
}

static const unsigned long IMPROVED_MS = 6000;  // a better lap is marked this long

int F1LiveState::top(char tag, Row *rows, bool intervals) const {
  int count = 0;
  for (int i = 0; i < ROWS; i++) rows[i] = Row{"", 0xFFFF, "", false, false, false, false};
  const Driver *fastest = nullptr;  // a race: whoever has the best lap of all
  long fastestMs = 0;
  if (tag == 'R' || tag == 'S')
    for (const Driver &d : _drivers) {
      long ms = lapMs(d.best);
      if (d.number && d.code[0] && ms > 0 && (!fastest || ms < fastestMs)) {
        fastest = &d;
        fastestMs = ms;
      }
    }
  for (const Driver &d : _drivers) {
    if (!d.number || !d.code[0] || d.position < 1 || d.position > ROWS) continue;
    Row &row = rows[d.position - 1];
    strcpy(row.code, d.code);
    row.color = d.color;
    row.fastest = &d == fastest;
    const char *text = "";
    bool leader = d.position == 1;
    if (tag == 'R' || tag == 'S') {
      text = _finished && d.retired && !d.finished ? "DNF" : leader ? "LEAD" : intervals && !_finished ? d.interval : d.gap;
    } else if (tag == 'P') {
      text = leader ? d.best : d.diff;
    } else {  // qualifying: the latest part that has a time
      // Only the times of the part that is on: a driver without a time in it yet shows none (not the one of an earlier part)
      int part = -1;
      if (_part >= 1 && _part <= 3) {
        part = _part - 1;
      } else {
        for (int p = 2; p >= 0 && part < 0; p--)
          if (d.bestQ[p][0]) part = p;
      }
      if (part >= 0 && d.bestQ[part][0]) text = leader ? d.bestQ[part] : d.diffQ[part];
    }
    row.pit = d.inPit;
    row.finished = (tag == 'R' || tag == 'S') && d.finished;
    if (tag == 'Q' && d.improvedAt && (long)(millis() - d.improvedAt) < IMPROVED_MS && _part >= 1 && d.improvedPart == _part - 1) row.improved = true;
    strncpy(row.time, text, sizeof(row.time) - 1);
    row.time[sizeof(row.time) - 1] = 0;
    if (d.position > count) count = d.position;
  }
  return count;
}
