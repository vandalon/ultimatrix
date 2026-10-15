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
  c.pages = PAGE_HOME | PAGE_ENERGY;
  c.pageSecs = 10;
  c.gridImportPositive = 1;
  c.batteryDischargePositive = 1;
  c.tiles[0] = {TILE_TEMP, 1, 0, 0, 0, "", ""};
  c.tiles[1] = {TILE_HUMIDITY, 1, 0, 0, 0, "", ""};
  c.tiles[2] = {TILE_PEOPLE, 1, 0, 0, 0, "", ""};
  c.alarmFlags = AF_NIGHT | AF_BRIGHT | AF_FLASH;  // an alarm is not to be missed
  c.doorbellFlags = AF_NIGHT | AF_BRIGHT;
  const uint8_t icons[kIcons] = {ICON_DOOR, ICON_WINDOW, ICON_GARAGE, ICON_WASHER, ICON_LIGHTS, ICON_LOCK};
  for (int i = 0; i < kIcons; i++) c.icons[i] = {icons[i], uint8_t(autoAvailable(false, icons[i]) ? 1 : 0), uint8_t(i == 2 ? 10 : 0), 0, 0, "", ""};
}

bool autoAvailable(bool tile, uint8_t type) {
  if (tile) return type == TILE_TEMP || type == TILE_HUMIDITY || type == TILE_PEOPLE;
  return type == ICON_DOOR || type == ICON_WINDOW || type == ICON_GARAGE || type == ICON_LIGHTS || type == ICON_LOCK;
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

}  // namespace ha
