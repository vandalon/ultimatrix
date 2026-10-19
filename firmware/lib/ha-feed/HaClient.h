// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>
#include <atomic>
#include <mutex>
#include "HaTemplate.h"

// The connection to Home Assistant: one WebSocket (ws://, or wss:// without certificate checks like the feeds),
// authenticated with a long-lived access token, with a single render_template subscription. Home Assistant pushes a
// new result whenever the rendered line changes, so alerts arrive at once and nothing is polled.
// A background task owns the connection and reconnects with a growing pause; the face only reads snapshots.
class HaClient {
  public:
    enum State : uint8_t { IDLE, CONNECTING, AUTHENTICATING, SUBSCRIBING, LIVE, FAILED };

    // The sensors of Home Assistant's Energy dashboard, asked for over the open connection
    struct EnergyPrefs {
      enum : uint8_t { NONE, PENDING, READY, FAILED_ASKING } state = NONE;
      char id[ha::E_COUNT][ha::kEntity] = {};
      char gridTo[ha::kEntity] = "";  // the meters of what the grid receives
      char price[ha::kEntity] = "";   // the grid's tariff: entity ID or fixed number
      char note[56] = "";
    };
    void requestPrefs();             // ask (the answer arrives in a moment)
    void prefs(EnergyPrefs &out);    // the latest answer

    // The last hours of usage and solar power (kW), from the recorder; hour is when the newest bar starts (epoch seconds)
    struct History { float use[ha::kHistoryBars] = {}, solar[ha::kHistoryBars] = {}; uint32_t useMask = 0, solarMask = 0, hour = 0; uint32_t version = 0; };
    void history(History &out);
    uint32_t historyVersion() const { return _historyVersion; }

    // The energy sensors in use, with "auto" resolved from the Energy dashboard; false until a connection has resolved them
    bool energyIds(char (*out)[ha::kEntity]);
    uint32_t energyVersion() const { return _resolvedVersion; }

    static HaClient &instance();
    void begin();  // starts the task if it is not running (it ends itself while the feeds are held)

    // The latest values; false until a first result of this connection has arrived
    bool snapshot(ha::Snapshot &out);
    State state() const { return _state; }
    void status(char *out, size_t size);  // for the settings page: short text, no secrets
    uint32_t updates() const { return _updates; }  // changes with every new result

  private:
    HaClient() = default;
    static void task(void *self);
    void run();
    bool session(uint32_t version);
    void set(State state, const char *text);

    std::mutex _lock;
    std::atomic<bool> _wantPrefs{false};
    EnergyPrefs _prefs;
    float _today[4] = {};      // today's kWh of usage, solar, battery and grid, from the recorder's statistics
    uint8_t _todayOk = 0;
    History _history;
    char _resolved[ha::E_COUNT][ha::kEntity] = {};
    char _gridTo[ha::kEntity] = "";  // with the grid on auto: the production meters, taken off the grid's kWh
    bool _resolvedOk = false;
    std::atomic<uint32_t> _resolvedVersion{0};
    std::atomic<uint32_t> _historyVersion{0};
    ha::Snapshot _snapshot{};
    bool _valid = false;
    char _status[64] = "";
    std::atomic<State> _state{IDLE};
    std::atomic<uint32_t> _updates{0};
    std::atomic<bool> _started{false};
};
