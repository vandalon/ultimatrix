// Renders the shared boot screens (logo, connecting, access point QR code, connected, sync, error) for docs/previews/boot.gif.
#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <string>
#include <fstream>
#include "BootScreen.h"

static unsigned long virtualMs = 0;
unsigned long millis() { return virtualMs; }
void delay(unsigned long ms) { virtualMs += ms; }
void (*onFlip)(MatrixPanel_I2S_DMA *) = nullptr;
static MatrixPanel_I2S_DMA display;
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

// The main loop's pace (layout::frameMs); a frame is captured only when the screen redrew
static void play(boot::Screen &screen, boot::Kind kind, const char *name, unsigned long duration, const char *text = "") {
  fprintf(stdout, "%5lu ms: %s\n", virtualMs, name);
  unsigned long start = virtualMs;
  while (virtualMs - start < duration) {
    if (screen.update(display, kind, virtualMs - start, text)) capture();
    delay(50);
  }
}

int main(int argc, char **argv) {
  if (argc != 2) return 1;
  outDir = argv[1];
  timing.open(outDir + "/frames.tsv");
  boot::Screen screen;
  screen.setAccessPoint("UM-SETUP", "", "192.168.4.1");  // the open access point the Wi-Fi manager starts
  play(screen, boot::Kind::Logo, "the U logo fades in", 1000);
  play(screen, boot::Kind::Connecting, "connecting: the U dims, the blue Wi-Fi icon pulses", 4800);
  play(screen, boot::Kind::Setup, "access point: scan to join, SSID and address fade in turn", 12000);
  play(screen, boot::Kind::Joined, "someone joined the access point", 3000);
  play(screen, boot::Kind::Connecting, "connecting again", 2400);
  play(screen, boot::Kind::Syncing, "linked, syncing the time: blue, address shown", 3000, "192.168.100.100");
  play(screen, boot::Kind::Connected, "linked and synced: green", 3000, "192.168.100.100");
  play(screen, boot::Kind::Error, "no time after a while: NTP error", 4800, "192.168.100.100");
  timing << "end\t" << virtualMs << '\n';
  return 0;
}
