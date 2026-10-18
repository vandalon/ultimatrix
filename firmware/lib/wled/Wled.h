// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// WLED overlay: a WLED-compatible 64x64 matrix light that takes the panel from any face while something asks for it
// (an effect, a drawn pixel, a pixel stream) and gives it back when the light goes off or the stream ends. State,
// effects, realtime pixel input and the JSON API live here; main.cpp draws the overlay next to the Notice.
namespace wled {

#ifndef UM_WLED_DEFAULT_ON
#define UM_WLED_DEFAULT_ON 1
#endif
constexpr int kWidth = 64, kHeight = 64, kPixels = kWidth * kHeight;
constexpr int kPresets = 16;       // preset ids 1..16
constexpr int kNameLength = 16;
constexpr int kTextLength = 64;     // the segment name is the text of the scrolling text effect
constexpr int kPlaylistSteps = 8;
constexpr int kTimers = 8;

// A preset is a look; a playlist (playlist = 1) is a list of presets with a time each, in 100 ms units
struct Preset {
  char name[kNameLength + 1];
  uint8_t used, on, bri, fx, sx, ix, pal;
  uint8_t col[3][3];
  uint8_t playlist, count, repeat, end;   // repeat 0 = for ever; end = preset applied afterwards (0 none)
  uint8_t ps[kPlaylistSteps];
  uint16_t dur[kPlaylistSteps];
};

// On/off or a preset at a time of day: fixed, or relative to sunrise or sunset
struct Timer {
  uint8_t used, hour, minute, days;       // days: bit 0 Sunday .. bit 6 Saturday
  uint8_t type, action, preset;           // type 0 fixed, 1 sunrise, 2 sunset; action 0 off, 1 on, 2 preset
  int16_t offset;                         // minutes after sunrise/sunset (negative before)
};

// What the WLED apps and Home Assistant read and write: one segment covering the whole panel
struct State {
  bool on = false;      // the light is on = WLED has the panel; off = the face (set again by any command that starts an effect)
  uint8_t bri = 128;             // master brightness, scales every pixel in software (the panel's own brightness stays the clock's)
  uint8_t transition = 7;        // fade time in 100 ms units
  int16_t once = -1;             // fade time for the next change only ("tt"), -1 none
  int16_t ps = -1;               // preset last applied, -1 none
  int16_t pl = -1;               // playlist running, -1 none
  bool segOn = true;
  uint8_t segBri = 255;
  uint8_t fx = 0, sx = 128, ix = 128, pal = 0;
  uint8_t c1 = 128, c2 = 128, c3 = 16;
  uint8_t col[3][3] = {{255, 160, 0}, {0, 0, 0}, {0, 0, 0}};
  char name[kTextLength + 1] = "";
  bool udpSend = false, udpRecv = false;  // stored and echoed back for the apps; there is no UDP sync
  struct Night { bool on = false; uint8_t duration = 60, mode = 1, target = 0; uint32_t startMs = 0; uint8_t startBri = 0; } nl;
};

// Settings of the overlay (settings page card)
struct Options {
  uint8_t enabled = UM_WLED_DEFAULT_ON;  // the listeners, the mDNS record and the buffers exist only while this is 1
  uint8_t timeoutMin = 0;                // the light switches itself off after this many minutes; 0 = never
  uint8_t bootEffect = 255;              // effect after a restart; 255 = the one in use before
  uint8_t ignoreLive = 0;                // 1 = WLED commands and streams are ignored while a favourite match or an F1/MotoGP session is live
  uint8_t paintSlot = 255;               // saved drawing shown by Paint after a restart; 255 = none
  uint16_t cycleSec = 0;                 // Paint cycles through the saved drawings every this many seconds; 0 = off
};

// ---- Shared state ----
State &state();
Options &options();
void load();                  // from NVS; call before the first request
void touched();               // schedules a save after the settings settle
void saveIfDue(uint32_t now);
void markChanged();           // an input changed the look: forgets the applied preset

// ---- Presets ----
const Preset *preset(int id);          // nullptr when empty
bool savePreset(int id, const char *name);
bool deletePreset(int id);
bool applyPreset(int id);              // a preset, or starts a playlist
bool savePlaylist(int id, const char *name, const uint8_t *ps, const uint16_t *dur, int count, int repeat, int end);
bool setPreset(int id, const Preset &p);
void lookChanged();                    // the look changed by hand: forgets the preset and stops a playlist

// ---- Timers ----
const Timer *timer(int index);
bool setTimers(const Timer *list, int count);  // replaces all, stores
void timersTick(int minuteOfDay, int weekday, uint32_t now);
void setClock(int hour, int minute, int second, bool h24);   // for the clock effects
struct ClockNow { int hour, minute, second; bool h24; };
ClockNow clockNow();
void sunriseRamp(uint8_t progress, uint8_t out[3]);          // wake-up colours: dark red to warm white

// ---- Effects ----
int effectSlots();                     // length of the effect list, reserved slots included
const char *effectName(int index);     // "RSVD" for a slot without an effect
const char *effectData(int index);     // WLED fxdata string (sliders;colours;palette;flags)
bool effectKnown(int index);
int paletteSlots();
const char *paletteName(int index);

// Draws one frame of the current effect straight into the display's back buffer (no frame buffer)
void render(MatrixPanel_I2S_DMA &display, uint32_t now, uint16_t scale);  // scale: master brightness 0..256 from advance()
// Colours and brightness ease towards the state; returns the pixel scale (0..256)
uint16_t advance(uint32_t now);
const uint8_t *displayColor(int slot); // the eased colour 0..2 (r, g, b)
void restartEffect();                  // forget animation state (after realtime, effect change)

// ---- Canvas: the Paint effect (index 222) shows a 64x64 RGB565 buffer that the editor at /canvas, the segment's "i"
// array and saved drawings write to. The 8 KB buffer lives on the heap only while the effect is in use and the heap has room. ----
constexpr int kPaintEffect = 222;
constexpr int kDrawingSlots = 8;
namespace canvas {
bool indexedFrame(const uint8_t *&indexes, const uint8_t *&palette);  // RGB888 palette playback
uint16_t *buffer(bool create);                 // nullptr when not allocated (create = false) or no memory
void release();
void maintain(uint32_t now);  // reclaim abandoned uploads even when another effect is showing
bool importing(int &have, int &total);  // an import is uploading
bool holdsPanel();  // an import is uploading over the Paint effect: draw() leaves the panel as it is
void shrink();  // memory is short: gives back the last-frame copy first, then the import in memory
void clearImports();  // deletes the drawings the page imported (named "Import") from flash; saved drawings stay
void setPixel(int index, uint8_t r, uint8_t g, uint8_t b);   // also marks the drawing as edited
void tick(uint32_t now);                       // animation and cycling, called while Paint renders
int currentSlot();                             // saved drawing on show (unedited), -1 none
bool loadSlot(int id);
void attach(WebServer &server, void (*postRoute)(const char *, WebServer::THandlerFunction));
}

// ---- Overlay: called from main.cpp every frame once the boot is over ----
// Order on the panel: Notice, night clock, WLED, birthday screen, face.
bool enabled();                        // runtime setting; false = no listeners, no record, no state ticking
void service(bool allow, uint32_t now = 0, bool releaseMemory = true);  // releaseMemory: closing the listeners also gives back the canvas and an import; false for a Wi-Fi blip  // main loop, about every frame: opens the listeners when allowed and the heap has room, closes them when not
bool setEnabled(bool on);              // opens or closes the UDP listeners and the mDNS record; false when the heap is too low
void attachPanel(MatrixPanel_I2S_DMA &display);
void poll(int hour, int minute, int second, bool h24, int weekday, uint32_t now);  // state, fades, playlists, timers, stream timeout
bool feedsQuiet();  // WLED has parked the feeds and their tasks are gone: only then may it take memory (canvas, imports)
bool wanted(uint32_t now);             // WLED content wants the panel: the light is on, or a stream is live
void draw(MatrixPanel_I2S_DMA &display, uint32_t now);  // draws one frame and flips; call only while wanted() and nothing above takes the panel
void hidden();                         // wanted() but something above took the panel (or it ended): the listeners stop drawing
bool shown();                          // the overlay owned the last frame

// ---- Realtime input (listeners exist only while enabled) ----
bool realtimeActive(uint32_t now);     // a stream or UDP packet arrived within its timeout
bool realtimeListening();
void realtimeStop();                   // "live": false
const char *realtimeSource();          // "DDP", "UDP", "Art-Net", "E1.31" or ""
uint32_t realtimePackets();
void realtimeCounts(uint32_t out[9]);   // received per listener (DDP, UDP, Art-Net, E1.31), those with a valid start, dropped
uint32_t realtimeDropped();            // packets that arrived while the overlay was not on the panel (or the panel was busy)

// ---- JSON API ----
void apiAttach(WebServer &server, void (*postRoute)(const char *, WebServer::THandlerFunction));
void apiStateInfo(String &out);        // {"state":...,"info":...}
void apiApplyText(const char *text);   // a JSON command document (WebSocket)
void wsAttach(WebServer &server);      // GET /ws: one WebSocket client, as the WLED app expects
void wsService(uint32_t now);          // main loop: reads commands, sends the state when it changed
uint32_t changeStamp();                // grows whenever the state changes
void wsClose();
bool wantsLightPage(WebServer &server); // "/" from the WLED app: send it to /wled
bool mdnsAdd();                        // advertises _wled._tcp with the mac TXT record
void mdnsRemove();
bool mdnsAdded();                      // for the telnet "mdns" command

}  // namespace wled
