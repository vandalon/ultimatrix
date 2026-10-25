# Contributing to Ultimatrix

Thanks for wanting to help. Bug reports, photos of your build, new faces and documentation fixes are all welcome.

## Ways to help

- **Report a bug or suggest an improvement** with an [issue](https://github.com/vandalon/ultimatrix/issues/new/choose).
- **Ask for a new face** in the [Request a face](https://github.com/vandalon/ultimatrix/discussions/1) discussion.
- **Ask a question or show your clock** in [Discussions](https://github.com/vandalon/ultimatrix/discussions).
- **Pick up an issue** labelled [good first issue](https://github.com/vandalon/ultimatrix/labels/good%20first%20issue).

Check [LICENSE_SCOPE.md](LICENSE_SCOPE.md) for how the code is licensed before you contribute.

## Setting up

From the `firmware/` folder:

1. Run `./tools/setup-idf.sh` once to install the ESP-IDF SDK locally.
2. Build one face with `./tools/idf.sh <face> build`, where `<face>` is one of `home`, `football`, `f1`, `motogp`, `tetris`, `word`, `mario`, `luigi`, `pacman`, `invaders`, `pong`.
3. Flash it over USB with `./tools/idf.sh <face> app-flash -p <port>`.

Hardware, wiring and the browser installer are covered in [docs/HARDWARE.md](docs/HARDWARE.md) and the [README](README.md). Runtime design is in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Where things are

- `firmware/clockfaces/<name>/` holds one face (`Clockface.cpp` and `Clockface.h`), implementing `IClockface` from `firmware/lib/core/IClockface.h`.
- `firmware/lib/core/` holds the shared services (settings, time, updates, MQTT, notifications).
- `firmware/web/` holds the settings pages served by the clock.
- `.claude/filemap.md` is a one-line-per-file index of the repository.

## Adding a face

A face is small and self-contained. Start from the smallest existing one (`scanner`) or from a face close to what you want (`word` for text, `mario` or `pacman` for pixel art). A face is registered in a few places, so search the repository for an existing mode name such as `luigi` to find them all, and add yours the same way.

Native previews (`firmware/tools/preview/`) render a face on your computer without a panel, which is the quickest way to iterate on drawing code.

## Before you open a pull request

- Keep the change small and focused, and match the style of the code around it.
- Say which face or faces you built and what you checked on a real panel. Untested is fine, as long as the pull request says so.
- Update the README or docs if you change something a user can see.
- Never put Wi-Fi passwords, tokens or API keys in code, logs or screenshots.
