// Just enough of the Arduino API to compile the Football clockface and Adafruit GFX on a computer
#pragma once

#include <algorithm>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>

// Not in every libc: the ESP32 one has it
static inline size_t um_strlcpy(char *dst, const char *src, size_t size) {
  size_t n = strlen(src);
  if (size) { size_t c = n >= size ? size - 1 : n; memcpy(dst, src, c); dst[c] = 0; }
  return n;
}
#define strlcpy um_strlcpy

// Arduino's random(max) and random(min, max); a fixed seed keeps the pictures the same every time
inline uint32_t esp_random() { return 7; }  // a fixed value keeps the pictures the same every time
inline long um_random(long hi) { return hi <= 0 ? 0 : rand() % hi; }
inline long um_random(long lo, long hi) { return hi <= lo ? lo : lo + rand() % (hi - lo); }
inline void randomSeed(unsigned long seed) { srand((unsigned)seed); }
#define random um_random
#define radians(degrees) ((degrees) * 0.017453292519943295)
using TaskHandle_t = void *;
#define PROGMEM
#define pgm_read_byte(addr) (*(const uint8_t *)(addr))
#define pgm_read_word(addr) (*(const uint16_t *)(addr))
#define PI 3.14159265358979f
typedef bool boolean;
typedef uint8_t byte;
using std::max;
using std::min;
#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))

inline void yield() {}
unsigned long millis();
void delay(unsigned long ms);

class __FlashStringHelper;

class String {
 public:
  String(const char *s = "") : _s(s ? s : "") {}
  String(const std::string &s) : _s(s) {}
  String(char c) : _s(1, c) {}
  String(int v) : _s(std::to_string(v)) {}
  String(unsigned int v) : _s(std::to_string(v)) {}
  String(long v) : _s(std::to_string(v)) {}
  String(unsigned long v) : _s(std::to_string(v)) {}
  String(long long v) : _s(std::to_string(v)) {}
  String(unsigned long long v) : _s(std::to_string(v)) {}
  const char *c_str() const { return _s.c_str(); }
  unsigned int length() const { return _s.length(); }
  bool isEmpty() const { return _s.empty(); }
  char operator[](unsigned i) const { return _s[i]; }
  int indexOf(char c) const { size_t p = _s.find(c); return p == std::string::npos ? -1 : (int)p; }
  int indexOf(char c, unsigned from) const { size_t p = _s.find(c, from); return p == std::string::npos ? -1 : (int)p; }
  int indexOf(const char *s) const { size_t p = _s.find(s); return p == std::string::npos ? -1 : (int)p; }
  String substring(unsigned from) const { return from >= _s.size() ? String("") : String(_s.substr(from)); }
  String substring(unsigned from, unsigned to) const {
    if (from >= _s.size() || to <= from) return String("");
    return String(_s.substr(from, to - from));
  }
  bool startsWith(const String &p) const { return _s.compare(0, p._s.size(), p._s) == 0; }
  long toInt() const { return atol(_s.c_str()); }
  char charAt(unsigned i) const { return i < _s.size() ? _s[i] : 0; }
  void replace(const String &from, const String &to) {
    if (from._s.empty()) return;
    for (size_t p = 0; (p = _s.find(from._s, p)) != std::string::npos; p += to._s.size()) _s.replace(p, from._s.size(), to._s);
  }
  String &operator+=(const String &o) { _s += o._s; return *this; }
  String &operator+=(char c) { _s += c; return *this; }
  bool operator==(const String &o) const { return _s == o._s; }
  bool operator==(const char *o) const { return _s == o; }
  bool operator!=(const String &o) const { return _s != o._s; }
  bool operator!=(const char *o) const { return _s != o; }
  bool operator<(const String &o) const { return _s < o._s; }
  const std::string &str() const { return _s; }
 private:
  std::string _s;
};
inline String operator+(const String &a, const String &b) { return String(a.str() + b.str()); }
inline String operator+(const String &a, const char *b) { return String(a.str() + b); }
inline String operator+(const char *a, const String &b) { return String(std::string(a) + b.str()); }
inline String operator+(const String &a, char b) { return String(a.str() + b); }
template <typename T, typename = typename std::enable_if<std::is_arithmetic<T>::value>::type>
String operator+(const String &a, T b) { return String(a.str() + String(b).str()); }

#include "Print.h"
inline size_t Print::print(const String &s) { return write(s.c_str()); }
struct SerialStub {  // the face prints debug lines here; nobody listens in the preview
  template <typename... Args> void printf(const char *, Args...) {}
  template <typename T> void println(const T &) {}
  template <typename T> void print(const T &) {}
};
static SerialStub Serial;
