#include "JsonScan.h"

#include <stdio.h>
#include <string.h>

namespace {

struct Scanner {
  JsonScan::Next next;
  void *source;
  JsonScan::Leaf leaf;
  void *sink;
  int ahead = -2;
  bool failed = false;
  char path[JsonScan::MAX_DEPTH][JsonScan::KEY];
  char text[JsonScan::VALUE];

  Scanner(JsonScan::Next n, void *src, JsonScan::Leaf l, void *s) : next(n), source(src), leaf(l), sink(s) {}

  int peek() {
    if (ahead == -2) ahead = next(source);
    return ahead;
  }
  int take() {
    int c = peek();
    ahead = -2;
    return c;
  }
  void space() {
    while (peek() == ' ' || peek() == '\n' || peek() == '\r' || peek() == '\t') take();
  }

  // After the opening quote, up to and including the closing one
  void string(char *to, size_t size) {
    size_t at = 0;
    for (;;) {
      int c = take();
      if (c < 0) { failed = true; break; }
      if (c == '"') break;
      if (c == '\\') {
        c = take();
        if (c == 'u') {  // \u00e3 and the like, stored as UTF-8
          unsigned code = 0;
          for (int i = 0; i < 4; i++) {
            int h = take();
            code = code << 4 | (h >= '0' && h <= '9' ? h - '0' : (h | 0x20) >= 'a' && (h | 0x20) <= 'f' ? (h | 0x20) - 'a' + 10 : 0);
          }
          if (code >= 0xD800 && code < 0xE000) code = '?';  // half of a surrogate pair: nothing here needs them
          char utf8[3];
          int len = code < 0x80 ? 1 : code < 0x800 ? 2 : 3;
          if (len == 1) utf8[0] = code;
          else if (len == 2) { utf8[0] = 0xC0 | code >> 6; utf8[1] = 0x80 | (code & 0x3F); }
          else { utf8[0] = 0xE0 | code >> 12; utf8[1] = 0x80 | (code >> 6 & 0x3F); utf8[2] = 0x80 | (code & 0x3F); }
          for (int i = 0; i < len; i++)
            if (at + 1 < size) to[at++] = utf8[i];
          continue;
        } else if (c == 'n') c = '\n';
        else if (c == 't') c = '\t';
        else if (c < 0) { failed = true; break; }
      }
      if (at + 1 < size) to[at++] = c;
    }
    to[at] = 0;
  }

  // A number, true, false or null
  void bare() {
    size_t at = 0;
    for (int c = peek(); c >= 0 && c != ',' && c != '}' && c != ']' && c != ' ' && c != '\n' && c != '\r' && c != '\t'; c = peek()) {
      if (at + 1 < sizeof(text)) text[at++] = c;
      take();
    }
    text[at] = 0;
  }

  // One value whose key path is path[0..level)
  void value(int level) {
    if (failed) return;
    if (level > JsonScan::MAX_DEPTH + 10) { failed = true; return; }  // 20 deep, like ESPN needs (ArduinoJson had a limit of 20 too)
    space();
    int c = peek();
    if (c == '{') {
      take();
      for (;;) {
        space();
        c = take();
        if (c == '}') return;
        if (c == ',') continue;
        if (c != '"') { failed = true; return; }
        bool kept = level < JsonScan::MAX_DEPTH;
        string(kept ? path[level] : text, kept ? JsonScan::KEY : sizeof(text));
        space();
        if (take() != ':') { failed = true; return; }
        value(level + 1);
        if (failed) return;
      }
    } else if (c == '[') {
      take();
      for (int index = 0;; index++) {
        space();
        if (peek() == ']') { take(); return; }
        if (peek() == ',') { take(); index--; continue; }
        if (level < JsonScan::MAX_DEPTH) snprintf(path[level], JsonScan::KEY, "%d", index);
        value(level + 1);
        if (failed) return;
      }
    } else if (c == '"') {
      take();
      string(text, sizeof(text));
      if (!failed && level <= JsonScan::MAX_DEPTH) leaf(sink, path, level, text, true);
    } else if (c >= 0) {
      bare();
      if (level <= JsonScan::MAX_DEPTH) leaf(sink, path, level, text, false);
    } else {
      failed = true;
    }
  }
};

}  // namespace

bool JsonScan::read(Next next, void *source, Leaf leaf, void *sink) {
  Scanner scanner(next, source, leaf, sink);
  scanner.value(0);
  return !scanner.failed;
}

bool jsonPathIs(const char (*path)[JsonScan::KEY], int n, const char *pattern) {
  for (int i = 0; i < n; i++) {
    if (!*pattern) return false;
    const char *end = strchr(pattern, '.');
    size_t length = end ? (size_t)(end - pattern) : strlen(pattern);
    if (!(length == 1 && *pattern == '*') && (strlen(path[i]) != length || strncmp(path[i], pattern, length) != 0)) return false;
    pattern += length;
    if (*pattern == '.') pattern++;
    else if (i + 1 < n) return false;
  }
  return !*pattern;
}
