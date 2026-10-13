// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Clockface.h"
#include <Settings.h>
#include <FaceKit.h>
#include "WordFont.h"
#include "WordLanguages.h"

using namespace facekit;

namespace {

const int kMaxWidth = 62;           // widest line
const int kAreaTop = 3, kAreaHeight = 50;
const int kOutMs = 450, kInMs = 700;

struct Line {
  char text[44];
  bool big;
  uint8_t scale;
  int8_t topRow, bottomRow;
  int16_t x, y;
};
struct Layout {
  Line lines[8];
  uint8_t count = 0;
  int16_t height = 0;
};

// ---- Building the lines ----

int partCount(const char *text) {
  int parts = 1;
  for (const char *p = text; *p; p++) if (*p == '|') parts++;
  return parts;
}

// The parts [from, to) of a piece, joined, with a hyphen after them when the word goes on on the next line
void joinParts(const char *text, int from, int to, bool hyphen, char *out, size_t size) {
  int part = 0;
  size_t n = 0;
  for (const char *p = text;; p++) {
    if (*p == '|' || *p == 0) {
      part++;
      if (*p == 0) break;
      continue;
    }
    if (part >= from && part < to && n + 1 < size) out[n++] = *p;
  }
  if (hyphen && n + 1 < size) out[n++] = '-';
  out[n] = 0;
}

void setText(Line &line, const char *text) {
  strlcpy(line.text, text, sizeof(line.text));
  int top, bottom;
  wordfont::extent(line.text, false, top, bottom);
  line.topRow = top;
  line.bottomRow = bottom;
}

void addLine(Layout &l, const char *text, bool big, int scale) {
  if (l.count >= 8) return;
  Line &line = l.lines[l.count++];
  line.big = big;
  line.scale = scale;
  setText(line, text);
}

// A word on lines of its own: whole if it fits, otherwise broken at its break marks, otherwise smaller
void addWord(Layout &l, const char *text, bool big, int scale) {
  char whole[44];
  joinParts(text, 0, 99, false, whole, sizeof(whole));
  if (wordfont::width(whole, false, scale) <= kMaxWidth) { addLine(l, whole, big, scale); return; }
  const int parts = partCount(text);
  if (parts > 1) {
    Layout trial;
    bool fits = true;
    for (int from = 0; from < parts && fits;) {
      int to = from + 1;
      char best[44];
      joinParts(text, from, to, to < parts, best, sizeof(best));
      if (wordfont::width(best, false, scale) > kMaxWidth) { fits = false; break; }
      while (to < parts) {
        char more[44];
        joinParts(text, from, to + 1, to + 1 < parts, more, sizeof(more));
        if (wordfont::width(more, false, scale) > kMaxWidth) break;
        strlcpy(best, more, sizeof(best));
        to++;
      }
      addLine(trial, best, big, scale);
      from = to;
    }
    if (fits) {
      for (int i = 0; i < trial.count && l.count < 8; i++) l.lines[l.count++] = trial.lines[i];
      return;
    }
  }
  if (scale > 1) addWord(l, text, big, 1);
  else addLine(l, whole, big, 1);  // too wide even so: it is cut at the edge
}

void build(Layout &l, const WordPhrase &phrase, int bigScale) {
  l.count = 0;
  bool open = false;  // the last line is a line of small words that can take more
  for (int i = 0; i < phrase.count; i++) {
    char whole[44];
    joinParts(phrase.text[i], 0, 99, false, whole, sizeof(whole));
    if (phrase.big[i]) { addWord(l, phrase.text[i], true, bigScale); open = false; continue; }
    if (open) {
      char joined[96];
      snprintf(joined, sizeof(joined), "%s %s", l.lines[l.count - 1].text, whole);
      if (wordfont::width(joined, false, 1) <= kMaxWidth) { setText(l.lines[l.count - 1], joined); continue; }
    }
    const int before = l.count;
    addWord(l, phrase.text[i], false, 1);
    open = l.count == before + 1;
  }
}

// Stacks the lines and centres the block in the area of the panel for words
void place(Layout &l) {
  int total = 0;
  for (int i = 0; i < l.count; i++) {
    Line &line = l.lines[i];
    total += (line.bottomRow - line.topRow + 1) * line.scale;
    if (i) total += (line.big || l.lines[i - 1].big) ? 4 : 3;
  }
  l.height = total;
  int y = kAreaTop + (kAreaHeight - total) / 2;
  if (y < 1) y = 1;
  for (int i = 0; i < l.count; i++) {
    Line &line = l.lines[i];
    if (i) y += (line.big || l.lines[i - 1].big) ? 4 : 3;
    line.x = (W - wordfont::width(line.text, false, line.scale)) / 2;
    line.y = y;
    y += (line.bottomRow - line.topRow + 1) * line.scale;
  }
}

void layOut(Layout &l, const WordPhrase &phrase) {
  build(l, phrase, 2);
  place(l);
  if (l.height > kAreaHeight) {  // too many lines for big letters: everything at the small size
    build(l, phrase, 1);
    place(l);
  }
}

// ---- Colours: the football and F1 scheme (off-white hours, green minutes), the same all day ----

struct Theme { uint16_t big, small, faint; };
const Theme kTheme = {rgb(238, 246, 240), rgb(190, 235, 150), rgb(80, 90, 110)};
Theme themeAt(int) { return kTheme; }

// ---- State ----

Layout shown, coming;
int shownKey = -1, comingKey = -1;
uint32_t changedAt = 0;
bool fading = false;

int phraseKey(int hour, int minute, int language) { return (language * 24 + hour) * 60 + minute; }

void makeLayout(Layout &l, int hour24, int minute, int language) {
  WordPhrase phrase;
  WORD_LANGUAGES[language].say(hour24, minute, phrase);
  layOut(l, phrase);
}

void drawLayout(MatrixPanel_I2S_DMA &d, const Layout &l, const Theme &theme, int level) {
  if (level <= 0) return;
  for (int i = 0; i < l.count; i++) {
    const Line &line = l.lines[i];
    if (!line.big) { wordfont::draw(d, line.text, false, line.x, line.y, 1, line.topRow, dim(theme.small, level)); continue; }
    wordfont::draw(d, line.text, false, line.x + 1, line.y + 1, line.scale, line.topRow, dim(mix(0, theme.big, 40), level));  // a soft shadow
    wordfont::draw(d, line.text, false, line.x, line.y, line.scale, line.topRow, dim(theme.big, level));
  }
}

}  // namespace

Clockface::Clockface(MatrixPanel_I2S_DMA* display) { _display = display; }

void Clockface::setup(ClockTime *dateTime) {
  _dateTime = dateTime;
  shownKey = comingKey = -1;
  fading = false;
}

void Clockface::update() {
  MatrixPanel_I2S_DMA &d = *_display;
  const uint32_t now = millis();
  int language = Settings::getInstance()->numberFont;
  if (language >= WORD_LANGUAGE_COUNT) language = 1;
  const int hour24 = _dateTime->getHour24(), minute = _dateTime->getMinute();
  const int key = phraseKey(hour24, minute, language);
  if (shownKey < 0) {
    makeLayout(shown, hour24, minute, language);
    shownKey = key;
  } else if (key != (fading ? comingKey : shownKey)) {
    if (!fading) { fading = true; changedAt = now; }
    makeLayout(coming, hour24, minute, language);
    comingKey = key;
  }
  int levelOut = 255, levelIn = 0;
  if (fading) {
    const int age = (int)(now - changedAt);
    if (age < kOutMs) levelOut = 255 - 255 * ease(age, kOutMs) / 1000;
    else if (age < kOutMs + kInMs) { levelOut = 0; levelIn = 255 * ease(age - kOutMs, kInMs) / 1000; }
    else { shown = coming; shownKey = comingKey; fading = false; levelOut = 255; }
  }

  const Theme theme = themeAt(minuteOfDay(_dateTime));
  d.fillScreen(rgb(3, 4, 12));
  // A faint glow behind the words that follows the hour colour
  for (int y = 0; y < 56; y++) {
    const int edge = abs(y - 28);
    if (edge < 28) box(d, 0, y, W, 1, mix(rgb(3, 4, 12), dim(theme.big, 40), (28 - edge) * 28 / 28 * 255 / 255 / 4));
  }
  drawLayout(d, shown, theme, levelOut);
  if (fading) drawLayout(d, coming, theme, levelIn);

  // The date, small: the weekday and the day and month in the order of the language
  const WordLanguage &lang = WORD_LANGUAGES[language];
  char date[24], numbers[12];
  const int day = _dateTime->getDay(), month = _dateTime->getMonth();
  snprintf(numbers, sizeof(numbers), lang.dateFormat, lang.monthFirst ? month : day, lang.monthFirst ? day : month);
  snprintf(date, sizeof(date), "%s %s", lang.weekdays[_dateTime->getWeekday() % 7], numbers);
  const int dateWidth = wordfont::width(date, true, 1);
  wordfont::draw(d, date, true, (W - dateWidth) / 2, 57, 1, 0, theme.faint);

  // The minute as a thin line along the bottom
  const int ms = msOfMinute(_dateTime);
  const int filled = ms * W / 60000;
  box(d, 0, 63, W, 1, rgb(10, 12, 24));
  box(d, 0, 63, filled, 1, dim(theme.small, 120));
  put(d, filled, 63, dim(theme.small, 255));

  d.flipDMABuffer();
}
