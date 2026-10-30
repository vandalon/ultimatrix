// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <stdint.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// Draws the Home Assistant face from a plain Frame: the screens never look at Home Assistant or at settings, so the
// device and the native preview share this code. A Frame is plain data compared with memcmp; the face redraws when it changes.
namespace home {

constexpr int kBars = 21;  // hours on the graph: the current hour in the middle (column 10), 10 before and 10 after, 2 px wide, 3 px apart, 1 px clear at both edges
inline int barX(int i, bool) { return 1 + i * 3; }
inline int barW(int, bool) { return 2; }
// The rows are 9 px apart from y 11; the bars start 3 px under the last one (2 clear rows) and fill what is left down to y 61
// Without the export graph the top rows under the numbers hold the scale text (5 rows and a clear one)
constexpr int kScaleBand = 6;
enum ScaleSpot : uint8_t { SCALE_CENTRE, SCALE_RIGHT, SCALE_LEFT };
// The width of the scale text "NN/px" (3x5 digits, a clear column between the glyphs)
inline int scaleWidth(int wh) { return 4 * (wh >= 1000 ? 4 : wh >= 100 ? 3 : 2) + 11; }
inline int barRoom(int rows) { return 51 - 9 * rows; }

enum Page : uint8_t { SCREEN_HOME, SCREEN_ENERGY, SCREEN_STATUS, SCREEN_MEDIA, SCREEN_DEVICES };
constexpr int kArt = 64;  // the album art on the Now playing page fills the panel, RGB565
enum Alert : uint8_t { ALERT_NONE, ALERT_BELL, ALERT_WASHER, ALERT_OPEN, ALERT_GARAGE, ALERT_ALARM };
// Colours by role, in the Home Assistant palette
enum Tone : uint8_t { T_BLUE, T_AMBER, T_ORANGE, T_GREEN, T_RED, T_DIM, T_MUTE, T_SUN, T_BATTERY, T_WHITE, T_FADED, T_PASTEL };
enum TileIcon : uint8_t { TI_NONE, TI_THERMOMETER, TI_DROP, TI_PERSON, TI_BOLT, TI_RING, TI_CO2, TI_RAIN, TI_CAR, TI_CAR_CHARGING, TI_CAR_PLUGGED, TI_PRICE, TI_OUTDOOR_DROP, TI_WEATHER };
// 7x7 state icons (the same indices as ha::IconType, then the energy rows' own)
enum SmallIcon : uint8_t { SI_DOOR, SI_WINDOW, SI_GARAGE, SI_WASHER, SI_DRYER, SI_BULB, SI_LOCK, SI_PLUG, SI_NONE, SI_HOUSE, SI_SUN, SI_POLE, SI_BATTERY, SI_PRESENCE, SI_LOW_BATTERY, SI_OFFLINE, SI_LEAK, SI_BLINDS, SI_MEDIA, SI_HEATING };

// The pictures for a player's source, in the order of clockfaces/home/SourceArt.h; SRC_TEXT: no picture, the name in big letters
enum SourceIcon : uint8_t { SRC_NONE, SRC_TV, SRC_LINE, SRC_OPTICAL, SRC_HDMI, SRC_BLUETOOTH, SRC_AIRPLAY, SRC_RADIO, SRC_PHONO, SRC_TEXT };

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
  uint8_t rowCount;  // rows in use, from the top: house, solar, battery, grid (those that are set up)
  uint8_t rowIcon[4], rowTone[4], rowLeftTone[4], rowRightTone[4], batterySoc;  // the icon takes rowTone; the power and the day's energy their own
  int8_t batteryFlow;    // 1 charging, -1 discharging, 0 idle: the battery icon sweeps up or down
  uint8_t batteryStep;   // 0..4, the position in that sweep
  uint8_t houseLoad;  // the house row colour: 0 the plain tone, else 1..255 = how close the usage is to its highest of the last month
  char left[4][8], right[4][8];
  uint8_t priceBand[kBars];  // the tariff of that hour: 0 unknown, 1 cheap, 2 normal, 3 expensive, 4 not published yet (a strip along the bottom edge)
  // The day, 20 hours of it: used and solar power so far (pixels high), the expected usage and solar of the coming hours, the hour now
  uint8_t useBars[kBars], solarBars[kBars], batteryBars[kBars], expectBars[kBars], forecastBars[kBars], halfRow[kBars];  // halfRow: the row (1 = lowest) that is half solar, half grid; 0 = none
  // With the export graph: the bars stand on baseY, the power sent out hangs below it (solar green, battery teal; expected export dim), negRows deep
  uint8_t baseY, negRows;
  uint8_t thinClock;  // the time and date in 1 px strokes instead of 2 px ones
  uint8_t scaleSpot;  // without the export graph: where the scale text stands in the top row of the graph (SCALE_CENTRE, _RIGHT, _LEFT)
  uint16_t whPerPx;  // the energy one pixel of bar height stands for (0: do not show the scale)
  uint8_t exportSolar[kBars], exportBattery[kBars], exportExpect[kBars];
  uint8_t narrowStart;      // 1: the 1 px bar is the first one (the last three hours of the day), else the last one
  int8_t hourFrom, hourTo;  // the hours at the two corners of the graph (0..23), -1 for none
  int8_t nowCol;  // the column of the current hour; before it is the past, after it the coming hours
  // Now playing: what is playing, and the album art (null while it loads or when there is none)
  char title[48], artist[48], station[48];  // the station shares the artist's line, one after the other, while showStation is 1
  uint8_t showStation;  // 1: the second line shows the station instead of the artist
  uint8_t stationOnly;  // 1: nothing but the station name to show: one line at the bottom on a half-height band
  uint8_t sourceIcon;   // Now playing with nothing but the player's source to show: the icon for it (SourceIcon), or SRC_TEXT for its name
  char source[16];      // that source's name
  uint8_t stationTop;   // 1: the station name takes the date's place at the top right (a title and an artist are both known)
  uint16_t artVersion, scrollTick;  // scrollTick counts 70 ms steps while a line is wider than the panel, else stays 0
  uint8_t progress;  // 0 when unknown, else 1 + pixels of the 64 played
  const uint16_t *art;
  uint8_t backdrop;  // 1: the art is the dimmed background of this page (not the Now playing page itself)
  // Devices: up to seven rows of low batteries then offline devices (kind SI_LOW_BATTERY / SI_OFFLINE), the last may be "+N MORE"
  char device[9][25];  // eight rows, a ninth while the list glides a row up
  uint8_t deviceKind[9], deviceDy;  // deviceDy: how many pixels (0..6) the rows have moved up
  // An alert's two lines, or the status page's
  char line1[12], line2[12];
};

uint16_t tone(uint8_t tone);
bool scrolls(const Frame &frame);  // a line of the frame is wider than its room: scrollTick has to run
void draw(MatrixPanel_I2S_DMA &display, const Frame &frame);
// Pages slide in from the right: progress 0..1000 (already eased); canSlide says whether two frames are two pages of the carousel
bool canSlide(const Frame &from, const Frame &to);
void drawSlide(MatrixPanel_I2S_DMA &display, const Frame &from, const Frame &to, int progress);

}  // namespace home
