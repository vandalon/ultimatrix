// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <stddef.h>

// Entity suggestions for the settings page: Home Assistant renders a short list (entity ID, name, unit) for one kind of
// entity, filtered by what has been typed, through its template API. Blocks for the length of one request.
namespace ha {

// kind: power, energy, battery, temperature, humidity, person, door, window, garage, light, lock, binary, sensor or any.
// The answer is "id|name|unit;id|name|unit;..." with at most 25 entries. False when Home Assistant cannot be asked.
bool suggest(const char *kind, const char *typed, char *out, size_t capacity, char *error, size_t errorSize);

}  // namespace ha
