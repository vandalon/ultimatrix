// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

// The time in words, in several languages. A language says the time as a few pieces of text:
// small ones in the minute font ("ten", "past") and big ones in the hour font ("seven").
// WordLayout puts them on the panel, in the order given.
// The text is UTF-8; a '|' marks where a long word may be broken with a hyphen.
struct WordPhrase {
  static const uint8_t MAX = 10;
  const char *text[MAX];
  bool big[MAX];
  uint8_t align[MAX];  // 0 centred, 1 left, 2 right
  bool white[MAX];     // a small word in the colour of the big ones
  uint8_t count = 0;
  uint8_t nextAlign = 0;  // what the words added from here on get
  bool nextWhite = false;

  void small(const char *t) { add(t, false); }
  void hour(const char *t) { add(t, true); }
  void add(const char *t, bool isBig) {
    if (count < MAX) {
      text[count] = t;
      big[count] = isBig;
      align[count] = nextAlign;
      white[count] = nextWhite;
      count++;
    }
  }
};

struct WordLanguage {
  const char *name;  // in the language itself
  void (*say)(int hour24, int minute, WordPhrase &out);
  const char *weekdays[7];  // Sunday first, in capitals
  const char *dateFormat;   // printf format for the day and the month
  bool monthFirst = false;  // the format gets the month and then the day
};

// In the order of the Language list on the settings page, which stores the position:
// a new language goes at the end, here and on the page.
extern const WordLanguage WORD_LANGUAGES[];
extern const uint8_t WORD_LANGUAGE_COUNT;

// The date as a phrase: the weekday (0 is Sunday), the day (1-31) and the month (1-12) said the way the language says them
void sayDate(int language, int weekday, int day, int month, WordPhrase &out);
