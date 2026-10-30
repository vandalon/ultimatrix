// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "HaPrices.h"

#include <mutex>
#include <stdlib.h>
#include <string.h>

namespace ha {
namespace prices {
namespace {
std::mutex lock;
Snapshot current{};
bool haveSnapshot = false;
uint32_t changes = 0;
}  // namespace

size_t buildTemplate(const char *entity, char *out, size_t capacity) {
  // The first long list among the attributes that holds a price for the hours of today. Entries are numbers (the list called today) or
  // objects with a start time under one of a few names and a price under one of a few names. Dictionaries cannot be changed in
  // Home Assistant's templates, so the (hour, price) pairs are collected in a list and averaged per hour afterwards (15-minute prices).
  static const char kHead[] = "{% set e = '";
  static const char kBody[] =
      "' %}{% set ns = namespace(out='') %}{% if states[e] is not none %}"
      "{% set a = namespace(hp=[]) %}"
      "{% for k, v in states[e].attributes.items() %}"
      "{% if v is iterable and v is not string and v is not mapping and v|length >= 12 %}"
      "{% for it in v %}"
      "{% set q = namespace(t=none, p=none) %}"
      "{% if it is mapping %}"
      "{% for tk in ['start','from','start_time','time','start_date','datetime'] %}{% if q.t is none and it.get(tk) is not none %}{% set q.t = it.get(tk) %}{% endif %}{% endfor %}"
      "{% for pk in ['value','price','price_per_kwh','electricity_price'] %}{% if q.p is none and it.get(pk) is not none %}{% set q.p = it.get(pk) %}{% endif %}{% endfor %}"
      "{% if q.p is none and it.get('price_tax_included') is mapping %}{% set q.p = it.get('price_tax_included').get('amount') %}{% endif %}"
      "{% if q.t is not none and is_number(q.p) %}{% set t0 = as_datetime(q.t, none) %}"
      "{% if t0 is not none %}{% set t = as_local(t0) %}{% set off = (t.date() - now().date()).days %}{% if off == 0 or off == 1 %}{% set a.hp = a.hp + [[off, t.hour, q.p|float]] %}{% endif %}{% endif %}"
      "{% endif %}"
      "{% elif k in ['today', 'tomorrow'] and is_number(it) %}{% set a.hp = a.hp + [[0 if k == 'today' else 1, loop.index0 * 24 // (v|length), it|float]] %}"
      "{% endif %}"
      "{% endfor %}"
      "{% endif %}{% endfor %}"
      "{% if a.hp|length >= 12 %}{% set o = namespace(l=[], n=[]) %}"
      "{% for h in range(24) %}{% set sel = a.hp|selectattr('0','eq',0)|selectattr('1','eq',h)|map(attribute='2')|list %}{% set o.l = o.l + [((sel|sum) / (sel|length))|round(5)|string if sel else ''] %}"
      "{% set nxt = a.hp|selectattr('0','eq',1)|selectattr('1','eq',h)|map(attribute='2')|list %}{% set o.n = o.n + [((nxt|sum) / (nxt|length))|round(5)|string if nxt else ''] %}{% endfor %}"
      "{% set ns.out = now().day ~ '|' ~ o.l|join(',') ~ '|' ~ o.n|join(',') %}{% endif %}"
      "{% endif %}{{ ns.out }}";
  const size_t length = strlen(entity);
  if (sizeof(kHead) - 1 + length + sizeof(kBody) > capacity) return 0;
  memcpy(out, kHead, sizeof(kHead) - 1);
  memcpy(out + sizeof(kHead) - 1, entity, length);
  memcpy(out + sizeof(kHead) - 1 + length, kBody, sizeof(kBody));
  return sizeof(kHead) - 1 + length + sizeof(kBody) - 1;
}

void parseResult(const char *text, Snapshot &out) {
  memset(&out, 0, sizeof(out));
  const char *bar = strchr(text, '|');
  if (!bar) return;
  out.day = (uint8_t)atoi(text);
  const char *at = bar + 1;
  for (int day = 0; day < 2 && at; day++) {  // today's hours, then tomorrow's after a second bar
    float *hour = day ? out.next : out.hour;
    uint32_t &mask = day ? out.nextMask : out.mask;
    for (int h = 0; h < kHours; h++) {
      const char *end = strpbrk(at, ",|");
      const size_t length = end ? (size_t)(end - at) : strlen(at);
      if (length) { hour[h] = strtof(at, nullptr); mask |= 1u << h; }
      if (!end || *end == '|') { at = end ? end + 1 : nullptr; break; }
      at = end + 1;
    }
  }
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

void clear() {
  std::lock_guard<std::mutex> guard(lock);
  if (!haveSnapshot) return;
  memset(&current, 0, sizeof(current));
  haveSnapshot = false;
  changes++;
}

uint32_t version() { return changes; }

}  // namespace prices
}  // namespace ha
