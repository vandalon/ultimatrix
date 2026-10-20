// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "AdminPin.h"
#include <Preferences.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>
#include <string.h>

namespace adminpin {
namespace {
constexpr const char *kNamespace = "ultimatrix";
constexpr const char *kKey = "adminPin";  // 16 bytes of salt, then the 32-byte hash
constexpr size_t kSalt = 16, kHash = 32, kBlob = kSalt + kHash;
constexpr int kSlots = 4;
constexpr uint32_t kTokenLife = 12UL * 3600 * 1000;  // a token stops working 12 hours after the login
constexpr int kFreeTries = 5;  // wrong PINs allowed before a lock-out; it then doubles from 30 s up to an hour

int8_t cached = -1;  // -1 unknown, 0 no PIN, 1 PIN
struct Slot { uint8_t token[16]; uint32_t at; bool used; } slots[kSlots];
int failures = 0;
uint32_t lockedUntil = 0;
bool locked = false;

void digest(const uint8_t *salt, const char *pin, uint8_t out[kHash]) {
  uint8_t in[kSalt + 8];
  size_t n = strlen(pin);
  memcpy(in, salt, kSalt);
  memcpy(in + kSalt, pin, n);
  mbedtls_sha256(in, kSalt + n, out, 0);
}

bool same(const uint8_t *a, const uint8_t *b, size_t n) {  // constant time
  uint8_t diff = 0;
  for (size_t i = 0; i < n; i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

void hex(const uint8_t *in, size_t n, char *out) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { out[2 * i] = digits[in[i] >> 4]; out[2 * i + 1] = digits[in[i] & 15]; }
  out[2 * n] = 0;
}

void forgetTokens() { memset(slots, 0, sizeof(slots)); }

void issue(char token[33]) {
  int pick = 0;
  for (int i = 0; i < kSlots; i++) {  // a free slot, else the oldest
    if (!slots[i].used) { pick = i; break; }
    if (slots[i].at < slots[pick].at) pick = i;
  }
  esp_fill_random(slots[pick].token, sizeof(slots[pick].token));
  slots[pick].at = millis();
  slots[pick].used = true;
  hex(slots[pick].token, sizeof(slots[pick].token), token);
}
}  // namespace

bool isSet() {
  if (cached < 0) {
    Preferences prefs;
    cached = prefs.begin(kNamespace, true) && prefs.getBytesLength(kKey) == kBlob;
    prefs.end();
  }
  return cached == 1;
}

bool validFormat(const char *pin) {
  size_t n = pin ? strlen(pin) : 0;
  if (n < 4 || n > 8) return false;
  for (size_t i = 0; i < n; i++) if (pin[i] < '0' || pin[i] > '9') return false;
  return true;
}

bool set(const char *pin) {
  if (!validFormat(pin)) return false;
  uint8_t blob[kBlob];
  esp_fill_random(blob, kSalt);
  digest(blob, pin, blob + kSalt);
  Preferences prefs;
  bool ok = prefs.begin(kNamespace, false) && prefs.putBytes(kKey, blob, kBlob) == kBlob;
  prefs.end();
  if (ok) { cached = 1; forgetTokens(); failures = 0; locked = false; }
  return ok;
}

bool clear() {
  Preferences prefs;
  bool ok = prefs.begin(kNamespace, false);
  if (ok && prefs.isKey(kKey)) ok = prefs.remove(kKey);
  prefs.end();
  if (ok) { cached = 0; forgetTokens(); failures = 0; locked = false; }
  return ok;
}

int login(const char *pin, char token[33], uint32_t &waitSeconds) {
  token[0] = 0;
  waitSeconds = 0;
  if (!isSet()) { issue(token); return 0; }
  uint32_t now = millis();
  if (locked) {
    if ((int32_t)(lockedUntil - now) > 0) { waitSeconds = (lockedUntil - now + 999) / 1000; return 2; }
    locked = false;
  }
  uint8_t blob[kBlob], test[kHash];
  Preferences prefs;
  bool have = prefs.begin(kNamespace, true) && prefs.getBytes(kKey, blob, kBlob) == kBlob;
  prefs.end();
  digest(blob, pin ? pin : "", test);
  if (have && same(test, blob + kSalt, kHash)) { failures = 0; issue(token); return 0; }
  if (++failures >= kFreeTries) {
    int shift = (failures - kFreeTries) / kFreeTries;
    uint32_t seconds = 30UL << (shift > 7 ? 7 : shift);  // 30 s, 60 s ... capped at an hour
    if (seconds > 3600) seconds = 3600;
    locked = true;
    lockedUntil = now + seconds * 1000;
    waitSeconds = seconds;
    return 2;
  }
  return 1;
}

void newToken(char token[33]) { issue(token); }

bool tokenOk(const String &token) {
  if (token.length() != 32) return false;
  uint8_t given[16];
  for (int i = 0; i < 16; i++) {
    char a = token[2 * i], b = token[2 * i + 1];
    auto nibble = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    if (nibble(a) < 0 || nibble(b) < 0) return false;
    given[i] = (nibble(a) << 4) | nibble(b);
  }
  uint32_t now = millis();
  bool ok = false;
  for (int i = 0; i < kSlots; i++) {  // every slot is compared, so the time does not show which one matched
    bool live = slots[i].used && now - slots[i].at < kTokenLife;
    ok |= live && same(given, slots[i].token, 16);
  }
  return ok;
}
}  // namespace adminpin
