// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>
#include "IClockface.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "HaTemplate.h"
#include "HaClient.h"
#include "HaMedia.h"
#include "HaDevices.h"
#include "HomeRenderer.h"
#define CLOCKFACE_NAME "home"

// The clock with Home Assistant on the side: a Home page (four configurable tiles, six state icons) and an Energy page (usage,
// solar or battery, grid, and the hours so far), rotating as chosen in the settings, with alerts (doorbell, a finished
// washer, a door left open, the alarm) over whichever page is up. The values come from HaClient; HomeRenderer draws.
class Clockface: public IClockface {
  private:
    ClockTime* _dateTime;
    MatrixPanel_I2S_DMA* _display;
    home::Frame _drawn{};
    bool _haveDrawn = false;
    home::Frame _slideFrom{};  // the page sliding out
    uint32_t _slideStart = 0;  // millis() when the slide began
    bool _sliding = false;

    uint8_t _pageIndex = 0;
    uint32_t _pageSince = 0;

    ha::Snapshot _now{}, _before{};
    bool _haveNow = false, _haveBefore = false;
    uint32_t _seenUpdates = 0;

    uint8_t _alert = home::ALERT_NONE;
    uint32_t _alertUntil = 0;
    uint8_t _alertFlags = 0;  // ha::AlertFlag bits of the popup on show
    uint32_t _openAlertAt[ha::kIcons] = {};
    char _line1[12] = "", _line2[12] = "";

    // Now playing: the players that are playing, the one on show and its album art (kept here so the display never waits for the fetch)
    ha::media::Snapshot _media{};
    uint32_t _mediaSeen = 0, _scrollSince = 0;
    char _scrollKey[ha::media::kText + 4] = "";

    ha::devices::Snapshot _devices{};
    uint32_t _devicesSeen = 0, _devicesSince = 0;

    float _use[home::kBars] = {}, _solar[home::kBars] = {};  // kW per hour, the newest last
    float _useSum = 0, _solarSum = 0;
    int _samples = 0, _hourKey = -1;
    uint32_t _historySeen = 0;
    HaClient::History _past;       // the recorder's last hours, kept to work out today's usage
    float _useToday = 0;           // kWh of usage since midnight, from the hourly means, when no energy sensor says
    bool _useTodayOk = false;

    void followSnapshot(uint32_t now);
    void raise(uint8_t kind, uint32_t now, const char *line1, const char *line2, uint8_t flags, uint32_t ms = 10000);
    void collectHours(int hourKey, int hour24, int minute);
    int pagesAvailable(uint8_t *pages) const;
    void fillHome(home::Frame &frame) const;
    void fillEnergy(home::Frame &frame) const;
    void fillStatus(home::Frame &frame) const;
    void fillMedia(home::Frame &frame, uint32_t now);
    void fillDevices(home::Frame &frame, uint32_t now) const;

  public:
    Clockface(MatrixPanel_I2S_DMA* display);
    void setup(ClockTime *dateTime);
    void poll();
    void update();
    bool alertOverridesNight() const { return _alert != home::ALERT_NONE && (_alertFlags & ha::AF_NIGHT); }
    bool alertFullBrightness() const { return _alert != home::ALERT_NONE && (_alertFlags & ha::AF_BRIGHT); }
    void invalidate() { _haveDrawn = false; }
};
