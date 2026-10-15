// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "HaTemplate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace ha {

namespace {

// Everything below is one line with single quotes only, so it goes into the JSON message without escaping
const char kMacros[] =
    "{%- macro kw(l) -%}{%- set t = namespace(s=0,n=0) -%}{%- for e in l -%}{%- set v = states(e) -%}{%- if is_number(v) -%}"
    "{%- set t.s = t.s + v|float * (0.001 if state_attr(e,'unit_of_measurement')=='W' else 1) -%}{%- set t.n = t.n + 1 -%}{%- endif -%}{%- endfor -%}"
    "{%- if t.n -%}{{ t.s|round(3) }}{%- else -%}-{%- endif -%}{%- endmacro -%}"
    "{%- macro kwh(l) -%}{%- set t = namespace(s=0,n=0) -%}{%- for e in l -%}{%- set v = states(e) -%}{%- if is_number(v) -%}"
    "{%- set u = state_attr(e,'unit_of_measurement') -%}{%- set t.s = t.s + v|float * (0.001 if u=='Wh' else 1000 if u=='MWh' else 1) -%}{%- set t.n = t.n + 1 -%}{%- endif -%}{%- endfor -%}"
    "{%- if t.n -%}{{ t.s|round(2) }}{%- else -%}-{%- endif -%}{%- endmacro -%}"
    "{%- macro num(e) -%}{%- set v = states(e) -%}{%- if is_number(v) -%}{{ v|float|round(2) }}{%- else -%}-{%- endif -%}{%- endmacro -%}"
    "{%- macro grp(l) -%}{{ l|count }}/{{ (((now() - (l|map(attribute='last_changed')|min)).total_seconds() / 60)|int) if l else 0 }}{%- endmacro -%}";

const char kAverages[] =
    "{%- set ns = namespace(t=0,n=0,h=0,m=0) -%}"
    "{%- for s in states.sensor if s.attributes.device_class == 'temperature' and is_number(s.state) -%}"
    "{%- set ns.t = ns.t + s.state|float -%}{%- set ns.n = ns.n + 1 -%}{%- endfor -%}"
    "{%- for s in states.sensor if s.attributes.device_class == 'humidity' and is_number(s.state) -%}"
    "{%- set ns.h = ns.h + s.state|float -%}{%- set ns.m = ns.m + 1 -%}{%- endfor -%}";

struct Out {
  char *at;
  size_t left;
  bool full = false;
  void add(const char *text) {
    size_t n = strlen(text);
    if (n >= left) { full = true; return; }
    memcpy(at, text, n);
    at += n;
    left -= n;
    *at = 0;
  }
  void field(const char *expression) {  // ";{{ expression }}"
    add(";{{ ");
    add(expression);
    add(" }}");
  }
};

// The states that count as "lit" for an icon type
const char *litStates(uint8_t type) {
  switch (type) {
    case ICON_WASHER: case ICON_DRYER: case ICON_CHARGER: return "['on','running','washing','drying','charging','active']";
    case ICON_LIGHTS: return "['on']";
    case ICON_LOCK: return "['armed_home','armed_away','armed_night','armed_vacation','armed_custom_bypass','locked']";
    default: return "['on','open','opening']";
  }
}

// The HA states that make up an icon: its Auto rule, or the entity (a group is expanded to its members)
bool iconList(const Slot &slot, char *out, size_t size) {
  if (slot.type >= ICON_NONE) return false;
  if (slot.autoMode && autoAvailable(false, slot.type)) {
    const char *list = nullptr;
    switch (slot.type) {
      case ICON_DOOR: list = "states.binary_sensor|selectattr('attributes.device_class','in',['door'])|selectattr('state','eq','on')|list"; break;
      case ICON_WINDOW: list = "states.binary_sensor|selectattr('attributes.device_class','eq','window')|selectattr('state','eq','on')|list"; break;
      case ICON_GARAGE:
        list = "(states.cover|selectattr('attributes.device_class','eq','garage')|selectattr('state','in',['open','opening'])|list"
               " + states.binary_sensor|selectattr('attributes.device_class','eq','garage_door')|selectattr('state','eq','on')|list)";
        break;
      case ICON_LIGHTS: list = "states.light|selectattr('state','eq','on')|list"; break;
      case ICON_LOCK:
        list = "(states.alarm_control_panel|selectattr('state','match','armed')|list + states.lock|selectattr('state','eq','locked')|list)";
        break;
    }
    if (!list) return false;
    snprintf(out, size, "%s", list);
    return true;
  }
  if (!slot.entity[0]) return false;
  snprintf(out, size, "expand('%s')|selectattr('state','in',%s)|list", slot.entity, litStates(slot.type));
  return true;
}

bool tileExpression(const Slot &slot, char *out, size_t size) {
  const bool automatic = slot.autoMode && autoAvailable(true, slot.type);
  switch (slot.type) {
    case TILE_TEMP:
      if (automatic) { snprintf(out, size, "((ns.t / ns.n)|round(1)) if ns.n else '-'"); return true; }
      break;
    case TILE_HUMIDITY:
      if (automatic) { snprintf(out, size, "((ns.h / ns.m)|round(0)) if ns.m else '-'"); return true; }
      break;
    case TILE_PEOPLE:
      if (automatic) { snprintf(out, size, "states.person|selectattr('state','eq','home')|list|count"); return true; }
      if (slot.entity[0]) { snprintf(out, size, "expand('%s')|selectattr('state','eq','home')|list|count", slot.entity); return true; }
      return false;
    case TILE_POWER:
      if (slot.entity[0]) { snprintf(out, size, "kw('%s')", slot.entity); return true; }
      return false;
    default: break;
  }
  if (!slot.entity[0]) return false;
  snprintf(out, size, "num('%s')", slot.entity);
  return true;
}

}  // namespace

size_t buildTemplate(const Config &c, char *out, size_t capacity) {
  Out o{out, capacity};
  if (capacity) *out = 0;
  o.add(kMacros);
  bool averages = false;
  for (const auto &tile : c.tiles)
    if (tile.autoMode && ((tile.type == TILE_TEMP) || (tile.type == TILE_HUMIDITY))) averages = true;
  if (averages) o.add(kAverages);
  o.add("H");

  char expression[320];
  for (const auto &tile : c.tiles) {
    if (tileExpression(tile, expression, sizeof(expression))) o.field(expression);
    else o.add(";-");
  }
  char list[300];
  for (const auto &icon : c.icons) {
    if (iconList(icon, list, sizeof(list))) {
      snprintf(expression, sizeof(expression), "grp(%s)", list);
      o.field(expression);
    } else {
      o.add(";-");
    }
  }
  static const char *const macroOf[E_COUNT] = {"kw", "kw", "kw", "num", "kw", "kwh", "kwh", "kwh", "kwh"};
  for (int i = 0; i < E_COUNT; i++) {
    if (!c.energy[i][0]) { o.add(";-"); continue; }
    if (!strcmp(c.energy[i], "auto")) { o.add(";-"); continue; }  // not resolved (the Energy dashboard has no such sensor)
    if (macroOf[i][0] == 'n') { snprintf(expression, sizeof(expression), "num('%s')", c.energy[i]); o.field(expression); continue; }
    int at = snprintf(expression, sizeof(expression), "%s(['", macroOf[i]);
    for (const char *p = c.energy[i]; *p && at < (int)sizeof(expression) - 8; ++p) {
      if (*p == ',') at += snprintf(expression + at, sizeof(expression) - at, "','");
      else expression[at++] = *p;
    }
    snprintf(expression + at, sizeof(expression) - at, "'])");
    o.field(expression);
  }
  o.field("states.alarm_control_panel|selectattr('state','eq','triggered')|list|count");
  if (c.doorbell[0]) {
    snprintf(expression, sizeof(expression), "expand('%s')|selectattr('state','eq','on')|list|count", c.doorbell);
    o.field(expression);
  } else {
    o.add(";0");
  }
  o.field("today_at().isoformat()");
  return o.full ? 0 : capacity - o.left;
}

bool parseResult(const char *text, size_t length, Snapshot &out) {
  char line[400];
  if (length >= sizeof(line)) return false;
  memcpy(line, text, length);
  line[length] = 0;
  char *start = strstr(line, "H;");
  if (!start) return false;
  start += 2;
  memset(&out, 0, sizeof(out));
  char *fields[kFields];
  int count = 0;
  fields[count++] = start;
  for (char *p = start; *p && count < kFields; ++p)
    if (*p == ';') { *p = 0; fields[count++] = p + 1; }
  if (count != kFields) return false;

  int at = 0;
  auto number = [](const char *field, float &value) {
    if (!*field || (field[0] == '-' && !field[1]) || strcmp(field, "unknown") == 0 || strcmp(field, "unavailable") == 0) return false;
    char *end = nullptr;
    value = strtof(field, &end);
    return end != field && !*end;
  };
  for (int i = 0; i < kTiles; i++, at++)
    if (number(fields[at], out.tile[i])) out.tileOk |= 1 << i;
  for (int i = 0; i < kIcons; i++, at++) {
    const char *slash = strchr(fields[at], '/');
    if (!slash) continue;
    out.iconCount[i] = atoi(fields[at]) > 255 ? 255 : atoi(fields[at]);
    int age = atoi(slash + 1);
    out.iconAge[i] = age < 0 ? 0 : age > 65535 ? 65535 : age;
    out.iconOk |= 1 << i;
  }
  for (int i = 0; i < E_COUNT; i++, at++)
    if (number(fields[at], out.energy[i])) out.energyOk |= 1 << i;
  out.alarm = atoi(fields[at++]) > 0;
  out.doorbell = atoi(fields[at++]) > 0;
  strlcpy(out.midnight, fields[at++], sizeof(out.midnight));
  return true;
}

namespace {

// The end of the string that starts at the quote p, or null
const char *stringEnd(const char *p) {
  for (++p; *p; ++p) {
    if (*p == '\\' && p[1]) ++p;
    else if (*p == '"') return p;
  }
  return nullptr;
}

// The end (the closing brace or bracket) of the object or array that starts at p
const char *blockEnd(const char *p) {
  int depth = 0;
  for (; *p; ++p) {
    if (*p == '"') { p = stringEnd(p); if (!p) return nullptr; }
    else if (*p == '{' || *p == '[') ++depth;
    else if (*p == '}' || *p == ']') { if (--depth == 0) return p; }
  }
  return nullptr;
}

// "key":"value" inside [from, to)
bool stringValue(const char *from, const char *to, const char *key, char *out, size_t size) {
  char needle[40];
  snprintf(needle, sizeof(needle), "\"%s\":\"", key);
  const char *at = from;
  const size_t length = strlen(needle);
  for (; at + length < to; ++at)
    if (!strncmp(at, needle, length)) break;
  if (at + length >= to) return false;
  at += length;
  size_t n = 0;
  while (at < to && *at != '"' && n + 1 < size) out[n++] = *at++;
  out[n] = 0;
  return validEntity(out) && out[0];
}

bool contains(const char *from, const char *to, const char *text) {
  const size_t length = strlen(text);
  for (const char *at = from; at + length <= to; ++at)
    if (!strncmp(at, text, length)) return true;
  return false;
}

}  // namespace

bool parseEnergyPrefs(const char *json, char (&ids)[E_COUNT][kEntity], char (&gridTo)[kEntity]) {
  const char *sources = strstr(json, "\"energy_sources\"");
  if (!sources) return false;
  const char *p = strchr(sources, '[');
  if (!p) return false;
  const char *arrayEnd = blockEnd(p);
  bool any = false;
  for (++p; *p && (!arrayEnd || p < arrayEnd); ++p) {
    if (*p != '{') continue;
    const char *end = blockEnd(p);
    if (!end) break;
    int power = -1, energy = -1;
    if (contains(p, end, "\"type\":\"grid\"")) { power = E_GRID; energy = E_GRID_KWH; }
    else if (contains(p, end, "\"type\":\"solar\"")) { power = E_SOLAR; energy = E_SOLAR_KWH; }
    else if (contains(p, end, "\"type\":\"battery\"")) { power = E_BATTERY; energy = E_BATTERY_KWH; }
    if (power >= 0) {
      char id[kEntity];
      if (stringValue(p, end, "stat_rate", id, sizeof(id)) && !ids[power][0]) { strlcpy(ids[power], id, kEntity); any = true; }
      // A grid or a solar source can have several meters (two tariffs, two inverters): all of them are added up
      auto collect = [&](const char *name, char *into) {
        char key[40];
        snprintf(key, sizeof(key), "\"%s\":\"", name);
        const size_t keyLength = strlen(key);
        for (const char *from = p; from < end;) {
          const char *hit = nullptr;
          for (const char *q = from; q + keyLength <= end; ++q) if (!strncmp(q, key, keyLength)) { hit = q; break; }
          if (!hit) break;
          char one[kEntity];
          if (stringValue(hit, end, name, one, sizeof(one))) {
            const size_t used = strlen(into);
            if (used + strlen(one) + 2 < kEntity) { if (used) { into[used] = ','; into[used + 1] = 0; } strlcat(into, one, kEntity); any = true; }
          }
          from = hit + keyLength;
        }
      };
      collect("stat_energy_from", ids[energy]);
      if (power == E_GRID) collect("stat_energy_to", gridTo);
    }
    p = end;
  }
  return any;
}

uint8_t parseStatistics(const char *json, const char (*ids)[kEntity], int count, float *values) {
  uint8_t found = 0;
  for (int i = 0; i < count; i++) {
    if (!ids[i][0]) continue;
    char needle[kEntity + 8];
    snprintf(needle, sizeof(needle), "\"%s\":[", ids[i]);
    const char *at = strstr(json, needle);
    if (!at) continue;
    at += strlen(needle) - 1;
    const char *end = blockEnd(at);
    if (!end) continue;
    float sum = 0;
    bool any = false;
    for (const char *p = at; p < end; ++p) {
      if (strncmp(p, "\"change\":", 9)) continue;
      p += 9;
      if (!strncmp(p, "null", 4)) continue;
      sum += strtof(p, nullptr);
      any = true;
    }
    if (any) { values[i] = sum; found |= 1 << i; }
  }
  return found;
}

uint32_t parseHourly(const char *json, const char *id, uint32_t lastHourStart, float *out) {
  uint32_t mask = 0;
  char needle[kEntity + 8];
  snprintf(needle, sizeof(needle), "\"%s\":[", id);
  const char *at = strstr(json, needle);
  if (!at) return 0;
  at += strlen(needle) - 1;
  const char *end = blockEnd(at);
  if (!end) return 0;
  for (const char *p = at; p < end; ++p) {
    if (*p != '{') continue;
    const char *objectEnd = blockEnd(p);
    if (!objectEnd) break;
    const char *start = nullptr, *mean = nullptr;
    for (const char *q = p; q + 8 < objectEnd; ++q) {
      if (!start && !strncmp(q, "\"start\":", 8)) start = q + 8;
      else if (!mean && !strncmp(q, "\"mean\":", 7)) mean = q + 7;
    }
    if (start && mean && strncmp(mean, "null", 4)) {
      double t = strtod(start, nullptr);
      if (t > 1e11) t /= 1000;  // milliseconds
      const long back = ((long)lastHourStart - (long)t) / 3600;
      if (back >= 0 && back < kHistoryBars) { out[kHistoryBars - 1 - back] = strtof(mean, nullptr); mask |= 1u << (kHistoryBars - 1 - back); }
    }
    p = objectEnd;
  }
  return mask;
}

}  // namespace ha
