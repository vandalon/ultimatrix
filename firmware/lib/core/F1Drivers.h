// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>
#include <string.h>

// The Formula 1 grid, shared by the football and the F1 tickers: the driver's code and team colour by surname,
// spelled the way ESPN spells it. Update it here when the grid changes.

struct F1Driver {
  const char *surname, *code;
  uint16_t color;
};

#define RGB565(r, g, b) ((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))

// The 2026 grid
static const F1Driver F1_DRIVERS[] = {
  {"Verstappen", "VER", RGB565(54, 113, 255)},  {"Hadjar", "HAD", RGB565(54, 113, 255)},      // Red Bull
  {"Russell", "RUS", RGB565(39, 244, 210)},     {"Antonelli", "ANT", RGB565(39, 244, 210)},   // Mercedes
  {"Leclerc", "LEC", RGB565(232, 0, 45)},       {"Hamilton", "HAM", RGB565(232, 0, 45)},      // Ferrari
  {"Norris", "NOR", RGB565(255, 128, 0)},       {"Piastri", "PIA", RGB565(255, 128, 0)},      // McLaren
  {"Alonso", "ALO", RGB565(34, 153, 113)},      {"Stroll", "STR", RGB565(34, 153, 113)},      // Aston Martin
  {"Gasly", "GAS", RGB565(255, 135, 188)},      {"Colapinto", "COL", RGB565(255, 135, 188)},  // Alpine
  {"Albon", "ALB", RGB565(100, 196, 255)},      {"Sainz", "SAI", RGB565(100, 196, 255)},      // Williams
  {"Lawson", "LAW", RGB565(170, 190, 255)},     {"Lindblad", "LIN", RGB565(170, 190, 255)},   // Racing Bulls
  {"H\xC3\xBClkenberg", "HUL", RGB565(210, 50, 0)}, {"Bortoleto", "BOR", RGB565(210, 50, 0)},  // Audi
  {"Ocon", "OCO", RGB565(255, 255, 255)},       {"Bearman", "BEA", RGB565(255, 255, 255)},    // Haas
  {"P\xC3\xA9rez", "PER", RGB565(255, 215, 0)},   {"Bottas", "BOT", RGB565(255, 215, 0)},     // Cadillac
};
#undef RGB565

static inline const F1Driver *f1DriverBySurname(const char *surname) {
  for (const F1Driver &driver : F1_DRIVERS)
    if (strcmp(driver.surname, surname) == 0) return &driver;
  return nullptr;
}

static inline uint16_t f1DriverColor(const char *code) {
  for (const F1Driver &driver : F1_DRIVERS)
    if (strcmp(driver.code, code) == 0) return driver.color;
  return 0xFFFF;
}
