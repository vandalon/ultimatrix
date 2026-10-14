#pragma once
#include <Preferences.h>
#include <esp_crc.h>
#include <memory>
#include <new>
#include <string.h>
#include <type_traits>

// A struct kept in NVS as one blob with a header: magic, schema version, the struct's size and a CRC of its bytes.
// A blob from another layout (any size change, or a bumped version) or a damaged one reads as "no cache" instead of
// being reinterpreted. Bump the version whenever the struct's layout or meaning changes without its size changing.
namespace cacheblob {

struct Header { uint32_t magic; uint16_t version, size; uint32_t crc; };
constexpr uint32_t kMagic = 0x55434231;  // "UCB1"

template <typename T> bool put(Preferences &prefs, const char *key, const T &value, uint16_t version) {
  static_assert(std::is_trivially_copyable<T>::value, "cache blobs are plain data");
  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[sizeof(Header) + sizeof(T)]);
  if (!buffer) return false;
  Header header{kMagic, version, (uint16_t)sizeof(T), esp_crc32_le(0, reinterpret_cast<const uint8_t *>(&value), sizeof(T))};
  memcpy(buffer.get(), &header, sizeof(header));
  memcpy(buffer.get() + sizeof(header), &value, sizeof(T));
  return prefs.putBytes(key, buffer.get(), sizeof(header) + sizeof(T)) == sizeof(header) + sizeof(T);
}

template <typename T> bool get(Preferences &prefs, const char *key, T &out, uint16_t version) {
  static_assert(std::is_trivially_copyable<T>::value, "cache blobs are plain data");
  if (prefs.getBytesLength(key) != sizeof(Header) + sizeof(T)) return false;
  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[sizeof(Header) + sizeof(T)]);
  if (!buffer || prefs.getBytes(key, buffer.get(), sizeof(Header) + sizeof(T)) != sizeof(Header) + sizeof(T)) return false;
  Header header;
  memcpy(&header, buffer.get(), sizeof(header));
  const uint8_t *payload = buffer.get() + sizeof(header);
  if (header.magic != kMagic || header.version != version || header.size != sizeof(T) || header.crc != esp_crc32_le(0, payload, sizeof(T))) return false;
  memcpy(&out, payload, sizeof(T));
  return true;
}

// Text fields come back from flash: make sure each ends in a terminator whatever the bytes were
template <size_t N> void terminate(char (&text)[N]) { text[N - 1] = 0; }

}  // namespace cacheblob
