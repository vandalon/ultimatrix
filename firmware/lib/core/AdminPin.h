// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>

// Optional admin PIN for the settings API. Off by default. It is stored as a salted SHA-256 hash and never leaves
// the clock. A browser trades the PIN for a random token (POST /api/auth) and sends the token with every request
// that changes something. Tokens live in RAM only, so a restart logs everybody out.
namespace adminpin {
bool isSet();
bool validFormat(const char *pin);  // 4 to 8 digits
bool set(const char *pin);  // stores (or replaces) the PIN; every token stops working
bool clear();  // removes the PIN
// 0 = ok (token filled in, 32 hex characters), 1 = wrong PIN, 2 = locked after repeated failures (waitSeconds set)
int login(const char *pin, char token[33], uint32_t &waitSeconds);
void newToken(char token[33]);  // a token for the caller that just set the PIN
bool tokenOk(const String &token);
}  // namespace adminpin
