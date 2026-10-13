#!/usr/bin/env python3
"""Records a MotoGP weekend from the results API into showcase-data.json (key "motogp") for the MotoGP preview."""
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
API = "https://api.pulselive.motogp.com/motogp/v1"
MAKER_COLORS = {"Ducati": "E81828", "Aprilia": "9660F0", "KTM": "FF7A14", "Yamaha": "2878FF", "Honda": "FFFFFF"}
# The same codes as lib/motogp-feed/MotoGpRiders.h
CODES = dict(re.findall(r'\{(\d+), "(\w+)", "[^"]*", Maker::\w+\}', (HERE.parent.parent / "lib/motogp-feed/MotoGpRiders.h").read_text()))
NAMES = {n: m for n, m in re.findall(r'\{(\d+), "\w+", "([^"]*)", Maker::\w+\}', (HERE.parent.parent / "lib/motogp-feed/MotoGpRiders.h").read_text())}


def get(path):
    return json.loads(subprocess.run(["curl", "-s", "-m", "30", "-A", "Mozilla/5.0", API + path], capture_output=True, check=True).stdout)


def stamp(text):
    return int(datetime.fromisoformat(text).astimezone(timezone.utc).timestamp())


def lap(text):
    text = text.lstrip("0") if text[:1] == "0" and text[1:2].isdigit() else text
    return text.replace("'", ":")


def gap(value):
    g = float(value)
    return f"+.{int(g * 1000 + 0.5):03d}" if g < 0.9995 else f"+{g:.2f}" if g < 9.995 else f"+{g:.1f}" if g < 99.95 else f"+{int(g + 0.5)}"


def rows(session, kind):
    out = []
    for r in get(f"/results/session/{session['id']}/classification?test=false")["classification"]:
        if r["position"] is None:
            continue
        number = str(r["rider"]["number"])
        first = r["position"] == 1
        if kind in "PQ":
            time = lap(r["best_lap"]["time"]) if first else gap(r["gap"]["first"])
        else:
            time = "LEAD" if first else gap(r["gap"]["first"])
        out.append({"code": CODES.get(number, r["rider"]["full_name"].split()[-1][:3].upper()), "time": time, "fastest": False,
                    "color": MAKER_COLORS.get(r["constructor"]["name"], "999999")})
    return out


season = next(s for s in get("/results/seasons") if s["current"])
category = next(c for c in get(f"/results/categories?seasonUuid={season['id']}") if c["name"].startswith("MotoGP"))
events = [e for e in get(f"/results/events?seasonUuid={season['id']}") if not e["test"]]
finished = [e for e in events if e["status"] == "FINISHED"]
event = finished[-1] if len(sys.argv) < 2 else next(e for e in events if e["short_name"] == sys.argv[1])
upcoming = next(e for e in events if e["status"] != "FINISHED")
sessions = sorted(get(f"/results/sessions?eventUuid={event['id']}&categoryUuid={category['id']}"), key=lambda s: s["date"])
pick = lambda t, n=None: next(s for s in sessions if s["type"] == t and (n is None or s["number"] == n))
practice, q2, q1, race = pick("PR"), pick("Q", 2), pick("Q", 1), pick("RAC")
qualifying = rows(q2, "Q")
for r in rows(q1, "Q"):
    if all(r["code"] != x["code"] for x in qualifying):
        qualifying.append(r)
standings = []
for r in get(f"/results/standings?seasonUuid={season['id']}&categoryUuid={category['id']}")["classification"][:22]:
    number = str(r["rider"]["number"])
    standings.append({"code": CODES.get(number, r["rider"]["full_name"].split()[-1][:3].upper()), "points": r["points"],
                      "color": MAKER_COLORS.get(r["constructor"]["name"], "999999"),
                      "name": NAMES.get(number, r["rider"]["full_name"].split()[-1].upper())})
data = json.loads((HERE / "showcase-data.json").read_text())
data["motogp"] = {
    "circuit": f"{event['country']['name'].upper()[:14]} {event['circuit']['name'][:21]} {event['circuit']['place'].upper()[:10]}",
    "standings": standings, "practice": rows(practice, "P"), "qualifying": qualifying, "race": rows(race, "R"),
    "practiceStart": stamp(practice["date"]), "qualifyingStart": stamp(q2["date"]), "raceStart": stamp(race["date"]),
    "raceLaps": 27, "raceEnd": stamp(race["date"]) + 55 * 60, "qName": "Q2",
    "nextSession": {"name": "SPRINT", "start": stamp(practice["date"]) + 3600 * 24},
    "nextRace": {"city": upcoming["circuit"]["place"].upper(), "circuit": f"{upcoming['country']['name'].upper()[:14]} {upcoming['circuit']['name'][:21]} {upcoming['circuit']['place'].upper()[:10]}",
                 "start": int(datetime.fromisoformat(upcoming["date_start"]).replace(tzinfo=timezone.utc).timestamp()) + 9 * 3600},
}
(HERE / "showcase-data.json").write_text(json.dumps(data, indent=1, ensure_ascii=False) + "\n")
print(f"MotoGP preview data from {event['name']}: {len(standings)} riders, {len(qualifying)} qualifiers")
