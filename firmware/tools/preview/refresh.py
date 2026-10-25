#!/usr/bin/env python3
"""Render the clock GIFs, face-picker PNGs and README overview from current code."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from PIL import Image, ImageDraw
import faces
import showcase

REPO = showcase.REPO
# Capture a settled frame that identifies each face, rather than a boot or transition frame.
CLOCKS = (
    ('home', 'Home', 1500),
    ('football', 'Football', 41000),
    ('f1', 'Formula 1', 24000),
    ('motogp', 'MotoGP', 24000),
    ('tetris', 'Tetris', 10000),
    ('word', 'Words', 2000),
    ('mario', 'Mario', 2000),
    ('luigi', 'Luigi', 2000),
    ('pacman', 'Pac-Man', 6000),
)


# Web-sized GIFs: small, low frame rate and a tiny palette (the README shows them at 128 px).
GIF_SIZE, GIF_FPS, GIF_COLOURS, GIF_MAX_SECONDS = 160, 10, 64, 40


def small_gif(frames, destination, size):
    """Render the styled GIF, then shrink it with ffmpeg and keep at most GIF_MAX_SECONDS."""
    big = destination.with_name(destination.stem + '.full.gif')
    showcase.animate(frames, big, size)
    try:
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-t', str(GIF_MAX_SECONDS), '-i', str(big), '-vf',
                        f'fps={GIF_FPS},scale={GIF_SIZE}:{GIF_SIZE}:flags=lanczos,split[a][b];'
                        f'[a]palettegen=max_colors={GIF_COLOURS}:stats_mode=diff[p];'
                        '[b][p]paletteuse=dither=none:diff_mode=rectangle',
                        '-loop', '0', str(destination)], check=True)
    finally:
        big.unlink(missing_ok=True)
    print(f'{destination.name}: {destination.stat().st_size:,} bytes (small)', flush=True)


def timeline(folder):
    return [(name, int(at)) for name, at in
            (line.split('\t') for line in (folder / 'frames.tsv').read_text().splitlines())]


def still(folder, at):
    name, _ = min(timeline(folder)[:-1], key=lambda frame: abs(frame[1] - at))
    with Image.open(folder / name) as image:
        return image.convert('RGB')


def sheet(items, destination, columns=4, size=256, leds=True):
    gap, heading = 16, 28
    rows = (len(items) + columns - 1) // columns
    canvas = Image.new('RGB', (columns * (size + gap) + gap,
                              rows * (size + heading + gap) + gap), (14, 20, 27))
    draw = ImageDraw.Draw(canvas)
    gaps = showcase.panel_gaps() if leds else None
    for index, (label, source) in enumerate(items):
        x = gap + (index % columns) * (size + gap)
        y = gap + (index // columns) * (size + heading + gap)
        draw.text((x, y + 5), label, fill=(224, 237, 245))
        picture = showcase.panel_image(source, size, gaps) if leds else source.resize((size, size), Image.Resampling.NEAREST)
        canvas.paste(picture, (x, y + heading))
    canvas.save(destination)
    print(f'{destination.name}: {canvas.width} x {canvas.height}', flush=True)


def clip(folder, work, name, start, end):
    destination = work / name
    destination.mkdir(exist_ok=True)
    records = [(filename, at) for filename, at in timeline(folder)[:-1] if start <= at < end]
    if not records:
        raise ValueError(f'No frames between {start} and {end} in {folder}')
    lines = []
    for filename, at in records:
        shutil.copyfile(folder / filename, destination / filename)
        lines.append(f'{filename}\t{at}')
    lines.append(f'end\t{end}')
    (destination / 'frames.tsv').write_text('\n'.join(lines) + '\n')
    return destination


def refresh(out, web_out, work, reuse):
    data = json.loads((showcase.HERE / 'showcase-data.json').read_text())
    showcase.write_data(data, work / 'showcase-data.h')
    (work / 'motogp').mkdir(exist_ok=True)
    showcase.write_data(data, work / 'motogp' / 'showcase-data.h', 'motogp')
    captures = []
    for name, label, at in CLOCKS:
        print(f'Rendering {label} from firmware sources...', flush=True)
        if reuse:
            frames = work / name
        elif name in faces.FACES:
            frames = faces.render(name, work, [])
        else:
            frames = showcase.render(name, work)
        picture = still(frames, at)
        picture.save(web_out / f'{name}.png')
        captures.append((label, picture))
        small_gif(frames, out / f'{name}.gif', 400)
    sheet(captures, out / 'overview.png')

    print('Rendering boot, update and welcome screens...', flush=True)
    boot = work / 'boot' if reuse else showcase.render_boot(work)
    small_gif(boot, out / 'boot.gif', 400)

    # Refresh the shorter examples too, using the same current Words replay.
    for name, face, start, end in (
        ('mario-hour', 'mario', 42000, 64000),
        ('luigi-hour', 'luigi', 42000, 64000),
        ('word-transitions', 'word', 19200, 35200),
    ):
        frames = clip(work / face, work, name, start, end)
        small_gif(frames, out / f'{name}.gif', 384)

    palette_work = work / 'palettes'
    palette_work.mkdir(exist_ok=True)
    palette = palette_work / 'pacman' if reuse else faces.render('pacman', palette_work, ['-DPREVIEW_PACMAN_PALETTES'])
    walls = ('Default', 'Pastel purple', 'Pastel green', 'Pastel pink', 'Pastel blue',
             'Peach', 'Lemon', 'Orange', 'Red', 'Cyan', 'Custom ghosts / dots')
    colours = ('Default', 'Pastel green', 'Pastel pink', 'Pastel blue', 'Lemon',
               'Peach', 'Pastel purple', 'Red', 'Orange', 'Cyan', 'White')
    sheet([(label, still(palette, index * 500 + 250)) for index, label in enumerate(walls)],
          out / 'pacman-colours.png', columns=5, size=160, leds=False)
    sheet([(label, still(palette, 5500 + index * 500 + 250)) for index, label in enumerate(colours)],
          out / 'pacman-clock-colours.png', columns=5, size=160, leds=False)
    animations = palette_work / 'word' if reuse else faces.render('word', palette_work, ['-DPREVIEW_WORD_ANIMATIONS'])
    labels = ('Rotate all effects', 'Letter shuffle', 'Letter lift', 'Pixel dissolve', 'Soft wipe', 'None')
    sheet([(label, still(animations, index * 4000 + 2600)) for index, label in enumerate(labels)],
          out / 'word-animation-settings.png', columns=6, size=128, leds=False)
    print('All clock images refreshed. Rebuild firmware to embed the face-picker PNGs.', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=REPO / 'docs/previews')
    parser.add_argument('--web-out', type=Path, default=REPO / 'firmware/web/faces')
    parser.add_argument('--work', type=Path, help='keep native frames for inspection')
    parser.add_argument('--reuse-frames', action='store_true', help='export previously rendered --work frames')
    args = parser.parse_args()
    if args.reuse_frames and not args.work:
        parser.error('--reuse-frames needs --work')
    args.out.mkdir(parents=True, exist_ok=True)
    args.web_out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='ultimatrix-refresh-') as temporary:
        work = args.work or Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        refresh(args.out, args.web_out, work, args.reuse_frames)


if __name__ == '__main__':
    main()
