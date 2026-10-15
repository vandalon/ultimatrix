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
constexpr uint32_t kMagic = 0x55434231;      // "UCB1": the payload is the struct's bytes as they are
constexpr uint32_t kMagicPacked = 0x55434232;  // "UCB2": zero runs packed as 0x00, count (1..255); saves the padding of text fields

// Returns the packed length, or 0 when packing would not make it shorter than `size`
inline size_t pack(const uint8_t *in, size_t size, uint8_t *out) {
  size_t n = 0;
  for (size_t i = 0; i < size;) {
    if (in[i]) { if (n >= size) return 0; out[n++] = in[i++]; continue; }
    size_t run = 0;
    while (i < size && !in[i] && run < 255) { run++; i++; }
    if (n + 2 > size) return 0;
    out[n++] = 0; out[n++] = (uint8_t)run;
  }
  return n < size ? n : 0;
}

// Unpacks exactly `size` bytes; false when the data does not fill them exactly
inline bool unpack(const uint8_t *in, size_t length, uint8_t *out, size_t size) {
  size_t n = 0;
  for (size_t i = 0; i < length;) {
    if (in[i]) { if (n >= size) return false; out[n++] = in[i++]; continue; }
    if (i + 1 >= length || !in[i + 1] || n + in[i + 1] > size) return false;
    memset(out + n, 0, in[i + 1]); n += in[i + 1]; i += 2;
  }
  return n == size;
}

template <typename T> bool put(Preferences &prefs, const char *key, const T &value, uint16_t version) {
  static_assert(std::is_trivially_copyable<T>::value, "cache blobs are plain data");
  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[sizeof(Header) + sizeof(T)]);
  if (!buffer) return false;
  const uint8_t *raw = reinterpret_cast<const uint8_t *>(&value);
  Header header{kMagic, version, (uint16_t)sizeof(T), esp_crc32_le(0, raw, sizeof(T))};
  const size_t packed = pack(raw, sizeof(T), buffer.get() + sizeof(header));
  if (packed) header.magic = kMagicPacked;
  else memcpy(buffer.get() + sizeof(header), raw, sizeof(T));
  memcpy(buffer.get(), &header, sizeof(header));
  const size_t total = sizeof(header) + (packed ? packed : sizeof(T));
  return prefs.putBytes(key, buffer.get(), total) == total;
}

template <typename T> bool get(Preferences &prefs, const char *key, T &out, uint16_t version) {
  static_assert(std::is_trivially_copyable<T>::value, "cache blobs are plain data");
  const size_t length = prefs.getBytesLength(key);
  if (length <= sizeof(Header) || length > sizeof(Header) + sizeof(T)) return false;
  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[length]);
  std::unique_ptr<uint8_t[]> payload(new (std::nothrow) uint8_t[sizeof(T)]);
  if (!buffer || !payload || prefs.getBytes(key, buffer.get(), length) != length) return false;
  Header header;
  memcpy(&header, buffer.get(), sizeof(header));
  if (header.version != version || header.size != sizeof(T)) return false;
  const uint8_t *body = buffer.get() + sizeof(header);
  if (header.magic == kMagic) { if (length != sizeof(Header) + sizeof(T)) return false; memcpy(payload.get(), body, sizeof(T)); }
  else if (header.magic != kMagicPacked || !unpack(body, length - sizeof(header), payload.get(), sizeof(T))) return false;
  if (header.crc != esp_crc32_le(0, payload.get(), sizeof(T))) return false;
  memcpy(&out, payload.get(), sizeof(T));
  return true;
}

// Text fields come back from flash: make sure each ends in a terminator whatever the bytes were
template <size_t N> void terminate(char (&text)[N]) { text[N - 1] = 0; }

}  // namespace cacheblob
