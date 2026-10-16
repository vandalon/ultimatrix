// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>
#include <stdint.h>

// The Home Assistant face's settings: where Home Assistant is, and which entity (or "auto") feeds every slot.
// Kept in one NVS blob of its own (CacheBlob.h), outside the main settings record. The token is write-only
// on the settings page: it is never sent back or logged.
namespace ha {

constexpr int kHomeTiles = 6;  // visible slots; keep the original three-slot NVS blob layout stable (the rest have NVS keys of their own)
constexpr int kTiles = 3, kIcons = 6, kEntity = 96, kIconEntity = 1024, kTokenMax = 255, kHostMax = 63;

enum TileType : uint8_t { TILE_TEMP, TILE_HUMIDITY, TILE_PEOPLE, TILE_POWER, TILE_CUSTOM, TILE_CO2, TILE_RAIN, TILE_EV, TILE_PRICE, TILE_OUTDOOR_HUMIDITY, TILE_WEATHER, TILE_NONE, TILE_TYPES };
enum IconType : uint8_t { ICON_DOOR, ICON_WINDOW, ICON_GARAGE, ICON_WASHER, ICON_DRYER, ICON_LIGHTS, ICON_LOCK, ICON_CHARGER, ICON_NONE, ICON_PRESENCE, ICON_LOW_BATTERY, ICON_OFFLINE, ICON_LEAK, ICON_BLINDS, ICON_MEDIA, ICON_HEATING, ICON_TYPES };
// The energy page's entities, in the order of the template result
enum EnergyEntity : uint8_t { E_USAGE, E_SOLAR, E_BATTERY, E_SOC, E_GRID, E_USAGE_KWH, E_SOLAR_KWH, E_BATTERY_KWH, E_GRID_KWH, E_COUNT };
enum Page : uint8_t { PAGE_HOME = 1, PAGE_ENERGY = 2, PAGE_MEDIA = 4, PAGE_DEVICES = 8, PAGE_ART_BACKDROP = 16 };  // the last is no page: the cover of what plays is the dimmed background of every page
constexpr int kRooms = 4;  // media players the Now playing page can show
// Per popup (alarm, doorbell, every icon that can raise one): break through the night clock, light the panel fully, flash the whole panel
enum AlertFlag : uint8_t { AF_NIGHT = 1, AF_BRIGHT = 2, AF_FLASH = 4 };

struct Slot {
  uint8_t type;       // TileType or IconType
  uint8_t autoMode;   // 1: HA's own device classes pick the entities (only for types that have a rule)
  uint8_t alertMins;  // icons: flash an alert once it has been lit this long, 0 = never
  uint8_t decimals;   // custom tile: 0 or 1
  uint8_t flags;      // icons: AlertFlag bits for the popup this icon can raise
  char unit[4];       // custom tile
  char entity[kEntity];
};

struct Config {
  char host[kHostMax + 1];  // name or address, without scheme
  uint16_t port;
  uint8_t tls;             // wss instead of ws (certificate not verified, as for the feeds)
  uint8_t pages;           // Page bits
  uint8_t pageSecs;        // seconds per page when more than one is on
  uint8_t gridImportPositive;      // grid power: positive means importing
  uint8_t batteryDischargePositive;  // battery power: positive means discharging
  uint8_t doorbellFlags, alarmFlags;  // AlertFlag bits
  char token[kTokenMax + 1];
  Slot tiles[kTiles];
  Slot icons[kIcons];
  char energy[E_COUNT][kEntity];
  char doorbell[kEntity];
};

Config &config();
// Separate NVS key: adding weather must not invalidate the saved connection and slots.
char (&weatherEntity())[kEntity];
Slot &extraTile(int index);  // tiles kTiles..kHomeTiles-1
const Slot &tileSlot(const Config &c, int index);
// The entity lists of the state icons (doors, windows, motion... can be many): heap strings of their own length, up to kIconEntity - 1,
// separate NVS keys; the icon slots' own entity stays empty. Safe to call from any task.
size_t iconEntityCopy(int index, char *out, size_t size);
bool setIconEntity(int index, const char *list);
bool hasIconEntity(int index);
char (*mediaPlayers())[kEntity];  // the kRooms media_player entities of the Now playing page; separate NVS key
Slot &washerAlert();  // independent of the six visible icons; separate NVS key
void defaults(Config &c);
void load();
const char *saveFailure();      // which part of the last failed save() did not fit
bool save();                    // also wakes the client so it reconnects with the new settings
bool configured();              // host and token are set
uint32_t version();             // changes with every save
bool autoAvailable(bool tile, uint8_t type);  // does this slot type have an Auto rule?
// An energy field: "auto" (the sensors of Home Assistant's Energy dashboard) or entity IDs separated by commas, which are added up
bool validEnergyList(const char *list);
bool nextListed(const char *&at, char *out, size_t size);  // the next ID of such a list; false at the end
bool validEntityList(const char *list);  // one or more entity IDs separated by commas ("auto" is not an ID); empty is allowed
bool multiTile(uint8_t type);  // tile types that combine several entities (people, power, temperature, humidity)
bool validEntity(const char *id);  // lower case "domain.object", nothing else: the template is built from it unescaped

}  // namespace ha
