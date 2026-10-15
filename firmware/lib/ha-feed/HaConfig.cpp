// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "HaConfig.h"

#include <Preferences.h>
#include <memory>
#include <new>
#include <esp_log.h>
#include <nvs.h>
#include <atomic>
#include <string.h>
#include "CacheBlob.h"

namespace ha {

namespace {
constexpr uint16_t kBlobVersion = 3;
std::atomic<uint32_t> savedVersion{1};
Config *current = nullptr;  // on the heap, created on first use, so the update-only boot does not reserve it
const char *failedStep = "";
char weather[kEntity] = {};
Slot extras[kHomeTiles - kTiles] = {{TILE_WEATHER, 1, 0, 0, 0, "", ""}, {TILE_NONE, 0, 0, 0, 0, "", ""}, {TILE_NONE, 0, 0, 0, 0, "", ""}};  // fourth, fifth and sixth tile
Slot washer = {ICON_WASHER, 0, 0, 0, 0, "", ""};
struct Rooms { char id[kRooms][kEntity]; } rooms = {};

void terminateAll(Config &c) {
  cacheblob::terminate(c.host);
  cacheblob::terminate(c.token);
  cacheblob::terminate(c.doorbell);
  for (auto &slot : c.tiles) { cacheblob::terminate(slot.unit); cacheblob::terminate(slot.entity); }
  for (auto &slot : c.icons) { cacheblob::terminate(slot.unit); cacheblob::terminate(slot.entity); }
  for (auto &entity : c.energy) cacheblob::terminate(entity);
}
}  // namespace

Config &config() {
  if (!current) { current = new Config; defaults(*current); }
  return *current;
}

char (&weatherEntity())[kEntity] { return weather; }
char (*mediaPlayers())[kEntity] { return rooms.id; }
Slot &washerAlert() { return washer; }
Slot &extraTile(int index) { return extras[index - kTiles]; }
const Slot &tileSlot(const Config &c, int index) { return index < kTiles ? c.tiles[index] : extras[index - kTiles]; }

void load() {
  Config c;
  bool washerStored = false, tileStored = false, barStored = false;
  Preferences p;
  bool ok = p.begin("ultimatrix", true) && cacheblob::get(p, "haCfg", c, kBlobVersion);
  if (ok) {
    barStored = p.getBool("haBarV", false);
    p.getString("haWeather", weather, sizeof(weather));
    tileStored = cacheblob::get(p, "haTile4", extras[0], 1);
    if (!tileStored) {
      extras[0] = {TILE_WEATHER, uint8_t(weather[0] ? 0 : 1), 0, 0, 0, "", ""};
      strlcpy(extras[0].entity, weather, sizeof(extras[0].entity));
    }
    for (int i = 1; i < kHomeTiles - kTiles; i++) {
      char key[8];
      snprintf(key, sizeof(key), "haTile%d", kTiles + 1 + i);
      if (!cacheblob::get(p, key, extras[i], 1)) extras[i] = {TILE_NONE, 0, 0, 0, 0, "", ""};
    }
    for (int i = 0; i < kHomeTiles - kTiles; i++) {
      cacheblob::terminate(extras[i].entity); cacheblob::terminate(extras[i].unit);
      if (extras[i].type >= TILE_TYPES || !validEntityList(extras[i].entity)) extras[i] = i ? Slot{TILE_NONE, 0, 0, 0, 0, "", ""} : Slot{TILE_WEATHER, 1, 0, 0, 0, "", ""};
    }
    washerStored = cacheblob::get(p, "haWasher", washer, 1);
    if (!washerStored) washer = {ICON_WASHER, 0, 0, 0, 0, "", ""};
    if (!cacheblob::get(p, "haMedia", rooms, 1)) memset(&rooms, 0, sizeof(rooms));
    for (auto &id : rooms.id) {
      cacheblob::terminate(id);
      if (id[0] && (!validEntity(id) || strncmp(id, "media_player.", 13))) id[0] = 0;
    }
    cacheblob::terminate(washer.entity);
    if (!validEntity(washer.entity)) washer.entity[0] = 0;
  }
  p.end();
  if (weather[0] && (!validEntity(weather) || strncmp(weather, "weather.", 8))) weather[0] = 0;
  if (ok) {
    Preferences q;
    if (q.begin("ultimatrix", true)) {
      cacheblob::get(q, "haTok", c.token, 1);
      cacheblob::get(q, "haEn", c.energy, 1);
      q.end();
    }
    terminateAll(c);
  }
  else {
    defaults(c);
    // A blob of an older layout can never be read again, and it would need the room twice when the new one is written
    Preferences stale;
    if (stale.begin("ultimatrix", false)) { if (stale.isKey("haCfg")) stale.remove("haCfg"); stale.end(); }
  }
  // Move the old default washer slot to its own alert without dropping its sensor or popup options.
  if (ok && !washerStored && c.icons[3].type == ICON_WASHER) {
    if (!washer.entity[0]) washer = c.icons[3];
    c.icons[3] = {ICON_PRESENCE, 1, 0, 0, 0, "", ""};
  }
  if (ok && !tileStored && c.tiles[2].type == TILE_PEOPLE && c.tiles[2].autoMode && !c.tiles[2].entity[0])
    c.tiles[2] = {TILE_EV, 0, 0, 0, 0, "", ""};
  if (ok && !barStored) {
    const uint8_t oldIcons[kIcons] = {ICON_DOOR, ICON_WINDOW, ICON_GARAGE, ICON_PRESENCE, ICON_LIGHTS, ICON_LOCK};
    bool untouched = true;
    for (int i = 0; i < kIcons; i++) untouched &= c.icons[i].type == oldIcons[i] && c.icons[i].autoMode && !c.icons[i].entity[0] && !c.icons[i].flags;
    if (untouched) {
      const uint8_t newIcons[kIcons] = {ICON_LOW_BATTERY, ICON_OFFLINE, ICON_LEAK, ICON_BLINDS, ICON_MEDIA, ICON_HEATING};
      for (int i = 0; i < kIcons; i++) c.icons[i] = {newIcons[i], 1, 0, 0, 0, "", ""};
    }
  }
  if (!current) current = new Config;
  *current = c;
}

const char *saveFailure() { return failedStep; }

bool save() {
  failedStep = "";
  Preferences p;
  if (!p.begin("ultimatrix", false)) { failedStep = "open"; return false; }
  // The token and the energy entities are blobs of their own, written first: the main blob holds them as zeros, and an
  // older blob that still has them is read as before
  bool ok = cacheblob::put(p, "haTok", config().token, 1) && cacheblob::put(p, "haEn", config().energy, 1);
  if (!ok) failedStep = "token";
  std::unique_ptr<Config> core(ok ? new (std::nothrow) Config(config()) : nullptr);
  if (ok && !core) { ok = false; failedStep = "memory"; }
  if (ok) {
    memset(core->token, 0, sizeof(core->token));
    memset(core->energy, 0, sizeof(core->energy));
    ok = cacheblob::put(p, "haCfg", *core, kBlobVersion);
    // The flash of settings is small: when the new blob does not fit next to the old one, replace it instead of updating it
    if (!ok) { p.remove("haCfg"); ok = cacheblob::put(p, "haCfg", *core, kBlobVersion); }
    if (!ok) failedStep = "connection";
  }
  if (ok) {
    if (weather[0]) ok = p.putString("haWeather", weather) == strlen(weather);
    else if (p.isKey("haWeather")) ok = p.remove("haWeather");
    if (!ok) failedStep = "weather";
  }
  if (ok && !(ok = cacheblob::put(p, "haWasher", washer, 1))) failedStep = "washer";
  for (int i = 0; ok && i < kHomeTiles - kTiles; i++) {
    char key[8];
    snprintf(key, sizeof(key), "haTile%d", kTiles + 1 + i);
    if (!(ok = cacheblob::put(p, key, extras[i], 1))) failedStep = "tile";
  }
  if (ok) {
    ok = cacheblob::put(p, "haMedia", rooms, 1);
    if (!ok) { p.remove("haMedia"); ok = cacheblob::put(p, "haMedia", rooms, 1); }  // settings flash is small: replace instead of update
    if (!ok) failedStep = "media";
  }
  if (ok && !(ok = p.putBool("haBarV", true) == 1)) failedStep = "marker";
  p.end();
  if (ok) savedVersion.fetch_add(1);
  else {
    nvs_stats_t st;
    if (nvs_get_stats(nullptr, &st) == ESP_OK) ESP_LOGE("ha", "save failed at %s: NVS %u used, %u free, %u total entries", failedStep, (unsigned)st.used_entries, (unsigned)st.free_entries, (unsigned)st.total_entries);
  }
  return ok;
}

bool configured() { return config().host[0] && config().token[0]; }

uint32_t version() { return savedVersion.load(); }

}  // namespace ha
