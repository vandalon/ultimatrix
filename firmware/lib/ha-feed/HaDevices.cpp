// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "HaDevices.h"

#include <Arduino.h>
#include <atomic>
#include <mutex>
#include <string.h>

namespace ha {
namespace devices {

namespace {
std::mutex lock;
Snapshot current{};
bool haveSnapshot = false;
std::atomic<uint32_t> changes{0};

// "a;b;c" into names[], counting every one; returns the total
uint8_t split(const char *at, const char *end, char *pool, size_t poolSize, uint8_t &shown) {
  uint8_t total = 0;
  size_t used = 0;
  bool full = false;
  shown = 0;
  while (at < end) {
    const char *stop = (const char *)memchr(at, ';', end - at);
    if (!stop) stop = end;
    if (stop > at) {
      const size_t n = std::min<size_t>(stop - at, kName - 1);
      if (!full && used + (used ? 1 : 0) + n + 1 <= poolSize) {
        if (used) pool[used++] = ';';
        memcpy(pool + used, at, n);
        used += n;
        pool[used] = 0;
        shown++;
      } else full = true;
      if (total < 255) total++;
    }
    at = stop + 1;
  }
  return total;
}
}  // namespace

bool nameAt(const char *pool, int k, char *out, size_t size) {
  for (; k > 0 && *pool; k--) {
    const char *stop = strchr(pool, ';');
    if (!stop) return false;
    pool = stop + 1;
  }
  if (!*pool || !size) return false;
  const char *stop = strchr(pool, ';');
  const size_t n = std::min<size_t>(stop ? stop - pool : strlen(pool), size - 1);
  memcpy(out, pool, n);
  out[n] = 0;
  return true;
}

size_t buildTemplate(char *out, size_t capacity) {
  // A device counts once however many entities it has; its name is the one the user gave it, else Home Assistant's
  static const char kTemplate[] =
      "{% set ns = namespace(l=[], o=[]) %}"
      "{% for s in states.sensor|selectattr('attributes.device_class','eq','battery')|list + states.binary_sensor|selectattr('attributes.device_class','eq','battery')|list %}"
      "{% if (s.domain == 'binary_sensor' and s.state == 'on') or (is_number(s.state) and s.attributes.unit_of_measurement == '%' and s.state|float <= 20) %}"
      "{% set d = (device_attr(s.entity_id,'name_by_user') or device_attr(s.entity_id,'name') or s.name)|string|replace(';',' ')|replace('~',' ')|truncate(24,true,'') %}"
      "{% if d not in ns.l %}{% set ns.l = ns.l + [d] %}{% endif %}{% endif %}{% endfor %}"
      "{% for s in states|selectattr('domain','in',['sensor','binary_sensor','light','switch','cover','climate','media_player','fan','lock'])|selectattr('state','eq','unavailable') %}"
      "{% set d = (device_attr(s.entity_id,'name_by_user') or device_attr(s.entity_id,'name') or s.name)|string|replace(';',' ')|replace('~',' ')|truncate(24,true,'') %}"
      "{% if d not in ns.o %}{% set ns.o = ns.o + [d] %}{% endif %}{% endfor %}"
      "{{ ns.l|join(';') ~ '~' ~ ns.o|join(';') }}";
  if (sizeof(kTemplate) > capacity) return 0;
  memcpy(out, kTemplate, sizeof(kTemplate));
  return sizeof(kTemplate) - 1;
}

void parseResult(const char *text, Snapshot &out) {
  memset(&out, 0, sizeof(out));
  const char *split_ = strchr(text, '~');
  if (!split_) return;
  out.lowTotal = split(text, split_, out.low, sizeof(out.low), out.lowShown);
  out.offlineTotal = split(split_ + 1, text + strlen(text), out.offline, sizeof(out.offline), out.offlineShown);
}

void publish(const Snapshot &snapshot) {
  std::lock_guard<std::mutex> guard(lock);
  current = snapshot;
  haveSnapshot = true;
  changes++;
}

bool snapshot(Snapshot &out) {
  std::lock_guard<std::mutex> guard(lock);
  out = current;
  return haveSnapshot;
}

uint32_t version() { return changes; }

}  // namespace devices
}  // namespace ha
