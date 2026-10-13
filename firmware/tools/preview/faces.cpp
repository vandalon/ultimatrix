// Replays the pixel-art faces (Pac-Man, Mario, Luigi, Time in Words) at chosen times for the preview GIFs.
#include <Arduino.h>
#include <Settings.h>
#include <Console.h>
#include <fstream>
#include <string>
#include "Clockface.h"

static unsigned long virtualMs = 0;
unsigned long millis() { return virtualMs; }
void delay(unsigned long ms) { virtualMs += ms; }
namespace ezt { time_t fakeNow = 0; }
ConsoleStub Console;
void (*onFlip)(MatrixPanel_I2S_DMA *) = nullptr;
static MatrixPanel_I2S_DMA display;
static ClockTime clockTime;
static std::string outDir;
static std::ofstream timing;
static unsigned int frameNumber = 0;

static void capture() {
  char name[32];
  snprintf(name, sizeof(name), "frame-%05u.ppm", frameNumber++);
  FILE *f = fopen((outDir + "/" + name).c_str(), "wb");
  if (!f) { perror(name); exit(1); }
  fprintf(f, "P6\n64 64\n255\n");
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++) {
      uint16_t c = display.getPixel(x, y);
      unsigned char rgb[] = {(unsigned char)((c >> 11) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63),
                             (unsigned char)((c & 31) * 255 / 31)};
      fwrite(rgb, 1, sizeof(rgb), f);
    }
  fclose(f);
  timing << name << '\t' << virtualMs << '\n';
}

static time_t at(int year, int month, int day, int hour, int minute, int second) {
  struct tm date = {};
  date.tm_year = year - 1900; date.tm_mon = month - 1; date.tm_mday = day;
  date.tm_hour = hour; date.tm_min = minute; date.tm_sec = second;
  return timegm(&date) - 7200;  // the preview clock is UTC+2
}

static void setTime(time_t utc, long ms) {
  ezt::fakeNow = utc;
  clockTime.utcOffset = 7200;
  clockTime.ms = ms;
  time_t local = utc + clockTime.utcOffset;
  struct tm date;
  gmtime_r(&local, &date);
  clockTime.year = date.tm_year + 1900; clockTime.month = date.tm_mon + 1; clockTime.day = date.tm_mday;
  clockTime.hour = date.tm_hour; clockTime.minute = date.tm_min; clockTime.second = date.tm_sec; clockTime.weekday = date.tm_wday;
}

static const unsigned long kFrameMs = 50;  // the face is called at layout::frameMs
static Clockface *face;
#ifdef FACE_NIGHT_AWARE
static bool night = false;
#endif

// Runs the face for `duration` ms from the local time `utc`; restart = a face that has just booted
static void show(const char *name, unsigned long duration, time_t utc, bool restart = true) {
  fprintf(stdout, "%6lu ms: %s\n", virtualMs, name);
  setTime(utc, 0);
  if (restart) face->setup(&clockTime);
  unsigned long start = virtualMs;
  while (virtualMs - start < duration) {
    unsigned long elapsed = virtualMs - start;
    setTime(utc + elapsed / 1000, elapsed % 1000);
#ifdef FACE_NIGHT_AWARE
    face->setNight(night);
#endif
    face->update();
    capture();
    delay(kFrameMs);
  }
}

#include "faces-scenarios.h"

int main(int argc, char **argv) {
  if (argc != 2) return 1;
  outDir = argv[1];
  timing.open(outDir + "/frames.tsv");
  randomSeed(7);
  static Clockface instance(&display);
  face = &instance;
  scenarios();
  timing << "end\t" << virtualMs << '\n';
  return 0;
}
