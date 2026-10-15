// Renders the Home Assistant face's screens with the real face code and made-up Home Assistant values.
// Usage: home <output folder>; writes one PPM per scene and scenes.tsv (file, caption).
#include <Arduino.h>
#include <Console.h>
#include <fstream>
#include <string>
#include <atomic>
#include <mutex>
#include "HaTemplate.h"
#define private public  // the preview sets what the connection task would
#include "HaClient.h"
#undef private
#include "Clockface.h"
#include "HaConfig.h"
#include "HaMedia.h"

static unsigned long virtualMs = 100000;
unsigned long millis() { return virtualMs; }
void delay(unsigned long ms) { virtualMs += ms; }
namespace ezt { time_t fakeNow = 0; }
ConsoleStub Console;
void (*onFlip)(MatrixPanel_I2S_DMA *) = nullptr;
std::atomic<bool> firmwareUpdating{false};

// The values Home Assistant would give, set per scene
static ha::Snapshot gSnapshot;
static bool gConfigured = true;
static ha::Config gConfig;
namespace ha {
Config &config() { return gConfig; }
char (&weatherEntity())[kEntity] { static char entity[kEntity] = {}; return entity; }
Slot &extraTile(int index) { static Slot slots[kHomeTiles - kTiles] = {{TILE_WEATHER, 1, 0, 0, 0, "", ""}, {TILE_NONE, 0, 0, 0, 0, "", ""}, {TILE_NONE, 0, 0, 0, 0, "", ""}}; return slots[index - kTiles]; }
const Slot &tileSlot(const Config &c, int index) { return index < kTiles ? c.tiles[index] : extraTile(index); }
Slot &washerAlert() { static Slot slot = {ICON_WASHER, 0, 0, 0, 0, "", "binary_sensor.washer_running"}; return slot; }
bool configured() { return gConfigured; }
char (*mediaPlayers())[kEntity] { static char rooms[kRooms][kEntity] = {}; return rooms; }
uint32_t version() { return 1; }
}  // namespace ha
// Now playing: what the player task and the art fetch would give
static ha::media::Snapshot gMedia;
static uint16_t gArt[ha::media::kArt * ha::media::kArt];
static bool gArtReady = true;
static uint32_t gMediaVersion = 1;
namespace ha { namespace media {
uint32_t version() { return gMediaVersion; }
bool snapshot(Snapshot &out) { out = gMedia; return true; }
void release() {}
ArtState art(const char *, const uint16_t **pixels, uint16_t *id) { if (!gArtReady) return ART_LOADING; *pixels = gArt; *id = 1; return ART_READY; }
} }
HaClient &HaClient::instance() { static HaClient client; return client; }
void HaClient::begin() {}
bool HaClient::snapshot(ha::Snapshot &out) { out = gSnapshot; return _state == LIVE; }
void HaClient::history(History &out) { out = History{}; }
bool HaClient::energyIds(char (*)[ha::kEntity]) { return false; }
void HaClient::status(char *out, size_t size) { strlcpy(out, _state == FAILED ? "Home Assistant refused the token" : "Connecting", size); }

static MatrixPanel_I2S_DMA display;
static ClockTime clockTime;
static std::string outDir;
static std::ofstream scenes;
static int sceneNumber = 0;

static void writePpm(const char *name) {
  FILE *f = fopen((outDir + "/" + name).c_str(), "wb");
  if (!f) { perror(name); exit(1); }
  fprintf(f, "P6\n64 64\n255\n");
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++) {
      uint16_t c = display.getPixel(x, y);
      unsigned char rgb[] = {(unsigned char)((c >> 11) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63), (unsigned char)((c & 31) * 255 / 31)};
      fwrite(rgb, 1, sizeof(rgb), f);
    }
  fclose(f);
}

static void capture(const char *caption) {
  char name[32];
  snprintf(name, sizeof(name), "scene-%02d.ppm", sceneNumber++);
  writePpm(name);
  scenes << name << '\t' << caption << '\n';
}

static void setClock(int hour, int minute) {
  clockTime.hour = hour; clockTime.minute = minute; clockTime.day = 15; clockTime.month = 10;
}

static void feed(const ha::Snapshot &snapshot) { gSnapshot = snapshot; HaClient::instance()._state = HaClient::LIVE; HaClient::instance()._updates++; }

static ha::Snapshot base() {
  ha::Snapshot s;
  memset(&s, 0, sizeof(s));
  // Home page: 22 degrees, 52 % humidity, 2 people home; the garage is open, the washer runs, lights on, house secured
  s.tile[3] = 14; strlcpy(s.weather[3], "partlycloudy", sizeof(s.weather[3]));
  s.tile[0] = 22.4f; s.tile[1] = 52; s.tile[2] = 78; s.tileOk = 15;
  for (int i = 0; i < 6; i++) { s.iconCount[i] = 0; s.iconAge[i] = 0; }
  s.iconOk = 63;
  s.washerCount = 1; s.washerOk = 1;
  const uint8_t counts[ha::kIcons] = {2, 1, 0, 3, 1, 2};
  memcpy(s.iconCount, counts, sizeof(counts));
  return s;
}

static void energyValues(ha::Snapshot &s, float usage, float solar, float battery, float soc, float grid, float usageKwh, float solarKwh, float batteryKwh, float gridKwh) {
  const float values[ha::E_COUNT] = {usage, solar, battery, soc, grid, usageKwh, solarKwh, batteryKwh, gridKwh};
  for (int i = 0; i < ha::E_COUNT; i++) s.energy[i] = values[i];
  s.energyOk = (1 << ha::E_COUNT) - 1;
}

// A day of hours, ending now: usage and solar in kW
static void history(Clockface &) {}

int main(int argc, char **argv) {
  if (argc < 2) return 1;
  const bool story = argc == 3 && std::string(argv[2]) == "story";  // frames.tsv and a timed run for the GIF, instead of the scenes
  outDir = argv[1];
  scenes.open(outDir + "/scenes.tsv");
  ha::defaults(gConfig);
  const uint8_t previousIcons[ha::kIcons] = {ha::ICON_DOOR, ha::ICON_WINDOW, ha::ICON_GARAGE, ha::ICON_PRESENCE, ha::ICON_LIGHTS, ha::ICON_LOCK};
  strlcpy(gConfig.tiles[2].entity, "sensor.ev_charge", ha::kEntity);
  strlcpy(gConfig.energy[ha::E_USAGE], "sensor.house_power", ha::kEntity);
  strlcpy(gConfig.energy[ha::E_SOLAR], "sensor.solar_power", ha::kEntity);
  strlcpy(gConfig.energy[ha::E_BATTERY], "sensor.battery_power", ha::kEntity);
  strlcpy(gConfig.energy[ha::E_SOC], "sensor.battery_soc", ha::kEntity);
  strlcpy(gConfig.energy[ha::E_GRID], "sensor.grid_power", ha::kEntity);
  gConfig.pageSecs = 10;

  static Clockface face(&display);
  setClock(13, 5);
  clockTime.hour = 13;
  face.setup(&clockTime);

  auto step = [&](unsigned long ms) { virtualMs += ms; face.poll(); face.update(); };
  auto phase = [&](int wanted) { while ((int)((virtualMs / 250) & 1) != wanted) virtualMs += 50; step(0); };  // the animation phase of flashing popups

  // Midday, solar producing, exporting. A day of hourly samples first, so the bars fill
  ha::Snapshot s = base();
  energyValues(s, 0.84f, 2.1f, 0.0f, 80, -1.3f, 4.2f, 6.8f, 1.1f, -2.6f);
  feed(s);
  const float use[] = {0.8f, 1.0f, 1.2f, 1.5f, 0.9f, 0.4f, 0.25f, 0.2f, 0.2f, 0.2f, 0.2f, 0.22f, 0.3f, 0.6f, 1.1f, 0.9f, 0.5f, 0.4f, 0.45f, 0.84f};
  const float sun[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.1f, 0.5f, 1.0f, 1.6f, 2.0f, 2.3f, 2.1f};
  const int firstHour = 24 + 14 - home::kBars;
  for (int i = 0; i < home::kBars; i++) {
    ha::Snapshot hour = s;
    hour.energy[ha::E_USAGE] = use[i]; hour.energy[ha::E_SOLAR] = sun[i];
    feed(hour);
    clockTime.day = 14 + (i + firstHour >= 24 ? 1 : 0);
    clockTime.hour = (firstHour + i) % 24;
    step(50);
  }
  clockTime.day = 15; clockTime.hour = 13;
  feed(s);
  if (story) {
    // A short film: both pages rotating, the doorbell flashing, the washer finishing, back to the home page
    std::ofstream timeline(outDir + "/frames.tsv");
    unsigned number = 0;
    unsigned long film = 0;  // only the seconds that are filmed count: the popups that run out unseen are skipped
    auto run = [&](unsigned long ms) {
      for (unsigned long t = 0; t < ms; t += 100) {
        step(50); step(50);
        char name[32];
        snprintf(name, sizeof(name), "frame-%05u.ppm", number++);
        writePpm(name);
        timeline << name << '\t' << film << '\n';
        film += 100;
      }
    };
    gConfig.pages = ha::PAGE_HOME | ha::PAGE_ENERGY;
    gConfig.pageSecs = 4;
    face.setup(&clockTime);
    run(8000);  // the Energy page first, then the Home page
    gConfig.pages = ha::PAGE_ENERGY;
    gConfig.doorbellFlags = ha::AF_NIGHT | ha::AF_BRIGHT | ha::AF_FLASH;
    ha::Snapshot ringing = s; ringing.doorbell = 1;
    feed(ringing); step(50);
    run(3500);
    feed(s); virtualMs += 12000; step(50);  // the popup runs out unseen
    ha::Snapshot finished = s; finished.washerCount = 0;
    feed(finished); step(50);
    run(3000);
    feed(s); virtualMs += 12000; step(50);
    run(2000);
    timeline << "end\t" << film << '\n';
    return 0;
  }
  gConfig.pages = ha::PAGE_HOME; step(50);
  capture("Home page");
  const ha::Config original = gConfig;
  const uint8_t statusIcons[ha::kIcons] = {ha::ICON_LOW_BATTERY, ha::ICON_OFFLINE, ha::ICON_LEAK, ha::ICON_BLINDS, ha::ICON_MEDIA, ha::ICON_HEATING};
  for (int i = 0; i < ha::kIcons; i++) gConfig.icons[i] = {statusIcons[i], 1, 0, 0, 0, "", ""};
  ha::Snapshot status = s;
  const uint8_t counts[ha::kIcons] = {2, 1, 0, 3, 1, 2};
  memcpy(status.iconCount, counts, sizeof(counts));
  feed(status); step(50); capture("Low batteries, offline devices, leaks, blinds, media, heating");
  status.iconCount[2] = 1; feed(status); step(50); capture("Water leak active");
  gConfig = original;
  for (int i = 0; i < ha::kIcons; i++) gConfig.icons[i] = {previousIcons[i], 1, uint8_t(i == 2 ? 10 : 0), 0, 0, "", ""};
  feed(s); step(50);
  const char *conditions[] = {"sunny", "clear-night", "cloudy", "rainy", "pouring", "snowy", "snowy-rainy", "hail", "fog", "windy", "windy-variant", "lightning", "lightning-rainy", "exceptional", "unavailable"};
  for (const char *condition : conditions) {
    ha::Snapshot weather = s;
    strlcpy(weather.weather[3], condition, sizeof(weather.weather[3]));
    if (!strcmp(condition, "unavailable")) weather.tileOk &= ~(1 << 3);
    feed(weather); step(50); capture(condition);
  }
  feed(s); step(50);
  const ha::Config savedConfig = gConfig;
  const ha::Slot savedFourth = ha::extraTile(3);
  gConfig.tiles[0].type = ha::TILE_CO2; gConfig.tiles[1].type = ha::TILE_RAIN;
  ha::Snapshot options = s; options.tile[0] = 850; options.tile[1] = 1.2f;
  feed(options); step(50); capture("CO2, rain, EV charge, weather");
  gConfig.tiles[0].type = ha::TILE_WEATHER; gConfig.tiles[1].type = ha::TILE_PRICE;
  gConfig.tiles[2].type = ha::TILE_PEOPLE; ha::extraTile(3).type = ha::TILE_OUTDOOR_HUMIDITY;
  options.tile[0] = 14; options.tile[1] = 0.25f; options.tile[2] = 2; options.tile[3] = 81;
  strlcpy(options.weather[0], "sunny", sizeof(options.weather[0]));
  feed(options); step(50); capture("Weather moved, energy price, people, outdoor humidity");
  ha::extraTile(3).type = ha::TILE_NONE; step(50); capture("Fourth tile off");
  // Six tiles, then fewer: the ones that are on spread evenly
  gConfig = savedConfig; ha::extraTile(3) = savedFourth;
  ha::extraTile(4).type = ha::TILE_CO2; ha::extraTile(5).type = ha::TILE_PEOPLE;
  ha::Snapshot six = s; six.tile[4] = 850; six.tile[5] = 2; six.tileOk = 63;
  feed(six); step(50); capture("Six tiles");
  ha::extraTile(5).type = ha::TILE_NONE; feed(six); step(50); capture("Five tiles");
  ha::extraTile(4).type = ha::TILE_NONE; ha::extraTile(3).type = ha::TILE_NONE; step(50); capture("Three tiles");
  gConfig.tiles[2].type = ha::TILE_NONE; step(50); capture("Two tiles");
  gConfig.tiles[1].type = ha::TILE_NONE; step(50); capture("One tile");
  gConfig = savedConfig; ha::extraTile(3) = savedFourth; ha::extraTile(4).type = ha::TILE_NONE; ha::extraTile(5).type = ha::TILE_NONE; feed(s); step(50);
  ha::Snapshot car = s; car.tile[2] = 1078; feed(car); step(50); capture("EV plugged in");  // charge + 1000 plugged in, + 2000 charging
  car.tile[2] = 2078; feed(car); step(50); capture("EV charging");
  feed(s); step(50);
  gConfig.pages = ha::PAGE_ENERGY; step(50);
  capture("Energy, solar producing and exporting");

  // Evening, no solar, battery discharging, importing a little
  setClock(21, 47);
  ha::Snapshot e = base();
  strlcpy(e.weather[3], "clear-night", sizeof(e.weather[3])); e.tile[3] = 9;
  energyValues(e, 1.7f, 0.0f, 1.2f, 62, 0.5f, 11.2f, 8.9f, 3.4f, 2.3f);
  feed(e);
  gConfig.pages = ha::PAGE_HOME; step(50);
  capture("Evening, home page");
  gConfig.pages = ha::PAGE_ENERGY; step(50);
  capture("Energy, battery supplies the house");

  // Night, nothing from the battery, no solar
  setClock(2, 13);
  ha::Snapshot n = base();
  energyValues(n, 0.2f, 0.0f, 0.0f, 55, 0.2f, 11.9f, 8.9f, 3.4f, 2.7f);
  feed(n);
  step(50);
  capture("Energy, no solar and an idle battery");

  // Alerts over the home page
  setClock(21, 47);
  gConfig.pages = ha::PAGE_HOME;
  feed(e); step(50);
  ha::Snapshot bell = e; bell.doorbell = 1;
  feed(bell); step(50);
  capture("Alert: doorbell");
  virtualMs += 500; step(50);
  capture("Alert: doorbell, other frame");
  virtualMs += 20000; step(50);
  ha::Snapshot washer = e; washer.washerCount = 0;
  feed(e); step(50);
  feed(washer); step(50);
  capture("Alert: washer done");
  virtualMs += 20000; step(50);
  ha::Snapshot garage = e; garage.iconCount[2] = 1; garage.iconAge[2] = 12;
  feed(garage); step(50);
  capture("Alert: garage open for 12 minutes");
  virtualMs += 20000; step(50);
  capture("Garage open, icon row only");
  ha::Snapshot alarm = e; alarm.alarm = 1;
  feed(alarm); step(50);
  phase(0);
  capture("Alert: alarm");
  phase(1);
  capture("Alert: alarm, other frame");

  // The same popups set to flash the whole panel
  gConfig.doorbellFlags = ha::AF_NIGHT | ha::AF_BRIGHT | ha::AF_FLASH;
  gConfig.alarmFlags = ha::AF_NIGHT | ha::AF_BRIGHT | ha::AF_FLASH;
  feed(e); step(50);
  feed(bell); gConfig.doorbellFlags = ha::AF_FLASH; step(50);  // a doorbell that rings again once the alarm is over
  feed(e); step(50);
  virtualMs += 30000; step(50);
  ha::Snapshot ring = e; ring.doorbell = 1;
  feed(ring); step(50);
  phase(0);
  capture("Doorbell flashing, dark phase");
  phase(1);
  capture("Doorbell flashing, lit phase");
  virtualMs += 30000; step(50);
  feed(alarm); step(50);
  phase(0);
  capture("Alarm flashing, dark phase");
  phase(1);
  capture("Alarm flashing, lit phase");

  firmwareUpdating = true; step(50);
  capture("Updating the panel");
  firmwareUpdating = false;

  // Now playing: an album cover made of a gradient and a disc, in two rooms
  for (int y = 0; y < ha::media::kArt; y++)
    for (int x = 0; x < ha::media::kArt; x++) {
      const int dx = x - 32, dy = y - 26, d2 = dx * dx + dy * dy;
      const int r = 200 - y * 2, g = 60 + x, b = 80 + y * 2;
      gArt[y * ha::media::kArt + x] = d2 < 300 ? 0xFFE0 : d2 < 360 ? 0x0000 : ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }
  strlcpy(gMedia.player[0].room, "Living room", ha::media::kRoom);
  strlcpy(gMedia.player[0].title, "Blue in Green", ha::media::kText);
  strlcpy(gMedia.player[0].artist, "Miles Davis", ha::media::kText);
  strlcpy(gMedia.player[0].picture, "/api/media_player_proxy/media_player.living_room?token=x", ha::media::kPicture);
  strlcpy(gMedia.player[1].room, "Kitchen", ha::media::kRoom);
  strlcpy(gMedia.player[1].title, "Everything In Its Right Place", ha::media::kText);
  strlcpy(gMedia.player[1].artist, "Radiohead and a very long list of guests", ha::media::kText);
  strlcpy(gMedia.player[1].picture, "/api/media_player_proxy/media_player.kitchen?token=y", ha::media::kPicture);
  gMedia.player[0].position = 95; gMedia.player[0].duration = 340; gMedia.player[1].position = 40; gMedia.player[1].duration = 0;
  gMedia.count = 2; gMediaVersion++;
  gConfig.pages = ha::PAGE_MEDIA; gConfig.pageSecs = 10;
  face.setup(&clockTime); feed(s); step(50);
  capture("Now playing: short title");
  gMedia.player[0] = gMedia.player[1]; gMediaVersion++; virtualMs += 1000; step(50); step(50);
  capture("Now playing: long text, scrolling");
  virtualMs += 3000; step(50);
  capture("Now playing: Kitchen, later in the scroll");
  gArtReady = true; gMedia.count = 1; gMediaVersion++;
  gConfig.pages = ha::PAGE_HOME | ha::PAGE_ART_BACKDROP; step(50); step(50);
  capture("Album art, dimmed, behind the home page");
  gConfig.pages = ha::PAGE_ENERGY | ha::PAGE_ART_BACKDROP; step(50); step(50);
  capture("Album art, dimmed, behind the energy page");
  gConfig.pages = ha::PAGE_MEDIA; step(50);
  gArtReady = false; gMedia.count = 1; gMediaVersion++; step(50); step(50);
  capture("Now playing: album art loading");

  // Not connected
  HaClient::instance()._state = HaClient::FAILED;
  face.invalidate(); step(50);
  capture("Home Assistant not reachable");
  gConfigured = false;
  face.invalidate(); step(50);
  capture("Not set up");
  return 0;
}
