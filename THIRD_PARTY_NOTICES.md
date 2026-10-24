# Third-party notices

The source imports retain their required copyright notices. See LICENSE_SCOPE.md.

Pinned firmware dependencies and preserved licence copies:

- uzlib inflate/checksums (Joergen Ibsen and Paul Sokolovsky, zlib licence), firmware/vendor/uzlib: LICENSES/uzlib.txt. The local configuration enables extra input checks.
- HUB75 matrix DMA driver: LICENSES/HUB75-MIT.txt.
- Adafruit GFX: LICENSES/Adafruit-GFX-BSD-2-Clause.txt.
- Adafruit BusIO: LICENSES/Adafruit-BusIO-MIT.txt.
- WiFiManager: LICENSES/WiFiManager-MIT.txt. A small hook (`setWaitCallback`) was added to the vendored copy.
- QR Code generator library (Project Nayuki, MIT), lib/core/qrcodegen.c/.h, unmodified copy of the version in the
  Espressif `qrcode` component: LICENSES/Nayuki-QR-MIT.txt, https://www.nayuki.io/page/qr-code-generator-library.

- Tetris pieces, digit tables and rotation rule (lib/tetris/TetrisDigits.*, TetrisShapes.*): derived from
  TetrisAnimation, https://github.com/toblum/TetrisAnimation, copyright 2018 Tobias Blum,
  LGPL-2.1-or-later: LICENSES/LGPL-2.1.txt. The digit tables carried no notice of their own in the source project;
  they are treated as part of that library, which should be confirmed before a release. The library's letter tables
  (font by Mike Swan, https://github.com/n00dles101) are not used or included.

- Time in Words language tables (clockfaces/word/WordLanguages.*): derived from the Clockwise fork's cw-cf-0x02
  (MIT), https://github.com/vandalon/clockwizer.
- MotoGP data: the results and live-timing API of motogp.com (api.pulselive.motogp.com), read-only, public, unauthenticated.
  No licence is given for it here; check its terms before distributing a release.
- Pac-Man Classic (clockfaces/pacmanclassic): grew out of the Pac-Man clockface of the MIT-licensed Clockwise project (cw-cf-0x05
  in the Clockwise fork): the ghosts that live on the clock and leave through its floor, and the way pacman looks for food. The
  licence terms of that part are those of the Clockwise MIT licence.
- Mario, Luigi, Goomba, Boo, Pac-Man and the ghosts are characters of Nintendo and Bandai Namco. The sprites are drawn for
  this project from scratch; no original assets are used.

The Football flip digits are derived from DejaVu Sans Condensed Bold. Retain
its font licence when redistributing those glyphs.
Arduino ESP32 and ESP-IDF components retain their SDK and dependency licences.
No licence here replaces dependency terms or grants rights to external feed data.

The F1 loading tyre bitmap was derived from a user-supplied image; its original
image permission was not documented in the source project's inventory. Complete
font and image provenance and transitive SDK notices before distributing a release.
