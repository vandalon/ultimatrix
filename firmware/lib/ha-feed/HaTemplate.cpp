// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "HaTemplate.h"

#include <math.h>
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
    "{%- macro avg(l) -%}{%- set t = namespace(s=0,n=0) -%}{%- for e in l -%}{%- set v = states(e) -%}{%- if is_number(v) -%}"
    "{%- set t.s = t.s + v|float -%}{%- set t.n = t.n + 1 -%}{%- endif -%}{%- endfor -%}{%- if t.n -%}{{ (t.s / t.n)|round(2) }}{%- else -%}-{%- endif -%}{%- endmacro -%}"
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
    case ICON_MEDIA: return "['playing']";
    case ICON_OFFLINE: return "['unavailable']";
    case ICON_BLINDS: return "['open','opening','closing']";
    case ICON_LEAK: case ICON_LIGHTS: return "['on']";
    case ICON_LOCK: return "['armed_home','armed_away','armed_night','armed_vacation','armed_custom_bypass','locked']";
    default: return "['on','open','opening']";
  }
}

// "a.b,c.d" -> "a.b','c.d": the inside of a quoted, comma separated list in the template (the IDs are validated)
void quoted(const char *list, char *out, size_t size) {
  size_t n = 0;
  for (const char *c = list; *c && n + 4 < size; ++c) {
    if (*c == ',') { out[n++] = '\''; out[n++] = ','; out[n++] = '\''; }
    else out[n++] = *c;
  }
  out[n] = 0;
}

// The HA states that make up an icon: its Auto rule, or the entity (a group is expanded to its members)
bool iconList(const Slot &slot, char *out, size_t size) {
  if (slot.type == ICON_NONE || slot.type >= ICON_TYPES) return false;
  if (slot.autoMode && autoAvailable(false, slot.type)) {
    const char *list = nullptr;
    switch (slot.type) {
      case ICON_DOOR: list = "states.binary_sensor|selectattr('attributes.device_class','in',['door'])|selectattr('state','eq','on')|list"; break;
      case ICON_WINDOW: list = "states.binary_sensor|selectattr('attributes.device_class','eq','window')|selectattr('state','eq','on')|list"; break;
      case ICON_GARAGE:
        list = "(states.cover|selectattr('attributes.device_class','eq','garage')|selectattr('state','in',['open','opening'])|list"
               " + states.binary_sensor|selectattr('attributes.device_class','eq','garage_door')|selectattr('state','eq','on')|list)";
        break;
      case ICON_LOW_BATTERY:
        list = "(states.sensor|selectattr('attributes.device_class','eq','battery')|list + states.binary_sensor|selectattr('attributes.device_class','eq','battery')|list)"; break;
      case ICON_OFFLINE:
        list = "states|selectattr('domain','in',['sensor','binary_sensor','light','switch','cover','climate','media_player','fan','lock'])|selectattr('state','eq','unavailable')|list"; break;
      case ICON_LEAK: list = "states.binary_sensor|selectattr('attributes.device_class','eq','moisture')|selectattr('state','eq','on')|list"; break;
      case ICON_BLINDS:
        list = "states.cover|selectattr('attributes.device_class','in',['blind','curtain','shade','shutter'])|selectattr('state','in',['open','opening','closing'])|list"; break;
      case ICON_MEDIA: list = "states.media_player|selectattr('state','eq','playing')|list"; break;
      case ICON_HEATING: list = "states.climate|selectattr('attributes.hvac_action','eq','heating')|rejectattr('state','in',['unknown','unavailable'])|list"; break;
      case ICON_PRESENCE: list = "states.binary_sensor|selectattr('attributes.device_class','in',['occupancy','motion','presence'])|selectattr('state','eq','on')|list"; break;
      case ICON_CHARGER:  // charging binary sensors, only those named like a car or charger (phones and vacuums use the same class)
        list = "states.binary_sensor|selectattr('attributes.device_class','eq','battery_charging')|selectattr('state','eq','on')"
               "|selectattr('entity_id','search','car|ev_|_ev|tesla|vehicle|charger|wallbox')|list";
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
  char ids[kEntity * 3];
  quoted(slot.entity, ids, sizeof(ids));
  if (slot.type == ICON_LOW_BATTERY) snprintf(out, size, "expand('%s')|list", ids);
  else if (slot.type == ICON_HEATING) snprintf(out, size, "expand('%s')|selectattr('attributes.hvac_action','eq','heating')|rejectattr('state','in',['unknown','unavailable'])|list", ids);
  else snprintf(out, size, "expand('%s')|selectattr('state','in',%s)|list", ids, litStates(slot.type));
  return true;
}

char autoPrice[kEntity] = "";

bool tileExpression(const Slot &slot, char *out, size_t size) {
  const bool automatic = slot.autoMode && autoAvailable(true, slot.type);
  char ids[kEntity * 3];
  quoted(slot.entity, ids, sizeof(ids));
  switch (slot.type) {
    case TILE_TEMP:
      if (automatic) { snprintf(out, size, "((ns.t / ns.n)|round(1)) if ns.n else '-'"); return true; }
      if (strchr(slot.entity, ',')) { snprintf(out, size, "avg(['%s'])", ids); return true; }
      break;
    case TILE_HUMIDITY:
      if (automatic) { snprintf(out, size, "((ns.h / ns.m)|round(0)) if ns.m else '-'"); return true; }
      if (strchr(slot.entity, ',')) { snprintf(out, size, "avg(['%s'])", ids); return true; }
      break;
    case TILE_PEOPLE:
      if (automatic) { snprintf(out, size, "states.person|selectattr('state','eq','home')|list|count"); return true; }
      if (slot.entity[0]) { snprintf(out, size, "expand('%s')|selectattr('state','eq','home')|list|count", ids); return true; }
      return false;
    case TILE_POWER:
      if (slot.entity[0]) { snprintf(out, size, "kw(['%s'])", ids); return true; }
      return false;
    case TILE_EV: {  // "battery,charging,plugged" (a part left empty or "-" is found by itself): charge + 1000 * (1 plugged + 2 charging), charge 999 when unknown
      if (!slot.entity[0] && !automatic) return false;
      static const char kCar[] = "car|ev_|_ev|tesla|vehicle|wallbox";
      char part[3][kEntity] = {"", "", ""};
      const char *at = slot.entity;
      for (int k = 0; k < 3 && nextListed(at, part[k], sizeof(part[k])); k++) if (!strcmp(part[k], "-")) part[k][0] = 0;
      size_t n = 0;
      auto add = [&](const char *format, const char *arg) { if (n < size) n += snprintf(out + n, size - n, format, arg); };
      add("%s", "((");
      const bool none[3] = {!strcmp(part[0], "off"), !strcmp(part[1], "off"), !strcmp(part[2], "off")};  // "off": not used
      if (none[0]) add("%s", "'-'");
      else if (part[0][0]) add("num('%s')", part[0]);
      else add("num((states.sensor|selectattr('attributes.device_class','eq','battery')|selectattr('entity_id','search','%s')|map(attribute='entity_id')|list|first) or '')", kCar);
      add("%s", "|float(999)) + 1000 * ((2 if (");
      if (none[1]) add("%s", "false");
      else if (part[1][0]) add("expand('%s')|selectattr('state','in',['on','charging','running','active'])|list|count > 0", part[1]);
      else add("states.binary_sensor|selectattr('attributes.device_class','eq','battery_charging')|selectattr('state','eq','on')|selectattr('entity_id','search','%s')|list|count > 0", kCar);
      add("%s", ") else 0) + (1 if (");
      if (none[2]) add("%s", "false");
      else if (part[2][0]) add("expand('%s')|selectattr('state','in',['on','connected','plugged_in','plugged','charging'])|list|count > 0", part[2]);
      else add("%s", "states.binary_sensor|selectattr('attributes.device_class','eq','plug')|selectattr('state','eq','on')|list|count > 0");
      add("%s", ") else 0)))");
      if (n >= size) return false;
      return true;
    }
    case TILE_PRICE:
      if (automatic) {
        if (!autoPrice[0]) return false;
        if (autoPrice[0] >= '0' && autoPrice[0] <= '9') snprintf(out, size, "%s", autoPrice);
        else snprintf(out, size, "num('%s')", autoPrice);
        return true;
      }
      break;
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
  for (int i = 0; i < kHomeTiles; i++)
    if (tileSlot(c, i).autoMode && ((tileSlot(c, i).type == TILE_TEMP) || (tileSlot(c, i).type == TILE_HUMIDITY))) averages = true;
  if (averages) o.add(kAverages);
  o.add("H");

  bool presence = false;
  for (const auto &slot : c.icons) presence |= slot.type == ICON_PRESENCE || (slot.type == ICON_LIGHTS && !slot.autoMode);
  if (presence) o.add("{%- macro rooms(l) -%}{%- set n = namespace(ids=[]) -%}{%- for s in l -%}"
                      "{%- set a = area_id(s.entity_id) -%}{%- if a and a not in n.ids -%}"
                      "{%- set n.ids = n.ids + [a] -%}{%- endif -%}{%- endfor -%}{{ n.ids|count }}/0{%- endmacro -%}");
  bool devices = false, lowBattery = false;
  for (const auto &slot : c.icons) {
    devices |= slot.type == ICON_OFFLINE || slot.type == ICON_LOW_BATTERY;
    lowBattery |= slot.type == ICON_LOW_BATTERY;
  }
  if (devices) o.add("{%- macro devices(l) -%}{%- set n = namespace(ids=[]) -%}{%- for s in l -%}"
                     "{%- set id = device_id(s.entity_id) or s.entity_id -%}{%- if id not in n.ids -%}"
                     "{%- set n.ids = n.ids + [id] -%}{%- endif -%}{%- endfor -%}{{ n.ids|count }}/0{%- endmacro -%}");
  if (lowBattery) o.add("{%- macro low(l) -%}{%- set n = namespace(items=[]) -%}{%- for s in l -%}"
                        "{%- if (s.domain == 'binary_sensor' and s.state == 'on') or "
                        "(is_number(s.state) and s.attributes.unit_of_measurement == '%' and s.state|float <= 20) -%}"
                        "{%- set n.items = n.items + [s] -%}{%- endif -%}{%- endfor -%}{{ devices(n.items) }}{%- endmacro -%}");
  char expression[800];
  for (int i = 0; i < kHomeTiles; i++) {
    const Slot &tile = tileSlot(c, i);
    if (tile.type == TILE_WEATHER || tile.type == TILE_OUTDOOR_HUMIDITY) {
      if (tile.autoMode || !tile.entity[0])
        snprintf(expression, sizeof(expression), "{%%- set w%d = states.weather|rejectattr('state','in',['unknown','unavailable'])|map(attribute='entity_id')|first|default('') -%%}", i);
      else snprintf(expression, sizeof(expression), "{%%- set w%d = '%s' -%%}", i, tile.entity);
      o.add(expression);
      if (tile.type == TILE_OUTDOOR_HUMIDITY && !tile.autoMode && strncmp(tile.entity, "weather.", 8)) {
        snprintf(expression, sizeof(expression), "num('%s')", tile.entity);
      } else {
        const char *attr = tile.type == TILE_WEATHER ? "temperature" : "humidity";
        snprintf(expression, sizeof(expression), "state_attr(w%d,'%s') if w%d and is_number(state_attr(w%d,'%s')) else '-'", i, attr, i, i, attr);
      }
      o.field(expression);
    } else if (tile.type != TILE_NONE && tileExpression(tile, expression, sizeof(expression))) o.field(expression);
    else o.add(";-");
  }
  char list[600];
  for (const auto &icon : c.icons) {
    if (iconList(icon, list, sizeof(list))) {
      snprintf(expression, sizeof(expression), "%s(%s)", (icon.type == ICON_PRESENCE || (icon.type == ICON_LIGHTS && !icon.autoMode)) ? "rooms" : icon.type == ICON_OFFLINE ? "devices" : icon.type == ICON_LOW_BATTERY ? "low" : "grp", list);
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
  if (washerAlert().entity[0]) {
    snprintf(expression, sizeof(expression), "{%%- set we = '%s' -%%}", washerAlert().entity);
    o.add(expression);
    snprintf(expression, sizeof(expression), "grp(expand(we)|selectattr('state','in',%s)|list) if states(we) not in ['unknown','unavailable'] else '-'", litStates(ICON_WASHER));
    o.field(expression);
  } else o.add(";-");
  for (int i = 0; i < kHomeTiles; i++) {
    if (tileSlot(c, i).type == TILE_WEATHER) {
      snprintf(expression, sizeof(expression), "states(w%d) if w%d else '-'", i, i);
      o.field(expression);
    } else o.add(";-");
  }
  return o.full ? 0 : capacity - o.left;
}

bool parseResult(const char *text, size_t length, Snapshot &out) {
  char line[512];
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
    return end != field && !*end && isfinite(value);
  };
  for (int i = 0; i < kHomeTiles; i++, at++)
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
  if (strchr(fields[at], '/')) {
    const int count = atoi(fields[at]);
    out.washerCount = count < 0 ? 0 : count > 255 ? 255 : count;
    out.washerOk = 1;
  }
  at++;
  for (int i = 0; i < kHomeTiles; i++) strlcpy(out.weather[i], fields[at++], sizeof(out.weather[i]));
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

void setAutoPrice(const char *price) { strlcpy(autoPrice, price ? price : "", sizeof(autoPrice)); }

bool parseEnergyPrefs(const char *json, char (&ids)[E_COUNT][kEntity], char (&gridTo)[kEntity], char *price) {
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
      if (power == E_GRID && price && !price[0]) {
        if (stringValue(p, end, "entity_energy_price", price, kEntity)) any = true;
        else {
          const char *n = nullptr;
          for (const char *q = p; q + 22 < end; ++q) if (!strncmp(q, "\"number_energy_price\":", 22)) { n = q + 22; break; }
          size_t len = 0;
          while (n && n + len < end && len + 1 < kEntity && ((n[len] >= '0' && n[len] <= '9') || n[len] == '.')) len++;
          if (len && n[0] != '.') { memcpy(price, n, len); price[len] = 0; any = true; }
        }
      }
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
