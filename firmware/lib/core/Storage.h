#pragma once
#include <Arduino.h>

// The flash disk (LittleFS) shared by the saved Canvas drawings and the faces' caches.
namespace storage {
// {"available":bool,"free":..,"total":..,"drawings":{"files":..,"bytes":..},"other":{"files":..,"bytes":..,"list":[{"n":name,"b":bytes}]}}
String json();
// Removes every cached item: files that are not saved drawings and the F1/MotoGP cache in settings flash. Returns how many items went.
int clear();
// Formats the flash disk: every saved drawing and cached file is gone. For a disk that cannot be mounted. Returns false when that failed.
bool reformat();
}  // namespace storage
