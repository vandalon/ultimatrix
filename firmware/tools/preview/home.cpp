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
bool configured() { return gConfigured; }
uint32_t version() { return 1; }
}  // namespace ha
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
  s.tile[0] = 22.4f; s.tile[1] = 52; s.tile[2] = 2; s.tileOk = 7;
  for (int i = 0; i < 6; i++) { s.iconCount[i] = 0; s.iconAge[i] = 0; }
  s.iconOk = 63;
  s.iconCount[3] = 1; s.iconCount[4] = 3; s.iconCount[5] = 1;
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
  strlcpy(gConfig.energy[ha::E_USAGE], "sensor.house_power", ha::kEntity);
  strlcpy(gConfig.energy[ha::E_SOLAR], "sensor.solar_power", ha::kEntity);
  strlcpy(gConfig.energy[ha::E_BATTERY], "sensor.battery_power", ha::kEntity);
  strlcpy(gConfig.energy[ha::E_SOC], "sensor.battery_soc", ha::kEntity);
  strlcpy(gConfig.energy[ha::E_GRID], "sensor.grid_power", ha::kEntity);
  gConfig.icons[3].autoMode = 0;
  strlcpy(gConfig.icons[3].entity, "binary_sensor.washer_running", ha::kEntity);
  gConfig.icons[3].type = ha::ICON_WASHER;
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
  const float use[] = {1.2f, 1.5f, 0.9f, 0.4f, 0.25f, 0.2f, 0.2f, 0.2f, 0.2f, 0.22f, 0.3f, 0.6f, 1.1f, 0.9f, 0.5f, 0.4f, 0.45f, 0.84f};
  const float sun[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.1f, 0.5f, 1.0f, 1.6f, 2.0f, 2.3f, 2.1f};
  for (int i = 0; i < 18; i++) {
    ha::Snapshot hour = s;
    hour.energy[ha::E_USAGE] = use[i]; hour.energy[ha::E_SOLAR] = sun[i];
    feed(hour);
    clockTime.day = 14 + (i + 20 >= 24 ? 1 : 0);
    clockTime.hour = (20 + i) % 24;
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
    ha::Snapshot finished = s; finished.iconCount[3] = 0;
    feed(finished); step(50);
    run(3000);
    feed(s); virtualMs += 12000; step(50);
    run(2000);
    timeline << "end\t" << film << '\n';
    return 0;
  }
  gConfig.pages = ha::PAGE_HOME; step(50);
  capture("Home page");
  gConfig.pages = ha::PAGE_ENERGY; step(50);
  capture("Energy, solar producing and exporting");

  // Evening, no solar, battery discharging, importing a little
  setClock(21, 47);
  ha::Snapshot e = base();
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
  ha::Snapshot washer = e; washer.iconCount[3] = 0;
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

  // Not connected
  HaClient::instance()._state = HaClient::FAILED;
  face.invalidate(); step(50);
  capture("Home Assistant not reachable");
  gConfigured = false;
  face.invalidate(); step(50);
  capture("Not set up");
  return 0;
}
