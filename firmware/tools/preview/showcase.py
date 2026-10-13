#!/usr/bin/env python3
"""Render GitHub GIFs from recorded feeds using the actual Football, F1 and Tetris faces."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import subprocess
import tempfile
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageOps

HERE = Path(__file__).resolve().parent
FIRMWARE = HERE.parent.parent
REPO = FIRMWARE.parent


def literal(text):
    return json.dumps(text, ensure_ascii=True)


def rgb565(hexcolor):
    r, g, b = (int(hexcolor[i:i + 2], 16) for i in (0, 2, 4))
    return f"0x{((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3):04X}"


def write_data(data, path, key="f1"):
    lines = ["static void initData() {", "#ifdef SHOWCASE_F1"]
    f1 = data[key]
    # MotoGP rows carry their own colour (the maker's); Formula 1 ones are looked up by code
    paint = lambda row: rgb565(row["color"]) if "color" in row else f"f1DriverColor({literal(row['code'])})"
    lines.append(f"  copyText(source.circuit, {literal(f1['circuit'])});")
    for i, row in enumerate(f1["standings"]):
        lines += [f"  copyText(source.standings[{i}].code, {literal(row['code'])});",
                  f"  source.standings[{i}].color = {paint(row)};",
                  f"  source.standings[{i}].points = {row['points']};"]
        if "name" in row:
            lines.append(f"  copyText(source.standings[{i}].name, {literal(row['name'])});")
    lines.append(f"  source.standingsCount = {len(f1['standings'])};")
    upcoming = f1["nextRace"]
    for field in ("city", "circuit"):
        lines.append(f"  copyText(source.upcoming[0].{field}, {literal(upcoming[field])});")
    lines += [f"  source.upcoming[0].start = {upcoming['start']};", "  source.upcomingCount = 1;"]
    coming = f1["nextSession"]
    lines += [f"  copyText(source.coming[0].name, {literal(coming['name'])});",
              f"  source.coming[0].start = {coming['start']};", "  source.comingCount = 1;",
              "  source.next.valid = true;", "  source.next.tag = 'Q';",
              f"  copyText(source.next.name, {literal(coming['name'])});",
              f"  source.next.start = {coming['start']};"]
    for name, tag, label, session_id in (("practice", "P", "FP1", 401839113),
                                        ("qualifying", "Q", f1.get("qName", "Q3"), 401839116),
                                        ("race", "R", "RACE", 401839117)):
        lines += [f"  {name}Start = {f1[name + 'Start']};", f"  {name}.start = {name}Start;",
                  f"  {name}.valid = {name}.fromFeed = true;", f"  {name}.tag = '{tag}';",
                  f"  {name}.id = {session_id};", f"  copyText({name}.name, {literal(label)});",
                  f"  {name}.count = {len(f1[name])};"]
        for i, row in enumerate(f1[name]):
            lines += [f"  copyText({name}.rows[{i}].code, {literal(row['code'])});",
                      f"  {name}.rows[{i}].color = {paint(row)};",
                      f"  copyText({name}.rows[{i}].time, {literal(row['time'])});",
                      f"  {name}.rows[{i}].fastest = {'true' if row['fastest'] else 'false'};"]
    lines += [f"  race.period = race.totalLaps = {f1['raceLaps']};", f"  raceEnd = {f1['raceEnd']};", "#elif !defined(SHOWCASE_TETRIS)"]
    football = data["football"]
    for table in football["tables"]:
        lines += ["  {", "    Table t;", f"    t.name = {literal(table['name'])};"]
        for row in table["rows"]:
            lines += [f"    t.teams.push_back({literal(row['code'])});", f"    t.points.push_back({row['points']});"]
        lines += ["    tables.push_back(t);", "  }"]
    for game in football["games"]:
        lines += ["  {", "    Game e = {};", "    e.scoreColor = e.barColor = color(\"50DC78\");",
                  "    e.progress = 1; e.detail = \"FT\"; e.competition = \"ERE\";",
                  f"    e.score = {literal(str(game['homeScore']) + '-' + str(game['awayScore']))};"]
        for side in ("home", "away"):
            team = game[side]
            lines += [f"    e.{side} = {literal(team['code'])};",
                      f"    e.{side}Kit = kit({literal(team['code'])}, {literal(game['league'])}, {literal(team['color'])}, {literal(team['second'])});",
                      f"    e.{side}Pos = {team['position']};"]
        for event in game["events"]:
            lines += ["    { FootballTicker::Event ev = {};",
                      f"      ev.minute = {event['minute']}; ev.kind = '{event['kind']}'; ev.home = {'true' if event['home'] else 'false'};",
                      f"      ev.number = {event['number']}; copyText(ev.name, {literal(event['name'])}); e.events.push_back(ev); }}"]
        lines += ["    games.push_back(e);", f"    kickoffs.push_back({game['kickoff']});", "  }"]
    for game in football["upcoming"]:
        lines += ["  {", "    Coming u = {};", "    u.color = color(\"50DC78\");",
                  f"    u.kickoff = {game['kickoff']};"]
        for side in ("home", "away"):
            team = game[side]
            lines += [f"    u.{side} = {literal(team['code'])};",
                      f"    u.{side}Kit = kit({literal(team['code'])}, {literal(game['league'])}, {literal(team['color'])}, {literal(team['second'])});",
                      f"    u.{side}Pos = {team['position']};"]
        lines += ["    coming.push_back(u);", "  }"]
    sub = football["substitution"]
    lines += [f"  substitution.home = games[{sub['game']}].home; substitution.away = games[{sub['game']}].away;",
              f"  substitution.homeKit = games[{sub['game']}].homeKit; substitution.awayKit = games[{sub['game']}].awayKit;",
              "  substitution.kind = 's';", f"  substitution.homeTeam = {'true' if sub['home'] else 'false'};",
              f"  substitution.minute = {sub['minute']}; substitution.number = {sub['number']}; substitution.numberOn = {sub['numberOn']};",
              "#endif", "}"]
    path.write_text("\n".join(lines) + "\n")


def render_boot(work):
    gfx = FIRMWARE / "vendor/gfx"
    common = FIRMWARE / "lib/core"
    output = work / "boot"
    output.mkdir(exist_ok=True)
    compiler = os.environ.get("CXX", "c++")
    qr = work / "qrcodegen.o"
    subprocess.run([os.environ.get("CC", "cc"), "-O2", "-w", "-c", str(common / "qrcodegen.c"), "-o", str(qr)], check=True)
    command = [compiler, "-std=c++17", "-O2", "-DARDUINO=100", "-w"]
    for include in (HERE / "shim", gfx, common):
        command += ["-I", str(include)]
    executable = work / "showcase-boot"
    command += [str(HERE / "boot.cpp"), str(common / "BootScreen.cpp"), str(gfx / "Adafruit_GFX.cpp"), str(qr), "-o", str(executable)]
    subprocess.run(command, check=True)
    subprocess.run([str(executable), str(output)], check=True)
    return output


def render(face, work):
    if face == "boot":
        return render_boot(work)
    folder = FIRMWARE / "clockfaces" / face
    gfx = FIRMWARE / "vendor/gfx"
    if not gfx.is_dir():
        raise SystemExit(f"Missing {gfx}: restore the pinned vendor sources.")
    common = FIRMWARE / "lib/core"
    library = FIRMWARE / "lib" / {"f1": "f1-feed", "motogp": "f1-feed", "tetris": "tetris"}.get(face, "football-feed")
    if face == "motogp":
        folder = FIRMWARE / "clockfaces" / "f1"
    output = work / face
    output.mkdir(exist_ok=True)
    command = [os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-DARDUINO=100", "-w"]
    command += {"f1": ["-DSHOWCASE_F1", "-DUM_PIXEL_DEPTH_BITS=8"], "motogp": ["-DSHOWCASE_F1", "-DUM_MOTOGP", "-DUM_PIXEL_DEPTH_BITS=8"],
                "tetris": ["-DSHOWCASE_TETRIS"]}.get(face, ["-DUM_FOOTBALL_ONLY"])
    for include in (HERE / "shim", work / face if face == "motogp" else work, gfx, folder, library, FIRMWARE / "lib/motogp-feed", common):
        command += ["-I", str(include)]
    command += [str(HERE / "showcase.cpp"), str(folder / "Clockface.cpp"), str(gfx / "Adafruit_GFX.cpp")]
    if face in ("f1", "motogp"):
        command += [str(folder / "F1Renderer.cpp"), str(folder / "F1Screens.cpp")]
    if face == "tetris":
        command += [str(folder / "TetrisRenderer.cpp"), str(folder / "TetrisScreens.cpp")]
        command += [str(path) for path in sorted(library.glob("*.cpp"))]
    if face == "football":
        command += [str(folder / "FootballRenderer.cpp"), str(folder / "FootballScreens.cpp"), str(library / "GoalAnimation.cpp"), str(library / "TeamColors.cpp")]
    executable = work / ("showcase-" + face)
    command += ["-o", str(executable)]
    subprocess.run(command, check=True)
    subprocess.run([str(executable), str(output)], check=True)
    return output


def animate(frames, destination, size):
    timing = [line.split("\t") for line in (frames / "frames.tsv").read_text().splitlines()]
    # Merge identical holds; all animation frames keep their real timing.
    changes = []
    previous = None
    core_colors = Counter()
    for name, at in timing[:-1]:
        raw = (frames / name).read_bytes()
        if raw != previous:
            changes.append((name, int(at)))
            for count, color in Image.open(frames / name).convert("RGB").getcolors(4096):
                core_colors[color] += count
            previous = raw
    end = int(timing[-1][1])
    mask = Image.new("L", (512, 512))
    mask_dots = ImageDraw.Draw(mask)
    for y in range(64):
        for x in range(64):
            mask_dots.ellipse((x * 8 + 1, y * 8 + 1, x * 8 + 6, y * 8 + 6), fill=255)
    gaps = ImageOps.invert(mask).convert("RGB")

    def panel(name):
        source = Image.open(frames / name).convert("RGB")
        canvas = Image.new("RGB", (512, 512), (8, 8, 10))
        light = Image.new("RGB", canvas.size)
        dots, glow = ImageDraw.Draw(canvas), ImageDraw.Draw(light)
        for y in range(64):
            for x in range(64):
                color = source.getpixel((x, y))
                bounds = (x * 8 + 1, y * 8 + 1, x * 8 + 6, y * 8 + 6)
                dots.ellipse(bounds, fill=color if any(color) else (12, 12, 14))
                if max(color) > 64:
                    glow.ellipse(bounds, fill=color)
        light = light.filter(ImageFilter.GaussianBlur(1.2)).point(lambda c: round(c * 0.22))
        light = ImageChops.multiply(light, gaps)
        return ImageChops.add(canvas, light).resize((size, size), Image.Resampling.LANCZOS)

    # One palette for the whole replay keeps dim LEDs and glow steady between frames.
    samples = Image.new("RGB", (1024, 1024))
    for i in range(64):
        at = changes[0][1] + (end - changes[0][1]) * i / 64
        name, _ = min(changes, key=lambda frame: abs(frame[1] - at))
        samples.paste(panel(name).resize((128, 128), Image.Resampling.NEAREST), ((i % 8) * 128, (i // 8) * 128))
    palette = samples.quantize(colors=192, method=Image.Quantize.MEDIANCUT)
    # Reserve exact renderer colours alongside the antialias/glow palette, so clock and team colours stay faithful.
    pinned = [component for color, _ in core_colors.most_common(64) for component in color]
    colors = palette.getpalette()[:192 * 3] + pinned
    palette.putpalette(colors + [0] * (768 - len(colors)))
    images, durations = [], []
    for i, (name, at) in enumerate(changes):
        until = changes[i + 1][1] if i + 1 < len(changes) else end
        images.append(panel(name).quantize(palette=palette, dither=Image.Dither.NONE))
        durations.append(max(10, round((until - at) / 10) * 10))
    images[0].save(destination, save_all=True, append_images=images[1:], duration=durations, loop=0, disposal=1, optimize=True)
    print(f"{destination.name}: {len(changes)} frames, {(end - changes[0][1]) / 1000:.1f}s, {destination.stat().st_size:,} bytes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path, default=HERE / "showcase-data.json")
    parser.add_argument("--out", type=Path, default=REPO / "docs/images/faces")
    parser.add_argument("--work", type=Path, help="keep native render frames here for review")
    parser.add_argument("--size", type=int, default=400)
    parser.add_argument("--face", choices=("football", "f1", "motogp", "tetris", "boot"), help="render only one face")
    parser.add_argument("--reuse-frames", action="store_true", help="restyle existing --work frames without compiling")
    args = parser.parse_args()
    if args.reuse_frames and not args.work:
        raise SystemExit("--reuse-frames requires --work.")
    data = json.loads(args.data.read_text())
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ultimatrix-showcase-") as temporary:
        work = args.work or Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        write_data(data, work / "showcase-data.h")
        if "motogp" in data:
            (work / "motogp").mkdir(exist_ok=True)
            write_data(data, work / "motogp" / "showcase-data.h", "motogp")
        for face in ([args.face] if args.face else ("football", "f1", "motogp", "tetris", "boot")):
            frames = work / face if args.reuse_frames else render(face, work)
            animate(frames, args.out / f"{face}.gif", args.size)


if __name__ == "__main__":
    main()
