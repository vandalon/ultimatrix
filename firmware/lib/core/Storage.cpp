#include "Storage.h"
#include "DiskCache.h"
#include "Console.h"
#include <Preferences.h>
#include <nvs.h>

namespace storage {
namespace {
constexpr const char *kF1Cache = "um-f1cache";  // the F1 and MotoGP faces keep their cache in settings flash, not on the disk

// Saved drawings are /cv0.bin to /cv7.bin (and the temporary file of a save in progress): see WledCanvas.cpp.
bool drawing(String name) {
  if (!name.startsWith("/")) name = "/" + name;
  return name == "/canvas-save.tmp" || (name.length() == 8 && name.startsWith("/cv") && name.endsWith(".bin") && isDigit(name[3]));
}

struct Totals {
  int files = 0;
  size_t bytes = 0;
  String list;
  void add(const String &name, size_t size) {
    if (files < 24) list += String(files ? "," : "") + "{\"n\":\"" + name + "\",\"b\":" + (unsigned)size + "}";
    bytes += size;
    files++;
  }
};

// The F1 cache entries (each blob or number is one item).
void f1Entries(Totals &other) {
  Preferences prefs;
  if (!prefs.begin(kF1Cache, true)) return;
  nvs_iterator_t it = nullptr;
  for (esp_err_t err = nvs_entry_find("nvs", kF1Cache, NVS_TYPE_ANY, &it); err == ESP_OK; err = nvs_entry_next(&it)) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    size_t size = info.type == NVS_TYPE_BLOB ? prefs.getBytesLength(info.key) : 4;
    other.add(String("F1 cache (settings flash): ") + info.key, size);
  }
  nvs_release_iterator(it);
  prefs.end();
}
}  // namespace

String json() {
  size_t drawingBytes = 0;
  int drawingFiles = 0;
  Totals other;
  size_t freeBytes = 0, totalBytes = 0;
  const bool available = beginDiskCache();
  if (available) {
    File root = LittleFS.open("/");
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
      if (f.isDirectory()) continue;
      if (drawing(f.name())) { drawingBytes += f.size(); drawingFiles++; }
      else other.add(f.name(), f.size());
    }
    totalBytes = LittleFS.totalBytes();
    freeBytes = totalBytes - LittleFS.usedBytes();
  }
  f1Entries(other);
  return String("{\"available\":") + (available ? "true" : "false") + ",\"free\":" + (unsigned)freeBytes + ",\"total\":" + (unsigned)totalBytes +
         ",\"drawings\":{\"files\":" + drawingFiles + ",\"bytes\":" + (unsigned)drawingBytes + "},\"other\":{\"files\":" + other.files + ",\"bytes\":" + (unsigned)other.bytes + ",\"list\":[" + other.list + "]}}";
}

int clear() {
  int removed = 0;
  if (beginDiskCache()) {
    for (int guard = 0; guard < 64; ++guard) {  // the directory is read again after each removal
      String victim;
      {
        File root = LittleFS.open("/");
        for (File f = root.openNextFile(); f && !victim.length(); f = root.openNextFile())
          if (!f.isDirectory() && !drawing(f.name())) victim = f.name();
      }
      if (!victim.length()) break;
      if (!victim.startsWith("/")) victim = "/" + victim;
      LittleFS.remove(victim);
      removed++;
    }
  }
  Totals f1;
  f1Entries(f1);
  if (f1.files) {
    Preferences prefs;
    if (prefs.begin(kF1Cache, false)) { prefs.clear(); prefs.end(); removed += f1.files; }
  }
  consoleLog("[Storage] Cleared %d items\n", removed);
  return removed;
}

bool reformat() {
  const bool ok = formatDiskCache();
  consoleLog("[Storage] Flash disk formatted: %s\n", ok ? "ok" : "failed");
  return ok;
}
}  // namespace storage
