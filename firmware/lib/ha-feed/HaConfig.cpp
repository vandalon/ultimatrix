// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "HaConfig.h"

#include <Preferences.h>
#include <atomic>
#include <string.h>
#include "CacheBlob.h"

namespace ha {

namespace {
constexpr uint16_t kBlobVersion = 3;
std::atomic<uint32_t> savedVersion{1};
Config current;
bool loaded = false;

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
  if (!loaded) { defaults(current); loaded = true; }
  return current;
}

void load() {
  Config c;
  Preferences p;
  bool ok = p.begin("ultimatrix", true) && cacheblob::get(p, "haCfg", c, kBlobVersion);
  p.end();
  if (ok) terminateAll(c);
  else {
    defaults(c);
    // A blob of an older layout can never be read again, and it would need the room twice when the new one is written
    Preferences stale;
    if (stale.begin("ultimatrix", false)) { if (stale.isKey("haCfg")) stale.remove("haCfg"); stale.end(); }
  }
  current = c;
  loaded = true;
}

bool save() {
  Preferences p;
  if (!p.begin("ultimatrix", false)) return false;
  bool ok = cacheblob::put(p, "haCfg", config(), kBlobVersion);
  // The flash of settings is small: when the new blob does not fit next to the old one, replace it instead of updating it
  if (!ok) { p.remove("haCfg"); ok = cacheblob::put(p, "haCfg", config(), kBlobVersion); }
  p.end();
  if (ok) savedVersion.fetch_add(1);
  return ok;
}

bool configured() { return config().host[0] && config().token[0]; }

uint32_t version() { return savedVersion.load(); }

}  // namespace ha
