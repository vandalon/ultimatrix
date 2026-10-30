#!/usr/bin/env python3
"""Finds the minimum panel brightness that keeps colours right on your panel (needs a build with /api/dim-colour; no admin PIN set).

The LEDs of different panels give different light per plane weight, so below a certain brightness a dim amber can turn orange or a pale white turn green. The
clock ships with the plain brightness table (no correction); the cleanest fix is a calibrated floor: the lowest brightness that stays above that point (the same
calibration is on the settings page, Display > Calibrate colours). This script puts the
test grid (16 rows: red, vermilion, orange, amber, yellow, lime, green, spring, cyan, sky, blue, violet, magenta, pink, white, warm white; columns from bright
to dim) on the panel at rising brightness and asks at which level the colours look right and can store it as the floor.

With --tune it then also tries the optional corrections (hue guard, dim-colour search, per-LED weights) on top of that; each step builds on the choices made
before it, and nothing is stored until you say so at the end. Ctrl-C puts the old settings back.

usage: python3 tools/colour-calibrate.py HOST [--levels 12,17,20,24,31,40,50,64,80,100] [--tune] [--level 17] [--check 36,70,136] [--narrow] [--face]
  --levels LIST panel brightness (0-255) steps of the minimum-brightness test, dimmest first (255 = full)
  --tune        also tune the optional corrections, at the dim level --level (default 17), and compare them with plain at --check levels
  --narrow      the 8-row grid (no white, no lime/spring/sky/violet/magenta) instead of the 16-row one
  --face        skip the grid: just apply each --tune candidate so you can judge it on the face you normally run
"""
import argparse, json, sys, urllib.parse, urllib.request

parser = argparse.ArgumentParser()
parser.add_argument("host")
parser.add_argument("--levels", default="12,17,20,24,31,40,50,64,80,100")
parser.add_argument("--tune", action="store_true")
parser.add_argument("--level", type=int, default=17)
parser.add_argument("--check", default="36,70,136")
parser.add_argument("--narrow", action="store_true")
parser.add_argument("--face", action="store_true")
args = parser.parse_args()
base = "http://" + args.host
steps = [int(v) for v in args.levels.split(",") if v.strip()]
levels = [args.level] + [int(v) for v in args.check.split(",") if v.strip()]
KEYS = ("search", "guard", "restore", "brightness", "extra", "limit", "guardMin", "wg", "wb", "gg", "gb", "og", "ob", "bg", "bb")


def call(path, method="GET", **params):
    url = base + path + ("?" + urllib.parse.urlencode(params) if params else "")
    request = urllib.request.Request(url, method=method, data=b"" if method == "POST" else None)
    with urllib.request.urlopen(request, timeout=10) as reply:
        body = reply.read().decode()
    return json.loads(body) if body.startswith("{") else body


def grid(level):
    if not args.face:
        call("/api/hue-test", "POST", b=level, secs=600, wide=0 if args.narrow else 1)


def show(settings, level=None):
    """Apply a full setting (reset first, so nothing from an earlier candidate lingers; not stored) and put the test grid up."""
    call("/api/dim-colour", "POST", save=0, reset=1, **settings)
    grid(level or args.level)


def pick(title, options, names, chosen):
    """Show each option on top of the choices so far and ask which was best. Returns that option, or {} for 'keep what I have'."""
    print("\n" + title)
    while True:
        for i, option in enumerate(options, 1):
            show({**chosen, **option})
            input("  %d/%d  %s   - look, then press Enter" % (i, len(options), names(option)))
        answer = input("Which number looked best? (1-%d, r = see them again, Enter = keep what I have): " % len(options)).strip().lower()
        if answer == "r": continue
        if answer.isdigit() and 1 <= int(answer) <= len(options): return options[int(answer) - 1]
        return {}


def ask(question):
    return input(question + " [y/N]: ").strip().lower() == "y"


original = call("/api/dim-colour")
try:
    call("/api/dim-colour", "POST", save=0, reset=1)  # the plain table: what the clock ships with
    print("Minimum brightness: the grid goes from bright (left) to dim (right). Look at the colours of the rows, mainly amber, yellow, white and warm white.")
    print("Answer for each level whether the colours look right (the dimmest columns may be black: that is fine, they just must not look wrong).")
    minimum = None
    for level in steps:
        grid(level)
        answer = input("  panel brightness %d/255 (%d%%): colours right? [y = yes, Enter = no, q = stop]: " % (level, round(level / 2.55))).strip().lower()
        if answer == "q": break
        if answer == "y": minimum = level; break
    if minimum is None:
        print("\nNo level looked right. Try brighter levels with --levels, or the optional corrections with --tune.")
    else:
        print("\nLowest level with right colours: %d/255 (%d%%)." % (minimum, round(minimum / 2.55)))
        if ask("Store it on the clock as the calibrated floor (brightness 1 = this level, 255 = full)?"):
            call("/api/dim-colour", "POST", floor=minimum)
            print("Stored. Set the Minimum slider in Settings > Display to its lowest position so the panel can go down to the floor. Remove it with: curl -X POST '%s/api/dim-colour?floor=0'" % base)
    if not args.tune:
        raise KeyboardInterrupt

    chosen = {"search": 0, "guard": 0, "restore": 1}
    print("\n--tune: the optional corrections, judged at the dim level %d/255." % args.level)
    show(chosen)
    input("Start: the plain setting (no hue guard, no search). Press Enter to continue.")

    chosen.update(pick("1/8  Hue guard: lights a dim pixel instead of leaving it black or grey. Option 1 is plain; the others set the lowest light it still lifts.",
                       [{"guard": 0}] + [{"guard": 1, "guardMin": g} for g in (0.04, 0.08, 0.12, 0.16)],
                       lambda o: "plain" if not o["guard"] else "hue guard, threshold %.2f" % o["guardMin"], chosen))
    plain = dict(chosen)  # the plain setting with the best guard choice is what the search has to beat

    chosen["search"] = 1
    chosen.update(pick("2/8  Warm colours (red, orange, amber, yellow): the amber should look amber, not orange or green. Weight 1.0 is neutral.",
                       [{"wg": w, "wb": w} for w in (1.0, 1.2, 1.4, 1.6, 1.8, 2.0)], lambda o: "green/blue weight %.1f" % o["wg"], chosen))
    chosen.update(pick("3/8  White and warm white (the bottom two rows): should stay neutral, not tinted green or magenta.",
                       [{"gg": g, "gb": b} for g, b in ((1.0, 1.0), (1.2, 1.2), (1.4, 1.4), (1.2, 1.0), (1.0, 1.2))],
                       lambda o: "green %.1f, blue %.1f" % (o["gg"], o["gb"]), chosen))
    chosen.update(pick("4/8  Green-led colours (green, teal): a pale green should stay pale and not darken.",
                       [{"og": g, "ob": b} for g, b in ((1.0, 1.0), (0.7, 1.0), (0.5, 1.0), (0.4, 1.3), (0.25, 1.3))],
                       lambda o: "green %.2f, blue %.2f" % (o["og"], o["ob"]), chosen))
    chosen.update(pick("5/8  Blue-led colours (cyan, blue): shades of blue should stay different, not violet or washed out.",
                       [{"bg": g, "bb": b} for g, b in ((1.0, 1.0), (0.8, 0.8), (0.65, 0.65), (0.5, 0.65), (0.65, 1.0))],
                       lambda o: "green %.2f, blue %.2f" % (o["bg"], o["bb"]), chosen))
    chosen.update(pick("6/8  Search strength: how much it favours brightness over exact colour (higher = brighter but less exact hue).",
                       [{"brightness": b} for b in (0.02, 0.06, 0.12, 0.25)], lambda o: "brightness weight %.2f" % o["brightness"], chosen))
    chosen.update(pick("7/8  Search reach: the light level below which the search runs (higher = more colours handled, can look blotchy).",
                       [{"limit": v} for v in (4, 6, 8, 12, 16)], lambda o: "limit %d" % o["limit"], chosen))

    print("\n8/8  Tuned vs plain at the levels %s." % ", ".join(map(str, levels)))
    wins = 0
    for level in levels:
        show(plain, level);  input("  level %d, PLAIN  - look, then press Enter" % level)
        show(chosen, level); input("  level %d, TUNED  - look, then press Enter" % level)
        wins += ask("  Tuned better at level %d?" % level)
    result = chosen if wins * 2 > len(levels) else plain
    print("\nTuned wins at %d of %d levels: keeping the %s setting." % (wins, len(levels), "tuned" if result is chosen else "plain"))
    print("Result: " + ", ".join("%s=%s" % (k, v) for k, v in result.items()))
    show(result)
    if not ask("Store this on the clock?"):
        raise KeyboardInterrupt
    call("/api/dim-colour", "POST", reset=1, **result)
    print("Stored. Reset any time with: curl -X POST '%s/api/dim-colour?reset=1'" % base)
except (KeyboardInterrupt, EOFError):
    print("\nNothing stored: putting the previous settings back.")
    call("/api/dim-colour", "POST", save=0, reset=1, **{k: v for k, v in original.items() if k in KEYS})
finally:
    try: call("/api/hue-test", "POST", b=0)
    except Exception: pass
