#pragma once
#include <Preferences.h>
#include <string.h>

// The Wi-Fi networks the clock knows (NVS blob "wifiNets" in the "ultimatrix" namespace): at most five, the one that connected
// longest ago makes room for a new one. `seq` grows with every successful connection, so the highest is the last one that worked.
namespace wifinets {

constexpr int kMax = 5;
struct Net { char ssid[33]; char pass[65]; uint32_t seq; };
struct List { Net net[kMax]; uint8_t count; };

inline void load(List &l) {
  memset(&l, 0, sizeof(l));
  Preferences prefs;
  if (!prefs.begin("ultimatrix", true)) return;
  if (prefs.getBytesLength("wifiNets") == sizeof(List)) prefs.getBytes("wifiNets", &l, sizeof(List));
  prefs.end();
  if (l.count > kMax) l.count = 0;
}

inline void save(const List &l) {
  Preferences prefs;
  if (!prefs.begin("ultimatrix", false)) return;
  prefs.putBytes("wifiNets", &l, sizeof(List));
  prefs.end();
}

inline int find(const List &l, const char *ssid) {
  for (int i = 0; i < l.count; i++) if (!strcmp(l.net[i].ssid, ssid)) return i;
  return -1;
}

// A network that connected: new, or moved to the front. Returns the entry that made room (its name), or "" when none was dropped.
inline void remember(const char *ssid, const char *pass, char *dropped = nullptr, size_t droppedSize = 0) {
  if (dropped && droppedSize) dropped[0] = 0;
  if (!ssid || !*ssid) return;
  List l;
  load(l);
  uint32_t top = 0;
  for (int i = 0; i < l.count; i++) if (l.net[i].seq > top) top = l.net[i].seq;
  int at = find(l, ssid);
  if (at < 0) {
    if (l.count < kMax) at = l.count++;
    else {
      at = 0;
      for (int i = 1; i < l.count; i++) if (l.net[i].seq < l.net[at].seq) at = i;
      if (dropped && droppedSize) strlcpy(dropped, l.net[at].ssid, droppedSize);
    }
    memset(&l.net[at], 0, sizeof(Net));
    strlcpy(l.net[at].ssid, ssid, sizeof(l.net[at].ssid));
  }
  strlcpy(l.net[at].pass, pass ? pass : "", sizeof(l.net[at].pass));
  l.net[at].seq = top + 1;
  save(l);
}

}  // namespace wifinets
