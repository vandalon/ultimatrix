// mbedTLS allocator with one reserved block for big TLS records (CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC, see sdkconfig.btproxy).
// Next to the Bluetooth proxy the free heap of the classic ESP32 is split into small pieces: a 16 KB record from Home Assistant
// finds no place although 25 KB is free, and the connection drops. The block is taken once, early, and lent to the first big
// request; everything else (and a second big request at the same time) comes from the heap as before.
#include <sdkconfig.h>
#ifdef CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <atomic>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#include "TlsPool.h"

#ifndef UM_TLS_POOL
#define UM_TLS_POOL 0
#endif

namespace {
constexpr size_t kBigRequest = 4096;       // from this size on a request may use the reserved block
uint8_t *pool = nullptr;
size_t poolSize = 0;
std::atomic<bool> poolBusy{false};
std::atomic<uint32_t> hits{0}, misses{0}, biggest{0};
}  // namespace

static void *poolCalloc(size_t n, size_t size);
static void poolFree(void *ptr);

namespace tlspool {
void reserve() {
  if (pool || !UM_TLS_POOL) return;
  pool = (uint8_t *)heap_caps_malloc(UM_TLS_POOL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  poolSize = pool ? UM_TLS_POOL : 0;
  if (pool) mbedtls_platform_set_calloc_free(poolCalloc, poolFree);
}
int state(uint32_t *h, uint32_t *m, uint32_t *big) { *h = hits; *m = misses; *big = biggest; return pool != nullptr; }
}  // namespace tlspool

static void *poolCalloc(size_t n, size_t size) {
  const size_t total = n * size;
  if (total > biggest) biggest = (uint32_t)total;
  if (total >= kBigRequest) {
    if (pool && total <= poolSize && !poolBusy.exchange(true)) {
      hits++;
      memset(pool, 0, total);
      return pool;
    }
    misses++;
  }
  return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void poolFree(void *ptr) {
  if (ptr && ptr == pool) { poolBusy.store(false); return; }
  heap_caps_free(ptr);
}
#else
#include "TlsPool.h"
namespace tlspool { void reserve() {} int state(uint32_t *h, uint32_t *m, uint32_t *big) { *h = *m = *big = 0; return 0; } }
#endif
