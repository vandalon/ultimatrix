# Pinned Arduino libraries

These source copies retain their package licence files and are compiled as an
ESP-IDF component. Previews use this same GFX source; PlatformIO is not required.

| Directory | Source | Revision |
| --- | --- | --- |
| hub75 | mrcodetastic/ESP32-HUB75-MatrixPanel-DMA | f17fb7fe9d487e9643f919eb5aeedea8d9d1f8d7 |
| gfx | adafruit/Adafruit-GFX-Library | 1.12.6 |
| busio | adafruit/Adafruit_BusIO | 1.17.4 |
| wifi-manager | tzapu/WiFiManager | 4131fe617d01d746677e8b909379597162c2ca58 |

Local change: wifi-manager has a `setWaitCallback()` hook (WiFiManager.h/.cpp, called every ~20 ms while a connection
attempt waits) so the boot animation keeps running. Reapply it when updating the pinned revision.
