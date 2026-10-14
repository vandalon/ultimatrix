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
const int kOutMs = 380, kInMs = 650, kInDelayMs = 240;
const int kLetterStaggerMs = 180, kLineStaggerMs = 35;
const int kTransitionMs = kInDelayMs + kLetterStaggerMs + 7 * kLineStaggerMs + kInMs;
const int kReflowOutMs = 180, kReflowMoveMs = 320;
const int kReflowMs = kReflowOutMs + kReflowMoveMs;

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
bool changing = false;
bool reflow = false;
enum class Transition : uint8_t { Shuffle, Lift, Dissolve, Wipe };
enum class Animation : uint8_t { None, Shuffle, Rotate, Lift, Dissolve, Wipe };  // saved timeStyle values
Transition transition = Transition::Shuffle;
uint8_t nextTransition = 0;
int8_t matched[8], sourceLine[8];  // incoming lines' unchanged matches and previous text

int phraseKey(int hour, int minute, int language) { return (language * 24 + hour) * 60 + minute; }

void makeLayout(Layout &l, int hour24, int minute, int language) {
  WordPhrase phrase;
  WORD_LANGUAGES[language].say(hour24, minute, phrase);
  layOut(l, phrase);
}

void matchLines() {
  bool used[8] = {};
  reflow = false;
  for (int i = 0; i < coming.count; i++) {
    matched[i] = sourceLine[i] = -1;
    const Line &to = coming.lines[i];
    for (int j = 0; j < shown.count; j++) {
      const Line &from = shown.lines[j];
      if (!used[j] && from.big == to.big && from.scale == to.scale && !strcmp(from.text, to.text)) {
        matched[i] = sourceLine[i] = j;
        used[j] = true;
        reflow |= from.x != to.x || from.y != to.y;
        break;
      }
    }
  }
  // Changed lines keep the previous letters until their individual shuffle starts.
  for (int i = 0; i < coming.count; i++) {
    if (matched[i] >= 0) continue;
    int best = -1;
    for (int j = 0; j < shown.count; j++) {
      if (used[j] || shown.lines[j].big != coming.lines[i].big || shown.lines[j].scale != coming.lines[i].scale) continue;
      if (best < 0 || abs(shown.lines[j].y - coming.lines[i].y) < abs(shown.lines[best].y - coming.lines[i].y)) best = j;
    }
    sourceLine[i] = best;
    if (best >= 0) used[best] = true;
  }
}

bool stays(int oldLine) {
  for (int i = 0; i < coming.count; i++) if (matched[i] == oldLine) return true;
  return false;
}

struct Ink {
  Transition effect;
  int progress, left, width, top, offsetY;
  uint32_t seed;
  bool entering, shadow;
};

void transitionPixel(MatrixPanel_I2S_DMA &d, int x, int y, uint16_t color, void *context) {
  const Ink &ink = *static_cast<Ink *>(context);
  const int localX = x - ink.left - (ink.shadow ? 1 : 0);
  const int localY = y - ink.top - (ink.shadow ? 1 : 0);
  if (ink.effect == Transition::Shuffle) {
    if (localX < 0 || localX >= ink.width) return;  // keep each cycling glyph inside its final letter's slot
  } else if (ink.effect == Transition::Lift) {
    color = dim(color, (ink.entering ? ink.progress : 1000 - ink.progress) * 255 / 1000);
  } else {
    const int threshold = ink.effect == Transition::Dissolve
        ? 1 + hash(ink.seed ^ (uint32_t)(localX + 64 * localY)) % 999
        : 1 + localX * 998 / (ink.width > 1 ? ink.width - 1 : 1);
    if (ink.entering ? ink.progress < threshold : ink.progress >= threshold) return;
    const int edge = ink.entering ? ink.progress - threshold : threshold - ink.progress;
    if (!ink.shadow && edge < 110) color = mix(color, rgb(245, 255, 245), (110 - edge) * 160 / 110);
  }
  y += ink.offsetY;
  if ((unsigned)x < W && y >= 1 && y < 55) d.drawPixel(x, y, color);
}

void drawLine(MatrixPanel_I2S_DMA &d, const Line &line, const Theme &theme, wordfont::Plot plot = nullptr, Ink *ink = nullptr) {
  if (line.big) {
    if (ink) ink->shadow = true;
    wordfont::draw(d, line.text, false, line.x + 1, line.y + 1, line.scale, line.topRow, mix(0, theme.big, 40), plot, ink);
    if (ink) ink->shadow = false;
  }
  wordfont::draw(d, line.text, false, line.x, line.y, line.scale, line.topRow, line.big ? theme.big : theme.small, plot, ink);
}

int letterCount(const char *text) {
  int count = 0;
  while (wordfont::next(text)) count++;
  return count;
}

// Similar-width letters keep the shuffle readable without stretching or moving its slots.
uint32_t cyclingLetter(uint32_t seed, int tick, int width) {
  const char *letters = width == 1 ? "il" : width == 2 ? "ijl" : width == 3 ? "frtvxz" : width == 5 ? "mw" : "abcdeghknopqsuy";
  return letters[(seed + tick) % strlen(letters)];
}

void drawChangingLine(MatrixPanel_I2S_DMA &d, const Line &line, const Line *previous, const Theme &theme, int age, int index,
                      bool entering) {
  Ink ink{transition, 0, line.x, wordfont::width(line.text, false, line.scale), line.y, 0,
          hash((uint32_t)comingKey + index * 97), entering, false};
  if (transition == Transition::Dissolve || transition == Transition::Wipe) {
    ink.progress = ease(age - index * kLineStaggerMs - (entering ? kInDelayMs : 0), entering ? kInMs : kOutMs);
    if ((entering && ink.progress > 0) || (!entering && ink.progress < 1000)) drawLine(d, line, theme, transitionPixel, &ink);
    return;
  }

  const int count = letterCount(line.text);
  const char *text = line.text, *old = previous ? previous->text : "";
  int x = line.x, letter = 0;
  while (*text) {
    const char *start = text, *oldStart = old;
    const uint32_t target = wordfont::next(text), oldLetter = wordfont::next(old);
    const uint32_t seed = hash(ink.seed + letter * 131);
    wordfont::Bitmap bitmap;
    wordfont::glyph(target, false, bitmap);
    Line glyphLine = line;
    const size_t bytes = text - start;
    memcpy(glyphLine.text, start, bytes);
    glyphLine.text[bytes] = 0;
    glyphLine.x = x;
    ink.left = x;
    ink.width = bitmap.width * line.scale;
    ink.offsetY = 0;
    bool visible = true;
    if (transition == Transition::Shuffle) {
      const int delay = seed % 220, duration = 520 + (seed >> 8) % 450;
      const int elapsed = age - delay;
      if (target != ' ' && target != oldLetter && elapsed < duration) {
        uint32_t letterToDraw = oldLetter;
        if (elapsed >= 0) letterToDraw = cyclingLetter(seed, elapsed / (55 + (seed >> 16) % 35), bitmap.width);
        if (!letterToDraw || letterToDraw == ' ') visible = false;
        else if (letterToDraw < 128) { glyphLine.text[0] = (char)letterToDraw; glyphLine.text[1] = 0; }
        else {  // Keep UTF-8 intact while showing a previous accented letter.
          memcpy(glyphLine.text, oldStart, old - oldStart);
          glyphLine.text[old - oldStart] = 0;
        }
      }
    } else {
      const int delay = count > 1 ? letter * kLetterStaggerMs / (count - 1) : 0;
      ink.progress = ease(age - index * kLineStaggerMs - delay - (entering ? kInDelayMs : 0), entering ? kInMs : kOutMs);
      ink.offsetY = entering ? (1000 - ink.progress) * 6 / 1000 : -ink.progress * 5 / 1000;
      visible = entering ? ink.progress > 0 : ink.progress < 1000;
    }
    if (visible) drawLine(d, glyphLine, theme, transitionPixel, &ink);
    x += (bitmap.width + 1) * line.scale;
    letter++;
  }
}

void drawWords(MatrixPanel_I2S_DMA &d, const Theme &theme, int age) {
  if (!changing) {
    for (int i = 0; i < shown.count; i++) drawLine(d, shown.lines[i], theme);
    return;
  }
  for (int i = 0; i < shown.count; i++) {
    if (stays(i)) continue;
    if (reflow) {
      // Clear changing lines before the retained words move through their old positions.
      if (age < kReflowOutMs) {
        const Line &line = shown.lines[i];
        Ink ink{Transition::Lift, ease(age, kReflowOutMs), line.x, 0, line.y, 0, 0, false, false};
        drawLine(d, line, theme, transitionPixel, &ink);
      }
    } else if (transition != Transition::Shuffle) drawChangingLine(d, shown.lines[i], nullptr, theme, age, i, false);
  }
  for (int i = 0; i < coming.count; i++) {
    if (matched[i] < 0) {
      if (reflow && age < kReflowMs) continue;
      const Line *previous = sourceLine[i] < 0 ? nullptr : &shown.lines[sourceLine[i]];
      drawChangingLine(d, coming.lines[i], previous, theme, age - (reflow ? kReflowMs : 0), i, true);
    } else {
      Line line = coming.lines[i];
      const Line &from = shown.lines[matched[i]];
      const int progress = reflow ? ease(age - kReflowOutMs, kReflowMoveMs) : 1000;
      line.x = from.x + (line.x - from.x) * progress / 1000;
      line.y = from.y + (line.y - from.y) * progress / 1000;
      drawLine(d, line, theme);
    }
  }
}

}  // namespace

Clockface::Clockface(MatrixPanel_I2S_DMA* display) { _display = display; }

void Clockface::setup(ClockTime *dateTime) {
  _dateTime = dateTime;
  shownKey = comingKey = -1;
  changing = false;
  nextTransition = 0;
}

void Clockface::update() {
  MatrixPanel_I2S_DMA &d = *_display;
  const uint32_t now = millis();
  const auto &settings = *Settings::getInstance();
  int language = settings.numberFont;
  const Animation animation = static_cast<Animation>(settings.timeStyle <= 5 ? settings.timeStyle : 2);
  if (language >= WORD_LANGUAGE_COUNT) language = 1;
  const int hour24 = _dateTime->getHour24(), minute = _dateTime->getMinute();
  const int key = phraseKey(hour24, minute, language);
  if (shownKey < 0) {
    makeLayout(shown, hour24, minute, language);
    shownKey = key;
  } else if (key != (changing ? comingKey : shownKey)) {
    if (changing) { shown = coming; shownKey = comingKey; }
    makeLayout(coming, hour24, minute, language);
    comingKey = key;
    matchLines();
    switch (animation) {
      case Animation::Rotate: transition = static_cast<Transition>(nextTransition++ % 4); break;
      case Animation::Lift: transition = Transition::Lift; break;
      case Animation::Dissolve: transition = Transition::Dissolve; break;
      case Animation::Wipe: transition = Transition::Wipe; break;
      default: transition = Transition::Shuffle; break;
    }
    changing = animation != Animation::None;
    if (!changing) { shown = coming; shownKey = comingKey; }
    changedAt = now;
  }
  const uint32_t age = now - changedAt;
  if (changing && (animation == Animation::None || age >= (uint32_t)(kTransitionMs + (reflow ? kReflowMs : 0)))) {
    shown = coming;
    shownKey = comingKey;
    changing = false;
  }

  const Theme theme = themeAt(minuteOfDay(_dateTime));
  d.fillScreen(rgb(3, 4, 12));
  // A faint glow behind the words that follows the hour colour
  for (int y = 0; y < 56; y++) {
    const int edge = abs(y - 28);
    if (edge < 28) box(d, 0, y, W, 1, mix(rgb(3, 4, 12), dim(theme.big, 40), (28 - edge) * 28 / 28 * 255 / 255 / 4));
  }
  drawWords(d, theme, changing ? (int)age : 0);

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
