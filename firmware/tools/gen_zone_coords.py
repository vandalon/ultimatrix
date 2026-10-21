#!/usr/bin/env python3
# Writes tools/zone_coords.csv: the main city (latitude, longitude in degrees) of every zone in tools/zones.csv,
# from the IANA zone.tab, so a clock with a manually chosen timezone can work out sunrise and sunset for that zone.
#   python3 tools/gen_zone_coords.py [zone.tab]     (default /usr/share/zoneinfo/zone.tab)
import csv, os, re, sys

# Same renamed zones as gen_tzdb.py: alias -> name in zone.tab
ALIASES = {"Europe/Kyiv": "Europe/Kiev", "Asia/Calcutta": "Asia/Kolkata", "Asia/Rangoon": "Asia/Yangon",
           "Atlantic/Faeroe": "Atlantic/Faroe", "Pacific/Kanton": "Pacific/Enderbury", "America/Ciudad_Juarez": "America/Ojinaga"}
here = os.path.dirname(os.path.abspath(__file__))
tab = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/zoneinfo/zone.tab"

with open(os.path.join(here, "zones.csv"), encoding="utf-8") as f:
    zones = [name for name, _ in csv.reader(f)]

def degrees(sign, deg, minutes, seconds):
    value = int(deg) + int(minutes) / 60 + int(seconds or 0) / 3600
    return round(-value if sign == "-" else value, 2)

coords = {}
with open(tab, encoding="utf-8") as f:
    for line in f:
        if line.startswith("#") or not line.strip():
            continue
        _, place, name = line.rstrip("\n").split("\t")[:3]
        m = re.match(r"([+-])(\d{2})(\d{2})(\d{2})?([+-])(\d{3})(\d{2})(\d{2})?$", place)
        coords[name] = (degrees(m[1], m[2], m[3], m[4]), degrees(m[5], m[6], m[7], m[8]))

rows = [(z,) + (coords.get(z) or coords.get(ALIASES.get(z, "")))
        for z in sorted(zones) if coords.get(z) or coords.get(ALIASES.get(z, ""))]
with open(os.path.join(here, "zone_coords.csv"), "w", encoding="utf-8", newline="") as f:
    csv.writer(f, lineterminator="\n").writerows(rows)
print("%d of %d zones have coordinates -> zone_coords.csv" % (len(rows), len(zones)))
