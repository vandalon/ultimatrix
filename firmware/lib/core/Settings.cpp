#include "Settings.h"
#include <esp_crc.h>
#include <stddef.h>

namespace {
constexpr uint32_t kMagic = 0x554D5331;
struct Record {
  uint32_t magic;
  uint16_t version, size;
  uint32_t flags;
  uint8_t brightness, rotation, matchSecs, timeStyle, numberFont, ldrPin, minimumBrightness, nightBrightness;
  uint16_t resultMins, ldrDark, ldrLight, nightStart, nightEnd;
  char timezone[81], ntpServer[129], leagues[401], teams[401], drivers[401];
  uint32_t crc;
};
static_assert(sizeof(Record) < 1800, "Keep the settings record bounded");
uint32_t checksum(const Record &record) {
  return esp_crc32_le(0, reinterpret_cast<const uint8_t *>(&record), offsetof(Record, crc));
}
bool terminated(const char *text, size_t size) { return memchr(text, 0, size) != nullptr; }
}

void Settings::load() {
  Preferences p;
  if (!p.begin("ultimatrix", false)) return;
  Record record{};
  bool valid = p.isKey("settingsV1") && p.getBytesLength("settingsV1") == sizeof(record) &&
      p.getBytes("settingsV1", &record, sizeof(record)) == sizeof(record) &&
      record.magic == kMagic && record.version == 1 && record.size == sizeof(record) && record.crc == checksum(record) &&
      terminated(record.timezone, sizeof(record.timezone)) && terminated(record.ntpServer, sizeof(record.ntpServer)) &&
      terminated(record.leagues, sizeof(record.leagues)) && terminated(record.teams, sizeof(record.teams)) &&
      terminated(record.drivers, sizeof(record.drivers));
  if (valid) {
    brightness = record.brightness; rotation = record.rotation; matchSecs = record.matchSecs;
    timeStyle = record.timeStyle; numberFont = record.numberFont; resultMins = record.resultMins;
    ldrPin = record.ldrPin; minimumBrightness = record.minimumBrightness; nightBrightness = record.nightBrightness;
    ldrDark = record.ldrDark; ldrLight = record.ldrLight; nightStart = record.nightStart; nightEnd = record.nightEnd;
    swapBlueGreen = record.flags & (1U << 0); ballRoll = record.flags & (1U << 1);
    showRankings = record.flags & (1U << 2); f1Flags = record.flags & (1U << 3); telnet = record.flags & (1U << 4);
    use24h = record.flags & (1U << 5); autoBrightness = record.flags & (1U << 6); nightEnabled = record.flags & (1U << 7);
    manualTimezone = record.flags & (1U << 8);
    timezone = record.timezone; ntpServer = record.ntpServer;
    footballLeagues = record.leagues; footballTeams = record.teams; f1Drivers = record.drivers;
  } else {
    // Legacy keys remain intact. A first save writes one versioned NVS blob;
    // the old firmware can still read its old settings after a rollback.
    brightness = constrain(p.getUInt("brightness", 32), 1U, 255U);
    rotation = std::min<uint32_t>(p.getUInt("rotation", 0), 3U);
    matchSecs = constrain(p.getUInt("matchSecs", 8), 3U, 60U);
    resultMins = min(p.getUInt("resultMins", 120), (uint32_t)1440);
    timeStyle = std::min<uint32_t>(p.getUInt("timeStyle", 2), 5U);
    numberFont = p.getUInt("numberFont", 1) == 1 ? 1 : 0;
    // Light sensor keys from the previous firmware: auto brightness was on when the bright threshold was above 1.
    uint32_t pin = p.getUInt("ldrPin", 35), dark = p.getUInt("autoBrightMin", 0), light = p.getUInt("autoBrightMax", 0);
    if (pin >= 32 && pin <= 39 && pin != 37 && pin != 38) ldrPin = pin;
    if (light > 1 && light > dark && light <= 4095) { autoBrightness = true; ldrDark = std::min<uint32_t>(dark, 4094); ldrLight = light; }
    minimumBrightness = constrain(p.getUInt("displayBrMin", 3), 1U, 255U);
    swapBlueGreen = p.getBool("swapBlueGreen", false); ballRoll = p.getBool("ballRoll", true);
    showRankings = p.getBool("showRankings", true); f1Flags = p.getBool("f1Flags", false); telnet = p.getBool("telnet", false);
    f1Drivers = p.isKey("f1Drivers") ? p.getString("f1Drivers") : "";
    footballLeagues = p.isKey("fbLeagues") ? p.getString("fbLeagues") : UM_DEFAULT_FOOTBALL_LEAGUES;
    footballTeams = p.isKey("fbTeams") ? p.getString("fbTeams") : UM_DEFAULT_FOOTBALL_TEAMS;
    timezone = p.isKey("timezone") ? p.getString("timezone") : "Europe/Amsterdam";
  }
  p.end();
}

bool Settings::save() {
  if (timezone.length() > 80 || ntpServer.isEmpty() || ntpServer.length() > 128 ||
      footballLeagues.length() > 400 || footballTeams.length() > 400 || f1Drivers.length() > 400) return false;
  Record record{};
  record.magic = kMagic; record.version = 1; record.size = sizeof(record);
  record.flags = (swapBlueGreen << 0) | (ballRoll << 1) | (showRankings << 2) | (f1Flags << 3) | (telnet << 4) |
                 (use24h << 5) | (autoBrightness << 6) | (nightEnabled << 7) | (manualTimezone << 8);
  record.brightness = brightness; record.rotation = rotation; record.matchSecs = matchSecs;
  record.timeStyle = timeStyle; record.numberFont = numberFont; record.resultMins = resultMins;
  record.ldrPin = ldrPin; record.minimumBrightness = minimumBrightness; record.nightBrightness = nightBrightness;
  record.ldrDark = ldrDark; record.ldrLight = ldrLight; record.nightStart = nightStart; record.nightEnd = nightEnd;
  strlcpy(record.timezone, timezone.c_str(), sizeof(record.timezone));
  strlcpy(record.ntpServer, ntpServer.c_str(), sizeof(record.ntpServer));
  strlcpy(record.leagues, footballLeagues.c_str(), sizeof(record.leagues));
  strlcpy(record.teams, footballTeams.c_str(), sizeof(record.teams));
  strlcpy(record.drivers, f1Drivers.c_str(), sizeof(record.drivers));
  record.crc = checksum(record);
  Preferences p;
  if (!p.begin("ultimatrix", false)) return false;
  bool saved = p.putBytes("settingsV1", &record, sizeof(record)) == sizeof(record);
  p.end();
  return saved;
}
