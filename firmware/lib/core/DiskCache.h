#pragma once
#include <LittleFS.h>
#include <esp_partition.h>

// Initialise genuinely erased flash only. A failed mount of existing data
// disables the optional cache instead of formatting it.
inline bool beginDiskCache() {
  if (LittleFS.begin(false, "/littlefs", 4)) return true;
  const esp_partition_t *partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!partition) return false;
  uint8_t bytes[256];
  for (size_t offset = 0; offset < partition->size; offset += sizeof(bytes)) {
    size_t count = partition->size - offset;
    if (count > sizeof(bytes)) count = sizeof(bytes);
    if (esp_partition_read(partition, offset, bytes, count) != ESP_OK) return false;
    for (size_t i = 0; i < count; ++i) if (bytes[i] != 0xFF) return false;
  }
  return LittleFS.begin(true, "/littlefs", 4);
}
