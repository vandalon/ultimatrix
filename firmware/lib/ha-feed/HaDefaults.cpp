// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "HaConfig.h"

#include <string.h>

// The parts of the settings that need no flash: defaults and the rules for slot types and entity IDs
namespace ha {

void defaults(Config &c) {
  memset(&c, 0, sizeof(c));
  strlcpy(c.host, "homeassistant.local", sizeof(c.host));
  c.port = 8123;
  c.pages = PAGE_HOME | PAGE_ENERGY | PAGE_DEVICES;
  c.pageSecs = 10;
  c.gridImportPositive = 1;
  c.batteryDischargePositive = 1;
  c.tiles[0] = {TILE_TEMP, 1, 0, 0, 0, "", ""};
  c.tiles[1] = {TILE_HUMIDITY, 1, 0, 0, 0, "", ""};
  c.tiles[2] = {TILE_EV, 0, 0, 0, 0, "", ""};
  c.alarmFlags = AF_NIGHT | AF_BRIGHT | AF_FLASH;  // an alarm is not to be missed
  c.doorbellFlags = AF_NIGHT | AF_BRIGHT;
  const uint8_t icons[kIcons] = {ICON_LOW_BATTERY, ICON_OFFLINE, ICON_LEAK, ICON_BLINDS, ICON_MEDIA, ICON_HEATING};
  for (int i = 0; i < kIcons; i++) c.icons[i] = {icons[i], uint8_t(autoAvailable(false, icons[i]) ? 1 : 0), 0, 0, 0, "", ""};
}

bool autoAvailable(bool tile, uint8_t type) {
  if (tile) return type == TILE_TEMP || type == TILE_HUMIDITY || type == TILE_PEOPLE || type == TILE_WEATHER || type == TILE_OUTDOOR_HUMIDITY || type == TILE_PRICE || type == TILE_EV;
  return type == ICON_DOOR || type == ICON_WINDOW || type == ICON_GARAGE || type == ICON_LIGHTS || type == ICON_LOCK || type == ICON_PRESENCE || type == ICON_CHARGER || (type >= ICON_LOW_BATTERY && type <= ICON_HEATING);
}

bool validEntity(const char *id) {
  if (!id || !*id) return true;  // empty is allowed: the slot is simply off
  bool dot = false;
  size_t n = 0;
  for (const char *c = id; *c; ++c, ++n) {
    if (*c == '.') { if (dot || n == 0) return false; dot = true; }
    else if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_')) return false;
  }
  return dot && n < kEntity && id[n - 1] != '.';
}

bool nextListed(const char *&at, char *out, size_t size) {
  while (*at == ',') ++at;
  if (!*at) return false;
  size_t n = 0;
  while (*at && *at != ',') { if (n + 1 < size) out[n++] = *at; ++at; }
  out[n] = 0;
  return true;
}

bool validEnergyList(const char *list) {
  if (!list || !*list || !strcmp(list, "auto")) return true;
  const char *at = list;
  char id[kEntity];
  int count = 0;
  while (nextListed(at, id, sizeof(id))) { if (!id[0] || !validEntity(id)) return false; count++; }
  return count > 0;
}

bool validEntityList(const char *list) {
  if (!list || !*list) return true;
  if (!strcmp(list, "auto")) return false;
  const char *at = list;
  char id[kEntity];
  int count = 0;
  while (nextListed(at, id, sizeof(id))) { if (strcmp(id, "-") && strcmp(id, "off") && !validEntity(id)) return false; count++; }  // EV tile: "-" is a part left to detection, "off" one that is not used
  return count > 0;
}

bool multiTile(uint8_t type) { return type == TILE_TEMP || type == TILE_HUMIDITY || type == TILE_PEOPLE || type == TILE_POWER; }

}  // namespace ha
