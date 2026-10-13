// Reads recorded MotoGP downloads (motogp-snapshot.py --save) with the real scanners and prints what the face would get.
#include <Arduino.h>
#include <stdio.h>
#include "MotoGpScan.h"

unsigned long millis() { return 0; }
void delay(unsigned long) {}
namespace ezt { time_t fakeNow = 0; }

struct FileSource { FILE *f; };
static int nextChar(void *s) { return fgetc(((FileSource *)s)->f); }
template <typename Scan> static bool scanFile(const char *path, Scan &scan) {
  FileSource src{fopen(path, "rb")};
  if (!src.f) { printf("cannot open %s\n", path); return false; }
  bool ok = JsonScan::read(nextChar, &src, Scan::leaf, &scan);
  fclose(src.f);
  return ok;
}

int main(int argc, char **argv) {
  if (argc != 6) return 2;  // events sessions standings classification live
  ezt::fakeNow = 1791800000;  // 12 Oct 2026
  EventScan events; events.now = ezt::fakeNow;
  printf("events ok=%d\n", scanFile(argv[1], events)); events.finish();
  for (int i = 0; i < eventCount; i++)
    printf("  %s %-24s %s %s %ld..%ld finished=%d\n", ::events[i].shortName, ::events[i].gp, ::events[i].place, ::events[i].country, (long)::events[i].start, (long)::events[i].end, ::events[i].finished);
  SessionScan sessions;
  printf("sessions ok=%d\n", scanFile(argv[2], sessions)); sessions.finish();
  for (int i = 0; i < sessions.count; i++) printf("  %s %s #%d %ld %s id=%s\n", sessions.found[i].type, sessions.found[i].status, sessions.found[i].number, (long)sessions.found[i].start, sessions.found[i].id, sessions.found[i].id);
  StandingScan standings;
  printf("standings ok=%d\n", scanFile(argv[3], standings)); standings.finish();
  for (int i = 0; i < standings.count; i++) printf("  %2d %s %-16s %d colour %04x\n", i + 1, standings.rows[i].code, standings.rows[i].name, standings.rows[i].points, standings.rows[i].color);
  ResultScan result; result.tag = 'R';
  printf("classification ok=%d\n", scanFile(argv[4], result)); result.finish();
  for (int i = 0; i < result.count; i++) printf("  %2d %s %-8s fin=%d\n", i + 1, result.rows[i].code, result.rows[i].time, result.rows[i].finished);
  LiveScan live; live.tag = 'R';
  printf("live ok=%d\n", scanFile(argv[5], live)); live.finish();
  printf("  session %s status %s remaining %s laps %d leaderLap %d rows %d\n", live.shortName, live.status, live.remaining, live.totalLaps, live.leaderLap, live.rowCount);
  for (int i = 0; i < live.rowCount; i++) printf("  %2d %s %-8s pit=%d\n", i + 1, live.rows[i].code, live.rows[i].time, live.rows[i].pit);
  return 0;
}
