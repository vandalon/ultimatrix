#include "Settings.h"
#include <esp_crc.h>
#include <stddef.h>

namespace {
constexpr uint32_t kMagic = 0x554D5331;
// Version 2 holds everything that is saved together: the face options each face keeps for itself, the Pac-Man
// clock colour and the location, so one write commits all of it.
struct Record {
  uint32_t magic;
  uint16_t version, size;
  uint32_t flags;
  uint8_t brightness, rotation, matchSecs, ldrPin, minimumBrightness, nightBrightness;
  uint8_t faceOptions[5], pacmanClockColor;
  uint16_t resultMins, ldrDark, ldrLight, nightStart, nightEnd;
  float latitude, longitude;
  char timezone[81], ntpServer[129], leagues[401], teams[401], drivers[401];
  uint32_t crc;
};
static_assert(sizeof(Record) < 1800, "Keep the settings record bounded");
template <typename T> uint32_t checksum(const T &record) {
  return esp_crc32_le(0, reinterpret_cast<const uint8_t *>(&record), offsetof(T, crc));
}

// Football digits, Words language, Mario/Luigi world, Words animation, sports clock effect: one slot each, so a face
// no longer reads (or overwrites) another face's choice. The running face's two are mirrored in numberFont/timeStyle.
enum Slot { Digits, Language, World, WordAnimation, SportsStyle };
#if defined(UM_WORD)
constexpr Slot kNumberSlot = Language, kStyleSlot = WordAnimation;
#elif defined(UM_MARIO) || defined(UM_LUIGI)
constexpr Slot kNumberSlot = World, kStyleSlot = SportsStyle;
#else
constexpr Slot kNumberSlot = Digits, kStyleSlot = SportsStyle;
#endif
bool terminated(const char *text, size_t size) { return memchr(text, 0, size) != nullptr; }
constexpr uint32_t kLocationFlag = 1U << 31;
}

// A light sensor is wired when the pin reads a steady value: an unconnected ADC pin floats and jumps about, a bare
// one sticks near zero. Used only to pick the first-run default for auto brightness.
static bool lightSensorReadable(uint8_t pin) {
  uint32_t sum = 0;
  uint16_t low = 4095, high = 0;
  for (int i = 0; i < 16; i++) {
    uint16_t value = analogRead(pin);
    sum += value;
    low = std::min(low, value);
    high = std::max(high, value);
    delay(3);
  }
  uint32_t mean = sum / 16;
  return mean >= 30 && mean <= 4000 && high - low < 300;
}

void Settings::load() {
  Preferences p;
  if (!p.begin("ultimatrix", false)) return;
  mdnsUseName = p.getBool("mdnsUseName", false);
  anyHost = p.getBool("anyHost", false);
  accessScope = p.getUChar("accessScope", 1);
  if (accessScope > 2) accessScope = 1;
  clockName = p.getString("clockName", "Ultimatrix");
  if (!validClockName(clockName)) clockName = "Ultimatrix";
  autoUpdate = p.getBool("autoUpdate", true);
  nightClock = p.getBool("nightClock", true);
  dither = p.getBool("dither", false);
  birthdays = p.getString("birthdays", "");
  if (birthdays.length() > 260) birthdays = "";
  {  // "order|home|lead|racing|fav|max"
    String v = p.getString("sports", "");
    int a = v.indexOf('|'), b = a < 0 ? -1 : v.indexOf('|', a + 1);
    int lead = 10, racing = 0, fav = 1, max = 4, others = 1, fbOthers = 1, hold = 15, bridge = 60;
    sportsOrder = ""; sportsHome = "";
    if (a >= 0 && b >= 0 && v.length() < 100 && sscanf(v.c_str() + b + 1, "%d|%d|%d|%d|%d|%d|%d|%d", &lead, &racing, &fav, &max, &others, &fbOthers, &hold, &bridge) >= 4) {
      sportsOrder = v.substring(0, a); sportsHome = v.substring(a + 1, b);
      sportsLead = constrain(lead, 0, 120); sportsRacing = racing >= 0 && racing <= 2 ? racing : 0; sportsFav = fav != 0; sportsMax = constrain(max, 1, 12); sportsOthers = others != 0; footballOthers = fbOthers != 0; sportsHold = constrain(hold, 0, 120); sportsBridge = constrain(bridge, 0, 240);
    }
  }
  nbaExtra = p.getUChar("nbaExtra", 0) != 0;
  nbaLiveOnly = p.getUChar("nbaLive", 1) != 0;
  nbaIdle = p.getUChar("nbaIdle", 0);
  if (nbaIdle > 2) nbaIdle = 0;
  nbaTables = p.getUChar("nbaTab", 0);
  if (nbaTables > 2) nbaTables = 0;
  nbaTeams = p.getString("nbaTeams", "");
  if (nbaTeams.length() > 60) nbaTeams = "";
  mlbExtra = p.getUChar("mlbExtra", 0) != 0;
  mlbLiveOnly = p.getUChar("mlbLive", 1) != 0;
  mlbIdle = p.getUChar("mlbIdle", 0);
  if (mlbIdle > 2) mlbIdle = 0;
  mlbTables = p.getUChar("mlbTab", 0);
  if (mlbTables > 2) mlbTables = 0;
  mlbHours = p.getUChar("mlbHrs", 10); mlbMinutes = p.getUChar("mlbMin", 5);
  if (mlbHours < 1 || mlbHours > 10) mlbHours = 10;
  if (mlbMinutes < 1 || mlbMinutes > 10) mlbMinutes = 5;
  mlbTeams = p.getString("mlbTeams", "");
  if (mlbTeams.length() > 60) mlbTeams = "";
  nflExtra = p.getUChar("nflExtra", 0) != 0;
  nflLiveOnly = p.getUChar("nflLive", 1) != 0;
  nflIdle = p.getUChar("nflIdle", 0);
  if (nflIdle > 2) nflIdle = 0;
  nflTables = p.getUChar("nflTab", 0);
  if (nflTables > 2) nflTables = 0;
  nflHours = p.getUChar("nflHrs", 10); nflMinutes = p.getUChar("nflMin", 3);
  if (nflHours < 1 || nflHours > 10) nflHours = 10;
  if (nflMinutes < 1 || nflMinutes > 10) nflMinutes = 3;
  nflTeams = p.getString("nflTeams", "");
  if (nflTeams.length() > 60) nflTeams = "";
  pongBackdrop = p.getUChar("pongBg", 3);
  if (pongBackdrop > 5) pongBackdrop = 3;
  arcHours = p.getUChar("arcHrs", 0); arcMinutes = p.getUChar("arcMin", 0); arcScore = p.getUChar("arcPts", 0);
  if (arcHours > 10) arcHours = 0;
  if (arcMinutes > 10) arcMinutes = 0;
  if (arcScore > 10) arcScore = 0;
  pongDate = p.getUChar("pongDt", 1);   // 0 hidden, 1 above the time, 2 below
  pongDay = p.getUChar("pongDy", 1);
  if (pongDate > 2) pongDate = 1;
  if (pongDay > 2) pongDay = 1;
  pongLevel = p.getUChar("pongLv", 0);
  if (pongLevel > 8) pongLevel = 0;
  pongSpeed = p.getUChar("pongSp", 2);
  if (pongSpeed > 4) pongSpeed = 2;
  Record record{};
  bool valid = p.isKey("settingsV2") && p.getBytesLength("settingsV2") == sizeof(record) &&
      p.getBytes("settingsV2", &record, sizeof(record)) == sizeof(record) &&
      record.magic == kMagic && record.version == 2 && record.size == sizeof(record) && record.crc == checksum(record) &&
      terminated(record.timezone, sizeof(record.timezone)) && terminated(record.ntpServer, sizeof(record.ntpServer)) &&
      terminated(record.leagues, sizeof(record.leagues)) && terminated(record.teams, sizeof(record.teams)) &&
      terminated(record.drivers, sizeof(record.drivers));
  if (valid) {
    brightness = record.brightness; rotation = record.rotation; matchSecs = record.matchSecs;
    memcpy(faceOptions, record.faceOptions, sizeof(faceOptions));
    numberFont = faceOptions[kNumberSlot]; timeStyle = faceOptions[kStyleSlot]; resultMins = record.resultMins;
    ldrPin = record.ldrPin; minimumBrightness = record.minimumBrightness; nightBrightness = record.nightBrightness;
    ldrDark = record.ldrDark; ldrLight = record.ldrLight; nightStart = record.nightStart; nightEnd = record.nightEnd;
    swapBlueGreen = record.flags & (1U << 0); ballRoll = record.flags & (1U << 1);
    showRankings = record.flags & (1U << 2); f1Flags = record.flags & (1U << 3); telnet = record.flags & (1U << 4);
    use24h = record.flags & (1U << 5); autoBrightness = record.flags & (1U << 6); nightEnabled = record.flags & (1U << 7);
    manualTimezone = record.flags & (1U << 8); nightSun = record.flags & (1U << 9);
    uint32_t colours = (record.flags & ~kLocationFlag) >> 10;
    if (colours >= pacmancolors::COMBINATIONS) colours = 0;
    dotColor = colours % pacmancolors::COLOR_COUNT; colours /= pacmancolors::COLOR_COUNT;
    for (int i = 3; i >= 0; i--) { ghostColors[i] = colours % pacmancolors::COLOR_COUNT; colours /= pacmancolors::COLOR_COUNT; }
    color = colours;
    {  // hours, minutes and shade share one byte: (shade * 11 + hours) * 11 + minutes
      constexpr uint8_t n = pacmancolors::COLOR_COUNT;
      uint8_t packed = record.pacmanClockColor;
      if (packed >= 2 * n * n) packed = 0;
      clockMinutesColor = packed % n; pacmanClockColor = packed / n % n; clockShade = packed / (n * n);
    }
    hasLocation = record.flags & kLocationFlag;
    if (hasLocation) { latitude = record.latitude; longitude = record.longitude; }
    timezone = record.timezone; ntpServer = record.ntpServer;
    footballLeagues = record.leagues; footballTeams = record.teams; f1Drivers = record.drivers;
  } else {
    // No valid record (a fresh or factory-reset clock; the keys of firmware before version 2 are erased at every start): the defaults
    rotation = 0; matchSecs = 8; resultMins = 120; timeStyle = 2; numberFont = 1;
    autoBrightness = lightSensorReadable(ldrPin);  // auto brightness on when a light sensor answers
    minimumBrightness = 31;  // 12 %: dimmer than this the colours lose their shades
    brightness = autoBrightness ? 255 : 128;  // full with auto brightness, otherwise half
    swapBlueGreen = false; ballRoll = true; showRankings = true; f1Flags = false; telnet = false;
    f1Drivers = "";
    footballLeagues = UM_DEFAULT_FOOTBALL_LEAGUES;
    footballTeams = UM_DEFAULT_FOOTBALL_TEAMS;
    timezone = "Europe/Amsterdam";
    pacmanClockColor = 0; clockMinutesColor = 0; clockShade = 0;
    hasLocation = false;
    faceOptions[kNumberSlot] = numberFont; faceOptions[kStyleSlot] = timeStyle;
  }
  p.end();
}

bool Settings::save() {
  if (!validClockName(clockName)) return false;
  if (timezone.length() > 80 || ntpServer.isEmpty() || ntpServer.length() > 128 ||
      footballLeagues.length() > 400 || footballTeams.length() > 400 || f1Drivers.length() > 400 || birthdays.length() > 260 ||
      sportsOrder.length() > 40 || sportsHome.length() > 15) return false;
  if (color >= pacmancolors::WALL_COUNT || dotColor >= pacmancolors::COLOR_COUNT || pacmanClockColor >= pacmancolors::COLOR_COUNT ||
      clockMinutesColor >= pacmancolors::COLOR_COUNT || clockShade > 1) return false;
  for (uint8_t ghost : ghostColors) if (ghost >= pacmancolors::COLOR_COUNT) return false;
  Record record{};
  record.magic = kMagic; record.version = 2; record.size = sizeof(record);
  record.flags = (swapBlueGreen << 0) | (ballRoll << 1) | (showRankings << 2) | (f1Flags << 3) | (telnet << 4) |
                 (use24h << 5) | (autoBrightness << 6) | (nightEnabled << 7) | (manualTimezone << 8) | ((uint32_t)nightSun << 9);
  record.flags |= pacmanColorKey() << 10;
  if (hasLocation) record.flags |= kLocationFlag;
  uint8_t options[5]; memcpy(options, faceOptions, sizeof(options));
  options[kNumberSlot] = numberFont; options[kStyleSlot] = timeStyle;
  memcpy(record.faceOptions, options, sizeof(options));
  record.pacmanClockColor = (clockShade * pacmancolors::COLOR_COUNT + pacmanClockColor) * pacmancolors::COLOR_COUNT + clockMinutesColor;
  record.latitude = latitude; record.longitude = longitude;
  record.brightness = brightness; record.rotation = rotation; record.matchSecs = matchSecs;
  record.resultMins = resultMins;
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
  bool saved = p.putBytes("settingsV2", &record, sizeof(record)) == sizeof(record);  // one write commits everything
  if (!saved) { p.remove("settingsV2"); saved = p.putBytes("settingsV2", &record, sizeof(record)) == sizeof(record); }  // the flash of settings is small: when the new record does not fit next to the old one, replace it instead of updating it
  if (saved) saved = p.putString("clockName", clockName) == clockName.length();
  if (saved) saved = p.putBool("mdnsUseName", mdnsUseName) == 1;
  if (saved) p.putBool("anyHost", anyHost);
  if (saved) p.putUChar("accessScope", accessScope);
  if (saved) p.putBool("dither", dither);
  if (saved) p.putBool("nightClock", nightClock);  // own key too: the record stays as it is
  if (saved) p.putBool("autoUpdate", autoUpdate);  // own key, so records saved before this setting keep it on
  if (saved) p.putString("birthdays", birthdays);  // own key as well: the record stays as it is
  if (saved) {
    char v[100];
    snprintf(v, sizeof(v), "%s|%s|%u|%u|%u|%u|%u|%u|%u|%u", sportsOrder.c_str(), sportsHome.c_str(), sportsLead, sportsRacing, sportsFav ? 1 : 0, sportsMax, sportsOthers ? 1 : 0, footballOthers ? 1 : 0, sportsHold, sportsBridge);
    p.putString("sports", v);
  }
  if (saved) { p.putUChar("arcHrs", arcHours); p.putUChar("arcMin", arcMinutes); p.putUChar("arcPts", arcScore); }
  if (saved) { p.putUChar("pongDt", pongDate); p.putUChar("pongDy", pongDay); }
  if (saved) p.putUChar("pongLv", pongLevel);
  if (saved) p.putUChar("pongSp", pongSpeed);
  if (saved) p.putString("nbaTeams", nbaTeams);
  if (saved) p.putUChar("nbaTab", nbaTables);
  if (saved) p.putUChar("nbaIdle", nbaIdle);
  if (saved) p.putUChar("nbaLive", nbaLiveOnly ? 1 : 0);
  if (saved) p.putUChar("nbaExtra", nbaExtra ? 1 : 0);
  if (saved) { p.putUChar("mlbHrs", mlbHours); p.putUChar("mlbMin", mlbMinutes); }
  if (saved) p.putString("mlbTeams", mlbTeams);
  if (saved) p.putUChar("mlbTab", mlbTables);
  if (saved) p.putUChar("mlbIdle", mlbIdle);
  if (saved) p.putUChar("mlbLive", mlbLiveOnly ? 1 : 0);
  if (saved) p.putUChar("mlbExtra", mlbExtra ? 1 : 0);
  if (saved) { p.putUChar("nflHrs", nflHours); p.putUChar("nflMin", nflMinutes); }
  if (saved) p.putString("nflTeams", nflTeams);
  if (saved) p.putUChar("nflTab", nflTables);
  if (saved) p.putUChar("nflIdle", nflIdle);
  if (saved) p.putUChar("nflLive", nflLiveOnly ? 1 : 0);
  if (saved) p.putUChar("nflExtra", nflExtra ? 1 : 0);
  if (saved) p.putUChar("pongBg", pongBackdrop);  // and this one: the record's size must not change
  p.end();
  if (saved) { memcpy(faceOptions, options, sizeof(options)); counter()++; }
  return saved;
}
