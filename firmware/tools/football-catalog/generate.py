#!/usr/bin/env python3
# Adds competitions (and their clubs, from ESPN) to lib/core/FootballCatalog.h and regroups the picker by continent.
# Reads the current header, so it can be run again: leagues already present are skipped.
#   python3 tools/football-catalog/generate.py
import json, re, sys, urllib.request
from pathlib import Path

HEADER = Path(__file__).resolve().parents[2] / "lib/core/FootballCatalog.h"
API = "https://site.api.espn.com/apis/site/v2/sports/soccer/%s/teams?limit=100"
CHUNK = 8000  # string literals stay below compiler limits

# code, label, colour (RGB565), name, group, clubs (fetch the club list for favourites)
NEW = [
    ("bra.1", "BSA", 0x07E4, "Brasileirão Série A", "South America", True),
    ("arg.1", "ARG", 0x5E7F, "Liga Profesional", "South America", True),
    ("conmebol.libertadores", "LIB", 0xFD20, "Copa Libertadores", "South America", False),
    ("conmebol.sudamericana", "SUD", 0x07FA, "Copa Sudamericana", "South America", False),
    ("jpn.1", "J1", 0xF8A3, "J1 League", "Asia", True),
    ("ksa.1", "KSA", 0x4FE9, "Saudi Pro League", "Asia", True),
    ("afc.champions", "ACL", 0x9CFF, "AFC Champions League Elite", "Asia", False),
    ("usa.1", "MLS", 0x3DFF, "MLS", "North America", True),
    ("mex.1", "MX", 0x0EE6, "Liga MX", "North America", True),
    ("concacaf.champions", "CCC", 0xFFE0, "CONCACAF Champions Cup", "North America", False),
]
CONTINENT_ORDER = ["Europe", "South America", "Asia", "North America", "National teams"]
# Every country group already in the catalog is European
EUROPE = {"England", "Spain", "Germany", "Italy", "France", "Netherlands", "Portugal", "Belgium", "Scotland", "Turkey", "Greece",
          "Austria", "Switzerland", "Denmark", "Sweden", "Norway", "Russia", "Romania", "Cyprus", "Israel"}

text = HEADER.read_text(encoding="utf-8")
leagues = [(c, l, int(col, 16)) for c, l, col in re.findall(r'\{"([^"]+)", "([^"]+)", (0x[0-9A-Fa-f]+)\}', text)]
names = re.search(r"FOOTBALL_LEAGUE_NAMES\[\] = \{(.*?)\};", text, re.S).group(1)
names = re.findall(r'"([^"]*)"', names)
picker = json.loads(re.search(r'FOOTBALL_LEAGUES_JSON\[\] PROGMEM = R"JSON\((.*?)\)JSON";', text, re.S).group(1))
teams = json.loads("".join(re.findall(r'R"JSON\((.*?)\)JSON"', text.split("FOOTBALL_TEAMS_JSON")[1], re.S)))
assert len(leagues) == len(names) == len(picker), (len(leagues), len(names), len(picker))

known = {c for c, _, _ in leagues}
labels = {l for _, l, _ in leagues}
ids = {t[0] for t in teams}
for code, label, color, name, group, clubs in NEW:
    if code in known:
        continue
    assert label not in labels, label
    leagues.append((code, label, color)); names.append(name); picker.append([code, label, name, group])
    if clubs:
        with urllib.request.urlopen(API % code) as response:
            found = json.load(response)["sports"][0]["leagues"][0]["teams"]
        for item in found:
            team = item["team"]
            if int(team["id"]) not in ids:
                ids.add(int(team["id"])); teams.append([int(team["id"]), team["abbreviation"], team["displayName"], "c"])
        print(code, len(found), "clubs")

for entry in picker:
    if entry[3] in EUROPE:
        entry[3] = "Europe"
picker.sort(key=lambda e: CONTINENT_ORDER.index(e[3]))  # stable: league order within a continent
order = {e[0]: i for i, e in enumerate(picker)}
paired = sorted(zip(leagues, names), key=lambda p: order[p[0][0]])
leagues, names = [p[0] for p in paired], [p[1] for p in paired]
teams = sorted((t for t in teams if t[3] == "c"), key=lambda t: t[2].casefold()) + sorted((t for t in teams if t[3] == "n"), key=lambda t: t[2].casefold())

dump = lambda value: json.dumps(value, ensure_ascii=False, separators=(",", ":"))
blob = dump(teams)
chunks = [blob[i:i + CHUNK] for i in range(0, len(blob), CHUNK)]
head = text.split("static const FootballLeague FOOTBALL_LEAGUES[]")[0]
out = head + "static const FootballLeague FOOTBALL_LEAGUES[] = {\n"
out += "".join('  {"%s", "%s", 0x%04X},\n' % row for row in leagues) + "};\n\n"
out += "#ifdef UM_FOOTBALL_ONLY\nstatic const char *const FOOTBALL_LEAGUE_NAMES[] = {\n" + "".join('  "%s",\n' % n for n in names) + "};\n#endif\n\n"
out += "// For the settings page: [code, label, name, group]\nstatic const char FOOTBALL_LEAGUES_JSON[] PROGMEM = R\"JSON(" + dump(picker) + ")JSON\";\n\n"
out += "// Favourites to pick from: [ESPN team id, abbreviation, name, 'c'lub or 'n'ational team]\nstatic const char *const FOOTBALL_TEAMS_JSON[] PROGMEM = {\n"
out += "".join('  R"JSON(%s)JSON",\n' % c for c in chunks) + "};\n"
HEADER.write_text(out, encoding="utf-8")
print(len(leagues), "leagues,", len(teams), "teams,", len(chunks), "chunks")
