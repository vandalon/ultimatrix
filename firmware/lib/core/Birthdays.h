#pragma once

#include <Arduino.h>

// The birthdays the clock celebrates, kept as one text setting: "MMDD:NAME:YYYY" entries separated by commas.
// The year is optional ("0511:ELIOT:" shows no age), a space in a name is written as "_".
namespace Birthdays {

constexpr int MAX = 10;
constexpr int NAME_LEN = 12;
constexpr size_t SETTING_MAX = 260;  // 10 entries of at most 4 + 1 + 12 + 1 + 4 + 1 characters

struct Entry {
  uint8_t month, day;
  char name[NAME_LEN + 1];  // capitals, digits and spaces: what the clock's small font can draw
  int year;                 // 0 = not given
};

// The entries of the setting; a broken or impossible one is skipped. Returns how many were read.
inline int parse(const String &text, Entry *out, int max = MAX) {
  static const uint8_t DAYS[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int count = 0;
  int from = 0;
  while (from <= (int)text.length() && count < max) {
    int to = text.indexOf(',', from);
    if (to < 0) to = text.length();
    String item = text.substring(from, to);
    from = to + 1;
    int c1 = item.indexOf(':'), c2 = item.indexOf(':', c1 + 1);
    if (c1 != 4 || c2 < 0) continue;
    int date = item.substring(0, 4).toInt();
    int month = date / 100, day = date % 100;
    if (month < 1 || month > 12 || day < 1 || day > DAYS[month - 1]) continue;
    Entry e = {(uint8_t)month, (uint8_t)day, "", (int)item.substring(c2 + 1).toInt()};
    if (e.year != 0 && (e.year < 1900 || e.year > 2100)) e.year = 0;
    int n = 0;
    for (int i = c1 + 1; i < c2 && n < NAME_LEN; i++) {
      char ch = toupper(item[i]);
      if (ch == '_') ch = ' ';
      if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == ' ') e.name[n++] = ch;
    }
    while (n > 0 && e.name[n - 1] == ' ') n--;  // no trailing spaces
    e.name[n] = 0;
    if (n == 0) continue;
    out[count++] = e;
  }
  return count;
}

// What gets stored: the same setting, with the broken entries dropped and the names in capitals
inline String clean(const String &text) {
  Entry list[MAX];
  int n = parse(text, list);
  String out;
  for (int i = 0; i < n; i++) {
    char head[8];
    snprintf(head, sizeof(head), "%02d%02d:", list[i].month, list[i].day);
    if (i) out += ',';
    out += head;
    for (const char *p = list[i].name; *p; p++) out += *p == ' ' ? '_' : *p;
    out += ':';
    if (list[i].year) out += String(list[i].year);
  }
  return out;
}

// The entry for a date, or nullptr. A 29 February birthday falls on 1 March in the years that have no 29 February.
// Called on every frame, so the setting is only parsed again when it has changed.
inline const Entry *on(const String &setting, int year, int month, int day) {
  static Entry list[MAX];
  static String parsedFrom;
  static int count = -1;  // -1 = not parsed yet
  if (count < 0 || setting != parsedFrom) {
    parsedFrom = setting;
    count = parse(setting, list);
  }
  bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  for (int i = 0; i < count; i++) {
    if (list[i].month == month && list[i].day == day) return &list[i];
    if (!leap && month == 3 && day == 1 && list[i].month == 2 && list[i].day == 29) return &list[i];
  }
  return nullptr;
}

}  // namespace Birthdays
