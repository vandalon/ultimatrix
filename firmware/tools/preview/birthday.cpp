// Renders the birthday screens frame by frame for the preview GIFs: birthday <out dir> <party> <seconds> [name] [age] [time]
#include <Arduino.h>
#include <fstream>
#include <string>
#include <BirthdayParty.h>

unsigned long millis() { return 0; }

struct Panel {
  uint8_t rgb[64 * 64 * 3];
  void drawPixelRGB888(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *o = &rgb[(y * 64 + x) * 3];
    o[0] = r; o[1] = g; o[2] = b;
  }
};

int main(int argc, char **argv) {
  if (argc < 4) return 1;
  std::string out = argv[1], scene = argv[2];
  int seconds = atoi(argv[3]);
  const char *name = argc > 4 ? argv[4] : "ELIOT";
  int age = argc > 5 ? atoi(argv[5]) : 7;
  const char *time = argc > 6 ? argv[6] : "17:42";
  const int frameMs = 60;
  std::ofstream timing(out + "/frames.tsv");
  static Panel panel;
  for (int n = 0, ms = 0; ms < seconds * 1000; n++, ms += frameMs) {
    birthday::party::draw(panel, ms, name, age, time);
    char file[32];
    snprintf(file, sizeof(file), "frame-%05d.ppm", n);
    FILE *f = fopen((out + "/" + file).c_str(), "wb");
    fprintf(f, "P6\n64 64\n255\n");
    fwrite(panel.rgb, 1, sizeof(panel.rgb), f);
    fclose(f);
    timing << file << '\t' << ms << '\n';
  }
  timing << "end\t" << seconds * 1000 << '\n';
}
