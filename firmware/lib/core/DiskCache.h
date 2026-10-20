#pragma once
#include <LittleFS.h>
#include <esp_partition.h>

// Set after a failed mount of existing data: not tried again until the disk is reformatted (Advanced > Storage),
// so a damaged disk is not scanned on every request.
inline bool &diskFailed() { static bool failed = false; return failed; }

// Initialise genuinely erased flash only. A failed mount of existing data
// disables the optional cache instead of formatting it.
inline bool beginDiskCache() {
  if (diskFailed()) return false;
  if (LittleFS.begin(false, "/littlefs", 4)) return true;
  const esp_partition_t *partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!partition) return false;
  uint8_t bytes[256];
  for (size_t offset = 0; offset < partition->size; offset += sizeof(bytes)) {
    size_t count = partition->size - offset;
    if (count > sizeof(bytes)) count = sizeof(bytes);
    if (esp_partition_read(partition, offset, bytes, count) != ESP_OK) { diskFailed() = true; return false; }
    for (size_t i = 0; i < count; ++i) if (bytes[i] != 0xFF) { diskFailed() = true; return false; }
  }
  if (LittleFS.begin(true, "/littlefs", 4)) return true;
  diskFailed() = true;
  return false;
}

// Erases the whole disk (saved drawings included) and starts it empty. The caller restarts the clock afterwards.
inline bool formatDiskCache() {
  LittleFS.end();
  const bool ok = LittleFS.format();
  if (ok) diskFailed() = false;
  return ok;
}
