// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <stdint.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// Draws the Home Assistant face from a plain Frame: the screens never look at Home Assistant or at settings, so the
// device and the native preview share this code. A Frame is plain data compared with memcmp; the face redraws when it changes.
namespace home {

constexpr int kBars = 20;

enum Page : uint8_t { SCREEN_HOME, SCREEN_ENERGY, SCREEN_STATUS, SCREEN_MEDIA, SCREEN_DEVICES };
constexpr int kArt = 64;  // the album art on the Now playing page fills the panel, RGB565
enum Alert : uint8_t { ALERT_NONE, ALERT_BELL, ALERT_WASHER, ALERT_OPEN, ALERT_GARAGE, ALERT_ALARM };
// Colours by role, in the Home Assistant palette
enum Tone : uint8_t { T_BLUE, T_AMBER, T_ORANGE, T_GREEN, T_RED, T_DIM, T_MUTE, T_SUN, T_BATTERY, T_WHITE, T_FADED };
enum TileIcon : uint8_t { TI_NONE, TI_THERMOMETER, TI_DROP, TI_PERSON, TI_BOLT, TI_RING, TI_CO2, TI_RAIN, TI_CAR, TI_CAR_CHARGING, TI_CAR_PLUGGED, TI_PRICE, TI_OUTDOOR_DROP, TI_WEATHER };
// 7x7 state icons (the same indices as ha::IconType, then the energy rows' own)
enum SmallIcon : uint8_t { SI_DOOR, SI_WINDOW, SI_GARAGE, SI_WASHER, SI_DRYER, SI_BULB, SI_LOCK, SI_PLUG, SI_NONE, SI_HOUSE, SI_SUN, SI_POLE, SI_BATTERY, SI_PRESENCE, SI_LOW_BATTERY, SI_OFFLINE, SI_LEAK, SI_BLINDS, SI_MEDIA, SI_HEATING };

struct Frame {
  uint8_t page, pageCount, pageIndex, colonLevel;
  uint8_t alert, alertPhase;  // phase flips for the animation
  uint8_t alertFlash;         // the whole panel flashes in the alert's colour, the writing in black on the lit phase
  char time[6], date[8];
  // Home: six selectable tiles and the row of state icons
  uint8_t tileIcon[6], tileTone[6];
  char tileText[6][8];  // '`' draws the degree sign
  uint8_t icon[6], iconTone[6], iconCount[6], iconOk;
  char weather[6][24];
  // Energy: three rows (house, sun or battery, grid), power now left and kWh today right, and the hourly bars
  uint8_t rowIcon[3], rowTone[3], batterySoc;
  char left[3][8], right[3][8];
  uint8_t useBars[kBars], solarBars[kBars];  // 0..12 pixels high, the newest last
  // Now playing: what is playing, and the album art (null while it loads or when there is none)
  char title[48], artist[48];
  uint16_t artVersion, scrollTick;  // scrollTick counts 70 ms steps while a line is wider than the panel, else stays 0
  uint8_t progress;  // 0 when unknown, else 1 + pixels of the 64 played
  const uint16_t *art;
  uint8_t backdrop;  // 1: the art is the dimmed background of this page (not the Now playing page itself)
  // Devices: up to seven rows of low batteries then offline devices (kind SI_LOW_BATTERY / SI_OFFLINE), the last may be "+N MORE"
  char device[7][25];
  uint8_t deviceKind[7];
  // An alert's two lines, or the status page's
  char line1[12], line2[12];
};

uint16_t tone(uint8_t tone);
void draw(MatrixPanel_I2S_DMA &display, const Frame &frame);
// Pages slide in from the right: progress 0..1000 (already eased); canSlide says whether two frames are two pages of the carousel
bool canSlide(const Frame &from, const Frame &to);
void drawSlide(MatrixPanel_I2S_DMA &display, const Frame &from, const Frame &to, int progress);

}  // namespace home
